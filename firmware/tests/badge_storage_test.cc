#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "badge_data.h"
#include "nvs.h"
#include "freertos/task.h"

bool initialized = false, worker_running = false, fail_commit = false;
unsigned commits = 0, permits = 0;
uint32_t notifications = 0;
void (*worker)(void*) = nullptr;
std::map<std::string, std::string> saved{{"name", "Saved wearer"}}, staged;
struct StopWorker {};
esp_err_t nvs_flash_init() { initialized = true; return ESP_OK; }
esp_err_t nvs_open(const char*, int, nvs_handle_t* out) { assert(initialized); *out = 1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* out, size_t* size) {
    const auto item = saved.find(key);
    if (item == saved.end()) return ESP_ERR_NOT_FOUND;
    assert(*size > item->second.size()); std::strcpy(out, item->second.c_str()); return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value) {
    assert(worker_running); staged[key] = value; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    assert(worker_running); ++commits;
    if (fail_commit) { fail_commit = false; return ESP_FAIL; }
    for (const auto& item : staged) saved[item.first] = item.second;
    staged.clear(); return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
int xTaskCreate(void (*entry)(void*), const char*, unsigned, void*, int, TaskHandle_t* out) {
    worker = entry; *out = &worker; return pdPASS;
}
int xTaskNotify(TaskHandle_t, uint32_t bits, eNotifyAction) { notifications |= bits; return pdPASS; }
int xTaskNotifyWait(uint32_t, uint32_t, uint32_t* out, TickType_t) {
    if (!permits) throw StopWorker{};
    --permits; *out = notifications; notifications = 0; return pdPASS;
}
void Pump(unsigned cycles) {
    permits = cycles; worker_running = true;
    try { worker(nullptr); } catch (const StopWorker&) {}
    worker_running = false;
}
int main() {
    auto& data = badge::Data::Instance(); data.Load();
    badge::Snapshot snapshot{}; data.Copy(snapshot);
    assert(std::string(snapshot.values[0]) == "Saved wearer" && commits == 0 && worker);
    const auto revision = data.Revision();
    data.Set(badge::Field::kName, "New wearer", 10);
    data.Set(badge::Field::kCompany, "Company", 7);
    assert(commits == 0 && notifications == ((1u << 0) | (1u << 4)));
    data.Copy(snapshot); assert(std::string(snapshot.values[0]) == "New wearer");
    assert(data.Revision() == revision + 2);
    Pump(1); assert(commits == 2 && saved["name"] == "New wearer" && saved["company"] == "Company");
    data.Set(badge::Field::kName, "New wearer", 10); assert(notifications == 0);
    data.Set(badge::Field::kName, "Retry wearer", 12); fail_commit = true;
    Pump(2); assert(saved["name"] == "Retry wearer" && commits == 4);
    std::string text(46, 'x'); text += "\xe4\xb8\xad";
    data.Set(badge::Field::kName, text.data(), text.size()); data.Copy(snapshot);
    assert(std::strlen(snapshot.values[0]) == 46);
    puts("Badge storage: initialized boot restore, copied UI snapshot, deferred/coalesced writes, retry and UTF-8 clipping passed.");
}
