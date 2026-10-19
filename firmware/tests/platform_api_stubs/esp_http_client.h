#pragma once
#include "esp_err.h"
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_DELETE };
constexpr int HTTP_EVENT_ON_DATA = 3;
struct esp_http_client_event_t { int event_id; void* data; int data_len; void* user_data; };
struct esp_http_client_config_t {
    const char* url = nullptr;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    int timeout_ms = 0;
    esp_err_t (*crt_bundle_attach)(void*) = nullptr;
    esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
    void* user_data = nullptr;
    int buffer_size = 0, buffer_size_tx = 0;
};
struct TestHttp;
using esp_http_client_handle_t = TestHttp*;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char*, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
