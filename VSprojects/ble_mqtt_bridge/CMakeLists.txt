cmake_minimum_required(VERSION 3.21)
project(ble_mqtt_bridge CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(simpleble CONFIG QUIET)

if(NOT TARGET simpleble::simpleble)
    include(FetchContent)
    FetchContent_Declare(
        simpleble
        GIT_REPOSITORY https://github.com/OpenBluetoothToolbox/SimpleBLE.git
        GIT_TAG v0.7.3
    )
    FetchContent_MakeAvailable(simpleble)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

add_executable(ble_mqtt_bridge ble_mqtt_bridge.cpp)

target_link_libraries(ble_mqtt_bridge PRIVATE
    simpleble::simpleble
    PkgConfig::MOSQUITTO
)