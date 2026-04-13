/**
 * @file wifi_driver.cpp
 * @brief WiFi driver implementation
 */

#include "wifi_driver.hpp"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include <cstring>

static const char* TAG = "WiFi_Driver";

namespace Drivers {
namespace WiFi {

namespace {
    bool initialized = false;
    bool connected = false;
    bool ap_started = false;
    bool sta_enabled = false;
    esp_netif_t* sta_netif = nullptr;
    esp_netif_t* ap_netif = nullptr;
    esp_event_handler_instance_t wifi_event_instance = nullptr;
    esp_event_handler_instance_t ip_event_instance = nullptr;
} // anonymous namespace

void handle_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (sta_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        const auto* disconnect = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d. Reconnecting...", disconnect ? disconnect->reason : -1);
        if (sta_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ap_started = true;
        ESP_LOGI(TAG, "WiFi access point started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ap_started = false;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const auto* connected_event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
        ESP_LOGI(TAG, "AP client joined, aid=%d", connected_event ? connected_event->aid : -1);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const auto* disconnected_event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        ESP_LOGI(TAG, "AP client left, aid=%d", disconnected_event ? disconnected_event->aid : -1);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

bool initialize(const Config& config) {
    if (initialized) {
        ESP_LOGW(TAG, "WiFi driver already initialized");
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_enabled = config.connect_sta && config.ssid && config.ssid[0] != '\0';
    const bool ap_enabled = config.start_ap && config.ap_ssid && config.ap_ssid[0] != '\0';
    if (!sta_enabled && !ap_enabled) {
        ESP_LOGE(TAG, "WiFi config did not enable STA or AP");
        return false;
    }

    if (sta_enabled) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (ap_enabled) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (sta_enabled && !sta_netif) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return false;
    }
    if (ap_enabled && !ap_netif) {
        ESP_LOGE(TAG, "Failed to create default WiFi AP netif");
        return false;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &handle_event,
                                                        nullptr, &wifi_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &handle_event,
                                                        nullptr, &ip_event_instance));

    if (sta_enabled && config.hostname && config.hostname[0] != '\0') {
        ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, config.hostname));
    }
    if (ap_enabled && config.hostname && config.hostname[0] != '\0') {
        ESP_ERROR_CHECK(esp_netif_set_hostname(ap_netif, config.hostname));
    }

    if (sta_enabled && config.use_static_ip) {
        esp_netif_dhcpc_stop(sta_netif);
        esp_netif_ip_info_t ip_info{};
        IP4_ADDR(&ip_info.ip, config.static_ip[0], config.static_ip[1], config.static_ip[2], config.static_ip[3]);
        IP4_ADDR(&ip_info.gw, config.static_gateway[0], config.static_gateway[1], config.static_gateway[2], config.static_gateway[3]);
        IP4_ADDR(&ip_info.netmask, config.static_netmask[0], config.static_netmask[1], config.static_netmask[2], config.static_netmask[3]);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));

        if (config.use_custom_dns) {
            esp_netif_dns_info_t dns_info{};
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            IP4_ADDR(&dns_info.ip.u_addr.ip4, config.dns_server[0], config.dns_server[1], config.dns_server[2], config.dns_server[3]);
            ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info));
        }
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (sta_enabled && ap_enabled) {
        mode = WIFI_MODE_APSTA;
    } else if (sta_enabled) {
        mode = WIFI_MODE_STA;
    } else {
        mode = WIFI_MODE_AP;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (sta_enabled) {
        wifi_config_t sta_config{};
        std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), config.ssid ? config.ssid : "", sizeof(sta_config.sta.ssid));
        sta_config.sta.ssid[sizeof(sta_config.sta.ssid) - 1] = '\0';
        std::strncpy(reinterpret_cast<char*>(sta_config.sta.password), config.password ? config.password : "", sizeof(sta_config.sta.password));
        sta_config.sta.password[sizeof(sta_config.sta.password) - 1] = '\0';
        sta_config.sta.threshold.authmode = (config.password && std::strlen(config.password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    if (ap_enabled) {
        wifi_config_t ap_config{};
        std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), config.ap_ssid, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid[sizeof(ap_config.ap.ssid) - 1] = '\0';
        ap_config.ap.ssid_len = std::strlen(reinterpret_cast<char*>(ap_config.ap.ssid));
        std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), config.ap_password ? config.ap_password : "", sizeof(ap_config.ap.password));
        ap_config.ap.password[sizeof(ap_config.ap.password) - 1] = '\0';
        ap_config.ap.channel = config.ap_channel == 0 ? 6 : config.ap_channel;
        ap_config.ap.max_connection = config.ap_max_connections == 0 ? 4 : config.ap_max_connections;
        ap_config.ap.authmode = std::strlen(reinterpret_cast<char*>(ap_config.ap.password)) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    initialized = true;
    ap_started = ap_enabled;
    ESP_LOGI(TAG, "WiFi driver started (mode=%s)", sta_enabled && ap_enabled ? "apsta" : (sta_enabled ? "sta" : "ap"));
    return true;
}

void process() {
    // Periodic processing for WiFi driver
    // This would handle connection monitoring, reconnection logic, etc.
}

bool is_connected() {
    return connected;
}

bool is_ap_started() {
    return ap_started;
}

void deinitialize() {
    if (!initialized) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing WiFi driver");
    esp_wifi_stop();
    esp_wifi_deinit();
    if (wifi_event_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance);
        wifi_event_instance = nullptr;
    }
    if (ip_event_instance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_instance);
        ip_event_instance = nullptr;
    }
    if (sta_netif) {
        esp_netif_destroy(sta_netif);
        sta_netif = nullptr;
    }
    if (ap_netif) {
        esp_netif_destroy(ap_netif);
        ap_netif = nullptr;
    }
    connected = false;
    ap_started = false;
    sta_enabled = false;
    initialized = false;
}

} // namespace WiFi
} // namespace Drivers
