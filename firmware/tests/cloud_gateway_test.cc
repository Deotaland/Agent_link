#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "cloud_api.h"
#include "cloud_log.h"
#include "cloud_platform.h"
#include "esp_http_client.h"
#include "sdkconfig.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL at line %d\n", __LINE__); std::exit(1); } } while (false)
struct TestHttp {
    std::string url, body;
    std::map<std::string, std::string> headers;
};
std::string response, last_body, last_url, logs;
int http_status = 200;
int live_clients = 0;
const char* stored_key = "fixture-stored-key";
extern "C" const char* cloud_cred_auth_key() { return stored_key; }
void cloud_platform_set_bound_agent(const cJSON*) {}

void platform_test_log(const char*, const char* format, ...) {
    char line[1024];
    va_list args; va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args); va_end(args);
    logs += line; logs += '\n';
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    CHECK(config->crt_bundle_attach != nullptr);
    auto* client = new TestHttp; client->url = config->url; ++live_clients; return client;
}
esp_err_t esp_http_client_set_url(TestHttp* client, const char* url) { client->url = url; return ESP_OK; }
esp_err_t esp_http_client_set_method(TestHttp*, esp_http_client_method_t method) { CHECK(method == HTTP_METHOD_POST); return ESP_OK; }
esp_err_t esp_http_client_set_header(TestHttp* client, const char* key, const char* value) { client->headers[key] = value; return ESP_OK; }
esp_err_t esp_http_client_open(TestHttp* client, int) { client->body.clear(); return ESP_OK; }
int esp_http_client_write(TestHttp* client, const char* body, int length) { client->body.assign(body, length); return length; }
int64_t esp_http_client_fetch_headers(TestHttp*) { return response.size(); }
int esp_http_client_get_status_code(TestHttp*) { return http_status; }
int esp_http_client_read_response(TestHttp* client, char* out, int capacity) {
    CHECK(response.size() <= static_cast<size_t>(capacity));
    CHECK(client->headers["Content-Type"] == "application/json");
    CHECK(client->headers.count("Authorization") == 0);
    last_body = client->body; last_url = client->url;
    std::memcpy(out, response.data(), response.size()); return static_cast<int>(response.size());
}
bool esp_http_client_is_complete_data_received(TestHttp*) { return true; }
bool esp_http_client_is_persistent_connection(TestHttp*) { return true; }
esp_err_t esp_http_client_close(TestHttp*) { return ESP_OK; }
esp_err_t esp_http_client_cleanup(TestHttp* client) { delete client; --live_clients; return ESP_OK; }

int main() {
    CHECK(cloud_api_init("https://platform.example.test") == ESP_OK);
    cloud_claim_t claim = {};
    cloud_api_err_t error = {};
    response = R"({"code":0,"success":true,"data":{"code":"123456","expires_in":600}})";
    CHECK(cloud_api_claim_code(6, "device-uuid", "44:BD:8D:F4:6B:0C", "esp32-s3", "2.0.0", &claim, &error) == ESP_OK);
    CHECK(last_url == "https://platform.example.test/api/device-gateway/claim-code");
    CHECK(last_body == R"({"product_id":6,"device_sn":"device-uuid","mac_address":"44:BD:8D:F4:6B:0C","chip_type":"esp32-s3","firmware_version":"2.0.0"})");
    CHECK(std::strcmp(claim.code, "123456") == 0);
#if CONFIG_IDF_TARGET_ESP32C3
    CHECK(live_clients == 0); // No idle HTTPS/TLS allocation may survive the exchange.
#else
    CHECK(live_clients == 1); // Other profiles retain connection reuse.
#endif

    logs.clear();
    response = R"({"code":1002,"success":false,"message":"product mismatch","data":{"auth_key":"fixture-response-secret"}})";
    CHECK(cloud_api_claim_code(6, "device-uuid", "MAC", "esp32-s3", "2.0.0", &claim, &error) == ESP_ERR_INVALID_RESPONSE);
    CHECK(error.code == 1002 && !error.success);
#if CONFIG_IDF_TARGET_ESP32C3
    CHECK(live_clients == 0); // Business failures also release the transport.
#endif
    CHECK(logs.find("HTTP 200, code=1002, message=product mismatch") != std::string::npos);
    CHECK(logs.find("fixture-response-secret") == std::string::npos);

    logs.clear();
    response = R"({"code":1,"message":"invalid fixture-submitted-key and fixture-stored-key\nretry","data":null})";
    CHECK(cloud_api_auth("fixture-submitted-key", "MAC", "2.0.0", nullptr, 0, &error) == ESP_ERR_INVALID_RESPONSE);
    CHECK(logs.find("code=1, message=invalid [redacted] and [redacted] retry") != std::string::npos);
    CHECK(logs.find("fixture-submitted-key") == std::string::npos && logs.find(stored_key) == std::string::npos);

    logs.clear(); http_status = 403;
    response = R"({"code":1003,"message":"product disabled","success":false})";
    CHECK(cloud_api_heartbeat(stored_key, &error) == ESP_ERR_INVALID_RESPONSE);
    CHECK(logs.find("HTTP 403, code=1003, message=product disabled") != std::string::npos);
    response = R"({"code":0,"success":true,"message":"gateway refusal"})";
    CHECK(cloud_api_heartbeat(stored_key, &error) == ESP_ERR_INVALID_RESPONSE && !error.success);

    logs.clear(); http_status = 200; response = R"({"code":1002,"message":null,"success":false})";
    CHECK(cloud_api_heartbeat(stored_key, &error) == ESP_ERR_INVALID_RESPONSE);
    CHECK(logs.find("code=1002, message=") != std::string::npos);
    char small[8]; cloud_log_safe_message(small, sizeof small, "fixture-stored-key", stored_key);
    CHECK(std::strcmp(small, "[redact") == 0);
    cloud_api_deinit();
    CHECK(live_clients == 0);
    std::puts("PASS: claim-code payload, HTTP/business error messages, key redaction and null messages");
}
