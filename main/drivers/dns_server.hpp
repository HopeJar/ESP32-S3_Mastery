/**
 * @file dns_server.hpp
 * @brief Minimal local DNS responder for ESP32 AP mode
 */

#pragma once

#include <cstdint>

namespace Drivers {
namespace DNSServer {

struct Config {
    char site_name[64];
    uint8_t ip[4];
};

bool start(const Config& config);
void stop();
bool is_running();

} // namespace DNSServer
} // namespace Drivers
