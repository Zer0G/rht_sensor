#include "device_console.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"

#define CONSOLE_LINE_SIZE 384
#define CONSOLE_MAX_ARGS  8

static const char *TAG = "console";

static void console_write(const char *format, ...)
{
    char output[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(output, sizeof(output), format, arguments);
    va_end(arguments);
    if (length <= 0) return;
    const size_t count = (size_t)length < sizeof(output) ?
                         (size_t)length : sizeof(output) - 1U;
    (void)usb_serial_jtag_write_bytes(output, count, pdMS_TO_TICKS(100));
}

static void print_help(void)
{
    console_write(
        "\r\nComandi disponibili:\r\n"
        "  help\r\n"
        "  show\r\n"
        "  wifi set <ssid> [password]\r\n"
        "  wifi clear\r\n"
        "  mqtt set <mqtt://host:porta> [utente] [password]\r\n"
        "  mqtt clear\r\n"
        "  reboot\r\n"
        "Usa le virgolette per valori con spazi e \\ per l'escape.\r\n"
        "L'input non viene ripetuto, cosi' le password restano nascoste.\r\n");
}

static int split_arguments(char *line, char **arguments, int capacity)
{
    int count = 0;
    char *read = line;
    while (*read) {
        while (*read == ' ' || *read == '\t') ++read;
        if (!*read) break;
        if (count == capacity) return -1;

        const char quote = (*read == '\'' || *read == '"') ? *read++ : '\0';
        char *write = read;
        arguments[count++] = write;
        while (*read && (quote ? *read != quote : (*read != ' ' && *read != '\t'))) {
            if (*read == '\\') {
                ++read;
                if (!*read) return -1;
            }
            *write++ = *read++;
        }
        if (quote) {
            if (*read != quote) return -1;
            ++read;
            if (*read && *read != ' ' && *read != '\t') return -1;
        } else if (*read) {
            ++read;
        }
        *write = '\0';
        while (*read == ' ' || *read == '\t') ++read;
    }
    return count;
}

static void print_configuration(void)
{
    char ssid[33];
    char wifi_password[65];
    char uri[NETWORK_MQTT_URI_SIZE];
    char username[NETWORK_MQTT_USERNAME_SIZE];
    char mqtt_password[NETWORK_MQTT_PASSWORD_SIZE];
    const bool wifi = network_get_wifi_credentials(ssid, sizeof(ssid),
                                                    wifi_password, sizeof(wifi_password));
    const bool mqtt = network_get_mqtt_config(uri, sizeof(uri), username, sizeof(username),
                                              mqtt_password, sizeof(mqtt_password));
    console_write("\r\nWi-Fi: %s\r\n  SSID: %s\r\n  password: %s\r\n"
                  "MQTT: %s\r\n  URI: %s\r\n  username: %s\r\n  password: %s\r\n",
                  wifi ? "configurato" : "non configurato", wifi ? ssid : "-",
                  wifi && wifi_password[0] ? "********" : "(vuota)",
                  mqtt ? "configurato" : "non configurato", mqtt ? uri : "-",
                  mqtt && username[0] ? username : "(vuoto)",
                  mqtt && mqtt_password[0] ? "********" : "(vuota)");
}

static void report_result(esp_err_t result)
{
    if (result == ESP_OK) {
        console_write("OK - configurazione salvata; sara' usata dal prossimo ciclo.\r\n");
    } else {
        console_write("ERRORE: %s\r\n", esp_err_to_name(result));
    }
}

static void execute_line(char *line)
{
    char *arguments[CONSOLE_MAX_ARGS];
    const int count = split_arguments(line, arguments, CONSOLE_MAX_ARGS);
    if (count < 0) {
        console_write("ERRORE: sintassi o virgolette non valide.\r\n");
    } else if (count == 0) {
        return;
    } else if ((!strcmp(arguments[0], "help") || !strcmp(arguments[0], "?")) &&
               count == 1) {
        print_help();
    } else if (!strcmp(arguments[0], "show") && count == 1) {
        print_configuration();
    } else if (!strcmp(arguments[0], "wifi") && count == 2 &&
               !strcmp(arguments[1], "clear")) {
        report_result(network_clear_wifi_credentials());
    } else if (!strcmp(arguments[0], "wifi") && count >= 3 && count <= 4 &&
               !strcmp(arguments[1], "set")) {
        report_result(network_save_wifi_credentials(arguments[2],
                                                     count == 4 ? arguments[3] : ""));
    } else if (!strcmp(arguments[0], "mqtt") && count == 2 &&
               !strcmp(arguments[1], "clear")) {
        report_result(network_clear_mqtt_config());
    } else if (!strcmp(arguments[0], "mqtt") && count >= 3 && count <= 5 &&
               !strcmp(arguments[1], "set")) {
        report_result(network_save_mqtt_config(arguments[2],
                                                count >= 4 ? arguments[3] : "",
                                                count == 5 ? arguments[4] : ""));
    } else if (!strcmp(arguments[0], "reboot") && count == 1) {
        console_write("Riavvio...\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        console_write("ERRORE: comando non riconosciuto. Digita 'help'.\r\n");
    }
}

static void console_task(void *argument)
{
    (void)argument;
    char line[CONSOLE_LINE_SIZE];
    size_t used = 0;
    bool was_connected = false;
    bool previous_was_cr = false;
    while (true) {
        const bool connected = usb_serial_jtag_is_connected();
        if (connected && !was_connected) {
            console_write("\r\nRHT console pronta. Digita 'help'.\r\nrht> ");
        }
        was_connected = connected;

        uint8_t input[32];
        const int received = usb_serial_jtag_read_bytes(input, sizeof(input),
                                                        pdMS_TO_TICKS(100));
        for (int i = 0; i < received; ++i) {
            const char value = (char)input[i];
            if (value == '\n' && previous_was_cr) {
                previous_was_cr = false;
                continue;
            }
            previous_was_cr = value == '\r';
            if (value == '\r' || value == '\n') {
                console_write("\r\n");
                if (used) {
                    line[used] = '\0';
                    execute_line(line);
                    used = 0;
                }
                console_write("rht> ");
            } else if (value == '\b' || value == 0x7f) {
                if (used) --used;
            } else if ((unsigned char)value >= 0x20 && used + 1U < sizeof(line)) {
                line[used++] = value;
            } else if ((unsigned char)value >= 0x20) {
                used = 0;
                console_write("\r\nERRORE: riga troppo lunga.\r\nrht> ");
            }
        }
    }
}

esp_err_t device_console_start(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 512,
    };
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(console_task, "usb_console", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB command console started");
    return ESP_OK;
}
