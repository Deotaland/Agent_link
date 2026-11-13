#include "launcher_update_stubs.h"
#include "launcher_update.h"
#include "agent_system.h"
#include "agent_launcher_boot.h"
#include "dynamic_layout_config.h"
#include "launcher_version.h"
#include <fstream>
#include <iterator>

using namespace agent_system;
namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) {
    return ESP_ERR_NOT_FOUND;
}
}  // namespace agent_system::community
namespace {
const std::string origin = "https://bot.deotaland.cn";
const std::string check = origin +
                          "/api/firmware/check-update?product_id=13&current_version_code=" +
                          std::to_string(AGENT_LAUNCHER_VERSION_CODE) + "&chip_type=esp32-s3";
const uint32_t candidate_code = AGENT_LAUNCHER_VERSION_CODE + 1;
const std::string candidate_version = "test." + std::to_string(candidate_code);
const std::string refresh = origin + "/api/firmware/42/download-url";
const std::string direct = origin + "/api/firmware/42/download";
const std::string cdn = "https://cdn.example.com/launcher.bin";
std::string candidate, md5;
void Wait() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (launcher::Busy() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    assert(!launcher::Busy());
}
alboot_record State() {
    auto io = dynamic_fixture::Io();
    alboot_record state;
    int bank;
    assert(alboot_load(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state, &bank) >= 0);
    return state;
}
std::string Release(const char* digest = nullptr, unsigned product = 13, bool official = true) {
    return "{\"code\":0,\"message\":\"\",\"success\":true,\"data\":{\"has_update\":true,\"firmware_"
           "id\":42,\"version\":\"" +
           candidate_version + "\",\"version_code\":" + std::to_string(candidate_code) +
           ",\"product_id\":" + std::to_string(product) +
           ",\"is_official\":" + (official ? "true" : "false") +
           ",\"chip_type\":\"esp32-s3\",\"flash_address\":\"0x0\",\"file_"
           "size\":" +
           std::to_string(candidate.size()) + ",\"file_md5\":\"" + (digest ? digest : md5) +
           "\",\"download_url\":\"https://expired.example.com/file\",\"changelog\":\"UI "
           "improvements\",\"is_forced\":false,\"min_support_version\":null,\"extra_info\":{}}}";
}
void Refresh(const std::string& url) {
    fixture::responses[refresh] =
        "{\"code\":0,\"message\":\"\",\"success\":true,\"data\":{\"id\":42,\"file_size\":" +
        std::to_string(candidate.size()) + ",\"file_md5\":\"" + md5 +
        "\",\"flash_address\":\"0x0\",\"download_url\":\"" + url + "\"}}";
}
void Available(const char* digest = nullptr) {
    fixture::responses[check] = Release(digest);
    assert(launcher::Check() == ESP_OK);
    Wait();
    assert(launcher::Read().state == launcher::State::kAvailable);
}
}  // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    candidate.assign(std::istreambuf_iterator<char>(input), {});
    uint8_t digest[16];
    assert(dynamic_fixture::Md5(candidate.data(), candidate.size(), digest) == 0);
    char text[33];
    for (unsigned i = 0; i < 16; ++i)
        snprintf(text + i * 2, 3, "%02x", digest[i]);
    md5 = text;
    auto io = dynamic_fixture::Io();
    alpt_entry slots[16];
    uint8_t table[4096];
    for (unsigned i = 0; i < 16; ++i)
        alpt_empty(&alpt_config, i, &slots[i]);
    assert(alpt_build(&alpt_config, &io, slots, table) == 0);
    memcpy(dynamic_fixture::flash.data() + alpt_config.table, table, sizeof table);
    dynamic_fixture::Reload();
    fixture::running = esp_partition_find_first(0, 30, nullptr);
    assert(fixture::running && launcher::Init() == ESP_OK);
    assert(launcher::ConfirmHealthy() == ESP_OK);
    memset(dynamic_fixture::flash.data() + ALBOOT_A_OFFSET, 0xA5, ALBOOT_IMAGE_BYTES);
    const auto old =
        std::vector<uint8_t>(dynamic_fixture::flash.begin() + ALBOOT_A_OFFSET,
                             dynamic_fixture::flash.begin() + ALBOOT_A_OFFSET + ALBOOT_IMAGE_BYTES);
    // No update and permission errors are explicit, with no flash writes.
    fixture::responses[check] =
        "{\"code\":0,\"message\":\"\",\"success\":true,\"data\":{\"has_update\":false}}";
    assert(launcher::Check() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kCurrent);
    fixture::http_status[check] = 401;
    fixture::responses[check] = "{\"message\":\"Missing required header\"}";
    assert(launcher::Check() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kError &&
           launcher::Read().http_status == 401);
    fixture::http_status[check] = 200;
    fixture::open_errors[check] = ESP_ERR_HTTP_CONNECT;
    fixture::tls_errors[check] = {MBEDTLS_ERR_X509_FATAL_ERROR, 8};
    assert(launcher::Check() == 0);
    Wait();
    assert(launcher::Read().error == kCertificateError && strstr(launcher::Read().message, "证书"));
    fixture::open_errors.clear();
    const auto writes = dynamic_fixture::mutations;
    for (const auto& response :
         {std::string(
              "{\"code\":403,\"message\":\"not published\",\"data\":{\"has_update\":false}}"),
          std::string("{\"code\":0,\"success\":false,\"message\":\"rejected\",\"data\":{\"has_"
                      "update\":false}}"),
          Release(nullptr, 1), Release(nullptr, 13, false)}) {
        fixture::responses[check] = response;
        assert(launcher::Check() == 0);
        Wait();
        assert(launcher::Read().state == launcher::State::kError);
    }
    assert(dynamic_fixture::mutations == writes);
    Refresh("");
    fixture::http_status[direct] = 302;
    fixture::http_headers[direct]["Location"] = cdn;
    fixture::responses[cdn] = candidate;
    Available();
    const auto before_tls = dynamic_fixture::mutations;
    fixture::open_errors[cdn] = ESP_ERR_HTTP_CONNECT;
    fixture::tls_errors[cdn] = {MBEDTLS_ERR_X509_FATAL_ERROR, 0};
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().error == kCertificateError && State().pending == 255 &&
           dynamic_fixture::mutations == before_tls);
    fixture::open_errors.clear();
    Available("00000000000000000000000000000000");
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255 &&
           State().active == 0);
    // Refreshed metadata must still refer to the release shown to the user.
    Available();
    Refresh("");
    auto& changed = fixture::responses[refresh];
    changed.replace(changed.find("\"id\":42"), 7, "\"id\":43");
    const auto before_refresh = dynamic_fixture::mutations;
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255 &&
           dynamic_fixture::mutations == before_refresh);
    Refresh("");
    // Cancellation before any download publication cannot switch the boot slot.
    Available();
    fixture::pause_open = true;
    fixture::open_entered = false;
    fixture::release_open = false;
    assert(launcher::Install() == 0);
    while (!fixture::open_entered)
        std::this_thread::yield();
    launcher::Cancel();
    fixture::release_open = true;
    Wait();
    fixture::pause_open = false;
    assert(launcher::Read().error == kCancelled && State().pending == 255);
    // A truncated body and a torn flash write must never publish a trial.
    Available();
    fixture::fail_after = 400;
    assert(launcher::Install() == 0);
    Wait();
    fixture::fail_after = SIZE_MAX;
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255);
    Available();
    dynamic_fixture::cut = dynamic_fixture::mutations + 5;
    dynamic_fixture::torn = 17;
    assert(launcher::Install() == 0);
    Wait();
    dynamic_fixture::cut = -1;
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255);
    // A refreshed signed URL is used directly, without following the expired one.
    Available();
    Refresh(cdn);
    fixture::responses[cdn] = std::string(candidate.size(), '\0');
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255);
    fixture::responses[cdn] = candidate;
    Refresh("");
    // Valid download follows the real 302 handler and refreshes the expired URL.
    Available();
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kReady);
    assert(State().pending == 1 && State().active == 0 && !State().attempted);
    assert(std::equal(old.begin(), old.end(), dynamic_fixture::flash.begin() + ALBOOT_A_OFFSET));
    for (const auto& request : fixture::requests) {
        assert(!request.headers.count("Authorization") && !request.headers.count("X-User-Info") &&
               !request.headers.count("X-Token"));
    }
    auto state = State();
    assert(alboot_attempt(&state, 1) == 0);
    assert(alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state) == 0);
    fixture::running = esp_partition_find_first(0, 31, nullptr);
    // Confirmation with the old binary's compiled version must reject the new trial.
    assert(launcher::ConfirmHealthy() == ESP_ERR_INVALID_STATE);
    assert(State().pending == 1 && State().active == 0);
    StopFactoryTasks();
    puts(
        "Production Launcher updater: anonymous endpoints, HTTP errors, URL refresh/302, "
        "cancellation, MD5, ESP image verification, inactive-slot-only writes and version-bound "
        "confirmation passed.");
}
