/**
 * BLE <-> MQTT шлюз для ESP32-C6 с GATT-сервисом
 * 
 * Дополнительно:
 * 
 * 1. после обновления шлюза нужно перезапустить контейнер одной (без пересборки контейнера)
 *      docker restart ble_bridge_cpp
 * 2. полная пересборка контейнера:
 *      docker compose up -d --build --remove-orphans
 * 3. контроль состояния контейнера:
 *      docker ps -a
 * 4. просмотр логов контейнера:
 *      docker logs -f ble_bridge_cpp
 * 5. остановка контейнера:
 *      docker stop ble_bridge_cpp
 * 6. удаление контейнера:
 *      docker rm ble_bridge_cpp
 */
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#include <simpleble/SimpleBLE.h>
#include <mosquitto.h>
// для PrintCreationTimeFile()
#include <cstdlib>
#include <filesystem>

// Конфигурация BLE и MQTT
const std::string TARGET_DEVICE_NAME = "ESP32C6-GATT-Sensor";
// UUID в каноническом формате Big-Endian
const SimpleBLE::BluetoothUUID SERVICE_UUID        = "12345678-1234-5678-1234-567812345678";
const SimpleBLE::BluetoothUUID CHARACTERISTIC_UUID = "43218765-4321-8765-4321-876543218765";

const char* MQTT_HOST          = "localhost";
const int   MQTT_PORT          = 1883;
const char* MQTT_CLIENT_ID     = "ble_duplex_gateway";
const char* TOPIC_ADC_DATA     = "esp32c6/adc_data"; // Исходящие данные от ESP32
const char* TOPIC_ADC_VOLTAGE  = "esp32c6/adc_voltage"; // Исходящие голые данные о напряжении от ESP32
const char* TOPIC_COMMANDS     = "esp32c6/commands"; // Входящие команды для ESP32

// Флаг для корректного завершения работы (по Ctrl+C или остановке Docker-контейнера)
std::atomic<bool> g_running(true);

void signal_handler(int signum) 
{
    std::cout << "\n[System] Получен сигнал остановки (" << signum << "). Завершение работы..." << std::endl;
    g_running = false;
}

// Обработчик входящих сообщений из MQTT (MQTT -> BLE)
void on_mqtt_message(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* message) 
{
    if (!message || message->payloadlen <= 0) return;

    auto* peripheral = static_cast<SimpleBLE::Peripheral*>(userdata);
    std::string command(static_cast<char*>(message->payload), message->payloadlen);

    std::cout << "[MQTT -> BLE] Топик: '" << message->topic << "' | Команда: '" << command << "'" << std::endl;

    if (peripheral && peripheral->is_connected()) 
    {
        try 
        {
            SimpleBLE::ByteArray bytes(command.begin(), command.end());
            peripheral->write_request(SERVICE_UUID, CHARACTERISTIC_UUID, bytes);
            std::cout << "[BLE] Команда успешно записана в ESP32-C6." << std::endl;
        } 
        catch (const std::exception& e) 
        {
            std::cerr << "[BLE Write Error] Не удалось отправить команду: " << e.what() << std::endl;
        }
    } 
    else
    {
        std::cerr << "[BLE Warning] Устройство ESP32-C6 не подключено, команда проигнорирована." << std::endl;
    }
}

// Основной цикл шлюза
int main() 
{
    // контроль в терминале docker'а: когда был создан/модифицирован
    // исполняемый в контейнере файл ble_mqtt_bridge
    std::error_code ec;
    auto self_path = std::filesystem::read_symlink("/proc/self/exe", ec);
    std::string target = ec ? "/app/ble_mqtt_bridge" : self_path.string();
    // std::string command = "stat \"" + target + "\"";// полная информация о файле
    std::string command = "stat --format='Modify: %y\nBirth:  %w' \"" + target + "\"";
    // Сбрасываем буфер C++ перед вызовом внешней утилиты, чтобы вывод не перемешался в логах Docker
    std::cout << "Запуск: " << command << std::endl << std::endl;
    int result = std::system(command.c_str());
    if (result != 0) {
        std::cerr << "Ошибка выполнения stat, код: " << result << std::endl;
    }
    //
    // Регистрация обработчиков сигналов завершения (важно для Docker stop)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    // Отключаем буферизацию вывода, чтобы логи сразу отображались в docker logs
    std::cout.setf(std::ios::unitbuf);
    // объект для хранения найденного периферийного устройства
    SimpleBLE::Peripheral target_peripheral;
    // Инициализация MQTT
    mosquitto_lib_init();
    struct mosquitto* mosq = mosquitto_new(MQTT_CLIENT_ID, true, &target_peripheral);
    if (!mosq) 
    {
        std::cerr << "[MQTT Error] Не удалось создать клиент Mosquitto." << std::endl;
        return 1;
    }
    // Регистрируем обработчик входящих сообщений
    mosquitto_message_callback_set(mosq, on_mqtt_message);

    // Ожидаем готовности брокера при старте системы
    while (g_running && mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS) 
    {
        std::cerr << "[MQTT] Брокер недоступен, повторная попытка через 3 сек..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    if (!g_running) return 0;

    // Подписываемся на топик команд управления
    if (mosquitto_subscribe(mosq, nullptr, TOPIC_COMMANDS, 0) == MOSQ_ERR_SUCCESS) 
    {
        std::cout << "[MQTT] Подписка на топик команд '" << TOPIC_COMMANDS << "' оформлена." << std::endl;
    } 
    else 
    {
        std::cerr << "[MQTT Error] Ошибка подписки на '" << TOPIC_COMMANDS << "'." << std::endl;
    }

    mosquitto_loop_start(mosq);
    std::cout << "[MQTT] Подключено к брокеру " << MQTT_HOST << ":" << MQTT_PORT << std::endl;

    // Инициализация Bluetooth-адаптера через SimpleBLE
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) 
    {
        std::cerr << "[BLE Error] Bluetooth-адаптеры не найдены!" << std::endl;
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    auto adapter = adapters[0];
    std::cout << "[BLE] Используется адаптер: " << adapter.identifier() << " [" << adapter.address() << "]" << std::endl;

    // std::atomic<bool> device_found{false};
    bool device_found = false;
    std::mutex peripheral_mutex;
    // Бесконечный цикл поддержания связи с автоматическим переподключением при разрыве
    while (g_running) 
    {   // Callback для идентификации целевого устройства по имени среди найденных устройств при сканировании 
        adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral peripheral) {
                // Если уже нашли — мгновенно выходим без блокировок
                if (device_found) {
                    return;
                }
                if (peripheral.identifier() == TARGET_DEVICE_NAME) {
                std::lock_guard<std::mutex> lock(peripheral_mutex);
                target_peripheral = peripheral;
                device_found = true;
                std::cout << "[BLE] Устройство найдено: " << peripheral.identifier()
                        << " [" << peripheral.address() << "]"
                        << " | Сигнал (RSSI): " << peripheral.rssi() << " dBm" << std::endl;
            }
        });
        // запускаем сканирование BLE-устройств
        std::cout << "[BLE] Поиск '" << TARGET_DEVICE_NAME << "'..." << std::endl;
        adapter.scan_start();
        // Сканируем 10 секунд
        for (int i = 0; i < 100 && !device_found && g_running; ++i) 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Останавливаем сканирование строго в главном потоке
        // если здесь то BT соединение не устанавливается ?
        adapter.scan_stop();
        // Обязательная пауза, чтобы BlueZ успел завершить остановку сканирования на USB-адаптере
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        //
        if (!device_found) 
        {
            if (g_running) 
            {
                std::cout << "[BLE] Устройство не обнаружено. Повторный поиск через 3 сек..."
                        << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            continue;
        }
        // Попытка подключения целевого устройства
        try 
        {
            std::cout << "[BLE] Подключение к " << TARGET_DEVICE_NAME
                      << " (RSSI: " << target_peripheral.rssi() << " dBm)..." << std::endl;
            target_peripheral.connect();
            std::cout << "[BLE] Соединение установлено!" << std::endl;
            // Регистрируем callback для заданного уведомления от целевого устройства (BLE -> MQTT)
            target_peripheral.notify(SERVICE_UUID, CHARACTERISTIC_UUID, [&](SimpleBLE::ByteArray payload) {
                std::string adc_data(payload.begin(), payload.end());
                std::string adc_voltage;
                for (auto it = adc_data.begin(); it != adc_data.end(); ++it) {
                    if (*it != ' ')
                        adc_voltage.push_back(*it);
                    else
                        break;
                }
                std::cout << "[BLE -> MQTT] Получено: " << adc_data << std::endl
                          << "                     U: " << adc_voltage << std::endl;
                // Публикуем данные в MQTT
                mosquitto_publish(mosq, nullptr, TOPIC_ADC_DATA, adc_data.length(), adc_data.c_str(), 0, false);
                mosquitto_publish(mosq, nullptr, TOPIC_ADC_VOLTAGE, adc_voltage.length(), adc_voltage.c_str(), 0, false);
            });
            std::cout << "[Gateway] Двусторонний обмен активен." << std::endl;
            // Удерживаем цикл, пока соединение активно
            while (g_running && target_peripheral.is_connected()) 
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            // если пользователь прервал работу отправляем разрываем соединение
            if (target_peripheral.is_connected()) {
                target_peripheral.disconnect();
            }
            std::cout << "[BLE] Соединение разорвано." << std::endl;
        } 
        catch (const std::exception& e) 
        {
            std::cerr << "[BLE Exception] " << e.what() << std::endl;
        }
        // сбрасываем флаг, чтобы при переподключении снова искать устройство            
        // device_found.store(false);
        device_found = false;
        if (g_running) 
        {
            std::cout << "[BLE] Переподключение через 3 сек..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    // Очистка ресурсов
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    std::cout << "[System] Шлюз остановлен." << std::endl;

    return 0;
}
