#include "agent_system_dynamic_stubs.h"
#include "agent_system.h"
#include "../components/agent_system/community_store.h"
#include "cJSON.h"
#include <iostream>

namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}
}  // namespace agent_system::community
namespace {
const std::string lookup = "https://bot.deotaland.cn/api/firmware/code/000001/download-url";
const std::string md5(32, 'a');
std::string Detail() {
    return "{\"code\":0,\"success\":true,\"data\":{\"id\":19,\"download_code\":\"000001\","
           "\"file_name\":\"radio.bin\",\"file_size\":3384016,\"file_md5\":\"" +
           md5 +
           "\",\"flash_address\":\"0x390000\",\"download_url\":\"https://cdn.example.com/"
           "app.bin?signature=first\","
           "\"firmware\":{\"id\":19,\"download_code\":\"000001\",\"product_id\":18,"
           "\"version\":\"1.0.0\",\"version_code\":1,\"chip_type\":\"esp32-s3\","
           "\"firmware_type\":\"ota\",\"file_name\":\"radio.bin\",\"file_size\":3384016,"
           "\"file_md5\":\"" +
           md5 +
           "\",\"is_public\":true,\"is_published\":true,"
           "\"review_status\":\"approved\"},\"product\":{\"id\":18,\"hardware_id\":2,"
           "\"name\":\"Radio\",\"chip_type\":\"esp32-s3\",\"icon_url\":\"/cover.png\"}}}";
}
std::string Changed(const char* section, const char* key, const char* json) {
    auto* root = cJSON_Parse(Detail().c_str());
    assert(root);
    auto* object = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (section)
        object = cJSON_GetObjectItemCaseSensitive(object, section);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(object, key, cJSON_Parse(json)));
    auto* encoded = cJSON_PrintUnformatted(root);
    assert(encoded);
    std::string result(encoded);
    cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}
}  // namespace
int main() {
    using namespace agent_system;
    CatalogApp app{};
    for (const char* code : {"", "12345", "1234567", "12/456", "12a456"})
        assert(FetchDownloadCode(code, &app) == ESP_ERR_INVALID_ARG);
    assert(FetchDownloadCode(nullptr, &app) == ESP_ERR_INVALID_ARG);
    assert(FetchDownloadCode("000001", nullptr) == ESP_ERR_INVALID_ARG);
    assert(fixture::requests.empty());
    fixture::responses[lookup] = Detail();
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    assert(app.firmware_id == 19 && app.firmware_version_code == 1 && !app.community_play);
    assert(!strcmp(app.name, "Radio") && !strcmp(app.version, "1.0.0"));
    assert(!strcmp(app.download_code, "000001") && !strcmp(app.file_md5, md5.c_str()));
    assert(!app.sha256[0] && !strcmp(app.cover_url, "https://bot.deotaland.cn/cover.png"));
    // Another product on the same Sugarat hardware is installable; its ID need not be 13.
    assert(app.package == CatalogApp::Package::kApplication);
    fixture::responses[lookup] =
        Changed(nullptr, "download_url", "\"https://cdn.example.com/app.bin?signature=refreshed\"");
    assert(community::PrepareDownload(&app) == ESP_OK);
    assert(strstr(app.url, "signature=refreshed"));
    for (const auto& response :
         {Changed(nullptr, "download_code", "\"000002\""), Changed(nullptr, "id", "20"),
          Changed(nullptr, "file_size", "3384017"), Changed(nullptr, "file_md5", "\"bad\""),
          Changed(nullptr, "download_url", "\"file:///app.bin\""),
          Changed("firmware", "download_code", "\"000002\""),
          Changed("firmware", "product_id", "13"), Changed("product", "hardware_id", "3"),
          Changed("firmware", "chip_type", "\"esp32-c3\""),
          Changed("firmware", "is_public", "false"), Changed("firmware", "is_published", "false"),
          Changed("firmware", "review_status", "\"pending\""),
          Changed("firmware", "version_code", "2"), Changed("firmware", "version", "\"1.0.1\"")}) {
        fixture::responses[lookup] = response;
        assert(community::PrepareDownload(&app) == kStoreError);
    }
    for (const auto& response : {Changed("firmware", "firmware_type", "\"full\""),
                                 Changed("firmware", "firmware_type", "\"factory\"")}) {
        fixture::responses[lookup] = response;
        assert(community::PrepareDownload(&app) == kUnsupportedPackage);
    }
    fixture::responses[lookup] = Detail();
    fixture::header_delay_ms[lookup] = 40000;
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    fixture::header_delay_ms.clear();
    fixture::header_results[lookup] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    assert(community::PrepareDownload(&app) == ESP_OK);
    fixture::header_results[lookup] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN,
                                       -ESP_ERR_HTTP_EAGAIN};
    assert(FetchDownloadCode("000001", &app) == ESP_ERR_TIMEOUT && !app.download_code[0]);
    fixture::header_results.clear();
    fixture::responses[lookup] =
        "{\"code\":1002201,\"success\":false,\"message\":\"Firmware not published\"}";
    assert(FetchDownloadCode("000001", &app) == kStoreError);
    StoreError error;
    LastStoreError(&error);
    assert(error.http_status == 200 && !strcmp(error.code, "1002201") &&
           !strcmp(error.message, "Firmware not published"));
    fixture::responses[lookup] = "{\"code\":0,\"success\":false,\"data\":{}}";
    assert(FetchDownloadCode("000001", &app) == kStoreError);
    fixture::responses[lookup] = Detail();
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    unsigned cancel_checks = 0;
    assert(community::PrepareDownload(
               &app, [](void* context) { return ++*static_cast<unsigned*>(context) >= 2; },
               &cancel_checks) == kCancelled);
    for (const auto& request : fixture::requests) {
        assert(request.url == lookup && request.method == HTTP_METHOD_GET);
        assert(!request.headers.count("Authorization") && !request.headers.count("X-User-Info") &&
               !request.headers.count("X-Token") && !request.headers.count("Cookie") &&
               !request.headers.count("Idempotency-Key"));
    }
    // Trust, hostname or validity failures stop the GET immediately, including
    // bundle callback failures which report -0x3000 without verification flags.
    for (const auto& tls : {std::pair<int, int>{MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, 8},
                            std::pair<int, int>{MBEDTLS_ERR_X509_FATAL_ERROR, 0}}) {
        fixture::open_errors[lookup] = ESP_ERR_HTTP_CONNECT;
        fixture::tls_errors[lookup] = tls;
        const auto requests = fixture::requests.size();
        assert(FetchDownloadCode("000001", &app) == kCertificateError);
        assert(fixture::requests.size() == requests + 1);
        LastStoreError(&error);
        assert(!strcmp(error.code, "TLS_CERT_VERIFY_FAILED") && error.message[0]);
    }
    fixture::open_errors.clear();
    const std::string file_url = "https://cdn.example.com/retry.bin";
    fixture::responses[file_url] = "file bytes";
    // Catalog/cover requests must not alter WiFi power saving.
    {
        community::File file;
        assert(file.Open(file_url.c_str()) == ESP_OK);
    }
    assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM && wifi_fixture::changes.empty());
    fixture::open_errors[file_url] = ESP_ERR_HTTP_CONNECT;
    fixture::tls_errors[file_url] = {MBEDTLS_ERR_X509_FATAL_ERROR, 0};
    auto requests = fixture::requests.size();
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == kCertificateError);
        assert(wifi_fixture::power_save == WIFI_PS_NONE);
    }
    assert(fixture::requests.size() == requests + 1);
    assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM);
    assert((wifi_fixture::changes == std::vector<wifi_ps_type_t>{WIFI_PS_NONE, WIFI_PS_MIN_MODEM}));
    // An ordinary transport failure still gets bounded retries.
    requests = fixture::requests.size();
    wifi_fixture::changes.clear();
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == ESP_ERR_HTTP_CONNECT);
    }
    assert(fixture::requests.size() == requests + 3);
    assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM && wifi_fixture::changes.size() == 2);
    fixture::open_errors.clear();
    // Keep the guard active through body reads; restore even when a read fails.
    for (const int bytes : {4, -1}) {
        fixture::read_results[file_url] = {bytes};
        {
            community::File file;
            assert(file.OpenDownload(file_url.c_str()) == ESP_OK);
            char buffer[4];
            assert(esp_http_client_read(file.handle, buffer, sizeof buffer) == bytes);
            assert(wifi_fixture::power_save == WIFI_PS_NONE);
        }
        assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM);
    }
    fixture::read_results.clear();
    // Cancellation after opening still restores the exact prior (MAX) mode.
    wifi_fixture::power_save = WIFI_PS_MAX_MODEM;
    int opens_before = fixture::opens;
    {
        community::File file;
        assert(file.OpenDownload(
                   file_url.c_str(),
                   [](void* before) { return fixture::opens > *static_cast<int*>(before); },
                   &opens_before) == kCancelled);
    }
    assert(wifi_fixture::power_save == WIFI_PS_MAX_MODEM);
    // Already-awake WiFi and pre-cancelled work must not cause mode changes.
    wifi_fixture::power_save = WIFI_PS_NONE;
    wifi_fixture::changes.clear();
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == ESP_OK);
    }
    assert(wifi_fixture::power_save == WIFI_PS_NONE && wifi_fixture::changes.empty());
    wifi_fixture::power_save = WIFI_PS_MIN_MODEM;
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str(), [](void*) { return true; }) == kCancelled);
    }
    assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM && wifi_fixture::changes.empty());
    // Missing WiFi state or a failed power change does not block the HTTP result.
    for (bool query_failed : {true, false}) {
        wifi_fixture::query_error = query_failed ? ESP_FAIL : ESP_OK;
        wifi_fixture::set_error = ESP_FAIL;
        wifi_fixture::changes.clear();
        {
            community::File file;
            assert(file.OpenDownload(file_url.c_str()) == ESP_OK);
        }
        assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM);
        assert(wifi_fixture::changes.size() == (query_failed ? 0u : 1u));
    }
    wifi_fixture::query_error = wifi_fixture::set_error = ESP_OK;
    assert(fixture::active_http_clients == 0);
    std::cout
        << "Firmware codes: anonymous GET, leading zeroes, nested metadata, hardware/products, "
           "MD5, refreshed URLs, changed releases, errors, retries, cancellation and WiFi power "
           "restoration passed.\n";
}
