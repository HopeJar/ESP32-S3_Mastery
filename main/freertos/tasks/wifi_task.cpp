/**
 * @file wifi_task.cpp
 * @brief WiFi task operations
 */

#include "freertos/tasks/wifi_task.hpp"
#include "drivers/wifi_driver.hpp"
#include "web/http_server.hpp"
#include "secrets/Hidden/wifi_secrets.hpp"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <cstdio>
#include <cstring>

namespace WiFiTask {

namespace {
    const char* TAG = "WiFiTaskOp";

    // Network settings (default: DHCP).
    constexpr bool kUseStaticIp = false;
    constexpr uint8_t kStaticIp[4] = {192, 168, 1, 50};
    constexpr uint8_t kStaticGateway[4] = {192, 168, 1, 1};
    constexpr uint8_t kStaticNetmask[4] = {255, 255, 255, 0};
    constexpr bool kUseCustomDns = false;
    constexpr uint8_t kDnsServer[4] = {8, 8, 8, 8};
    constexpr const char* kNetworkNvsNamespace = "q2net";
    constexpr const char* kAdminNvsNamespace = "q2admin";

    char hostname[32] = {};
    char default_ap_ssid[32] = {};
    bool initialized = false;
} // namespace

void BuildDefaultNames() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(hostname, sizeof(hostname), "espquake");
    std::snprintf(default_ap_ssid, sizeof(default_ap_ssid), "ESPQUAKE-%02X%02X", mac[4], mac[5]);
}

bool LooksLikePlaceholderSecret(const char* value) {
    return value == nullptr || value[0] == '\0' || std::strcmp(value, "YOUR_WIFI_SSID") == 0;
}

void CopyString(char* dest, size_t dest_size, const char* source) {
    if (dest_size == 0) {
        return;
    }

    std::strncpy(dest, source ? source : "", dest_size);
    dest[dest_size - 1] = '\0';
}

void ReadNvsString(nvs_handle_t nvs, const char* key, char* dest, size_t dest_size) {
    size_t size = dest_size;
    if (nvs_get_str(nvs, key, dest, &size) != ESP_OK) {
        return;
    }
    dest[dest_size - 1] = '\0';
}

void SiteNameToHostname(const char* site_name, char* dest, size_t dest_size) {
    if (dest_size == 0) {
        return;
    }

    size_t write = 0;
    for (size_t read = 0; site_name != nullptr && site_name[read] != '\0' && write + 1 < dest_size; ++read) {
        const unsigned char ch = static_cast<unsigned char>(site_name[read]);
        if (ch == '.') {
            break;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
            dest[write++] = static_cast<char>(ch);
        } else if (ch >= 'A' && ch <= 'Z') {
            dest[write++] = static_cast<char>(ch + ('a' - 'A'));
        }
    }

    while (write > 0 && dest[write - 1] == '-') {
        --write;
    }

    dest[write] = '\0';
    if (dest[0] == '\0') {
        CopyString(dest, dest_size, "espquake");
    }
}

void WiFiOperation() {
    if (!initialized) {
        BuildDefaultNames();

        char mode[8] = "apsta";
        char sta_ssid[33] = {};
        char sta_password[65] = {};
        char ap_ssid[33] = {};
        char ap_password[65] = {};
        char site_name[64] = "espquake.local";

        CopyString(ap_ssid, sizeof(ap_ssid), default_ap_ssid);

        if (!LooksLikePlaceholderSecret(WiFiSecrets::kWiFiSsid)) {
            CopyString(sta_ssid, sizeof(sta_ssid), WiFiSecrets::kWiFiSsid);
            CopyString(sta_password, sizeof(sta_password), WiFiSecrets::kWiFiPassword);
        }

        if (sta_ssid[0] == '\0') {
            CopyString(mode, sizeof(mode), "ap");
        }

        nvs_handle_t nvs = 0;
        if (nvs_open(kNetworkNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
            ReadNvsString(nvs, "mode", mode, sizeof(mode));
            ReadNvsString(nvs, "ssid", sta_ssid, sizeof(sta_ssid));
            ReadNvsString(nvs, "password", sta_password, sizeof(sta_password));
            ReadNvsString(nvs, "ap_ssid", ap_ssid, sizeof(ap_ssid));
            ReadNvsString(nvs, "ap_password", ap_password, sizeof(ap_password));
            nvs_close(nvs);
        }

        if (nvs_open(kAdminNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
            ReadNvsString(nvs, "site_name", site_name, sizeof(site_name));
            nvs_close(nvs);
        }
        SiteNameToHostname(site_name, hostname, sizeof(hostname));
        ESP_LOGI(TAG, "Hostname: %s (%s)", hostname, site_name);

        if (std::strcmp(mode, "ap") != 0 && std::strcmp(mode, "sta") != 0 && std::strcmp(mode, "apsta") != 0) {
            CopyString(mode, sizeof(mode), "apsta");
        }

        const bool connect_sta = std::strcmp(mode, "ap") != 0 && sta_ssid[0] != '\0';
        const bool start_ap = std::strcmp(mode, "sta") != 0 || !connect_sta;
        if (ap_ssid[0] == '\0') {
            CopyString(ap_ssid, sizeof(ap_ssid), default_ap_ssid);
        }

        Drivers::WiFi::Config config{};
        config.ssid = sta_ssid;
        config.password = sta_password;
        config.connect_sta = connect_sta;
        config.use_static_ip = kUseStaticIp;
        config.use_custom_dns = kUseCustomDns;
        config.hostname = hostname;
        config.start_ap = start_ap;
        config.ap_ssid = ap_ssid;
        config.ap_password = ap_password;
        config.ap_channel = 6;
        config.ap_max_connections = 4;
        std::memcpy(config.static_ip, kStaticIp, sizeof(config.static_ip));
        std::memcpy(config.static_gateway, kStaticGateway, sizeof(config.static_gateway));
        std::memcpy(config.static_netmask, kStaticNetmask, sizeof(config.static_netmask));
        std::memcpy(config.dns_server, kDnsServer, sizeof(config.dns_server));

        Drivers::WiFi::initialize(config);
        ESP_LOGI(TAG, "Network mode=%s, sta=%s, ap=%s", mode, connect_sta ? "enabled" : "disabled", start_ap ? ap_ssid : "disabled");
        initialized = true;
    }

    Drivers::WiFi::process();
    if (Drivers::WiFi::is_connected() || Drivers::WiFi::is_ap_started()) {
        if (!Web::HTTPServer::is_running()) {
            Web::HTTPServer::start();
        }
    } else if (Web::HTTPServer::is_running()) {
        Web::HTTPServer::stop();
    }

    vTaskDelay(pdMS_TO_TICKS(500));
}

} // namespace WiFiTask
