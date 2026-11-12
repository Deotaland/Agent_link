#include "agent_system_dynamic_stubs.h"
#include "agent_system.h"
#include "../components/agent_system/community_store.h"
#include <iostream>

namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) { return ESP_ERR_NOT_SUPPORTED; }
}
int main() {
    using namespace agent_system;
    const std::string origin = "https://sugapi.deotaland.cn";
    const std::string lookup = origin + "/api/v1/community/download-codes/000001";
    const std::string hash(64, 'a');
    const auto detail = [&](unsigned play = 2, unsigned version = 6, unsigned product = 1,
                            unsigned revision = 1, const char* code = "000001") {
        return "{\"code\":0,\"data\":{\"id\":" + std::to_string(play) +
            ",\"downloadCode\":\"" + code + "\",\"title\":\"Radio\",\"coverUrl\":"
            "\"http://sugapi.deotaland.cn/cover.jpg\",\"publishedVersion\":{\"id\":" +
            std::to_string(version) + ",\"version\":\"0.1.1\",\"product\":{\"id\":" +
            std::to_string(product) + "},\"hardwareRevision\":{\"id\":" +
            std::to_string(revision) + "},\"firmware\":{\"filename\":\"app.bin\","
            "\"mimeType\":\"application/octet-stream\",\"sizeBytes\":3384016,\"sha256\":\"" + hash + "\"}}}}";
    };
    const auto ticket = [&](const std::string& digest, unsigned size = 3384016) {
        return "{\"code\":0,\"data\":{\"downloadUrl\":\"http://sugapi.deotaland.cn/file\","
            "\"sizeBytes\":" + std::to_string(size) + ",\"sha256\":\"" + digest + "\"}}";
    };
    CatalogApp app{};
    for (const char* code : {"", "12345", "1234567", "12/456", "12a456"})
        assert(FetchDownloadCode(code, &app) == ESP_ERR_INVALID_ARG);
    assert(FetchDownloadCode(nullptr, &app) == ESP_ERR_INVALID_ARG);
    assert(FetchDownloadCode("000001", nullptr) == ESP_ERR_INVALID_ARG);
    assert(fixture::requests.empty());
    fixture::responses[lookup] = detail();
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    assert(app.community_play == 2 && app.community_version == 6 && app.size == 3384016);
    assert(!strcmp(app.download_code, "000001") && !strcmp(app.name, "Radio"));
    assert(!strcmp(app.cover_url, "https://sugapi.deotaland.cn/cover.jpg"));
    assert(fixture::requests.size() == 1 && fixture::requests[0].method == HTTP_METHOD_GET);
    fixture::responses[lookup + "/downloads"] = ticket(hash);
    assert(community::PrepareDownload(&app) == ESP_OK);
    assert(!strcmp(app.url, "https://sugapi.deotaland.cn/file"));
    assert(fixture::requests.back().method == HTTP_METHOD_POST);
    const auto before = fixture::requests.size();
    for (const auto& changed : {detail(3), detail(2, 7), detail(2, 6, 2),
                               detail(2, 6, 1, 2), detail(2, 6, 1, 1, "000002")}) {
        fixture::responses[lookup] = changed;
        assert(community::PrepareDownload(&app) == kStoreError);
        assert(fixture::requests.back().method == HTTP_METHOD_GET);
    }
    assert(fixture::requests.size() == before + 5);
    fixture::responses[lookup] = detail();
    fixture::responses[lookup + "/downloads"] = ticket(std::string(64, 'b'));
    assert(community::PrepareDownload(&app) == kStoreError);
    fixture::responses[lookup + "/downloads"] = ticket(hash, 3384017);
    assert(community::PrepareDownload(&app) == kStoreError);
    // Reproduce the real service's ~40-second POST without sleeping: a 15-second
    // client times out at this boundary, while the revised client completes.
    const auto post_url = lookup + "/downloads";
    fixture::responses[post_url] = ticket(hash);
    fixture::header_timeouts.clear(); fixture::read_timeouts.clear();
    fixture::header_delay_ms[lookup] = fixture::header_delay_ms[post_url] = 40000;
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    assert(community::PrepareDownload(&app) == ESP_OK);
    assert(fixture::header_timeouts[lookup] == std::vector<int>({60000,60000}));
    assert(fixture::header_timeouts[post_url] == std::vector<int>({60000}));
    for (const auto& url : {lookup, post_url})
        for (int timeout : fixture::read_timeouts[url]) assert(timeout == 15000);
    fixture::header_delay_ms.clear();
    const auto check_posts = [&](size_t start, unsigned expected) {
        std::string key;
        unsigned count = 0;
        for (size_t i = start; i < fixture::requests.size(); ++i) {
            const auto& request = fixture::requests[i];
            if (request.method != HTTP_METHOD_POST) continue;
            assert(request.url == post_url);
            if (key.empty()) key = request.headers.at("Idempotency-Key");
            assert(request.headers.at("Idempotency-Key") == key && !key.empty());
            ++count;
        }
        assert(count == expected);
        return key;
    };
    size_t retry_start = fixture::requests.size();
    fixture::header_results[post_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    assert(community::PrepareDownload(&app) == ESP_OK);
    const auto first_key = check_posts(retry_start, 3);
    retry_start = fixture::requests.size();
    fixture::header_results[post_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN,
                                       -ESP_ERR_HTTP_EAGAIN};
    assert(community::PrepareDownload(&app) == ESP_ERR_TIMEOUT);
    assert(check_posts(retry_start, 3) != first_key); // next logical install gets a new key
    fixture::header_results.clear();
    retry_start = fixture::requests.size();
    fixture::read_results[post_url] = {-ESP_ERR_HTTP_EAGAIN};
    assert(community::PrepareDownload(&app) == ESP_OK);
    check_posts(retry_start, 2); // response-body timeout also reuses the operation's key
    fixture::read_results.clear();
    fixture::header_results[lookup] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    fixture::header_results[lookup] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN,
                                     -ESP_ERR_HTTP_EAGAIN};
    assert(FetchDownloadCode("000001", &app) == ESP_ERR_TIMEOUT && !app.download_code[0]);
    fixture::header_results.clear();
    assert(FetchDownloadCode("000001", &app) == ESP_OK);
    retry_start = fixture::requests.size();
    fixture::responses[post_url] = "{\"code\":0,\"data\":null}";
    assert(community::PrepareDownload(&app) == kStoreError);
    check_posts(retry_start, 1); // complete but invalid metadata is not retried
    fixture::responses[post_url] = "{\"code\":\"BUSY\",\"retryAfterSeconds\":30}";
    fixture::http_status[post_url] = 503;
    retry_start = fixture::requests.size();
    assert(community::PrepareDownload(&app) == kStoreError);
    check_posts(retry_start, 1); // do not ignore the server's long cooldown
    fixture::http_headers[post_url]["Retry-After"] = "30";
    fixture::read_results[post_url] = {-ESP_ERR_HTTP_EAGAIN};
    retry_start = fixture::requests.size();
    assert(community::PrepareDownload(&app) == ESP_ERR_TIMEOUT);
    check_posts(retry_start, 1); // cooldown still applies if the error body times out
    fixture::http_headers.erase(post_url);
    fixture::http_status.erase(post_url);
    fixture::responses[post_url] = ticket(hash);
    retry_start = fixture::requests.size();
    assert(community::PrepareDownload(&app, [](void* context) {
        return fixture::requests.size() > *static_cast<size_t*>(context) &&
               fixture::requests.back().method == HTTP_METHOD_POST;
    }, &retry_start) == kCancelled);
    check_posts(retry_start, 1);
    assert(fixture::active_http_clients == 0);
    fixture::http_status[lookup] = 404;
    fixture::responses[lookup] = "{\"code\":\"NOT_FOUND\",\"message\":\"No available play\",\"requestId\":\"code-test\"}";
    assert(FetchDownloadCode("000001", &app) == kStoreError && !app.download_code[0]);
    StoreError error{}; LastStoreError(&error);
    assert(error.http_status == 404 && !strcmp(error.code, "NOT_FOUND") &&
           !strcmp(error.request_id, "code-test"));
    for (const auto& request : fixture::requests) {
        assert(request.url.find("/download-codes/000001") != std::string::npos);
        assert(!request.headers.count("Authorization") && !request.headers.count("X-User-Info"));
        if (request.method == HTTP_METHOD_POST) assert(request.headers.count("Idempotency-Key"));
    }
    const std::string old = "http://3.144.1.190:9004/api/v1/community/plays?productId=1";
    fixture::store["agent_nvs/al_system/catalog"] = {old.begin(), old.end()};
    fixture::store["agent_nvs/al_system/catalog"].push_back(0);
    char catalog[256]{};
    assert(CatalogUrl(catalog, sizeof catalog) == ESP_OK && catalog == std::string(community::DefaultCatalog()));
    assert(SetCatalogUrl("") == ESP_OK && CatalogUrl(catalog, sizeof catalog) == ESP_OK && !catalog[0]);
    assert(SetCatalogUrl("http://192.168.1.9:8765/api/apps") == ESP_OK);
    assert(CatalogUrl(catalog, sizeof catalog) == ESP_OK && !strcmp(catalog, "http://192.168.1.9:8765/api/apps"));
    // Inject slow headers at the transport boundary: no real sleeps or flash writes.
    const auto file_url = origin + "/retry-file";
    fixture::responses[file_url] = "firmware bytes";
    fixture::header_results[file_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    const int clients_before = fixture::active_http_clients;
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == ESP_OK);
        assert(fixture::header_timeouts[file_url] == std::vector<int>({60000,60000,60000}));
        assert(file.handle->timeout_ms == 15000 && file.handle->offset == 0);
    }
    assert(fixture::active_http_clients == clients_before);
    fixture::header_timeouts.clear();
    fixture::header_results[file_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN,
                                       -ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == ESP_ERR_TIMEOUT);
        assert(fixture::header_timeouts[file_url].size() == 3);
        assert(fixture::header_results[file_url].size() == 1); // bounded retries
    }
    fixture::header_timeouts.clear(); fixture::header_results.clear();
    fixture::header_results[file_url] = {ESP_FAIL};
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == ESP_ERR_INVALID_RESPONSE);
        assert(fixture::header_timeouts[file_url].size() == 1);
    }
    fixture::header_timeouts.clear(); fixture::header_results.clear();
    fixture::http_status[file_url] = 403;
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == kStoreError);
        assert(fixture::header_timeouts[file_url].size() == 1); // expired tickets are not retried
    }
    fixture::header_timeouts.clear();
    fixture::http_status[file_url] = 503;
    fixture::http_headers[file_url]["Retry-After"] = "1";
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == kStoreError);
        assert(fixture::header_timeouts[file_url].size() == 3);
    }
    fixture::header_timeouts.clear();
    fixture::http_headers[file_url]["Retry-After"] = "30";
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str()) == kStoreError);
        assert(fixture::header_timeouts[file_url].size() == 1); // respect long server cooldown
    }
    fixture::header_timeouts.clear(); fixture::http_status.clear(); fixture::http_headers.clear();
    fixture::header_results[file_url] = {-ESP_ERR_HTTP_EAGAIN};
    const auto requests_before_cancel = fixture::requests.size();
    unsigned cancel_checks = 0;
    {
        community::File file;
        assert(file.OpenDownload(file_url.c_str(), [](void* context) {
            return ++*static_cast<unsigned*>(context) >= 3;
        }, &cancel_checks) == kCancelled);
        assert(fixture::requests.size() == requests_before_cancel + 1);
    }
    fixture::header_results.clear(); fixture::header_timeouts.clear();
    {
        community::File file;
        assert(file.Open(file_url.c_str()) == ESP_OK);
        assert(fixture::header_timeouts[file_url] == std::vector<int>({15000}));
    }
    assert(fixture::active_http_clients == clients_before);
    std::cout << "Download codes: leading zeroes, exact endpoints, version/hardware checks, ticket integrity, HTTPS files, errors and migration passed\n";
}
