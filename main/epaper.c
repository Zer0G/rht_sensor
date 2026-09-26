#include "epaper.h"
#include "font_share_tech_mono.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qrcode.h"

#define EPD_WIDTH       200
#define EPD_HEIGHT      200
#define EPD_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 8)
#define EPD_SPI_HOST    SPI2_HOST
#define EPD_MOSI_GPIO   5
#define EPD_SCLK_GPIO   6
#define EPD_CS_GPIO     7
#define EPD_BUSY_GPIO   10
#define EPD_RESET_GPIO  11
#define EPD_DC_GPIO     15
#define EPD_BUSY_POLL_MS 10
#define EPD_BUSY_TIMEOUT_MS 30000

static const char *TAG = "epaper";
static spi_device_handle_t s_spi;
static uint8_t s_framebuffer[EPD_BUFFER_SIZE];

/* Waveform supplied for the 200x200 panel used by the Waveshare board. */
static const uint8_t s_full_lut[159] = {
    0x80,0x48,0x40,0,0,0,0,0,0,0,0,0, 0x40,0x48,0x80,0,0,0,0,0,0,0,0,0,
    0x80,0x48,0x40,0,0,0,0,0,0,0,0,0, 0x40,0x48,0x80,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0x0a, 0,0,0,0,0,0,0x08,0x01,0,0x08,0x01,0,0x02,
    0x0a,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0x22,0x22,0x22,0x22,0x22,0x22,0,
    0,0,0x22,0x17,0x41,0,0x32,0x20,
};

static esp_err_t spi_write(bool command, const void *data, size_t length)
{
    gpio_set_level(EPD_DC_GPIO, command ? 0 : 1);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t send_command(uint8_t command)
{
    return spi_write(true, &command, 1);
}

static esp_err_t send_data(const void *data, size_t length)
{
    return spi_write(false, data, length);
}

static esp_err_t send_byte(uint8_t value)
{
    return send_data(&value, 1);
}

static esp_err_t wait_ready(void)
{
    for (int elapsed_ms = 0; elapsed_ms < EPD_BUSY_TIMEOUT_MS;
         elapsed_ms += EPD_BUSY_POLL_MS) {
        if (gpio_get_level(EPD_BUSY_GPIO) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(EPD_BUSY_POLL_MS));
    }
    ESP_LOGE(TAG, "display busy timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t set_window_and_cursor(void)
{
    const uint8_t x_range[] = {0, (EPD_WIDTH - 1) >> 3};
    const uint8_t y_range[] = {EPD_HEIGHT - 1, 0, 0, 0};
    const uint8_t cursor_x = 0;
    const uint8_t cursor_y[] = {EPD_HEIGHT - 1, 0};
    ESP_RETURN_ON_ERROR(send_command(0x44), TAG, "x window cmd");
    ESP_RETURN_ON_ERROR(send_data(x_range, sizeof(x_range)), TAG, "x window");
    ESP_RETURN_ON_ERROR(send_command(0x45), TAG, "y window cmd");
    ESP_RETURN_ON_ERROR(send_data(y_range, sizeof(y_range)), TAG, "y window");
    ESP_RETURN_ON_ERROR(send_command(0x4e), TAG, "x cursor cmd");
    ESP_RETURN_ON_ERROR(send_byte(cursor_x), TAG, "x cursor");
    ESP_RETURN_ON_ERROR(send_command(0x4f), TAG, "y cursor cmd");
    return send_data(cursor_y, sizeof(cursor_y));
}

static esp_err_t load_lut(void)
{
    ESP_RETURN_ON_ERROR(send_command(0x32), TAG, "lut cmd");
    ESP_RETURN_ON_ERROR(send_data(s_full_lut, 153), TAG, "lut");
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "lut busy");
    ESP_RETURN_ON_ERROR(send_command(0x3f), TAG, "gate cmd");
    ESP_RETURN_ON_ERROR(send_byte(s_full_lut[153]), TAG, "gate");
    ESP_RETURN_ON_ERROR(send_command(0x03), TAG, "voltage cmd");
    ESP_RETURN_ON_ERROR(send_byte(s_full_lut[154]), TAG, "voltage");
    ESP_RETURN_ON_ERROR(send_command(0x04), TAG, "source voltage cmd");
    ESP_RETURN_ON_ERROR(send_data(&s_full_lut[155], 3), TAG, "source voltage");
    ESP_RETURN_ON_ERROR(send_command(0x2c), TAG, "vcom cmd");
    return send_byte(s_full_lut[158]);
}

static esp_err_t activate_full_refresh(void)
{
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "update cmd");
    ESP_RETURN_ON_ERROR(send_byte(0xc7), TAG, "update mode");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "display update");
    return wait_ready();
}

static esp_err_t clear_panel_to_white(void)
{
    memset(s_framebuffer, 0xff, sizeof(s_framebuffer));
    ESP_RETURN_ON_ERROR(set_window_and_cursor(), TAG, "clear cursor");

    /* Initialise both controller RAM planes as done by Waveshare's
     * DisplayPartBaseImage sequence. After EPD power cycling the old-image
     * plane is otherwise undefined and can leave visible remnants. */
    ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "clear new RAM cmd");
    ESP_RETURN_ON_ERROR(send_data(s_framebuffer, sizeof(s_framebuffer)), TAG,
                        "clear new RAM");
    ESP_RETURN_ON_ERROR(send_command(0x26), TAG, "clear old RAM cmd");
    ESP_RETURN_ON_ERROR(send_data(s_framebuffer, sizeof(s_framebuffer)), TAG,
                        "clear old RAM");
    return activate_full_refresh();
}

static esp_err_t controller_init(void)
{
    gpio_set_level(EPD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "reset busy");
    ESP_RETURN_ON_ERROR(send_command(0x12), TAG, "soft reset");
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "soft reset busy");

    const uint8_t driver[] = {0xc7, 0x00, 0x01};
    ESP_RETURN_ON_ERROR(send_command(0x01), TAG, "driver cmd");
    ESP_RETURN_ON_ERROR(send_data(driver, sizeof(driver)), TAG, "driver");
    ESP_RETURN_ON_ERROR(send_command(0x11), TAG, "entry mode cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x01), TAG, "entry mode");
    ESP_RETURN_ON_ERROR(set_window_and_cursor(), TAG, "RAM geometry");
    ESP_RETURN_ON_ERROR(send_command(0x3c), TAG, "border cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x01), TAG, "border");
    ESP_RETURN_ON_ERROR(send_command(0x18), TAG, "temp sensor cmd");
    ESP_RETURN_ON_ERROR(send_byte(0x80), TAG, "temp sensor");
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "waveform cmd");
    ESP_RETURN_ON_ERROR(send_byte(0xb1), TAG, "waveform");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "activate waveform");
    ESP_RETURN_ON_ERROR(wait_ready(), TAG, "waveform busy");
    return load_lut();
}

static void pixel(int x, int y, bool black)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }
    const size_t index = (size_t)y * (EPD_WIDTH / 8) + (size_t)(x >> 3);
    const uint8_t mask = (uint8_t)(0x80U >> (x & 7));
    if (black) {
        s_framebuffer[index] &= (uint8_t)~mask;
    } else {
        s_framebuffer[index] |= mask;
    }
}

static void text_with_font(int x, int y, const char *value, const bitmap_font_t *font)
{
    if (!value || !font) {
        return;
    }
    const size_t glyph_size = (size_t)font->stride * font->height;

    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor) {
        size_t glyph_index = 0;
        if (cursor[0] == 0xc2U && cursor[1] == 0xb0U) {
            glyph_index = SHARE_TECH_MONO_DEGREE_INDEX;
            cursor += 2;
        } else {
            unsigned char code = (unsigned char)toupper(*cursor++);
            if (code < SHARE_TECH_MONO_FIRST_CHAR || code > SHARE_TECH_MONO_LAST_CHAR) {
                code = '?';
            }
            glyph_index = (size_t)(code - SHARE_TECH_MONO_FIRST_CHAR);
        }
        const uint8_t *glyph = font->data + glyph_index * glyph_size;
        for (int cy = 0; cy < font->height; ++cy) {
            for (int cx = 0; cx < font->width; ++cx) {
                const uint8_t bits = glyph[(size_t)cy * font->stride + (cx >> 3)];
                if (bits & (0x80U >> (cx & 7))) {
                    pixel(x + cx, y + cy, true);
                }
            }
        }
        x += font->width;
    }
}

static size_t text_cell_count(const char *value)
{
    size_t count = 0;
    const unsigned char *cursor = (const unsigned char *)value;
    while (cursor && *cursor) {
        if (cursor[0] == 0xc2U && cursor[1] == 0xb0U) {
            cursor += 2;
        } else {
            ++cursor;
        }
        ++count;
    }
    return count;
}

static void text_bold(int x, int y, const char *value, int scale)
{
    if (scale < 1) {
        scale = 1;
    } else if (scale > 4) {
        scale = 4;
    }
    const bitmap_font_t *font = s_share_tech_mono_fonts[scale - 1];
    text_with_font(x, y, value, font);
    text_with_font(x + 1, y, value, font);
}

static void text_bold_right(int right, int y, const char *value, const bitmap_font_t *font)
{
    const int width = (int)text_cell_count(value) * font->width;
    const int x = right - width - 1;
    text_with_font(x, y, value, font);
    text_with_font(x + 1, y, value, font);
}

static void text_centered(int y, const char *value, int scale)
{
    if (scale < 1) {
        scale = 1;
    } else if (scale > 4) {
        scale = 4;
    }
    const bitmap_font_t *font = s_share_tech_mono_fonts[scale - 1];
    const int width = (int)text_cell_count(value) * font->width;
    text_with_font((EPD_WIDTH - width) / 2, y, value, font);
}

static void text_bold_centered(int y, const char *value, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    const bitmap_font_t *font = s_share_tech_mono_fonts[scale - 1];
    const int width = (int)text_cell_count(value) * font->width;
    const int x = (EPD_WIDTH - width) / 2;
    text_with_font(x, y, value, font);
    text_with_font(x + 1, y, value, font);
}

static void rssi_indicator(int x, int y, int8_t rssi, bool connected)
{
    if (!connected) {
        for (int px = x + 3; px < x + 12; ++px) {
            pixel(px, y + 7, true);
            pixel(px, y + 8, true);
        }
        return;
    }

    int level = 1;
    if (rssi >= -55) {
        level = 4;
    } else if (rssi >= -67) {
        level = 3;
    } else if (rssi >= -75) {
        level = 2;
    }

    const int bottom = y + 12;
    for (int bar = 0; bar < 4; ++bar) {
        const int left = x + bar * 4;
        const int height = 3 * (bar + 1);
        for (int dx = 0; dx < 3; ++dx) {
            const int top = bottom - height + 1 - dx;
            for (int py = top; py <= bottom; ++py) {
                const bool outline = dx == 0 || dx == 2 || py == top || py == bottom;
                if (bar < level || outline) {
                    pixel(left + dx, py, true);
                }
            }
        }
    }
}

static void line_segment(int x0, int y0, int x1, int y1)
{
    const int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx - dy;

    while (true) {
        pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled = error * 2;
        if (doubled > -dy) {
            error -= dy;
            x0 += sx;
        }
        if (doubled < dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void history_chart(const float *values, uint8_t count)
{
    const int left = 105;
    const int top = 143;
    const int right = 198;
    const int bottom = 196;
    line_segment(left, top, right, top);
    line_segment(right, top, right, bottom);
    line_segment(right, bottom, left, bottom);
    line_segment(left, bottom, left, top);
    if (!values || !count) {
        return;
    }

    float minimum = values[0];
    float maximum = values[0];
    for (uint8_t i = 1; i < count; ++i) {
        minimum = fminf(minimum, values[i]);
        maximum = fmaxf(maximum, values[i]);
    }
    if (maximum - minimum < 0.1f) {
        minimum -= 0.5f;
        maximum += 0.5f;
    }

    const int plot_left = left + 3;
    const int plot_right = right - 3;
    const int plot_top = top + 3;
    const int plot_bottom = bottom - 3;
    int previous_x = plot_left;
    int previous_y = (plot_top + plot_bottom) / 2;
    for (uint8_t i = 0; i < count; ++i) {
        const int x = count == 1 ? (plot_left + plot_right) / 2 :
            plot_left + (int)i * (plot_right - plot_left) / (count - 1U);
        const float normalized = (values[i] - minimum) / (maximum - minimum);
        const int y = plot_bottom - (int)lroundf(normalized * (plot_bottom - plot_top));
        if (i) {
            line_segment(previous_x, previous_y, x, y);
        }
        pixel(x, y, true);
        pixel(x + 1, y, true);
        previous_x = x;
        previous_y = y;
    }
}

static void horizontal_line(int y)
{
    for (int x = 0; x < EPD_WIDTH; ++x) {
        pixel(x, y, true);
        pixel(x, y + 1, true);
    }
}

static void render(const epaper_view_t *view)
{
    memset(s_framebuffer, 0xff, sizeof(s_framebuffer));
    char line[40];
    struct tm local = {0};
    const unsigned battery_pct = view->battery_pct > 100U ? 100U : view->battery_pct;
    if (view->time_valid) {
        localtime_r(&view->timestamp, &local);
        snprintf(line, sizeof(line), "%02d:%02d %02d/%02d/%02d",
                 local.tm_hour, local.tm_min, local.tm_mday, local.tm_mon + 1,
                 (local.tm_year + 1900) % 100);
    } else {
        snprintf(line, sizeof(line), "--:-- --/--/--");
    }
    rssi_indicator(1, 1, view->rssi, view->wifi_connected);
    text_with_font(18, 1, line, &s_share_tech_mono_status);
    text_with_font(19, 1, line, &s_share_tech_mono_status);
    snprintf(line, sizeof(line), "%u%%", battery_pct);
    text_bold_right(200, 1, line, &s_share_tech_mono_status);
    horizontal_line(20);

    if (view->page == 0) {
        snprintf(line, sizeof(line), "%.1f" "\xc2\xb0" "C", view->temperature_c);
        text_bold_centered(24, line, 3);
        snprintf(line, sizeof(line), "MIN %.1f MAX %.1f", view->day_min_c, view->day_max_c);
        text_centered(60, line, 2);
        snprintf(line, sizeof(line), "RH %.0f%%", view->humidity_pct);
        text_bold_centered(86, line, 3);
        snprintf(line, sizeof(line), "DEW %.1fC", view->dew_point_c);
        text_centered(116, line, 1);
        horizontal_line(137);
        text_bold(4, 142, "DELTA HOUR", 1);
        if (view->delta_valid) {
            snprintf(line, sizeof(line), "%+.1fC", view->delta_c);
        } else {
            snprintf(line, sizeof(line), "NO DATA");
        }
        text_centered(view->delta_valid ? 166 : 170, line, view->delta_valid ? 3 : 2);
    } else {
        text_bold(4, 25, "HISTORY DELTAS", 1);
        static const char *labels[] = {"DAY", "WEEK", "MONTH", "YEAR"};
        for (size_t i = 0; i < 4; ++i) {
            snprintf(line, sizeof(line), "%s", labels[i]);
            text_bold(8, 45 + (int)i * 20, line, 1);
            if (view->history_valid[i]) {
                snprintf(line, sizeof(line), "%+.1fC", view->history_delta_c[i]);
            } else {
                snprintf(line, sizeof(line), "NO DATA");
            }
            text_bold_right(96, 45 + (int)i * 20, line, s_share_tech_mono_fonts[1]);
        }
        horizontal_line(128);
        text_bold(4, 132, "BATTERY 30 DAYS", 1);
        history_chart(view->chart_values, view->chart_count);
    }
}

static esp_err_t prepare_display(void)
{
    ESP_RETURN_ON_ERROR(board_epaper_power(true), TAG, "display power");
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!s_spi) {
        const gpio_config_t output = {
            .pin_bit_mask = (1ULL << EPD_DC_GPIO) | (1ULL << EPD_RESET_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        const gpio_config_t input = {
            .pin_bit_mask = 1ULL << EPD_BUSY_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "display outputs");
        ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "display busy");

        const spi_bus_config_t bus = {
            .mosi_io_num = EPD_MOSI_GPIO,
            .miso_io_num = -1,
            .sclk_io_num = EPD_SCLK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = EPD_BUFFER_SIZE,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");
        const spi_device_interface_config_t device = {
            .clock_speed_hz = 20 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = EPD_CS_GPIO,
            .queue_size = 1,
        };
        ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_SPI_HOST, &device, &s_spi), TAG, "SPI device");
    }

    ESP_RETURN_ON_ERROR(controller_init(), TAG, "controller init");
    return clear_panel_to_white();
}

static esp_err_t present_framebuffer(void)
{
    ESP_RETURN_ON_ERROR(set_window_and_cursor(), TAG, "set cursor");
    ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "write RAM cmd");
    ESP_RETURN_ON_ERROR(send_data(s_framebuffer, sizeof(s_framebuffer)), TAG, "write RAM");
    return activate_full_refresh();
}

esp_err_t epaper_show(const epaper_view_t *view)
{
    if (!view) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
    render(view);
    return present_framebuffer();
}

static void provisioning_qr(esp_qrcode_handle_t qrcode)
{
    const int size = esp_qrcode_get_size(qrcode);
    const int quiet_modules = 4;
    const int available = 150;
    int scale = available / (size + quiet_modules * 2);
    if (scale > 5) scale = 5;
    if (scale < 2) scale = 2;
    const int total = (size + quiet_modules * 2) * scale;
    const int origin_x = (EPD_WIDTH - total) / 2 + quiet_modules * scale;
    const int origin_y = 18 + (available - total) / 2 + quiet_modules * scale;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!esp_qrcode_get_module(qrcode, x, y)) continue;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    pixel(origin_x + x * scale + dx, origin_y + y * scale + dy, true);
                }
            }
        }
    }
}

esp_err_t epaper_show_provisioning(const char *ap_ssid, const char *ap_password)
{
    if (!ap_ssid || !ap_password) {
        return ESP_ERR_INVALID_ARG;
    }
    char qr_payload[96];
    const int length = snprintf(qr_payload, sizeof(qr_payload),
                                "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_password);
    if (length < 0 || length >= (int)sizeof(qr_payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(prepare_display(), TAG, "display init");
    memset(s_framebuffer, 0xff, sizeof(s_framebuffer));
    text_centered(1, "WIFI SETUP", 1);
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func = provisioning_qr;
    config.max_qrcode_version = 6;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    ESP_RETURN_ON_ERROR(esp_qrcode_generate(&config, qr_payload), TAG, "QR generation");
    text_centered(171, ap_ssid, 1);
    char password_line[28];
    snprintf(password_line, sizeof(password_line), "PASS %s", ap_password);
    text_centered(186, password_line, 1);

    return present_framebuffer();
}

void epaper_shutdown(void)
{
    if (s_spi) {
        (void)send_command(0x10);
        (void)send_byte(0x01);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    (void)board_epaper_power(false);
}
