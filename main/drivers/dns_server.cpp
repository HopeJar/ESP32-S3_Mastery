/**
 * @file dns_server.cpp
 * @brief Minimal local DNS responder for ESP32 AP mode
 */

#include "drivers/dns_server.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <cstring>

namespace Drivers {
namespace DNSServer {

namespace {
constexpr const char* TAG = "DNS_Server";
constexpr uint16_t kDnsPort = 53;
constexpr size_t kMaxDnsPacket = 512;

TaskHandle_t task_handle = nullptr;
bool running = false;
Config active_config = {};

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void write_be16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[1] = static_cast<uint8_t>(value & 0xff);
}

void write_be32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>((value >> 24) & 0xff);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xff);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[3] = static_cast<uint8_t>(value & 0xff);
}

size_t find_query_end(const uint8_t* packet, size_t packet_len) {
    size_t offset = 12;
    while (offset < packet_len && packet[offset] != 0) {
        offset += static_cast<size_t>(packet[offset]) + 1;
    }

    if (offset + 5 > packet_len) {
        return 0;
    }

    return offset + 5;
}

bool dns_name_equals(const uint8_t* packet, size_t packet_len, const char* expected_name) {
    if (expected_name == nullptr || expected_name[0] == '\0') {
        return false;
    }

    size_t packet_offset = 12;
    size_t name_offset = 0;
    while (packet_offset < packet_len && packet[packet_offset] != 0) {
        const uint8_t label_len = packet[packet_offset++];
        if (label_len == 0 || label_len > 63) {
            return false;
        }

        if (name_offset != 0) {
            if (expected_name[name_offset++] != '.') {
                return false;
            }
        }

        for (uint8_t i = 0; i < label_len; ++i) {
            if (expected_name[name_offset] == '\0') {
                return false;
            }

            char left = static_cast<char>(packet[packet_offset++]);
            char right = expected_name[name_offset++];
            if (left >= 'A' && left <= 'Z') {
                left = static_cast<char>(left + ('a' - 'A'));
            }
            if (right >= 'A' && right <= 'Z') {
                right = static_cast<char>(right + ('a' - 'A'));
            }
            if (left != right) {
                return false;
            }
        }
    }

    return expected_name[name_offset] == '\0';
}

size_t build_response(const uint8_t* request, size_t request_len, uint8_t* response, size_t response_len) {
    if (request_len < 17 || response_len < request_len + 16) {
        return 0;
    }

    const uint16_t question_count = read_be16(request + 4);
    if (question_count == 0) {
        return 0;
    }

    const size_t query_end = find_query_end(request, request_len);
    if (query_end == 0 || query_end > response_len) {
        return 0;
    }

    const size_t qtype_offset = query_end - 4;
    const uint16_t qtype = read_be16(request + qtype_offset);
    const bool should_answer = qtype == 1 && dns_name_equals(request, request_len, active_config.site_name);

    std::memcpy(response, request, query_end);
    response[2] = 0x81;
    response[3] = 0x80;
    write_be16(response + 6, should_answer ? 1 : 0);
    write_be16(response + 8, 0);
    write_be16(response + 10, 0);

    if (!should_answer) {
        return query_end;
    }

    size_t offset = query_end;
    response[offset++] = 0xc0;
    response[offset++] = 0x0c;
    write_be16(response + offset, 1);
    offset += 2;
    write_be16(response + offset, 1);
    offset += 2;
    write_be32(response + offset, 60);
    offset += 4;
    write_be16(response + offset, 4);
    offset += 2;
    std::memcpy(response + offset, active_config.ip, 4);
    offset += 4;

    return offset;
}

void dns_task(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        running = false;
        task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(kDnsPort);

    if (bind(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket on port %u", static_cast<unsigned>(kDnsPort));
        close(sock);
        running = false;
        task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "DNS responder started for %s -> %u.%u.%u.%u",
             active_config.site_name,
             active_config.ip[0],
             active_config.ip[1],
             active_config.ip[2],
             active_config.ip[3]);

    uint8_t request[kMaxDnsPacket];
    uint8_t response[kMaxDnsPacket];
    while (running) {
        sockaddr_in source_addr = {};
        socklen_t source_len = sizeof(source_addr);
        const int received = recvfrom(sock, request, sizeof(request), 0, reinterpret_cast<sockaddr*>(&source_addr), &source_len);
        if (received <= 0) {
            continue;
        }

        const size_t response_size = build_response(request, static_cast<size_t>(received), response, sizeof(response));
        if (response_size > 0) {
            sendto(sock, response, response_size, 0, reinterpret_cast<sockaddr*>(&source_addr), source_len);
        }
    }

    close(sock);
    task_handle = nullptr;
    vTaskDelete(nullptr);
}
} // namespace

bool start(const Config& config) {
    if (running) {
        active_config = config;
        return true;
    }

    active_config = config;
    running = true;
    const BaseType_t created = xTaskCreate(dns_task, "dns_server", 4096, nullptr, tskIDLE_PRIORITY + 2, &task_handle);
    if (created != pdPASS) {
        running = false;
        task_handle = nullptr;
        ESP_LOGE(TAG, "Failed to create DNS task");
        return false;
    }

    return true;
}

void stop() {
    running = false;
}

bool is_running() {
    return running;
}

} // namespace DNSServer
} // namespace Drivers
