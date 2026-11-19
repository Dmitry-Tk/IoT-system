
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <mutex>

#include <simpleble/Config.h>
#include <simpleble/SimpleBLE.h>
#include <mosquitto.h>

// Конфигурация BLE (UUID в Big-Endian формате)
const std::string TARGET_DEVICE_NAME = "ESP32C6-GATT-Sensor";
const SimpleBLE::BluetoothUUID SERVICE_UUID        = "12345678-1234-5678-1234-567812345678";
const SimpleBLE::BluetoothUUID CHARACTERISTIC_UUID = "43218765-4321-8765-4321-876543218765";

// Конфигурация MQTT
const char* MQTT_HOST = "localhost";
const int   MQTT_PORT = 1883;
const char* MQTT_TOPIC = "esp32c6/adc_data";

// Callback-функция: вызывается при получении сообщения из MQTT
void on_mqtt_message(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* message) 
{
    if (message->payloadlen <= 0) return;

    // 1. Извлекаем указатель на подключенное BLE-устройство (передадим его в mosquitto_new)
    auto* peripheral = static_cast<SimpleBLE::Peripheral*>(userdata);

    // 2. Преобразуем полезную нагрузку (payload) в строку
    std::string command(static_cast<char*>(message->payload), message->payloadlen);
    std::cout << "[MQTT -> BLE] Топик: " << message->topic << " | Команда: " << command << std::endl;

    // 3. Отправляем команду по BLE в ESP32-C6 (если устройство подключено)
    if (peripheral && peripheral->is_connected()) 
    {
        try 
        {
            SimpleBLE::ByteArray bytes(command.begin(), command.end());
            // Укажите UUID характеристики, в которую ESP32 ожидает запись команд
            peripheral->write_request(SERVICE_UUID, CHARACTERISTIC_UUID, bytes);
            std::cout << "[BLE] Команда успешно передана на ESP32-C6" << std::endl;
        } 
        catch (const std::exception& e) 
        {
            std::cerr << "[BLE Write Error] " << e.what() << std::endl;
        }
    }
}

int main() 
{
    // BLE connections can take longer than SimpleBLE's 10-second Linux default.
    SimpleBLE::Config::SimpleBluez::connection_timeout = std::chrono::seconds(10);
    SimpleBLE::Peripheral target_peripheral; // Объявляем заранее, чтобы передать указатель

    // 1. Инициализация MQTT-клиента Mosquitto
    mosquitto_lib_init();
    // struct mosquitto* mosq = mosquitto_new("cpp_ble_gateway", true, nullptr);
    struct mosquitto* mosq = mosquitto_new("cpp_ble_gateway", true, &target_peripheral);
    if (!mosq) 
    {
        std::cerr << "Ошибка создания клиента Mosquitto" << std::endl;
        return 1;
    }
    // 2. Регистрируем обработчик входящих сообщений
    mosquitto_message_callback_set(mosq, on_mqtt_message);

    // 3. Подключаемся к брокеру
    if (mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS) 
    {
        std::cerr << "Ошибка подключения к MQTT брокеру" << std::endl;
        return 1;
    }

    // 4. Подписываемся на топик команд (QoS = 0)
    const char* COMMAND_TOPIC = "esp32c6/commands";
    int sub_rc = mosquitto_subscribe(mosq, nullptr, COMMAND_TOPIC, 0);
    if (sub_rc == MOSQ_ERR_SUCCESS) 
    {
        std::cout << "[MQTT] Подписка на топик '" << COMMAND_TOPIC << "' оформлена." << std::endl;
    } 
    else 
    {
        std::cerr << "[MQTT] Ошибка подписки: " << mosquitto_strerror(sub_rc) << std::endl;
    }

    // 5. Запускаем фоновый сетевой цикл (он сам будет слушать входящие пакеты и вызывать on_mqtt_message)
    mosquitto_loop_start(mosq);
    std::cout << "[MQTT] Успешное подключение к брокеру." << std::endl;

    // 2. Инициализация Bluetooth-адаптера через SimpleBLE
    auto adapter_list = SimpleBLE::Adapter::get_adapters();
    if (adapter_list.empty()) 
    {
        std::cerr << "[BLE] Bluetooth-адаптеры не найдены!" << std::endl;
        return 1;
    }

    auto adapter = adapter_list[0];
    std::cout << "[BLE] Используется адаптер: " << adapter.identifier() << std::endl;

    // 3. Сканирование устройств
    // SimpleBLE::Peripheral target_peripheral;
    SimpleBLE::Peripheral discovered_peripheral;
    std::mutex found_mutex;
    std::condition_variable found_condition;
    bool found = false;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral peripheral) {
        if (peripheral.identifier() == TARGET_DEVICE_NAME) {
            {
                std::lock_guard<std::mutex> lock(found_mutex);
                if (found) {
                    return;
                }
                discovered_peripheral = peripheral;
                found = true;
            }
            std::cout << "[BLE] Устройство найдено! MAC/Addr: " << peripheral.address() << std::endl;
            found_condition.notify_one();
        }
    });

    std::cout << "[BLE] Сканирование эфира для " << TARGET_DEVICE_NAME << "..." << std::endl;
    adapter.scan_start();

    {
        std::unique_lock<std::mutex> lock(found_mutex);
        found_condition.wait_for(lock, std::chrono::seconds(10), [&] { return found; });
    }
    adapter.scan_stop();

    bool device_found;
    {
        std::lock_guard<std::mutex> lock(found_mutex);
        device_found = found;
        if (device_found) 
        {
            target_peripheral = discovered_peripheral;
        }
    }

    if (!device_found) 
    {
        std::cerr << "[BLE] Ошибка: устройство не найдено." << std::endl;
        return 1;
    }

    // 4. Подключение и подписка на уведомления
    const char* ble_operation = "подключение";
    try 
    {
        std::cout << "[BLE] Подключение к " << target_peripheral.address() << "..." << std::endl;
        target_peripheral.connect();
        std::cout << "[BLE] Успешно подключено к " << TARGET_DEVICE_NAME << std::endl;

        // Регистрируем Callback на входящие Notifications
        ble_operation = "подписка на уведомления";
        target_peripheral.notify(SERVICE_UUID, CHARACTERISTIC_UUID, [&](SimpleBLE::ByteArray payload) {
            std::string data_str(payload.begin(), payload.end());
            std::cout << "[BLE -> MQTT] Получено: " << data_str << std::endl;

            // Публикуем сообщение в Mosquitto
            mosquitto_publish(mosq, nullptr, MQTT_TOPIC, data_str.length(), data_str.c_str(), 0, false);
        });

        std::cout << "[Gateway] Подписка активна. Ожидание данных (Ctrl+C для выхода)..." << std::endl;
        
        // Главный цикл удержания соединения
        while (target_peripheral.is_connected()) 
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } 
    catch (const std::exception& e) 
    {
        std::cerr << "[BLE Exception] Ошибка: " << ble_operation << " устройства "
                  << target_peripheral.address() << ": " << e.what() << std::endl;
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    // Очистка ресурсов
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}