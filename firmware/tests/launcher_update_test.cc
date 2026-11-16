#include "launcher_update_stubs.h"
#include "wifi_download_stubs.h"
#include "launcher_update.h"
#include "agent_system.h"
#include "agent_launcher_boot.h"
#include "dynamic_layout_config.h"
#include "launcher_version.h"
#include <fstream>
#include <iterator>

namespace agent_system {
std::mutex& OperationMutex();
}
using namespace agent_system;
namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) {
    return ESP_ERR_NOT_FOUND;
}
}  // namespace agent_system::community
namespace {
const std::string origin = "https://bot.deotaland.cn";
const std::string check =
    origin + "/api/firmware/check-update?product_id=25&current_version_code=" +
    std::to_string(AGENT_LAUNCHER_VERSION_CODE) + "&chip_type=esp32-s3&firmware_type=ota";
uint32_t candidate_code;
std::string candidate_version;
const std::string redirect = "https://cdn.example.com/redirect";
const std::string cdn = "https://cdn.example.com/launcher.bin";
std::string candidate, md5;
std::string snapshot_path;
void Snapshot(bool save) {
    if (save) {
        std::ofstream out(snapshot_path, std::ios::binary);
        uint32_t count = uint32_t(fixture::store.size());
        out.write(reinterpret_cast<const char*>(&count), sizeof count);
        for (const auto& [key, bytes] : fixture::store) {
            uint32_t n = uint32_t(key.size()), m = uint32_t(bytes.size());
            out.write(reinterpret_cast<const char*>(&n), sizeof n);
            out.write(key.data(), n);
            out.write(reinterpret_cast<const char*>(&m), sizeof m);
            out.write(reinterpret_cast<const char*>(bytes.data()), m);
        }
        out.write(reinterpret_cast<const char*>(dynamic_fixture::flash.data() + ALBOOT_B_OFFSET),
                  ALBOOT_IMAGE_BYTES);
        out.write(
            reinterpret_cast<const char*>(dynamic_fixture::flash.data() + ALBOOT_STATE_OFFSET),
            0x2000);
        assert(out);
    } else {
        std::ifstream in(snapshot_path, std::ios::binary);
        uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof count);
        assert(in && count < 100);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n = 0, m = 0;
            in.read(reinterpret_cast<char*>(&n), sizeof n);
            assert(n < 256);
            std::string key(n, '\0');
            in.read(key.data(), n);
            in.read(reinterpret_cast<char*>(&m), sizeof m);
            assert(m < 8192);
            std::vector<uint8_t> bytes(m);
            in.read(reinterpret_cast<char*>(bytes.data()), m);
            fixture::store[key] = std::move(bytes);
        }
        in.read(reinterpret_cast<char*>(dynamic_fixture::flash.data() + ALBOOT_B_OFFSET),
                ALBOOT_IMAGE_BYTES);
        in.read(reinterpret_cast<char*>(dynamic_fixture::flash.data() + ALBOOT_STATE_OFFSET),
                0x2000);
        assert(in);
    }
}
void Wait() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (launcher::Busy() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    assert(!launcher::Busy());
    assert(wifi_fixture::power_save == WIFI_PS_MIN_MODEM);
}
alboot_record State() {
    auto io = dynamic_fixture::Io();
    alboot_record state;
    int bank;
    assert(alboot_load(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state, &bank) >= 0);
    return state;
}
std::string Release(const char* digest = nullptr, unsigned product = 25, bool official = true,
                    uint32_t bytes = 0,
                    const std::string& url = "https://expired.example.com/file") {
    return "{\"code\":0,\"message\":\"\",\"success\":true,\"data\":{\"has_update\":true,\"firmware_"
           "id\":42,\"version\":\"" +
           candidate_version + "\",\"version_code\":" + std::to_string(candidate_code) +
           ",\"product_id\":" + std::to_string(product) +
           ",\"is_official\":" + (official ? "true" : "false") +
           ",\"firmware_type\":\"ota\",\"chip_type\":\"esp32-s3\",\"flash_address\":\"0x90000\","
           "\"file_"
           "size\":" +
           std::to_string(bytes ? bytes : candidate.size()) + ",\"file_md5\":\"" +
           (digest ? digest : md5) + "\",\"download_url\":\"" + url +
           "\",\"changelog\":\"UI "
           "improvements\",\"is_forced\":false,\"min_support_version\":null,\"extra_info\":{}}}";
}
void Refresh(const std::string& url = cdn, const char* digest = nullptr) {
    fixture::responses[check] = Release(digest, 25, true, 0, url);
}
std::string Changed(const std::string& field, const std::string& replacement) {
    auto response = Release(nullptr, 25, true, 0, cdn);
    const auto at = response.find(field);
    assert(at != std::string::npos);
    response.replace(at, field.size(), replacement);
    return response;
}
void Available(const char* digest = nullptr) {
    fixture::responses[check] = Release(digest);
    assert(launcher::Check() == ESP_OK);
    Wait();
    assert(launcher::Read().state == launcher::State::kAvailable);
}
}  // namespace
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const std::string mode = argc == 3 ? argv[2] : "";
    snapshot_path = std::string(argv[1]) + ".resume";
    if (mode == "--rollback")
        snapshot_path += ".rollback";
    if (mode == "--promote" || mode == "--offline-ready")
        snapshot_path += ".ready";
    std::ifstream input(argv[1], std::ios::binary);
    candidate.assign(std::istreambuf_iterator<char>(input), {});
    assert(candidate.size() >= 512);
    alboot_capability capability;
    memcpy(&capability, candidate.data() + 0x1E4, sizeof capability);
    candidate_code = capability.version_code;
    assert(capability.product_id == 25 && candidate_code > AGENT_LAUNCHER_VERSION_CODE);
    assert(memchr(candidate.data() + 0x30, 0, 32));
    candidate_version = candidate.data() + 0x30;
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
    if (mode == "--restore" || mode == "--rollback" || mode == "--promote" ||
        mode == "--offline-ready")
        Snapshot(false);
    assert(fixture::running && launcher::Init() == ESP_OK);
    assert(launcher::ConfirmHealthy() == ESP_OK);
    memset(dynamic_fixture::flash.data() + ALBOOT_A_OFFSET, 0xA5, ALBOOT_IMAGE_BYTES);
    const auto old =
        std::vector<uint8_t>(dynamic_fixture::flash.begin() + ALBOOT_A_OFFSET,
                             dynamic_fixture::flash.begin() + ALBOOT_A_OFFSET + ALBOOT_IMAGE_BYTES);
    if (mode == "--offline" || mode == "--offline-ready") {
        const bool ready = mode == "--offline-ready";
        fixture::open_errors[check] = ESP_ERR_HTTP_CONNECT;
        launcher::Tick(true, false);
        Wait();
        assert(launcher::Read().state ==
               (ready ? launcher::State::kReady : launcher::State::kPaused));
        assert(State().pending == 255 && fixture::restarts == 0);
        fixture::open_errors.clear();
        fixture::responses[check] = Release(nullptr, 25, true, 0, cdn);
        fixture::responses[cdn] = candidate;
        const auto requests = fixture::requests.size();
        fixture::timer_offset_us += 31000000;
        launcher::Tick(true, true);
        Wait();
        assert(launcher::Read().state == launcher::State::kReady && State().pending == 255);
        assert(ready ? fixture::requests.size() == requests : fixture::requests.size() > requests);
        StopFactoryTasks();
        puts(ready ? "A failed startup check preserves the already-staged offline update."
                   : "A failed startup check retries automatically when the network recovers.");
        return 0;
    }
    if (!mode.empty()) {
        fixture::responses[check] = Release(nullptr, 25, true, 0, cdn);
        fixture::responses[cdn] = candidate;
        fixture::header_delay_ms[cdn] = 5000;  // Slow TLS/header setup must not break background downloads.
        if (mode == "--forced" || mode == "--rollback" || mode == "--promote") {
            auto& json = fixture::responses[check];
            const auto at = json.find("\"is_forced\":false");
            assert(at != std::string::npos);
            json.replace(at, std::string("\"is_forced\":false").size(), "\"is_forced\":true");
            assert(launcher::SetAutoDownload(false) == ESP_OK);
        }
        const auto restored = launcher::Read();
        if (mode == "--restore")
            assert(restored.state == launcher::State::kPaused && restored.received >= 65536);
        launcher::Tick(true, false);
        Wait();
        if (mode == "--rollback") {
            assert(launcher::Read().state == launcher::State::kError && State().pending == 255);
            const auto requests = fixture::requests.size();
            fixture::timer_offset_us += 31000000;
            launcher::Tick(true, true);
            assert(!launcher::Busy() && fixture::requests.size() == requests);
            StopFactoryTasks();
            puts("Failed required release is quarantined; no repeated auto-update boot loop.");
            return 0;
        }
        assert(launcher::Read().state == launcher::State::kAvailable);
        launcher::Tick(true, true);
        Wait();
        assert(launcher::Read().state == launcher::State::kReady && fixture::restarts == 0);
        if (mode == "--promote") {
            assert(launcher::Read().forced && State().pending == 1);
            for (const auto& request : fixture::requests) assert(request.url == check);
            puts("An already-staged optional image becomes required without downloading it again.");
        } else if (mode == "--forced") {
            assert(!launcher::AutoDownloadEnabled() && launcher::Read().forced &&
                   State().pending == 1);
            auto failed = State();
            assert(alboot_attempt(&failed, 1) == 0);
            assert(alboot_attempt(&failed, 1) != 0 && failed.pending == 255);
            assert(alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &failed) == 0);
            snapshot_path += ".rollback";
            Snapshot(true);
            puts(
                "Required update: auto preference overridden, verified inactive image armed, no "
                "immediate restart.");
        } else {
            assert(State().pending == 255);
            bool resumed = false;
            for (const auto& request : fixture::requests)
                resumed |= request.headers.count("Range") != 0;
            assert(resumed);
            puts(
                "Fresh process restored persistent checkpoint and resumed the exact image via HTTP "
                "Range.");
        }
        StopFactoryTasks();
        return 0;
    }
    // Startup checks once per boot, even with auto-download disabled later.
    assert(launcher::AutoDownloadEnabled());
    fixture::responses[check] = "{\"code\":0,\"success\":true,\"data\":{\"has_update\":false}}";
    const auto initial_requests = fixture::requests.size();
    launcher::Tick(false, true);
    assert(!launcher::Busy() && fixture::requests.size() == initial_requests);
    launcher::Tick(true, false);
    Wait();
    assert(fixture::requests.size() == initial_requests + 1 && launcher_worker_priority == 1);
    launcher::Tick(false, false);
    launcher::Tick(true, false);
    assert(fixture::requests.size() == initial_requests + 1);
    assert(launcher::SetAutoDownload(false) == ESP_OK && !launcher::AutoDownloadEnabled());
    nvs_handle_t settings;
    uint8_t enabled = 1;
    assert(nvs_open_from_partition(SettingsPartition(), "launcher_update", NVS_READONLY,
                                   &settings) == ESP_OK);
    assert(nvs_get_u8(settings, "auto_dl", &enabled) == ESP_OK && enabled == 0);
    nvs_close(settings);
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
    assert(launcher::Read().error == kCertificateError &&
           strstr(launcher::Read().message, "certificate"));
    fixture::open_errors.clear();
    const auto writes = dynamic_fixture::mutations;
    for (const auto& response :
         {std::string(
              "{\"code\":403,\"message\":\"not published\",\"data\":{\"has_update\":false}}"),
          std::string("{\"code\":0,\"success\":false,\"message\":\"rejected\",\"data\":{\"has_"
                      "update\":false}}"),
          Release(nullptr, 13), Release(nullptr, 25, false),
          Release(nullptr, 25, true, 0x1D00000)}) {  // Server returned a 29 MiB USB image.
        fixture::responses[check] = response;
        assert(launcher::Check() == 0);
        Wait();
        assert(launcher::Read().state == launcher::State::kError);
    }
    assert(dynamic_fixture::mutations == writes);
    fixture::http_status[redirect] = 302;
    fixture::http_headers[redirect]["Location"] = cdn;
    fixture::responses[cdn] = candidate;
    Available();
    Refresh();
    const auto before_tls = dynamic_fixture::mutations;
    fixture::open_errors[cdn] = ESP_ERR_HTTP_CONNECT;
    fixture::tls_errors[cdn] = {MBEDTLS_ERR_X509_FATAL_ERROR, 0};
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().error == kCertificateError && State().pending == 255 &&
           dynamic_fixture::mutations == before_tls);
    fixture::open_errors.clear();
    Available("00000000000000000000000000000000");
    Refresh(cdn, "00000000000000000000000000000000");
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255 &&
           State().active == 0);
    // Refreshed metadata must still refer to the release shown to the user.
    for (const auto& changed :
         {std::string("{\"code\":0,\"success\":true,\"data\":{\"has_update\":false}}"),
          Changed("\"firmware_id\":42", "\"firmware_id\":43"),
          Changed("\"version_code\":" + std::to_string(candidate_code),
                  "\"version_code\":" + std::to_string(candidate_code + 1)),
          Changed("\"version\":\"" + candidate_version + "\"", "\"version\":\"changed\""),
          Release("00000000000000000000000000000000", 25, true, 0, cdn),
          Release(nullptr, 25, true, candidate.size() + 1, cdn),
          Changed("\"firmware_type\":\"ota\"", "\"firmware_type\":\"full\""),
          Release(nullptr, 25, true, 0, ""),
          Release(nullptr, 25, true, 0, std::string(1024, 'x'))}) {
        Available();
        fixture::responses[check] = changed;
        const auto before_refresh = dynamic_fixture::mutations;
        const auto requests = fixture::requests.size();
        assert(launcher::Install() == 0);
        Wait();
        assert(launcher::Read().state == launcher::State::kError && State().pending == 255 &&
               dynamic_fixture::mutations == before_refresh &&
               fixture::requests.size() == requests + 1 && launcher::Read().received == 0);
    }
    Available();
    fixture::http_status[check] = 401;
    const auto before_denied = dynamic_fixture::mutations;
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().http_status == 401 && dynamic_fixture::mutations == before_denied);
    fixture::http_status[check] = 200;
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
    Refresh();
    fixture::read_results[cdn] = {0, 0, -1};
    assert(launcher::Install() == 0);
    Wait();
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255 &&
           launcher::Read().received == 0);
    Available();
    Refresh();
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
    // Interrupt after a durable checkpoint, then reject malformed Content-Range.
    Available();
    Refresh();
    fixture::fail_after = 70000;
    assert(launcher::Install() == ESP_OK);
    Wait();
    assert(launcher::Read().state == launcher::State::kError && State().pending == 255);
    fixture::fail_after = size_t(-1);
    Available();
    Refresh();
    assert(launcher::Read().received == 65536 && launcher::Read().resumable);
    fixture::http_headers[cdn]["Content-Range"] = "bytes 1-2/3";
    const auto before_bad_range = dynamic_fixture::mutations;
    assert(launcher::Install() == ESP_OK);
    Wait();
    assert(launcher::Read().error == ESP_ERR_INVALID_RESPONSE &&
           dynamic_fixture::mutations == before_bad_range && State().pending == 255);
    fixture::http_headers[cdn].erase("Content-Range");
    // A server ignoring Range must restart from zero, never append a full file.
    Available();
    Refresh();
    fixture::ignore_range = true;
    fixture::fail_after = 100000;
    const auto before_ignored_range = fixture::requests.size();
    assert(launcher::Install() == ESP_OK);
    Wait();
    bool saw_range = false, saw_full_after_range = false;
    for (size_t i = before_ignored_range; i < fixture::requests.size(); ++i) {
        const auto& request = fixture::requests[i];
        if (request.url != cdn)
            continue;
        if (request.headers.count("Range"))
            saw_range = true;
        else if (saw_range)
            saw_full_after_range = true;
    }
    assert(saw_range && saw_full_after_range && launcher::Read().state == launcher::State::kError);
    fixture::ignore_range = false;
    fixture::fail_after = size_t(-1);
    // Foreground operations can acquire the flash mutex while the HTTP read runs.
    Available();
    Refresh();
    assert(launcher::SetAutoDownload(true) == ESP_OK);
    fixture::http_read_hook = [](size_t offset) {
        if (offset > 20000)
            assert(launcher::YieldToForeground());
    };
    launcher::Tick(true, true);
    Wait();
    fixture::http_read_hook = nullptr;
    assert(launcher::Read().state == launcher::State::kPaused && launcher_worker_priority == 1);
    assert(launcher::Read().received >= 65536 && State().pending == 255);
    Snapshot(true);  // Durable NVS + partial flash image for a fresh-process reboot test.
    {
        std::unique_lock<std::mutex> foreground(OperationMutex(), std::try_to_lock);
        assert(foreground.owns_lock());
    }
    // Disabling auto-download preserves progress and does not schedule more GETs.
    assert(launcher::SetAutoDownload(false) == ESP_OK);
    const auto paused_requests = fixture::requests.size();
    fixture::timer_offset_us += 31000000;
    launcher::Tick(true, true);
    assert(!launcher::Busy() && fixture::requests.size() == paused_requests);
    // WiFi loss pauses a resumed download too; recovery resumes via Range.
    assert(launcher::SetAutoDownload(true) == ESP_OK);
    const auto before_heavy = fixture::requests.size();
    launcher::Tick(true, false);
    assert(!launcher::Busy() && fixture::requests.size() == before_heavy);
    fixture::http_read_hook = [](size_t offset) {
        if (offset > 20000)
            assert(launcher::YieldToForeground());
    };
    launcher::Tick(true, true);
    Wait();
    fixture::http_read_hook = nullptr;
    assert(launcher::Read().state == launcher::State::kPaused && State().pending == 255);
    // Finishing a high-load task resumes immediately, without a 30-second wait.
    const auto before_wifi_loss = fixture::requests.size();
    fixture::http_read_hook = [](size_t) { launcher::Tick(false, false); };
    launcher::Tick(true, true);
    Wait();
    fixture::http_read_hook = nullptr;
    assert(fixture::requests.size() > before_wifi_loss &&
           launcher::Read().state == launcher::State::kPaused);
    // A genuine connection failure backs off, unlike a foreground-resource pause.
    fixture::open_errors[cdn] = ESP_ERR_HTTP_CONNECT;
    launcher::Tick(true, true);
    Wait();
    assert(launcher::Read().state == launcher::State::kPaused &&
           launcher::Read().error == ESP_ERR_HTTP_CONNECT);
    const auto failed_requests = fixture::requests.size();
    launcher::Tick(true, true);
    assert(!launcher::Busy() && fixture::requests.size() == failed_requests);
    fixture::open_errors.clear();
    // Repeated ordinary UI activity leaves the connection and transfer intact.
    static unsigned ordinary_updates = 0;
    fixture::http_read_hook = [](size_t) {
        for (unsigned i = 0; i < 4; ++i) {
            launcher::Tick(true, true);
            (void)launcher::Read();
            ++ordinary_updates;
        }
    };
    fixture::timer_offset_us += 31000000;
    const auto resume_requests = fixture::requests.size();
    launcher::Tick(true, true);
    Wait();
    fixture::http_read_hook = nullptr;
    assert(launcher::Read().state == launcher::State::kReady && State().pending == 255);
    bool resumed = false;
    unsigned file_requests = 0;
    for (size_t i = resume_requests; i < fixture::requests.size(); ++i)
        resumed |= fixture::requests[i].headers.count("Range") != 0;
    for (size_t i = resume_requests; i < fixture::requests.size(); ++i)
        file_requests += fixture::requests[i].url == cdn;
    assert(resumed && file_requests == 1 && ordinary_updates > 16);
    puts("Background scheduling: ordinary activity keeps one download connection; high-load work "
         "pauses and resumes immediately; network failures back off and resume verified data.");
    // Valid download follows the real 302 handler and refreshes the expired URL.
    assert(launcher::Read().state == launcher::State::kReady);
    assert(State().pending == 255 && State().active == 0 && !State().attempted);
    assert(fixture::restarts == 0);  // A downloaded image is never armed automatically.
    snapshot_path += ".ready";
    Snapshot(true);
    assert(std::equal(old.begin(), old.end(), dynamic_fixture::flash.begin() + ALBOOT_A_OFFSET));
    for (const auto& request : fixture::requests) {
        assert(!request.headers.count("Authorization") && !request.headers.count("X-User-Info") &&
               !request.headers.count("X-Token"));
        assert(request.url == check || request.url == cdn || request.url == redirect);
    }
    auto state = State();
    assert(launcher::Restart() == ESP_OK);
    Wait();
    state = State();
    assert(state.pending == 1 && fixture::restarts == 1);
    assert(alboot_attempt(&state, 1) == 0);
    assert(alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &state) == 0);
    fixture::running = esp_partition_find_first(0, 31, nullptr);
    // Confirmation with the old binary's compiled version must reject the new trial.
    assert(launcher::ConfirmHealthy() == ESP_ERR_INVALID_STATE);
    assert(State().pending == 1 && State().active == 0);
    StopFactoryTasks();
    puts(
        "Production Launcher updater: anonymous check refresh, pinned release metadata, HTTP "
        "errors, URL refresh/302, "
        "cancellation, MD5, ESP image verification, inactive-slot-only writes and version-bound "
        "confirmation passed.");
}
