#include "agent_system_dynamic_stubs.h"
#include "dynamic_layout_config.h"
#include "agent_system.h"
#include "../components/agent_system/community_store.h"
#include "cJSON.h"
#include <filesystem>
#include <fstream>
#include <iostream>

// Image decoding is covered separately against the real upstream codecs. This
// fixture exercises the production store and actual flash installer together.
namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}
}  // namespace agent_system::community
namespace {
using namespace agent_system;
const std::string origin = "https://sugapi.deotaland.cn", list = origin + "/api/v1/community/plays";
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
std::string Text(cJSON* object, const char* key) {
    auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(cJSON_IsString(value));
    return value->valuestring;
}
void Put(uint32_t address, const std::string& bytes) {
    memcpy(dynamic_fixture::flash.data() + address, bytes.data(), bytes.size());
}
void Reboot() {
    uint8_t scratch[4096];
    auto io = dynamic_fixture::Io();
    assert(alpt_restore(&alpt_config, &io, scratch) >= 0);
    dynamic_fixture::Reload();
    assert(Init() == ESP_OK);
}
size_t Count() {
    App apps[16];
    size_t n = 0;
    assert(List(apps, 16, &n) == ESP_OK);
    return n;
}
std::string Envelope(const std::string& data) {
    return "{\"code\":0,\"data\":" + data + "}";
}
std::string Page(unsigned page, bool more,
                 const std::string& items = "[{\"id\":1,\"latestVersion\":{\"id\":10}}]") {
    return Envelope("{\"items\":" + items + ",\"page\":" + std::to_string(page) +
                    ",\"pageSize\":8,\"total\":9,\"hasMore\":" + (more ? "true" : "false") + "}");
}
std::string PageUrl(unsigned page) {
    return list + "?productId=1&hardwareRevisionId=1&page=" + std::to_string(page) +
           "&pageSize=8&sort=featured";
}
std::string Detail(cJSON* file, const char* filename = "app.bin",
                   const char* mime = "application/octet-stream", unsigned product = 1,
                   unsigned version = 10) {
    return Envelope("{\"id\":1,\"title\":\"Test application\",\"publishedVersion\":{\"id\":" +
                    std::to_string(version) +
                    ",\"version\":\"release-2026\",\"product\":{\"id\":" + std::to_string(product) +
                    "},\"hardwareRevision\":{\"id\":1},\"firmware\":{\"filename\":\"" + filename +
                    "\",\"mimeType\":\"" + mime + "\",\"sizeBytes\":" +
                    std::to_string(static_cast<unsigned>(
                        cJSON_GetObjectItemCaseSensitive(file, "size")->valuedouble)) +
                    ",\"sha256\":\"" + Text(file, "sha256") + "\"}}}");
}
std::string Ticket(cJSON* file, const std::string& url) {
    return Envelope("{\"downloadUrl\":\"" + url + "\",\"sizeBytes\":" +
                    std::to_string(static_cast<unsigned>(
                        cJSON_GetObjectItemCaseSensitive(file, "size")->valuedouble)) +
                    ",\"sha256\":\"" + Text(file, "sha256") + "\"}");
}
}  // namespace
int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc == 3);
    const auto artifacts = std::filesystem::u8path(argv[2]);
    Put(0, Read(artifacts / "factory/bootloader/bootloader.bin"));
    Put(0x8000, Read(artifacts / "factory/partition_table/partition-table.bin"));
    Put(0x10000, Read(artifacts / "factory/agent_link.bin"));
    dynamic_fixture::Reload();
    assert(Init() == kRestartRequired);
    Reboot();
    assert(Count() == 0);
    fixture::store["agent_nvs/al_wifi/ssid"] = {'h', 'o', 'm', 'e', 0};
    const auto wifi = fixture::store.at("agent_nvs/al_wifi/ssid");
    char url[256];
    assert(CatalogUrl(url, sizeof url) == ESP_OK &&
           std::string(url) == community::DefaultCatalog());
    fixture::store.erase("agent_nvs/al_system/community_v1");
    std::string old = "http://192.168.1.9:8765/api/apps";
    fixture::store["agent_nvs/al_system/catalog"] = {old.begin(), old.end()};
    fixture::store["agent_nvs/al_system/catalog"].push_back(0);
    assert(CatalogUrl(url, sizeof url) == ESP_OK &&
           std::string(url) == community::DefaultCatalog());
    assert(SetCatalogUrl("") == ESP_OK && CatalogUrl(url, sizeof url) == ESP_OK && !url[0]);
    assert(SetCatalogUrl(old.c_str()) == ESP_OK && CatalogUrl(url, sizeof url) == ESP_OK &&
           url == old);
    assert(SetCatalogUrl(community::DefaultCatalog()) == ESP_OK && CommunityCatalog());
    assert(ValidServiceUrl((origin + "/file?sig=a%2Fb").c_str()));
    assert(!community::IsOrigin("https://sugapi.deotaland.cn.evil/file"));
    assert(!ValidServiceUrl("http://sugapi.deotaland.cn.evil/file"));
    assert(!ValidServiceUrl("http://8.8.8.8/file"));
    assert(!ValidServiceUrl((origin + "/file?sig=%xx").c_str()));
    auto* report = cJSON_Parse(Read(std::filesystem::u8path(argv[1])).c_str());
    assert(report);
    cJSON* native = nullptr;
    cJSON* vendor = nullptr;
    cJSON* entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(report, "apps")) {
        if (Text(entry, "image_format") == "agent-link")
            native = entry;
        else if (!vendor)
            vendor = entry;
    }
    assert(native && vendor);
    const std::string detail = list + "/1", ticket = list + "/1/versions/10/downloads",
                      file_url = origin + "/file?signature=a%2Fb";
    fixture::responses[PageUrl(1)] = Page(1, true);
    fixture::responses[PageUrl(2)] = Page(2, false, "[]");
    fixture::responses[detail] = Detail(vendor, "release.zip", "application/zip");
    CatalogApp apps[8];
    size_t count = 0;
    CatalogPage page;
    assert(FetchCatalogPage(apps, 8, &count, 1, &page) == ESP_OK && count == 1 && page.has_more &&
           page.total == 9);
    assert(apps[0].package == CatalogApp::Package::kFullPackage);
    const auto untouched = dynamic_fixture::flash;
    const auto requests = fixture::requests.size();
    assert(Install(apps[0]) == kUnsupportedPackage && untouched == dynamic_fixture::flash &&
           fixture::requests.size() == requests);
    assert(FetchCatalogPage(apps, 8, &count, 2, &page) == ESP_OK && count == 0 && !page.has_more &&
           page.page == 2);
    fixture::http_status[PageUrl(1)] = 429;
    fixture::http_headers[PageUrl(1)]["Retry-After"] = "17";
    fixture::responses[PageUrl(1)] =
        "{\"code\":\"RATE_LIMIT\",\"message\":\"Please wait\",\"requestId\":\"request-test\"}";
    assert(FetchCatalog(apps, 8, &count) == kStoreError && count == 0);
    StoreError error;
    LastStoreError(&error);
    assert(error.http_status == 429 && error.retry_after == 17 &&
           !strcmp(error.message, "Please wait") && !strcmp(error.request_id, "request-test"));
    fixture::http_status.clear();
    fixture::responses[PageUrl(1)] = Page(1, false);
    fixture::responses[detail] = Detail(vendor, "app.bin", "application/octet-stream", 13);
    assert(FetchCatalog(apps, 8, &count) == kStoreError && untouched == dynamic_fixture::flash);
    fixture::responses[detail] = Detail(vendor, "app.bin", "application/octet-stream", 1, 11);
    assert(FetchCatalog(apps, 8, &count) == kStoreError);
    fixture::responses[detail] = Detail(vendor);
    assert(FetchCatalog(apps, 8, &count) == ESP_OK && count == 1);
    fixture::responses[ticket] = Ticket(native, file_url);  // metadata mismatch before any erase
    assert(Install(apps[0]) == kStoreError && untouched == dynamic_fixture::flash);
    fixture::responses[ticket] = Ticket(vendor, file_url);
    fixture::responses[file_url] =
        "PK" + Read(std::filesystem::u8path(Text(vendor, "download_path"))).substr(2);
    assert(Install(apps[0]) == ESP_ERR_INVALID_VERSION && untouched == dynamic_fixture::flash);
    fixture::responses[file_url] = Read(std::filesystem::u8path(Text(vendor, "download_path")));
    fixture::responses[file_url][700] ^= 1;
    assert(Install(apps[0]) == ESP_ERR_INVALID_CRC && Count() == 0);
    fixture::responses[file_url][700] ^= 1;
    assert(Install(apps[0]) == kRestartRequired);
    Reboot();
    assert(Count() == 1);
    App installed[16];
    size_t n = 0;
    assert(List(installed, 16, &n) == ESP_OK && n == 1);
    assert(installed[0].community_play == 1 && installed[0].community_version == 10);
    assert(!strcmp(installed[0].descriptor.app_id, Text(vendor, "app_id").c_str()));
    assert(!strcmp(installed[0].release_version, "release-2026") &&
           !strcmp(installed[0].version, Text(vendor, "version").c_str()));
    assert(Install(apps[0]) == kAlreadyInstalled);
    assert(Remove(installed[0].slot) == kRestartRequired);
    Reboot();
    assert(Count() == 0);
    fixture::responses[detail] = Detail(native);
    fixture::responses[ticket] = Ticket(native, file_url);
    fixture::responses[file_url] = Read(std::filesystem::u8path(Text(native, "download_path")));
    const auto code_url = origin + "/api/v1/community/download-codes/000001";
    auto code_detail = Detail(native);
    code_detail.insert(code_detail.find("\"id\":1"), "\"downloadCode\":\"000001\",");
    fixture::responses[code_url] = code_detail;
    fixture::responses[code_url + "/downloads"] = Ticket(native, file_url);
    assert(FetchDownloadCode("000001", &apps[0]) == ESP_OK);
    const auto before_timeout = dynamic_fixture::flash;
    fixture::header_results[code_url + "/downloads"] = {
        -ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    InstallStatus ticket_timeout{};
    assert(Install(apps[0], nullptr, nullptr, &ticket_timeout) == ESP_ERR_TIMEOUT);
    assert(ticket_timeout.stage == InstallStage::kTicket && ticket_timeout.received == 0 &&
           before_timeout == dynamic_fixture::flash && Count() == 0);
    fixture::header_results[file_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN,
                                       -ESP_ERR_HTTP_EAGAIN};
    InstallStatus timeout_status{};
    assert(Install(apps[0], nullptr, nullptr, &timeout_status) == ESP_ERR_TIMEOUT);
    assert(timeout_status.stage == InstallStage::kConnect && timeout_status.received == 0 &&
           before_timeout == dynamic_fixture::flash && Count() == 0);
    fixture::header_results[code_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    fixture::header_results[code_url + "/downloads"] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    fixture::header_results[file_url] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN};
    fixture::header_delay_ms[code_url] = fixture::header_delay_ms[code_url + "/downloads"] = 40000;
    assert(Install(apps[0]) == kRestartRequired);
    Reboot();
    assert(List(installed, 16, &n) == ESP_OK && n == 1 &&
           installed[0].format == ImageFormat::kAgentLink && installed[0].community_play == 1);
    assert(fixture::store.at("agent_nvs/al_wifi/ssid") == wifi);
    std::string previous_key;
    std::vector<std::string> used_keys;
    bool previous_was_post = false;
    std::string previous_url;
    for (const auto& request : fixture::requests) {
        assert(!request.headers.count("Authorization") && !request.headers.count("X-User-Info") &&
               !request.headers.count("X-Auth-Key"));
        if (request.method == HTTP_METHOD_POST) {
            const auto key = request.headers.at("Idempotency-Key");
            assert(!key.empty());
            if (key == previous_key)
                assert(previous_was_post && previous_url == request.url);
            else {
                assert(std::find(used_keys.begin(), used_keys.end(), key) == used_keys.end());
                used_keys.push_back(key);
            }
            previous_key = key;
        }
        previous_was_post = request.method == HTTP_METHOD_POST;
        previous_url = request.url;
    }
    fixture::http_status[origin + "/redirect"] = 302;
    fixture::http_headers[origin + "/redirect"]["Location"] = file_url;
    {
        community::File file;
        assert(file.Open((origin + "/redirect").c_str()) == ESP_OK);
    }
    fixture::http_status["https://example.com/downgrade"] = 302;
    fixture::http_headers["https://example.com/downgrade"]["Location"] = "http://sugapi.deotaland.cn/file";
    {
        community::File file;
        assert(file.Open("https://example.com/downgrade") == ESP_ERR_INVALID_ARG);
    }
    cJSON_Delete(report);
    fixture::http_status[origin + "/expired"] = 403;
    fixture::responses[origin + "/expired"] = "{\"code\":\"EXPIRED\",\"message\":\"Get a fresh ticket\",\"requestId\":\"file-test\"}";
    {
        community::File file;
        assert(file.Open((origin + "/expired").c_str()) == kStoreError);
        LastStoreError(&error);
        assert(error.http_status == 403 && !strcmp(error.message, "Get a fresh ticket") && !strcmp(error.request_id, "file-test"));
    }
    std::cout << "Community store: anonymous pagination, package rejection, errors, tickets, "
                 "native/vendor installs and isolated settings passed\n";
}
