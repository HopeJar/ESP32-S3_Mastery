/**
 * @file https_server.cpp
 * @brief HTTPS server implementation
 */

#include "https_server.hpp"

#include "esp_err.h"
#include "esp_idf_version.h"
#include "logging/logger.hpp"

#if defined(__has_include)
#if __has_include("esp_https_server.h")
#define HAVE_HTTPS_SERVER_HEADER 1
#include "esp_https_server.h"
#endif
#endif

#include <cstddef>

namespace Web {
namespace HTTPSServer {

namespace {
const Logging::ModuleLogger kLog(Logging::Module::HTTPSServer);
#if defined(HAVE_HTTPS_CERTS) && defined(HAVE_HTTPS_SERVER_HEADER)
extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_pem_start[] asm("_binary_server_key_pem_start");
extern const uint8_t server_key_pem_end[] asm("_binary_server_key_pem_end");
#endif
} // namespace

bool start(httpd_handle_t* handle) {
    if (handle == nullptr) {
        return false;
    }

#if !defined(HAVE_HTTPS_SERVER_HEADER)
    APP_LOGW(kLog, "HTTPS server component not available.");
    return false;
#elif !defined(HAVE_HTTPS_CERTS)
    APP_LOGW(kLog, "HTTPS certs not embedded.");
    return false;
#else
#if defined(HTTPS_CERT_PATH)
    APP_LOGW(kLog, "HTTPS cert path: %s", HTTPS_CERT_PATH);
#endif
#if defined(HTTPS_KEY_PATH)
    APP_LOGW(kLog, "HTTPS key path: %s", HTTPS_KEY_PATH);
#endif

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.port_secure = 443;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    conf.servercert = server_cert_pem_start;
    conf.servercert_len = static_cast<size_t>(server_cert_pem_end - server_cert_pem_start);
#else
    conf.cacert_pem = server_cert_pem_start;
    conf.cacert_len = static_cast<size_t>(server_cert_pem_end - server_cert_pem_start);
#endif
    conf.prvtkey_pem = server_key_pem_start;
    conf.prvtkey_len = static_cast<size_t>(server_key_pem_end - server_key_pem_start);
    conf.httpd.max_uri_handlers = 28;
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_ssl_start(handle, &conf) != ESP_OK) {
        APP_LOGW(kLog, "Failed to start HTTPS server");
        return false;
    }

    APP_LOGI(kLog, "HTTPS server started on port 443");
    return true;
#endif
}

void stop(httpd_handle_t handle) {
#if defined(HAVE_HTTPS_CERTS) && defined(HAVE_HTTPS_SERVER_HEADER)
    if (handle != nullptr) {
        httpd_ssl_stop(handle);
    }
#else
    (void)handle;
#endif
}

} // namespace HTTPSServer
} // namespace Web
