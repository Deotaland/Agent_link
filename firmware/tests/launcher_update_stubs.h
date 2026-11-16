#pragma once
#include "agent_system_dynamic_stubs.h"
#include "factory_store_job_stubs.h"
#ifdef _WIN32
#define strcasecmp _stricmp
#endif
constexpr int ESP_ERR_HTTP_FETCH_HEADER = 0x7004, eSetValueWithOverwrite = 0;
inline unsigned launcher_worker_priority = 0;
inline void vTaskPrioritySet(TaskHandle_t, unsigned priority) {
    launcher_worker_priority = priority;
}
inline void xTaskNotify(TaskHandle_t task, uint32_t value, int) {
    std::lock_guard<std::mutex> lock(task->mutex);
    task->notifications = value;
    task->changed.notify_one();
}
inline int xTaskNotifyWait(uint32_t, uint32_t, uint32_t* value, unsigned) {
    *value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return pdTRUE;
}
struct TestTimer {
    void (*callback)(void*);
    void* arg;
    bool started = false;
};
using esp_timer_handle_t = TestTimer*;
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    const char* name;
};
inline int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    static TestTimer timer;
    timer = {args->callback, args->arg, false};
    *out = &timer;
    return ESP_OK;
}
inline int esp_timer_start_once(esp_timer_handle_t timer, uint64_t) {
    timer->started = true;
    return ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t timer) {
    timer->started = false;
    return ESP_OK;
}
