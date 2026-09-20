#include "board.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define I2C_SDA_GPIO       18
#define I2C_SCL_GPIO       8
#define I2C_PORT           I2C_NUM_0
#define I2C_SPEED_HZ       100000
#define I2C_TIMEOUT_MS     1000
#define I2C_PROBE_RETRIES  3
#define SHTC3_ADDRESS      0x70
#define PCF85063_ADDRESS   0x51
#define TCA9554_ADDRESS    0x20
#define TCA_OUTPUT_REG     0x01
#define TCA_CONFIG_REG     0x03
#define TCA_EPD_POWER_BIT  (1U << 0)
#define TCA_GREEN_LED_BIT  (1U << 4)
#define TCA_BAT_ENABLE_BIT (1U << 5)
#define BATTERY_ADC_CHAN   ADC_CHANNEL_0
#define BATTERY_ADC_SAMPLES 32
#define BATTERY_ADC_TRIM    4
#define BATTERY_DIVIDER_RATIO 2U

static const char *TAG = "board";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_shtc3;
static i2c_master_dev_handle_t s_rtc;
static i2c_master_dev_handle_t s_expander;
static uint8_t s_expander_output;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;

static esp_err_t add_i2c_device(uint8_t address, i2c_master_dev_handle_t *handle)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &config, handle);
}

static esp_err_t write_register(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), I2C_TIMEOUT_MS);
}

static void log_i2c_scan(void)
{
    bool found = false;
    ESP_LOGW(TAG, "I2C scan on SDA GPIO%d / SCL GPIO%d", I2C_SDA_GPIO, I2C_SCL_GPIO);
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        if (i2c_master_probe(s_bus, address, 50) == ESP_OK) {
            ESP_LOGW(TAG, "I2C device found at 0x%02x", address);
            found = true;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "no I2C device answered; check board power and SDA/SCL hardware");
    }
}

static esp_err_t probe_i2c_device(uint8_t address, const char *name)
{
    esp_err_t result = ESP_FAIL;
    for (int attempt = 1; attempt <= I2C_PROBE_RETRIES; ++attempt) {
        result = i2c_master_probe(s_bus, address, I2C_TIMEOUT_MS);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "%s detected at I2C address 0x%02x", name, address);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s at 0x%02x did not ACK (attempt %d/%d): %s",
                 name, address, attempt, I2C_PROBE_RETRIES, esp_err_to_name(result));
        (void)i2c_master_bus_reset(s_bus);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    log_i2c_scan();
    return result;
}

static esp_err_t expander_commit(void)
{
    return write_register(s_expander, TCA_OUTPUT_REG, s_expander_output);
}

static uint8_t shtc3_crc(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static uint8_t bcd_to_u8(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0fU));
}

static uint8_t u8_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

/* UTC civil date conversion, independent of the configured local timezone. */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned month_from_march = month > 2 ? month - 3 : month + 9;
    const unsigned doy = (153U * month_from_march + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

typedef struct {
    uint16_t millivolts;
    uint8_t percent;
} battery_ocv_point_t;

/* Generic 1S Li-ion/LiPo open-circuit curve at room temperature. The board
 * reads VBAT before Wi-Fi starts, so the load-induced voltage drop is small.
 * Tune these points if the fitted cell has a manufacturer OCV curve. */
static const battery_ocv_point_t s_battery_ocv_curve[] = {
    {3270,   0}, {3500,   5}, {3610,  10}, {3690,  15}, {3710,  20},
    {3730,  25}, {3750,  30}, {3770,  35}, {3790,  40}, {3800,  45},
    {3820,  50}, {3850,  55}, {3870,  60}, {3910,  65}, {3950,  70},
    {3980,  75}, {4020,  80}, {4050,  85}, {4080,  90}, {4100,  95},
    {4180, 100},
};

static uint8_t battery_percent_from_ocv(uint32_t millivolts)
{
    if (millivolts <= s_battery_ocv_curve[0].millivolts) {
        return 0;
    }
    for (size_t i = 1; i < sizeof(s_battery_ocv_curve) / sizeof(s_battery_ocv_curve[0]); ++i) {
        if (millivolts <= s_battery_ocv_curve[i].millivolts) {
            const battery_ocv_point_t *low = &s_battery_ocv_curve[i - 1];
            const battery_ocv_point_t *high = &s_battery_ocv_curve[i];
            const uint32_t voltage_span = high->millivolts - low->millivolts;
            const uint32_t percent_span = high->percent - low->percent;
            const uint32_t interpolated =
                ((millivolts - low->millivolts) * percent_span + voltage_span / 2U) /
                voltage_span;
            return (uint8_t)(low->percent + interpolated);
        }
    }
    return 100;
}

static void sort_millivolts(int *samples, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        const int value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
}

esp_err_t board_init(void)
{
    const gpio_config_t buttons = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_BOOT_GPIO) | (1ULL << BOARD_BUTTON_POWER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&buttons), TAG, "buttons");

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "I2C bus");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(probe_i2c_device(TCA9554_ADDRESS, "TCA9554"), TAG,
                        "TCA9554 not found");
    ESP_RETURN_ON_ERROR(add_i2c_device(SHTC3_ADDRESS, &s_shtc3), TAG, "SHTC3");
    ESP_RETURN_ON_ERROR(add_i2c_device(PCF85063_ADDRESS, &s_rtc), TAG, "RTC");
    ESP_RETURN_ON_ERROR(add_i2c_device(TCA9554_ADDRESS, &s_expander), TAG, "TCA9554");

    /* P2 (RTC interrupt) and P6 (touch interrupt) are inputs; all other pins outputs.
     * EXIO5 keeps the battery power path enabled after the PWR key is released.
     * The green LED is active-low on EXIO4 (the schematic's GP4 path is not fitted),
     * so leave its bit cleared while the device is awake. */
    s_expander_output = TCA_BAT_ENABLE_BIT;
    ESP_RETURN_ON_ERROR(expander_commit(), TAG, "TCA output");
    ESP_RETURN_ON_ERROR(write_register(s_expander, TCA_CONFIG_REG, 0x44), TAG, "TCA direction");

    const adc_oneshot_unit_init_cfg_t adc_unit = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_unit, &s_adc), TAG, "ADC unit");
    const adc_oneshot_chan_cfg_t adc_channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHAN, &adc_channel), TAG, "ADC channel");

    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHAN,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    s_adc_calibrated = adc_cali_create_scheme_curve_fitting(&cali, &s_adc_cali) == ESP_OK;
    if (!s_adc_calibrated) {
        ESP_LOGW(TAG, "ADC calibration unavailable; battery voltage is approximate");
    }

    /* 12.5 pF oscillator load, clock running. */
    (void)write_register(s_rtc, 0x00, 0x01);
    return ESP_OK;
}

esp_err_t board_epaper_power(bool enabled)
{
    if (enabled) {
        s_expander_output |= TCA_EPD_POWER_BIT;
    } else {
        s_expander_output &= (uint8_t)~TCA_EPD_POWER_BIT;
    }
    return expander_commit();
}

esp_err_t board_battery_measure_power(bool enabled)
{
    if (enabled) {
        s_expander_output |= TCA_BAT_ENABLE_BIT;
    } else {
        s_expander_output &= (uint8_t)~TCA_BAT_ENABLE_BIT;
    }
    return expander_commit();
}

esp_err_t board_read_environment(board_sample_t *sample)
{
    if (!sample) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t wake[] = {0x35, 0x17};
    const uint8_t measure[] = {0x78, 0x66};
    const uint8_t sleep[] = {0xb0, 0x98};
    uint8_t data[6];

    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_shtc3, wake, sizeof(wake), I2C_TIMEOUT_MS), TAG, "SHTC3 wake");
    /* SHTC3 needs at least 240 us after wake. With a 100 Hz FreeRTOS tick,
     * pdMS_TO_TICKS(1) is zero and previously sent the next command immediately. */
    esp_rom_delay_us(500);
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_shtc3, measure, sizeof(measure), I2C_TIMEOUT_MS), TAG, "SHTC3 measure");
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t result = i2c_master_receive(s_shtc3, data, sizeof(data), I2C_TIMEOUT_MS);
    (void)i2c_master_transmit(s_shtc3, sleep, sizeof(sleep), I2C_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(result, TAG, "SHTC3 read");

    if (shtc3_crc(data, 2) != data[2] || shtc3_crc(data + 3, 2) != data[5]) {
        ESP_LOGE(TAG, "SHTC3 CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    const uint16_t raw_t = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t raw_h = ((uint16_t)data[3] << 8) | data[4];
    sample->temperature_c = -45.0f + 175.0f * (float)raw_t / 65536.0f +
                            (float)CONFIG_RHT_TEMP_OFFSET_C_X100 / 100.0f;
    sample->humidity_pct = 100.0f * (float)raw_h / 65536.0f;
    sample->humidity_pct = fminf(100.0f, fmaxf(0.0f, sample->humidity_pct));
    return ESP_OK;
}

esp_err_t board_read_battery(float *voltage, uint8_t *percent)
{
    if (!voltage || !percent) {
        return ESP_ERR_INVALID_ARG;
    }
    int samples[BATTERY_ADC_SAMPLES];
    for (size_t i = 0; i < BATTERY_ADC_SAMPLES; ++i) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATTERY_ADC_CHAN, &raw), TAG, "ADC read");
        int millivolts = 0;
        if (s_adc_calibrated) {
            ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &millivolts), TAG, "ADC convert");
        } else {
            millivolts = (raw * 2500) / 4095;
        }
        samples[i] = millivolts;
        esp_rom_delay_us(250);
    }

    sort_millivolts(samples, BATTERY_ADC_SAMPLES);
    int64_t millivolts_sum = 0;
    for (size_t i = BATTERY_ADC_TRIM; i < BATTERY_ADC_SAMPLES - BATTERY_ADC_TRIM; ++i) {
        millivolts_sum += samples[i];
    }
    const uint32_t sample_count = BATTERY_ADC_SAMPLES - 2U * BATTERY_ADC_TRIM;
    const uint32_t adc_millivolts =
        (uint32_t)((millivolts_sum + sample_count / 2U) / sample_count);
    const uint32_t battery_millivolts = adc_millivolts * BATTERY_DIVIDER_RATIO;
    *voltage = (float)battery_millivolts / 1000.0f;
    *percent = battery_percent_from_ocv(battery_millivolts);
    ESP_LOGI(TAG, "battery OCV: %lu mV, SOC: %u%%",
             (unsigned long)battery_millivolts, (unsigned)*percent);
    return ESP_OK;
}

bool board_rtc_get(time_t *utc)
{
    if (!utc) {
        return false;
    }
    const uint8_t start = 0x04;
    uint8_t data[7] = {0};
    if (i2c_master_transmit_receive(s_rtc, &start, 1, data, sizeof(data), I2C_TIMEOUT_MS) != ESP_OK ||
        (data[0] & 0x80U)) {
        return false;
    }
    const int second = bcd_to_u8(data[0] & 0x7fU);
    const int minute = bcd_to_u8(data[1] & 0x7fU);
    const int hour = bcd_to_u8(data[2] & 0x3fU);
    const unsigned day = bcd_to_u8(data[3] & 0x3fU);
    const unsigned month = bcd_to_u8(data[5] & 0x1fU);
    const int year = 2000 + bcd_to_u8(data[6]);
    if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    const int64_t value = days_from_civil(year, month, day) * 86400LL +
                          hour * 3600LL + minute * 60LL + second;
    *utc = (time_t)value;
    const struct timeval tv = {.tv_sec = *utc, .tv_usec = 0};
    settimeofday(&tv, NULL);
    return true;
}

esp_err_t board_rtc_set(time_t utc)
{
    struct tm value;
    gmtime_r(&utc, &value);
    if (value.tm_year + 1900 < 2000 || value.tm_year + 1900 > 2099) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t data[] = {
        0x04,
        u8_to_bcd((uint8_t)value.tm_sec),
        u8_to_bcd((uint8_t)value.tm_min),
        u8_to_bcd((uint8_t)value.tm_hour),
        u8_to_bcd((uint8_t)value.tm_mday),
        u8_to_bcd((uint8_t)value.tm_wday),
        u8_to_bcd((uint8_t)(value.tm_mon + 1)),
        u8_to_bcd((uint8_t)(value.tm_year + 1900 - 2000)),
    };
    return i2c_master_transmit(s_rtc, data, sizeof(data), I2C_TIMEOUT_MS);
}

void board_prepare_for_sleep(void)
{
    (void)board_epaper_power(false);
    /* Active-low: turn the activity LED off before deep sleep. */
    s_expander_output |= TCA_GREEN_LED_BIT;
    (void)expander_commit();
}
