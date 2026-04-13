/**
 * @file wifi_driver.hpp
 * @brief WiFi driver interface
 */

#pragma once

#include <cstdint>

namespace Drivers {
namespace WiFi {

struct Config {
    const char* ssid;
    const char* password;
    bool connect_sta;
    bool use_static_ip;
    uint8_t static_ip[4];
    uint8_t static_gateway[4];
    uint8_t static_netmask[4];
    bool use_custom_dns;
    uint8_t dns_server[4];
    const char* hostname;
    bool start_ap;
    const char* ap_ssid;
    const char* ap_password;
    uint8_t ap_channel;
    uint8_t ap_max_connections;
};

/**
 * @brief Initialize the WiFi driver
 * @return true if initialization successful, false otherwise
 */
bool initialize(const Config& config);

/**
 * @brief Process WiFi driver events (call periodically)
 */
void process();

/**
 * @brief Check if WiFi is connected
 * @return true if connected, false otherwise
 */
bool is_connected();

/**
 * @brief Check if the setup access point is running
 * @return true if AP mode is active
 */
bool is_ap_started();

/**
 * @brief Cleanup and deinitialize the WiFi driver
 */
void deinitialize();

} // namespace WiFi
} // namespace Drivers
