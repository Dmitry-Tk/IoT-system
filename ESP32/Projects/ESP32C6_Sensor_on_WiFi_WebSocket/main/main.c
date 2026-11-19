#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "led_strip.h"

static const char *TAG = "ESP32C6";

// Настройки Wi-Fi SoftAP
#define WIFI_SSID      "ESP32C6_LocalNet"
#define WIFI_PASS      "27111974"
#define WIFI_CHANNEL   6
#define MAX_STA_CONN   2

// Настройки АЦП (ESP32-C6: ADC1_CHANNEL_0 соответствует GPIO0)
#define ADC_UNIT       ADC_UNIT_1
#define ADC_CHANNEL    ADC_CHANNEL_0
#define ADC_ATTEN      ADC_ATTEN_DB_12
// Настройки светодиода WS2812B
#define RGB_LED_PIN GPIO_NUM_8   // Светодиод WS2812B на плате WeAct ESP32-C6
static led_strip_handle_t led_strip = nullptr;

// Указатели на встроенный файл index.html во Flash-памяти
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define MAX_WS_CLIENTS 2
static int client_fds[MAX_WS_CLIENTS] = {-1, -1};
static httpd_handle_t server = NULL;

// Дескрипторы АЦП и калибровки
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;
static bool do_calibration = false;


// инициализация СД WS2812B
void init_rgb_led(void) {
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = RGB_LED_PIN;
    strip_config.max_leds = 1; // На плате установлен ровно 1 светодиод
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    // Создаем объект светодиодной ленты/диода через встроенную периферию RMT
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    // Очищаем (гасим) светодиод при старте
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "Адресный светодиод WS2812B (GPIO %d) успешно инициализирован.", RGB_LED_PIN);
}

void set_rgb_led(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip) {
        led_strip_set_pixel(led_strip, 0, r, g, b);
        led_strip_refresh(led_strip);
    }
}

// инициализация АЦП и калибровка
static void init_adc(void) {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT, // 12 бит (0..4095)
        .atten = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config));

    // Настройка калибровки (Curve Fitting для ESP32-C6)
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT,
        .chan = ADC_CHANNEL,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle) == ESP_OK) {
        do_calibration = true;
        ESP_LOGI(TAG, "Калибровка АЦП успешно инициализирована");
    } else {
        ESP_LOGW(TAG, "Калибровка АЦП недоступна, будут доступны только RAW значения");
    }
}

// управление клиентами WebSocket

static void add_ws_client(int fd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (client_fds[i] == -1) {
            client_fds[i] = fd;
            ESP_LOGI(TAG, "WS клиент добавлен (fd=%d)", fd);
            return;
        }
    }
}

static void remove_ws_client(int fd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (client_fds[i] == fd) {
            client_fds[i] = -1;
            ESP_LOGI(TAG, "WS клиент отключен (fd=%d)", fd);
            return;
        }
    }
}

static void on_client_disconnect(httpd_handle_t hd, int sockfd) {
    remove_ws_client(sockfd);
}

// --- Обработчики HTTP и WebSocket ---

// Пустой ответ для /favicon.ico, чтобы мобильный браузер не плодил лишние запросы
static esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Отдача главной HTML-страницы при заходе на http://192.168.4.1/
static esp_err_t index_get_handler(httpd_req_t *req) {
    size_t index_html_size = (index_html_end - index_html_start);
    if (index_html_size > 0 && index_html_start[index_html_size - 1] == '\0') {
        index_html_size--;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // Сразу закрываем HTTP-сокет после отдачи HTML, освобождая место для WebSocket
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

// Обработчик WebSocket (/ws)
static esp_err_t ws_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS рукопожатие (GET), fd=%d", fd);
        // Перезаписываем слот 0 при каждом новом подключении одной вкладки
        client_fds[0] = fd;
        return ESP_OK;
    }

    // При получении любого сообщения от клиента гарантированно сохраняем его живой fd
    client_fds[0] = fd;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;

        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK && ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            char ack_msg[64];

            if (strcmp((char *)ws_pkt.payload, "CMD:LED_ON") == 0) {
                set_rgb_led(0, 40, 35);
                snprintf(ack_msg, sizeof(ack_msg), "{\"ack\": \"WS2812B ВКЛ\"}");
            } else if (strcmp((char *)ws_pkt.payload, "CMD:LED_OFF") == 0) {
                set_rgb_led(0, 0, 0);
                snprintf(ack_msg, sizeof(ack_msg), "{\"ack\": \"WS2812B ВЫКЛ\"}");
            } else {
                snprintf(ack_msg, sizeof(ack_msg), "{\"ack\": \"OK\"}");
            }

            httpd_ws_frame_t resp_pkt = {
                .payload = (uint8_t *)ack_msg,
                .len = strlen(ack_msg),
                .type = HTTPD_WS_TYPE_TEXT
            };
            httpd_ws_send_frame(req, &resp_pkt);
        }
        free(buf);
    }
    return ESP_OK;
}
// Задача опроса АЦП и рассылки с частотой 1.0 Гц 
static void adc_telemetry_task(void *pvParameters) {
    char payload[128];
    int raw_val = 0;
    int voltage_mv = 0;

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000);

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL, &raw_val) == ESP_OK) {
            if (do_calibration) {
                adc_cali_raw_to_voltage(adc1_cali_handle, raw_val, &voltage_mv);
            } else {
                voltage_mv = -1;
            }
        }

        ESP_LOGI(TAG, "adc raw: %d, adc[V]: %.3f | target fd=%d", raw_val, voltage_mv / 1000.0, client_fds[0]);

        if (server == NULL || client_fds[0] == -1) continue;

        snprintf(payload, sizeof(payload), "{\"adc_raw\": %d, \"adc[V]\": %.3f}", raw_val, voltage_mv / 1000.0);

        httpd_ws_frame_t ws_pkt = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)payload,
            .len = strlen(payload)
        };

        // Пробуем синхронную отправку по живому дескриптору
        esp_err_t ret = httpd_ws_send_data(server, client_fds[0], &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Сбой отправки WS (fd=%d): %s", client_fds[0], esp_err_to_name(ret));
        }
    }
}
// Запуск веб-сервера и Wi-Fi
static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 10;      // Оставляем запас от 16 сокетов lwIP
    config.lru_purge_enable = true;    // Принудительно закрывать самые старые сокеты
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.close_fn = on_client_disconnect;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t favicon_uri = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &favicon_uri);

        httpd_uri_t ws_uri = {
            .uri          = "/ws",
            .method       = HTTP_GET,
            .handler      = ws_handler,
            .user_ctx     = NULL,
            .is_websocket = true,
            .handle_ws_control_frames = true
        };
        httpd_register_uri_handler(server, &ws_uri);

        ESP_LOGI(TAG, "Сервер запущен: http://192.168.4.1");
    }
}
// Инициализация Wi-Fi в режиме точки доступа (SoftAP)
static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    // 1. Инициализация памяти NVS (нужна для хранения настроек Wi-Fi стека)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_adc();
    init_rgb_led();
    wifi_init_softap();
    start_webserver();

    xTaskCreate(adc_telemetry_task, "adc_telemetry_task", 4096, NULL, 5, NULL);
}