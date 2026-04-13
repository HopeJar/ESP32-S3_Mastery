set(EMBED_TXTFILES
     "web/web_pages/index.html"
     "web/web_pages/style.css"
     "web/web_pages/control.js"
)

set(MAIN_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PROJECT_ROOT_DIR "${MAIN_COMPONENT_DIR}/..")
set(Q2_MAP_SOURCE "${PROJECT_ROOT_DIR}/third_party/q2dm1bsp/q2dm1.bsp")
set(THREE_JS_SOURCE "${MAIN_COMPONENT_DIR}/web/web_pages/vendor/three.module.min.js")
set(Q2_WEB_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/q2_web_assets")

set(REQUIRED_Q2_ASSETS
     "${Q2_MAP_SOURCE}"
     "${THREE_JS_SOURCE}")

set(MISSING_Q2_ASSETS "")
foreach(Q2_ASSET IN LISTS REQUIRED_Q2_ASSETS)
     if(NOT EXISTS "${Q2_ASSET}")
          list(APPEND MISSING_Q2_ASSETS "${Q2_ASSET}")
     endif()
endforeach()

if(MISSING_Q2_ASSETS)
     string(REPLACE ";" "\n  " MISSING_Q2_ASSETS_TEXT "${MISSING_Q2_ASSETS}")
     message(FATAL_ERROR
             "Missing Quake 2 browser assets:\n"
             "  ${MISSING_Q2_ASSETS_TEXT}")
endif()

file(MAKE_DIRECTORY "${Q2_WEB_ASSET_DIR}/maps")
file(MAKE_DIRECTORY "${Q2_WEB_ASSET_DIR}/vendor")
configure_file("${Q2_MAP_SOURCE}" "${Q2_WEB_ASSET_DIR}/maps/q2dm1.bsp" COPYONLY)
configure_file("${THREE_JS_SOURCE}" "${Q2_WEB_ASSET_DIR}/vendor/three.module.min.js" COPYONLY)

set(HTTPS_CERT "${MAIN_COMPONENT_DIR}/secrets/Hidden/server_cert.pem")
set(HTTPS_KEY "${MAIN_COMPONENT_DIR}/secrets/Hidden/server_key.pem")
set(HTTPS_CERTS_FOUND FALSE)

if(NOT EXISTS "${HTTPS_CERT}" OR NOT EXISTS "${HTTPS_KEY}")
     set(HTTPS_CERT "${MAIN_COMPONENT_DIR}/secrets/hidden/server_cert.pem")
     set(HTTPS_KEY "${MAIN_COMPONENT_DIR}/secrets/hidden/server_key.pem")
endif()

if(EXISTS "${HTTPS_CERT}" AND EXISTS "${HTTPS_KEY}")
     if(HTTPS_CERT MATCHES "/secrets/Hidden/")
          set(HTTPS_CERT_EMBED "secrets/Hidden/server_cert.pem")
          set(HTTPS_KEY_EMBED "secrets/Hidden/server_key.pem")
     else()
          set(HTTPS_CERT_EMBED "secrets/hidden/server_cert.pem")
          set(HTTPS_KEY_EMBED "secrets/hidden/server_key.pem")
     endif()

     list(APPEND EMBED_TXTFILES "${HTTPS_CERT_EMBED}" "${HTTPS_KEY_EMBED}")
     set(HTTPS_CERTS_FOUND TRUE)
endif()

set(HTTPS_SERVER_AVAILABLE FALSE)
if(DEFINED IDF_PATH)
     if(EXISTS "${IDF_PATH}/components/esp_https_server/include/esp_https_server.h")
          set(HTTPS_SERVER_AVAILABLE TRUE)
     endif()
endif()

set(MAIN_REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_timer mbedtls spiffs)
if(HTTPS_SERVER_AVAILABLE)
     list(APPEND MAIN_REQUIRES esp_https_server)
endif()

idf_component_register(SRCS "drivers/led_driver.cpp"
                                   "logging/logger.cpp"
                                   "drivers/wifi_driver.cpp"
                                   "drivers/ethernet_driver.cpp"
                                   "freertos/tasks/tasks.cpp"
                                   "freertos/tasks/blink_task.cpp"
                                   "freertos/tasks/wifi_task.cpp"
                                   "freertos/tasks/ethernet_task.cpp"
                                   "freertos/tasks/monitor_task.cpp"
                                   "middleware/wifi/wifi_driver.cpp"
                                   "middleware/uart/uart_driver.cpp"
                                   "middleware/spi/spi_driver.cpp"
                                   "middleware/ethernet/ethernet_driver.cpp"
                                   "middleware/comm/protocol.cpp"
                                   "web/http_server.cpp"
                                   "web/https_server.cpp"
                                   "devices/led.cpp"
                                   "devices/led1.cpp"
                                   "devices/led2.cpp"
                                   "devices/rgb_led.cpp"
                            INCLUDE_DIRS "."
                            EMBED_TXTFILES ${EMBED_TXTFILES}
                            REQUIRES ${MAIN_REQUIRES})

spiffs_create_partition_image(web_assets "${Q2_WEB_ASSET_DIR}" FLASH_IN_PROJECT)

if(HTTPS_CERTS_FOUND)
     target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_HTTPS_CERTS=1)
     target_compile_definitions(${COMPONENT_LIB} PRIVATE HTTPS_CERT_PATH="${HTTPS_CERT}")
     target_compile_definitions(${COMPONENT_LIB} PRIVATE HTTPS_KEY_PATH="${HTTPS_KEY}")
endif()

if(HTTPS_SERVER_AVAILABLE)
     target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_HTTPS_SERVER=1)
endif()
