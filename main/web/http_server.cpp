/**
 * @file http_server.cpp
 * @brief HTTP server implementation
 */

#include "http_server.hpp"

#include "drivers/wifi_driver.hpp"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "https_server.hpp"
#include "logging/logger.hpp"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Web {
namespace HTTPServer {

namespace {
const Logging::ModuleLogger kLog(Logging::Module::HTTPServer);

constexpr const char* kAssetPartitionLabel = "web_assets";
constexpr const char* kAssetBasePath = "/q2fs";
constexpr const char* kNetworkNvsNamespace = "q2net";
constexpr const char* kAdminNvsNamespace = "q2admin";
constexpr size_t kRequestBodyLimit = 1400;
constexpr size_t kNetworkJsonLimit = 900;
constexpr size_t kAdminJsonLimit = 1400;
constexpr size_t kMatchJsonLimit = 3600;
constexpr int kMaxPlayers = 8;
constexpr int64_t kPlayerTimeoutUs = 15 * 1000 * 1000;

struct GameSettings {
    char site_name[64];
    char server_name[40];
    char match_mode[8];
    uint8_t max_players;
    uint16_t time_limit_min;
    uint16_t frag_limit;
    uint16_t team_score_limit;
    bool friendly_fire;
    bool admin_configured;
};

struct PlayerState {
    bool active;
    uint32_t id;
    uint32_t color;
    uint8_t spawn_index;
    uint8_t team;
    int16_t score;
    int16_t deaths;
    char name[24];
    float x;
    float y;
    float z;
    float yaw;
    float pitch;
    int64_t last_seen_us;
};

bool running = false;
bool https_enabled = false;
bool q2_assets_mounted = false;
httpd_handle_t server = nullptr;
SemaphoreHandle_t match_mutex = nullptr;
int64_t match_started_us = 0;
uint32_t next_player_id = 1;
PlayerState players[kMaxPlayers] = {};

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t control_js_start[] asm("_binary_control_js_start");
extern const uint8_t control_js_end[] asm("_binary_control_js_end");
} // namespace

static void load_game_settings(GameSettings* settings);

static void set_no_store_headers(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

static esp_err_t send_text_error(httpd_req_t* req, const char* status, const char* message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    set_no_store_headers(req);
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_error(httpd_req_t* req, const char* status, const char* message) {
    char body[192];
    std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    return send_json(req, body);
}

static esp_err_t send_embedded(httpd_req_t* req, const uint8_t* start, const uint8_t* end, const char* type) {
    httpd_resp_set_type(req, type);
    set_no_store_headers(req);

    size_t len = static_cast<size_t>(end - start);
    if (len > 0 && start[len - 1] == 0) {
        len -= 1;
    }

    return httpd_resp_send(req, reinterpret_cast<const char*>(start), len);
}

static bool mount_q2_assets() {
    if (q2_assets_mounted) {
        return true;
    }

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kAssetBasePath;
    conf.partition_label = kAssetPartitionLabel;
    conf.max_files = 8;
    conf.format_if_mount_failed = false;

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        APP_LOGW(kLog, "Failed to mount SPIFFS partition '%s': %s", kAssetPartitionLabel, esp_err_to_name(err));
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(kAssetPartitionLabel, &total, &used) == ESP_OK) {
        APP_LOGI(kLog, "Mounted Quake 2 assets partition: used=%u total=%u", static_cast<unsigned>(used), static_cast<unsigned>(total));
    }

    q2_assets_mounted = true;
    return true;
}

static void unmount_q2_assets() {
    if (!q2_assets_mounted) {
        return;
    }

    esp_vfs_spiffs_unregister(kAssetPartitionLabel);
    q2_assets_mounted = false;
}

static esp_err_t send_spiffs_file(httpd_req_t* req, const char* relative_path, const char* type) {
    if (!q2_assets_mounted && !mount_q2_assets()) {
        return send_text_error(req, "503 Service Unavailable", "Quake 2 assets unavailable");
    }

    char full_path[160];
    const int path_len = std::snprintf(full_path, sizeof(full_path), "%s/%s", kAssetBasePath, relative_path);
    if (path_len <= 0 || path_len >= static_cast<int>(sizeof(full_path))) {
        return send_text_error(req, "500 Internal Server Error", "Asset path too long");
    }

    std::FILE* file = std::fopen(full_path, "rb");
    if (file == nullptr) {
        APP_LOGW(kLog, "Missing Quake 2 asset: %s", full_path);
        return send_text_error(req, "404 Not Found", "Asset not found");
    }

    httpd_resp_set_type(req, type);
    set_no_store_headers(req);

    char buffer[1400];
    while (true) {
        const size_t bytes_read = std::fread(buffer, 1, sizeof(buffer), file);
        if (bytes_read > 0) {
            if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
                std::fclose(file);
                httpd_resp_send_chunk(req, nullptr, 0);
                return ESP_FAIL;
            }
        }

        if (bytes_read < sizeof(buffer)) {
            break;
        }
    }

    std::fclose(file);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static void appendf(char*& out, size_t& remaining, const char* fmt, ...) {
    if (remaining == 0) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(out, remaining, fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    const size_t used = std::min(static_cast<size_t>(written), remaining - 1);
    out += used;
    remaining -= used;
}

static void append_json_string(char*& out, size_t& remaining, const char* value) {
    appendf(out, remaining, "\"");
    for (const char* cursor = value; cursor != nullptr && *cursor != '\0' && remaining > 1; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (ch == '\\' || ch == '"') {
            appendf(out, remaining, "\\%c", ch);
        } else if (ch >= 0x20 && ch <= 0x7e) {
            appendf(out, remaining, "%c", ch);
        }
    }
    appendf(out, remaining, "\"");
}

static void copy_string(char* dest, size_t dest_size, const char* source) {
    if (dest_size == 0) {
        return;
    }

    std::strncpy(dest, source ? source : "", dest_size);
    dest[dest_size - 1] = '\0';
}

static bool read_request_body(httpd_req_t* req, char* body, size_t body_size) {
    if (body_size == 0 || req->content_len >= body_size || req->content_len > kRequestBodyLimit) {
        return false;
    }

    size_t received = 0;
    size_t remaining = req->content_len;
    while (remaining > 0) {
        const int ret = httpd_req_recv(req, body + received, remaining);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }

        received += static_cast<size_t>(ret);
        remaining -= static_cast<size_t>(ret);
    }

    body[received] = '\0';
    return true;
}

static const char* find_json_value(const char* json, const char* key) {
    char pattern[40];
    std::snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* key_pos = std::strstr(json, pattern);
    if (key_pos == nullptr) {
        return nullptr;
    }

    const char* colon = std::strchr(key_pos + std::strlen(pattern), ':');
    if (colon == nullptr) {
        return nullptr;
    }

    const char* value = colon + 1;
    while (*value != '\0' && std::isspace(static_cast<unsigned char>(*value))) {
        ++value;
    }
    return value;
}

static bool json_get_string(const char* json, const char* key, char* out, size_t out_len) {
    if (out_len == 0) {
        return false;
    }

    const char* value = find_json_value(json, key);
    if (value == nullptr || *value != '"') {
        return false;
    }

    ++value;
    size_t written = 0;
    while (*value != '\0' && *value != '"' && written + 1 < out_len) {
        if (*value == '\\' && value[1] != '\0') {
            ++value;
        }

        const unsigned char ch = static_cast<unsigned char>(*value);
        out[written++] = (ch >= 0x20 && ch <= 0x7e) ? static_cast<char>(ch) : '_';
        ++value;
    }

    out[written] = '\0';
    return *value == '"';
}

static bool json_get_float(const char* json, const char* key, float* out) {
    const char* value = find_json_value(json, key);
    if (value == nullptr) {
        return false;
    }

    char* end = nullptr;
    const float parsed = static_cast<float>(std::strtod(value, &end));
    if (end == value) {
        return false;
    }

    *out = parsed;
    return true;
}

static bool json_get_uint32(const char* json, const char* key, uint32_t* out) {
    const char* value = find_json_value(json, key);
    if (value == nullptr) {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return false;
    }

    *out = static_cast<uint32_t>(parsed);
    return true;
}

static bool json_get_bool(const char* json, const char* key, bool* out) {
    const char* value = find_json_value(json, key);
    if (value == nullptr) {
        return false;
    }

    if (std::strncmp(value, "true", 4) == 0) {
        *out = true;
        return true;
    }

    if (std::strncmp(value, "false", 5) == 0) {
        *out = false;
        return true;
    }

    return false;
}

static uint16_t clamp_u16(uint32_t value, uint16_t min_value, uint16_t max_value) {
    return static_cast<uint16_t>(std::min<uint32_t>(std::max<uint32_t>(value, min_value), max_value));
}

static uint8_t clamp_u8(uint32_t value, uint8_t min_value, uint8_t max_value) {
    return static_cast<uint8_t>(std::min<uint32_t>(std::max<uint32_t>(value, min_value), max_value));
}

static void sanitize_domain_name(char* value, size_t value_len) {
    if (value_len == 0) {
        return;
    }

    size_t write = 0;
    for (size_t read = 0; value[read] != '\0' && write + 1 < value_len; ++read) {
        const unsigned char ch = static_cast<unsigned char>(value[read]);
        if (std::isalnum(ch)) {
            value[write++] = static_cast<char>(std::tolower(ch));
        } else if ((ch == '-' || ch == '.') && write > 0 && value[write - 1] != '-' && value[write - 1] != '.') {
            value[write++] = static_cast<char>(ch);
        }
    }

    while (write > 0 && (value[write - 1] == '-' || value[write - 1] == '.')) {
        --write;
    }

    value[write] = '\0';
    if (value[0] == '\0') {
        copy_string(value, value_len, "espquake.local");
    }
}

static void sanitize_server_name(char* value, size_t value_len) {
    if (value_len == 0) {
        return;
    }

    if (value[0] == '\0') {
        copy_string(value, value_len, "ESP Quake");
        return;
    }

    for (char* cursor = value; *cursor != '\0'; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\') {
            *cursor = '_';
        }
    }
}

static void sanitize_match_mode(char* value, size_t value_len) {
    if (std::strcmp(value, "teams") != 0 && std::strcmp(value, "ffa") != 0) {
        copy_string(value, value_len, "ffa");
    }
}

static void sanitize_player_name(char* name) {
    if (name[0] == '\0') {
        std::strncpy(name, "player", 24);
        name[23] = '\0';
        return;
    }

    for (char* cursor = name; *cursor != '\0'; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\') {
            *cursor = '_';
        }
    }
}

static bool ensure_match_mutex() {
    if (match_mutex != nullptr) {
        return true;
    }

    match_mutex = xSemaphoreCreateMutex();
    return match_mutex != nullptr;
}

static uint32_t player_color(uint32_t id) {
    constexpr uint32_t colors[] = {
        0xf04f45, 0x3ddc84, 0x4da3ff, 0xffd166,
        0x9b5de5, 0x00c2a8, 0xff7a59, 0xf15bb5,
    };
    return colors[id % (sizeof(colors) / sizeof(colors[0]))];
}

static void cleanup_players_locked(int64_t now_us) {
    for (PlayerState& player : players) {
        if (player.active && now_us - player.last_seen_us > kPlayerTimeoutUs) {
            player.active = false;
        }
    }
}

static PlayerState* find_player_locked(uint32_t id) {
    for (PlayerState& player : players) {
        if (player.active && player.id == id) {
            return &player;
        }
    }

    return nullptr;
}

static uint8_t pick_team_locked() {
    uint8_t red = 0;
    uint8_t blue = 0;
    for (const PlayerState& player : players) {
        if (!player.active) {
            continue;
        }
        if (player.team == 1) {
            ++red;
        } else if (player.team == 2) {
            ++blue;
        }
    }

    return red <= blue ? 1 : 2;
}

static void append_players_json_locked(char*& out, size_t& remaining) {
    appendf(out, remaining, "\"players\":[");
    bool first = true;
    for (const PlayerState& player : players) {
        if (!player.active) {
            continue;
        }

        if (!first) {
            appendf(out, remaining, ",");
        }

        appendf(out, remaining,
                "{\"id\":%lu,\"name\":",
                static_cast<unsigned long>(player.id));
        append_json_string(out, remaining, player.name);
        appendf(out, remaining,
                ",\"color\":%lu,\"spawn_index\":%u,\"team\":%u,\"score\":%d,\"deaths\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"yaw\":%.4f,\"pitch\":%.4f}",
                static_cast<unsigned long>(player.color),
                static_cast<unsigned>(player.spawn_index),
                static_cast<unsigned>(player.team),
                static_cast<int>(player.score),
                static_cast<int>(player.deaths),
                static_cast<double>(player.x),
                static_cast<double>(player.y),
                static_cast<double>(player.z),
                static_cast<double>(player.yaw),
                static_cast<double>(player.pitch));
        first = false;
    }
    appendf(out, remaining, "]");
}

static esp_err_t send_match_state(httpd_req_t* req, uint32_t self_id = 0) {
    if (!ensure_match_mutex()) {
        return send_json_error(req, "503 Service Unavailable", "match lock unavailable");
    }

    GameSettings settings = {};
    load_game_settings(&settings);

    char body[kMatchJsonLimit];
    char* out = body;
    size_t remaining = sizeof(body);
    const int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(match_mutex, portMAX_DELAY);
    cleanup_players_locked(now_us);
    appendf(out, remaining, "{\"ok\":true,\"self_id\":%lu,\"match\":{\"map\":\"q2dm1\",\"mode\":",
            static_cast<unsigned long>(self_id));
    append_json_string(out, remaining, settings.match_mode);
    appendf(out, remaining,
            ",\"server_name\":");
    append_json_string(out, remaining, settings.server_name);
    appendf(out, remaining,
            ",\"continuous\":true,\"uptime_ms\":%lld,\"time_limit_min\":%u,\"frag_limit\":%u,\"team_score_limit\":%u,\"max_players\":%u,\"friendly_fire\":%s},",
            static_cast<long long>((now_us - match_started_us) / 1000),
            static_cast<unsigned>(settings.time_limit_min),
            static_cast<unsigned>(settings.frag_limit),
            static_cast<unsigned>(settings.team_score_limit),
            static_cast<unsigned>(settings.max_players),
            settings.friendly_fire ? "true" : "false");
    append_players_json_locked(out, remaining);
    appendf(out, remaining, "}");
    xSemaphoreGive(match_mutex);

    body[sizeof(body) - 1] = '\0';
    return send_json(req, body);
}

static void nvs_read_string(nvs_handle_t nvs, const char* key, char* out, size_t out_len, const char* fallback) {
    if (out_len == 0) {
        return;
    }

    if (fallback != nullptr) {
        std::strncpy(out, fallback, out_len);
        out[out_len - 1] = '\0';
    } else {
        out[0] = '\0';
    }

    size_t size = out_len;
    if (nvs_get_str(nvs, key, out, &size) != ESP_OK) {
        if (fallback != nullptr) {
            std::strncpy(out, fallback, out_len);
            out[out_len - 1] = '\0';
        }
    }
}

static void load_default_game_settings(GameSettings* settings) {
    copy_string(settings->site_name, sizeof(settings->site_name), "espquake.local");
    copy_string(settings->server_name, sizeof(settings->server_name), "ESP Quake");
    copy_string(settings->match_mode, sizeof(settings->match_mode), "ffa");
    settings->max_players = 8;
    settings->time_limit_min = 20;
    settings->frag_limit = 30;
    settings->team_score_limit = 50;
    settings->friendly_fire = false;
    settings->admin_configured = false;
}

static bool load_admin_secret(char* salt, size_t salt_len, char* hash, size_t hash_len) {
    salt[0] = '\0';
    hash[0] = '\0';

    nvs_handle_t nvs = 0;
    if (nvs_open(kAdminNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    nvs_read_string(nvs, "salt", salt, salt_len, "");
    nvs_read_string(nvs, "hash", hash, hash_len, "");
    nvs_close(nvs);

    return salt[0] != '\0' && hash[0] != '\0';
}

static void load_game_settings(GameSettings* settings) {
    load_default_game_settings(settings);

    char salt[17] = {};
    char hash[65] = {};
    settings->admin_configured = load_admin_secret(salt, sizeof(salt), hash, sizeof(hash));

    nvs_handle_t nvs = 0;
    if (nvs_open(kAdminNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    nvs_read_string(nvs, "site_name", settings->site_name, sizeof(settings->site_name), settings->site_name);
    nvs_read_string(nvs, "server_name", settings->server_name, sizeof(settings->server_name), settings->server_name);
    nvs_read_string(nvs, "match_mode", settings->match_mode, sizeof(settings->match_mode), settings->match_mode);

    uint8_t max_players = settings->max_players;
    uint16_t time_limit_min = settings->time_limit_min;
    uint16_t frag_limit = settings->frag_limit;
    uint16_t team_score_limit = settings->team_score_limit;
    uint8_t friendly_fire = settings->friendly_fire ? 1 : 0;

    nvs_get_u8(nvs, "max_players", &max_players);
    nvs_get_u16(nvs, "time_limit", &time_limit_min);
    nvs_get_u16(nvs, "frag_limit", &frag_limit);
    nvs_get_u16(nvs, "team_limit", &team_score_limit);
    nvs_get_u8(nvs, "friendly", &friendly_fire);
    nvs_close(nvs);

    sanitize_domain_name(settings->site_name, sizeof(settings->site_name));
    sanitize_server_name(settings->server_name, sizeof(settings->server_name));
    sanitize_match_mode(settings->match_mode, sizeof(settings->match_mode));
    settings->max_players = clamp_u8(max_players, 1, kMaxPlayers);
    settings->time_limit_min = clamp_u16(time_limit_min, 0, 240);
    settings->frag_limit = clamp_u16(frag_limit, 0, 999);
    settings->team_score_limit = clamp_u16(team_score_limit, 0, 999);
    settings->friendly_fire = friendly_fire != 0;
}

static esp_err_t save_game_settings(nvs_handle_t nvs, const GameSettings& settings) {
    esp_err_t err = nvs_set_str(nvs, "site_name", settings.site_name);
    if (err == ESP_OK) err = nvs_set_str(nvs, "server_name", settings.server_name);
    if (err == ESP_OK) err = nvs_set_str(nvs, "match_mode", settings.match_mode);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "max_players", settings.max_players);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "time_limit", settings.time_limit_min);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "frag_limit", settings.frag_limit);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "team_limit", settings.team_score_limit);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "friendly", settings.friendly_fire ? 1 : 0);
    return err;
}

static void apply_json_game_settings(const char* json, GameSettings* settings) {
    uint32_t value = 0;
    bool flag = false;

    json_get_string(json, "site_name", settings->site_name, sizeof(settings->site_name));
    json_get_string(json, "server_name", settings->server_name, sizeof(settings->server_name));
    json_get_string(json, "match_mode", settings->match_mode, sizeof(settings->match_mode));
    if (json_get_uint32(json, "max_players", &value)) settings->max_players = clamp_u8(value, 1, kMaxPlayers);
    if (json_get_uint32(json, "time_limit_min", &value)) settings->time_limit_min = clamp_u16(value, 0, 240);
    if (json_get_uint32(json, "frag_limit", &value)) settings->frag_limit = clamp_u16(value, 0, 999);
    if (json_get_uint32(json, "team_score_limit", &value)) settings->team_score_limit = clamp_u16(value, 0, 999);
    if (json_get_bool(json, "friendly_fire", &flag)) settings->friendly_fire = flag;

    sanitize_domain_name(settings->site_name, sizeof(settings->site_name));
    sanitize_server_name(settings->server_name, sizeof(settings->server_name));
    sanitize_match_mode(settings->match_mode, sizeof(settings->match_mode));
}

static void append_settings_json(char*& out, size_t& remaining, const GameSettings& settings) {
    appendf(out, remaining, "\"settings\":{\"site_name\":");
    append_json_string(out, remaining, settings.site_name);
    appendf(out, remaining, ",\"server_name\":");
    append_json_string(out, remaining, settings.server_name);
    appendf(out, remaining,
            ",\"match_mode\":\"%s\",\"max_players\":%u,\"time_limit_min\":%u,\"frag_limit\":%u,\"team_score_limit\":%u,\"friendly_fire\":%s}",
            settings.match_mode,
            static_cast<unsigned>(settings.max_players),
            static_cast<unsigned>(settings.time_limit_min),
            static_cast<unsigned>(settings.frag_limit),
            static_cast<unsigned>(settings.team_score_limit),
            settings.friendly_fire ? "true" : "false");
}

static void bytes_to_hex(const uint8_t* bytes, size_t byte_count, char* out, size_t out_len) {
    constexpr char kHex[] = "0123456789abcdef";
    if (out_len < byte_count * 2 + 1) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    out[byte_count * 2] = '\0';
}

static void make_admin_salt(char* salt, size_t salt_len) {
    uint8_t bytes[8];
    for (uint8_t& byte : bytes) {
        byte = static_cast<uint8_t>(esp_random() & 0xff);
    }
    bytes_to_hex(bytes, sizeof(bytes), salt, salt_len);
}

static bool hash_admin_password(const char* password, const char* salt, char* out, size_t out_len) {
    if (password == nullptr || salt == nullptr || out_len < 65) {
        return false;
    }

    uint8_t digest[32] = {};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(salt), std::strlen(salt));
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(":"), 1);
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(password), std::strlen(password));
    if (rc == 0) rc = mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    if (rc != 0) {
        out[0] = '\0';
        return false;
    }

    bytes_to_hex(digest, sizeof(digest), out, out_len);
    return out[0] != '\0';
}

static bool check_admin_password(const char* password) {
    char salt[17] = {};
    char expected_hash[65] = {};
    if (!load_admin_secret(salt, sizeof(salt), expected_hash, sizeof(expected_hash))) {
        return false;
    }

    char actual_hash[65] = {};
    if (!hash_admin_password(password, salt, actual_hash, sizeof(actual_hash))) {
        return false;
    }

    return std::strcmp(actual_hash, expected_hash) == 0;
}

static esp_err_t index_get_handler(httpd_req_t* req) {
    return send_embedded(req, index_html_start, index_html_end, "text/html");
}

static esp_err_t style_get_handler(httpd_req_t* req) {
    return send_embedded(req, style_css_start, style_css_end, "text/css");
}

static esp_err_t control_get_handler(httpd_req_t* req) {
    return send_embedded(req, control_js_start, control_js_end, "application/javascript");
}

static esp_err_t q2_map_get_handler(httpd_req_t* req) {
    return send_spiffs_file(req, "maps/q2dm1.bsp", "application/octet-stream");
}

static esp_err_t three_js_get_handler(httpd_req_t* req) {
    return send_spiffs_file(req, "vendor/three.module.min.js", "application/javascript");
}

static esp_err_t favicon_get_handler(httpd_req_t* req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_type(req, "image/x-icon");
    set_no_store_headers(req);
    return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t status_get_handler(httpd_req_t* req) {
    return send_match_state(req);
}

static esp_err_t admin_get_handler(httpd_req_t* req) {
    GameSettings settings = {};
    load_game_settings(&settings);

    char body[kAdminJsonLimit];
    char* out = body;
    size_t remaining = sizeof(body);
    appendf(out, remaining, "{\"ok\":true,\"admin_configured\":%s,",
            settings.admin_configured ? "true" : "false");
    append_settings_json(out, remaining, settings);
    appendf(out, remaining, "}");
    body[sizeof(body) - 1] = '\0';
    return send_json(req, body);
}

static esp_err_t admin_setup_post_handler(httpd_req_t* req) {
    GameSettings settings = {};
    load_game_settings(&settings);
    if (settings.admin_configured) {
        return send_json_error(req, "409 Conflict", "admin is already configured");
    }

    char request[kRequestBodyLimit + 1];
    if (!read_request_body(req, request, sizeof(request))) {
        return send_json_error(req, "400 Bad Request", "invalid admin setup request");
    }

    char password[65] = {};
    json_get_string(request, "admin_password", password, sizeof(password));
    if (std::strlen(password) < 8) {
        return send_json_error(req, "400 Bad Request", "admin password must be at least 8 characters");
    }

    apply_json_game_settings(request, &settings);

    char salt[17] = {};
    char hash[65] = {};
    make_admin_salt(salt, sizeof(salt));
    if (!hash_admin_password(password, salt, hash, sizeof(hash))) {
        return send_json_error(req, "500 Internal Server Error", "admin password hash failed");
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kAdminNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        APP_LOGE(kLog, "nvs_open(%s) failed: %s", kAdminNvsNamespace, esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "admin storage unavailable");
    }

    err = nvs_set_str(nvs, "salt", salt);
    if (err == ESP_OK) err = nvs_set_str(nvs, "hash", hash);
    if (err == ESP_OK) err = save_game_settings(nvs, settings);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        APP_LOGE(kLog, "Failed to save admin setup: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "admin setup save failed");
    }

    settings.admin_configured = true;
    char body[kAdminJsonLimit];
    char* out = body;
    size_t remaining = sizeof(body);
    appendf(out, remaining, "{\"ok\":true,\"admin_configured\":true,");
    append_settings_json(out, remaining, settings);
    appendf(out, remaining, ",\"restart_required_for_name\":true}");
    body[sizeof(body) - 1] = '\0';
    return send_json(req, body);
}

static esp_err_t admin_login_post_handler(httpd_req_t* req) {
    char request[kRequestBodyLimit + 1];
    if (!read_request_body(req, request, sizeof(request))) {
        return send_json_error(req, "400 Bad Request", "invalid admin login request");
    }

    char password[65] = {};
    json_get_string(request, "admin_password", password, sizeof(password));
    if (!check_admin_password(password)) {
        return send_json_error(req, "403 Forbidden", "invalid admin password");
    }

    return admin_get_handler(req);
}

static esp_err_t admin_settings_post_handler(httpd_req_t* req) {
    GameSettings settings = {};
    load_game_settings(&settings);
    if (!settings.admin_configured) {
        return send_json_error(req, "403 Forbidden", "admin is not configured");
    }

    char request[kRequestBodyLimit + 1];
    if (!read_request_body(req, request, sizeof(request))) {
        return send_json_error(req, "400 Bad Request", "invalid admin settings request");
    }

    char password[65] = {};
    json_get_string(request, "admin_password", password, sizeof(password));
    if (!check_admin_password(password)) {
        return send_json_error(req, "403 Forbidden", "invalid admin password");
    }

    apply_json_game_settings(request, &settings);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kAdminNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        APP_LOGE(kLog, "nvs_open(%s) failed: %s", kAdminNvsNamespace, esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "admin storage unavailable");
    }

    err = save_game_settings(nvs, settings);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        APP_LOGE(kLog, "Failed to save admin settings: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "admin settings save failed");
    }

    char body[kAdminJsonLimit];
    char* out = body;
    size_t remaining = sizeof(body);
    appendf(out, remaining, "{\"ok\":true,\"admin_configured\":true,");
    append_settings_json(out, remaining, settings);
    appendf(out, remaining, ",\"restart_required_for_name\":true}");
    body[sizeof(body) - 1] = '\0';
    return send_json(req, body);
}

static esp_err_t network_get_handler(httpd_req_t* req) {
    char mode[8] = "apsta";
    char ssid[33] = "";
    char ap_ssid[33] = "ESPQUAKE";
    GameSettings settings = {};
    load_game_settings(&settings);

    nvs_handle_t nvs = 0;
    if (nvs_open(kNetworkNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_read_string(nvs, "mode", mode, sizeof(mode), "apsta");
        nvs_read_string(nvs, "ssid", ssid, sizeof(ssid), "");
        nvs_read_string(nvs, "ap_ssid", ap_ssid, sizeof(ap_ssid), "ESPQUAKE");
        nvs_close(nvs);
    }

    char body[kNetworkJsonLimit];
    char* out = body;
    size_t remaining = sizeof(body);
    appendf(out, remaining, "{\"ok\":true,\"network\":{\"mode\":");
    append_json_string(out, remaining, mode);
    appendf(out, remaining, ",\"ssid\":");
    append_json_string(out, remaining, ssid);
    appendf(out, remaining, ",\"ap_ssid\":");
    append_json_string(out, remaining, ap_ssid);
    appendf(out, remaining, ",\"site_name\":");
    append_json_string(out, remaining, settings.site_name);
    appendf(out, remaining, ",\"sta_connected\":%s,\"ap_started\":%s,\"restart_required_for_changes\":true}}",
            Drivers::WiFi::is_connected() ? "true" : "false",
            Drivers::WiFi::is_ap_started() ? "true" : "false");

    body[sizeof(body) - 1] = '\0';
    return send_json(req, body);
}

static esp_err_t network_post_handler(httpd_req_t* req) {
    char body[kRequestBodyLimit + 1];
    if (!read_request_body(req, body, sizeof(body))) {
        return send_json_error(req, "400 Bad Request", "invalid network request");
    }

    char mode[8] = "apsta";
    char ssid[33] = "";
    char password[65] = "";
    char ap_ssid[33] = "ESPQUAKE";
    char ap_password[65] = "";
    char admin_password[65] = "";

    json_get_string(body, "mode", mode, sizeof(mode));
    json_get_string(body, "ssid", ssid, sizeof(ssid));
    json_get_string(body, "password", password, sizeof(password));
    json_get_string(body, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    json_get_string(body, "ap_password", ap_password, sizeof(ap_password));
    json_get_string(body, "admin_password", admin_password, sizeof(admin_password));

    GameSettings settings = {};
    load_game_settings(&settings);
    if (settings.admin_configured && !check_admin_password(admin_password)) {
        return send_json_error(req, "403 Forbidden", "invalid admin password");
    }

    if (std::strcmp(mode, "ap") != 0 && std::strcmp(mode, "sta") != 0 && std::strcmp(mode, "apsta") != 0) {
        return send_json_error(req, "400 Bad Request", "mode must be ap, sta, or apsta");
    }

    if (std::strcmp(mode, "ap") != 0 && ssid[0] == '\0') {
        return send_json_error(req, "400 Bad Request", "station ssid is required");
    }

    if (ap_ssid[0] == '\0') {
        return send_json_error(req, "400 Bad Request", "ap ssid is required");
    }

    if (ap_password[0] != '\0' && std::strlen(ap_password) < 8) {
        return send_json_error(req, "400 Bad Request", "ap password must be at least 8 characters or blank");
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(kNetworkNvsNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        APP_LOGE(kLog, "nvs_open(%s) failed: %s", kNetworkNvsNamespace, esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "network storage unavailable");
    }

    err = nvs_set_str(nvs, "mode", mode);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ap_ssid", ap_ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ap_password", ap_password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        APP_LOGE(kLog, "Failed to save network config: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "network config save failed");
    }

    return send_json(req, "{\"ok\":true,\"restart_required\":true}");
}

static void reboot_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t* req) {
    GameSettings settings = {};
    load_game_settings(&settings);
    if (settings.admin_configured) {
        char request[kRequestBodyLimit + 1];
        if (!read_request_body(req, request, sizeof(request))) {
            return send_json_error(req, "400 Bad Request", "invalid reboot request");
        }

        char admin_password[65] = {};
        json_get_string(request, "admin_password", admin_password, sizeof(admin_password));
        if (!check_admin_password(admin_password)) {
            return send_json_error(req, "403 Forbidden", "invalid admin password");
        }
    }

    const BaseType_t created = xTaskCreate(reboot_task, "q2_reboot", 2048, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    if (created != pdPASS) {
        return send_json_error(req, "500 Internal Server Error", "reboot task failed");
    }

    return send_json(req, "{\"ok\":true,\"rebooting\":true}");
}

static esp_err_t match_get_handler(httpd_req_t* req) {
    return send_match_state(req);
}

static esp_err_t match_join_post_handler(httpd_req_t* req) {
    if (!ensure_match_mutex()) {
        return send_json_error(req, "503 Service Unavailable", "match lock unavailable");
    }

    char request[kRequestBodyLimit + 1];
    if (!read_request_body(req, request, sizeof(request))) {
        return send_json_error(req, "400 Bad Request", "invalid join request");
    }

    char name[24] = "player";
    json_get_string(request, "name", name, sizeof(name));
    sanitize_player_name(name);

    GameSettings settings = {};
    load_game_settings(&settings);

    const int64_t now_us = esp_timer_get_time();
    PlayerState snapshot = {};

    xSemaphoreTake(match_mutex, portMAX_DELAY);
    cleanup_players_locked(now_us);

    PlayerState* slot = nullptr;
    uint8_t spawn_index = 0;
    for (uint8_t i = 0; i < settings.max_players; ++i) {
        if (!players[i].active) {
            slot = &players[i];
            spawn_index = i;
            break;
        }
    }

    if (slot == nullptr) {
        xSemaphoreGive(match_mutex);
        return send_json_error(req, "503 Service Unavailable", "match is full");
    }

    slot->active = true;
    slot->id = next_player_id++;
    slot->color = player_color(slot->id);
    slot->spawn_index = spawn_index;
    slot->team = std::strcmp(settings.match_mode, "teams") == 0 ? pick_team_locked() : 0;
    slot->score = 0;
    slot->deaths = 0;
    std::strncpy(slot->name, name, sizeof(slot->name));
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->x = 0.0f;
    slot->y = 0.0f;
    slot->z = 96.0f;
    slot->yaw = 0.0f;
    slot->pitch = 0.0f;
    slot->last_seen_us = now_us;
    snapshot = *slot;
    xSemaphoreGive(match_mutex);

    char body[384];
    std::snprintf(body, sizeof(body),
                  "{\"ok\":true,\"player\":{\"id\":%lu,\"name\":\"%s\",\"color\":%lu,\"spawn_index\":%u,\"team\":%u,\"score\":%d,\"deaths\":%d},\"match\":{\"map\":\"q2dm1\",\"mode\":\"%s\",\"max_players\":%u}}",
                  static_cast<unsigned long>(snapshot.id),
                  snapshot.name,
                  static_cast<unsigned long>(snapshot.color),
                  static_cast<unsigned>(snapshot.spawn_index),
                  static_cast<unsigned>(snapshot.team),
                  static_cast<int>(snapshot.score),
                  static_cast<int>(snapshot.deaths),
                  settings.match_mode,
                  static_cast<unsigned>(settings.max_players));
    return send_json(req, body);
}

static esp_err_t player_state_post_handler(httpd_req_t* req) {
    if (!ensure_match_mutex()) {
        return send_json_error(req, "503 Service Unavailable", "match lock unavailable");
    }

    char request[kRequestBodyLimit + 1];
    if (!read_request_body(req, request, sizeof(request))) {
        return send_json_error(req, "400 Bad Request", "invalid player state request");
    }

    uint32_t id = 0;
    if (!json_get_uint32(request, "id", &id) || id == 0) {
        return send_json_error(req, "400 Bad Request", "player id is required");
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    json_get_float(request, "x", &x);
    json_get_float(request, "y", &y);
    json_get_float(request, "z", &z);
    json_get_float(request, "yaw", &yaw);
    json_get_float(request, "pitch", &pitch);

    const int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(match_mutex, portMAX_DELAY);
    cleanup_players_locked(now_us);
    PlayerState* player = find_player_locked(id);
    if (player == nullptr) {
        xSemaphoreGive(match_mutex);
        return send_json_error(req, "404 Not Found", "player not joined");
    }

    player->x = x;
    player->y = y;
    player->z = z;
    player->yaw = yaw;
    player->pitch = pitch;
    player->last_seen_us = now_us;
    xSemaphoreGive(match_mutex);

    return send_match_state(req, id);
}

static esp_err_t register_handler(httpd_handle_t handle, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = method;
    route.handler = handler;

    const esp_err_t err = httpd_register_uri_handler(handle, &route);
    if (err != ESP_OK) {
        APP_LOGE(kLog, "Failed to register %s handler: %s", uri, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t register_handlers(httpd_handle_t handle) {
    esp_err_t err = register_handler(handle, "/", HTTP_GET, index_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/index.html", HTTP_GET, index_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/style.css", HTTP_GET, style_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/control.js", HTTP_GET, control_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/assets/maps/q2dm1.bsp", HTTP_GET, q2_map_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/assets/vendor/three.module.min.js", HTTP_GET, three_js_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/favicon.ico", HTTP_GET, favicon_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/status", HTTP_GET, status_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/admin", HTTP_GET, admin_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/admin/setup", HTTP_POST, admin_setup_post_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/admin/login", HTTP_POST, admin_login_post_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/admin/settings", HTTP_POST, admin_settings_post_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/network", HTTP_GET, network_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/network", HTTP_POST, network_post_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/network/reboot", HTTP_POST, reboot_post_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/match", HTTP_GET, match_get_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/match/join", HTTP_POST, match_join_post_handler);
    if (err == ESP_OK) err = register_handler(handle, "/api/v1/player/state", HTTP_POST, player_state_post_handler);
    return err;
}

bool start() {
    if (running) {
        return true;
    }

    if (!ensure_match_mutex()) {
        APP_LOGE(kLog, "Failed to create match mutex");
        return false;
    }

    if (match_started_us == 0) {
        match_started_us = esp_timer_get_time();
    }

    mount_q2_assets();

    if (HTTPSServer::start(&server)) {
        https_enabled = true;
        running = (register_handlers(server) == ESP_OK);
        if (running) {
            return true;
        }

        HTTPSServer::stop(server);
        server = nullptr;
        https_enabled = false;
        APP_LOGW(kLog, "Failed to register HTTPS handlers, falling back to HTTP");
    } else {
        APP_LOGW(kLog, "Falling back to HTTP");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    if (httpd_start(&server, &config) == ESP_OK) {
        https_enabled = false;
        running = (register_handlers(server) == ESP_OK);
        APP_LOGI(kLog, "HTTP server started on port 80");
        return running;
    }

    return false;
}

void stop() {
    if (!running) {
        return;
    }

    APP_LOGI(kLog, "Stopping HTTP server");
    if (server) {
        if (https_enabled) {
            HTTPSServer::stop(server);
        } else {
            httpd_stop(server);
        }
        server = nullptr;
    }

    https_enabled = false;
    running = false;
    unmount_q2_assets();
}

bool is_running() {
    return running;
}

} // namespace HTTPServer
} // namespace Web
