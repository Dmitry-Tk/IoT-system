#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_adc/adc_oneshot.h"
#include "led_strip.h" 
// #define HOME

static const char *TAG = "MQTT_ADC_PROJECT";
// --- НАСТРОЙКИ Wi-Fi ---
#if !defined(HOME)
    #define WIFI_SSID "ASUP"
    #define WIFI_PASS "asuna2020"
    #define MQTT_BROKER_URL "mqtt://192.168.1.125:1883" // IP рабочего ПК С MQTT-брокером
#else
    #define WIFI_SSID "CXNK00128FA3"
    #define WIFI_PASS "6b2a861a65767fd8"
    #define MQTT_BROKER_URL "mqtt://192.168.100.34:1883" // IP домашнего ПК С MQTT-брокером
#endif
// --- НАСТРОЙКИ MQTT для ESP32-C6 ---
#define MQTT_TOPIC_STATE     "esp32c6/state"        // топик для выдачи состояния устройства
#define MQTT_TOPIC_TELEMETRY "esp32c6/adc_data"     // топик для выдачи данных с сенсоров ESP32-C6
#define MQTT_TOPIC_VOLTAGE   "esp32c6/adc_voltage"  // топик для выдачи напряжения (голые данные) 
#define MQTT_TOPIC_COMMAND   "esp32c6/commands"     // топик приема команд для ESP32-C6
// --- НАСТРОЙКИ ПЕРИФЕРИИ ---
#define ADC_CHANNEL ADC_CHANNEL_1 // GPIO 1 (Вход АЦП)
#define RGB_LED_PIN GPIO_NUM_8   // Светодиод WS2812B на плате WeAct ESP32-C6

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static led_strip_handle_t led_strip = nullptr;
static uint32_t rgb[3] = {0, 0, 0}; // RGB значения для светодиода (0-255)

// инициализация СД WS2812B
void init_rgb_led(void) 
{
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

// Обработчик событий Wi-Fi и IP
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
    {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        ESP_LOGI(TAG, "Потеряно соединение c Wi-Fi. Повторное подключение...");
        esp_wifi_connect();
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi подключен! Получен IP:" IPSTR, IP2STR(&event->ip_info.ip));
        if (mqtt_client) 
        {
            esp_mqtt_client_start(mqtt_client);
        }
    }
}

// Обработчик событий MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data) 
{
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
        {
            ESP_LOGI(TAG, "MQTT подключен успешно!");
            mqtt_connected = true;
            // публикуем статус устройства
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATE, "Wi-Fi:online", 0, 1, 1);            
            // подписываемся на топик команд
            int msg_id = esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_COMMAND, 1);
            ESP_LOGI(TAG, "Подписка на топик команд отправлена, msg_id=%d", msg_id);
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT отключен или соединение потеряно!");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA: // СОБЫТИЕ: ПРИШЛИ ДАННЫЕ ИЗ ПОДПИСКИ
            ESP_LOGI(TAG, "Получены данные из топика: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Содержимое команды: %.*s", event->data_len, event->data);
            // Проверяем, что сообщение пришло именно в наш топик команд
            if (strncmp(event->topic, MQTT_TOPIC_COMMAND, event->topic_len) == 0) 
            {    
                 // Выполнение команд управления светодиодом
                if (strncmp(event->data, "LED_RED on", event->data_len) == 0) {
                    rgb[0] = 30; // Красный
                } else if (strncmp(event->data, "LED_RED off", event->data_len) == 0) {
                    rgb[0] = 0; // Красный
                } else if (strncmp(event->data, "LED_GREEN on", event->data_len) == 0) {
                    rgb[1] = 30; // Зеленый
                } else if (strncmp(event->data, "LED_GREEN off", event->data_len) == 0) {
                    rgb[1] = 0; // Зеленый
                } else if (strncmp(event->data, "LED_BLUE on", event->data_len) == 0) {
                    rgb[2] = 30; // Синий
                } else if (strncmp(event->data, "LED_BLUE off", event->data_len) == 0) {
                    rgb[2] = 0; // Синий
                } else if (strncmp(event->data, "LED off", event->data_len) == 0) {
                    rgb[0] = 0;
                    rgb[1] = 0;
                    rgb[2] = 0;
                }
                else {
                    ESP_LOGW(TAG, "Неизвестная команда для светодиода: %.*s", event->data_len, event->data);
                }
                led_strip_set_pixel(led_strip, 0, rgb[0], rgb[1], rgb[2]);
                led_strip_refresh(led_strip);
            }
            break;
        default:
            break;
    }
}

// Задача чтения АЦП и отправки в сеть
void adc_mqtt_task(void *pvParameters) 
{
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {};
    init_config1.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {};
    config.bitwidth = ADC_BITWIDTH_DEFAULT;
    config.atten = ADC_ATTEN_DB_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config));

    char json_buffer[128];
    char adc_voltage_buffer[8];
    int uptime = 0;
    const float dV = 3.3f / 4095.0f;
    while (1) 
    {
        if (mqtt_connected) 
        {
            int raw_adc_value = 0;
            if (adc_oneshot_read(adc1_handle, ADC_CHANNEL, &raw_adc_value) == ESP_OK) 
            {
                float voltage = raw_adc_value * dV;
                snprintf(json_buffer, sizeof(json_buffer), 
                         "{\"raw_adc\":%d,\"voltage\":%.2f,\"uptime_sec\":%d}", 
                         raw_adc_value, voltage, uptime);
                snprintf(adc_voltage_buffer, sizeof(adc_voltage_buffer), 
                         "%.3f", voltage);
                ESP_LOGI(TAG, "Отправка: %s", json_buffer);
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_TELEMETRY, json_buffer, 0, 1, 0);
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_VOLTAGE, adc_voltage_buffer, 0, 1, 0);
            }
        }
        uptime += 3;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

extern "C" void app_main(void) 
{
    // 1. Инициализация памяти NVS (нужна для хранения настроек Wi-Fi стека)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 2. Инициализация сети
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3. Регистрация событий Wi-Fi
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                            &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                            &wifi_event_handler, NULL, NULL));

    // 4. Настройка параметров Wi-Fi подключения
    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASS, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 5. Настройка MQTT
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URL;
    mqtt_cfg.session.last_will.topic = MQTT_TOPIC_TELEMETRY;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);

    // 6. Инициализируем СД
    init_rgb_led();

    // 7. Запуск независимой задачи для АЦП
    xTaskCreate(adc_mqtt_task, "adc_mqtt_task", 4096, NULL, 5, NULL);
}
