#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
// Периферия
#include "esp_adc/adc_oneshot.h"
#include "led_strip.h"

static const char *TAG = "BLE_GATT_PROJECT";

#define ADC_CHANNEL ADC_CHANNEL_1
#define RGB_LED_PIN GPIO_NUM_8

static led_strip_handle_t led_strip;
static adc_oneshot_unit_handle_t adc1_handle;
static uint8_t ble_addr_type;
static uint16_t adc_val_attr_handle; // Хэндл нашей характеристики

// Кастомные UUID (Сгенерированы для нашего датчика)
// Сервис: 12345678-1234-5678-1234-567812345678
static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

// Характеристика: 43218765-4321-8765-4321-876543218765
static const ble_uuid128_t gatt_svr_chr_uuid =
    BLE_UUID128_INIT(0x65, 0x87, 0x21, 0x43, 0x65, 0x87, 0x21, 0x43, 0x65, 0x87, 0x21, 0x43, 0x65, 0x87, 0x21, 0x43);

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_chr_def gatt_svr_chrs[] = {
    {
        .uuid = &gatt_svr_chr_uuid.u,
        .access_cb = gatt_svr_chr_access,
        .arg = nullptr,
        .descriptors = nullptr,
        // Добавили BLE_GATT_CHR_F_WRITE к существующим флагам чтения и уведомлений
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE,
        .min_key_size = 0,
        .val_handle = &adc_val_attr_handle,
        .cpfd = nullptr,
    },
    {}, // Терминатор массива характеристик (обязателен)
};

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .includes = nullptr,
        .characteristics = gatt_svr_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_END,
        .uuid = nullptr,
        .includes = nullptr,
        .characteristics = nullptr,
    },
};

char adc_str_value[32] = "0";

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle, 
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
// {
//     if (attr_handle == adc_val_attr_handle) 
//     {
//         int rc = os_mbuf_append(ctxt->om, adc_str_value, strlen(adc_str_value));
//         return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
//     }
//     return BLE_ATT_ERR_ATTR_NOT_FOUND;
// }
{
    if (attr_handle != adc_val_attr_handle) 
    {
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    switch (ctxt->op) 
    {
        // 1. Клиент (ПК) читает текущее значение АЦП
        case BLE_GATT_ACCESS_OP_READ_CHR: 
        {
            int rc = os_mbuf_append(ctxt->om, adc_str_value, strlen(adc_str_value));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        // 2. Клиент (ПК) присылает команду из MQTT через write_request()
        case BLE_GATT_ACCESS_OP_WRITE_CHR: 
        {
            char rx_buf[64] = {0}; // Буфер под входящую команду
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);

            // Защита от переполнения буфера
            if (om_len >= sizeof(rx_buf)) 
            {
                ESP_LOGW(TAG, "Слишком длинная команда (%d байт)", om_len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            // Копируем байты из сетевого пакета NimBLE в обычную C-строку
            uint16_t copied_len = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buf, sizeof(rx_buf) - 1, &copied_len);
            if (rc != 0) 
            {
                return BLE_ATT_ERR_UNLIKELY;
            }
            rx_buf[copied_len] = '\0'; // Гарантируем завершающий ноль строки

            ESP_LOGI(TAG, "[BLE <- MQTT] Получена команда: '%s'", rx_buf);

            // --- ЗДЕСЬ ВАША ЛОГИКА ОБРАБОТКИ КОМАНД ---
            if (strcmp(rx_buf, "LED_RED") == 0) 
            {
                led_strip_set_pixel(led_strip, 0, 50, 0, 0);
                led_strip_refresh(led_strip);
            } 
            else if (strcmp(rx_buf, "LED_OFF") == 0) 
            {
                led_strip_clear(led_strip);
                led_strip_refresh(led_strip);
            }
            // ------------------------------------------

            return 0; // Подтверждаем успешную запись (ATT Write Response)
        }

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

// Переменная для хранения ID текущего подключения (0xFFFF = не подключен)
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
void start_ble_advertising(void);

// Callback-функция для обработки событий BLE GAP
static int gap_event_cb(struct ble_gap_event *event, void *arg) 
{
    switch (event->type) 
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) 
            {
                ESP_LOGI(TAG, "Клиент подключен! conn_handle = %d", event->connect.conn_handle);
                g_conn_handle = event->connect.conn_handle;
            } 
            else 
            {
                // Ошибка подключения, перезапускаем advertising
                start_ble_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Клиент отключен, перезапуск Advertising...");
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_ble_advertising();
            break;
        default:
            break;
    }
    return 0;
}

void start_ble_advertising(void) 
{
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    
    const char *name = "ESP32C6-GATT-Sensor";
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    // Добавлено: UUID сервиса в рекламный пакет
    fields.uuids128 = (ble_uuid128_t *)&gatt_svr_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    // ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    // Передаем gap_event_cb вместо NULL
    ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
}

void on_ble_sync(void) 
{
    ble_hs_id_infer_auto(0, &ble_addr_type);
    start_ble_advertising();
}

void nimble_host_task(void *param) 
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Поток чтения АЦП и отправки уведомлений подключенному ПК
void adc_poll_task(void *pvParameters) 
{
    while (1) 
    {
        int raw_adc_value = 0;
        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL, &raw_adc_value) == ESP_OK) 
        {
            float voltage = (raw_adc_value * 3.3f) / 4095.0f;
            snprintf(adc_str_value, sizeof(adc_str_value), "%.2f V (%d)", voltage, raw_adc_value);
            // ESP_LOGI(TAG, "Измерение: %s | conn_handle = %d", adc_str_value, g_conn_handle);
            // Отправляем данные ТОЛЬКО если есть активное подключение
            if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) 
            {
                uint16_t len = strlen(adc_str_value);
                // Создаем корректный mbuf из обычного C-массива байт
                struct os_mbuf *om = ble_hs_mbuf_from_flat(adc_str_value, len);
                if (om != NULL) 
                {
                    // Отправляем наполненный пакет
                    int rc = ble_gatts_notify_custom(g_conn_handle, adc_val_attr_handle, om);
                    if (rc != 0) 
                    {
                        ESP_LOGE(TAG, "Ошибка отправки Notification: %d", rc);
                    }
                    else 
                    {
                        ESP_LOGI(TAG, "Отправлены данные АЦП: %s", adc_str_value);
                    }                    
                }
                else 
                {
                    ESP_LOGE(TAG, "Не удалось выделить mbuf под данные");
                }
            }
            // Мигание светодиодом при измерении
            led_strip_set_pixel(led_strip, 0, 0, 0, 30);
            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(100));
            led_strip_clear(led_strip);
            led_strip_refresh(led_strip);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

extern "C" void app_main(void) 
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = RGB_LED_PIN;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);

    adc_oneshot_unit_init_cfg_t init_config1 = {};
    init_config1.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t config = {};
    config.atten = ADC_ATTEN_DB_12;
    config.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_device_name_set("ESP32C6-GATT-Sensor");
    
    // Регистрируем структуру GATT сервисов в стеке
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    ble_hs_cfg.sync_cb = on_ble_sync;

    nimble_port_freertos_init(nimble_host_task);
    xTaskCreate(adc_poll_task, "adc_poll_task", 4096, NULL, 5, NULL);
}
