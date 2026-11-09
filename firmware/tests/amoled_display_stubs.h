#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include "esp_heap_caps.h"

constexpr int ESP_ERR_INVALID_ARG = -2, ESP_ERR_INVALID_STATE = -3, ESP_ERR_INVALID_SIZE = -4, ESP_ERR_NO_MEM = -5;
constexpr unsigned portMAX_DELAY = ~0u;
constexpr int pdTRUE = 1, pdPASS = 1;
std::atomic<int> native_allocations{0};
bool native_fail_allocation = false;
void* heap_caps_malloc(size_t bytes, unsigned) {
    if (native_fail_allocation) return nullptr;
    void* value = std::malloc(bytes); if (value) ++native_allocations; return value;
}
void* heap_caps_aligned_alloc(size_t, size_t bytes, unsigned flags) { return heap_caps_malloc(bytes, flags); }
void heap_caps_free(void* value) { if (value) { --native_allocations; std::free(value); } }
struct NativeTask { std::thread thread; std::atomic<bool> stop{false}; };
using TaskHandle_t = NativeTask*;
thread_local NativeTask* native_task = nullptr;
struct NativeTaskStopped {};
struct NativeQueue {
    size_t size;
    std::deque<std::vector<uint8_t>> items;
    std::mutex mutex;
    std::condition_variable changed;
};
using QueueHandle_t = NativeQueue*;
QueueHandle_t xQueueCreate(unsigned, size_t size) { auto* queue = new NativeQueue; queue->size = size; return queue; }
void vQueueDelete(QueueHandle_t queue) { delete queue; }
int NativeQueueWait(NativeQueue* queue, std::unique_lock<std::mutex>& lock, bool empty, unsigned wait) {
    const auto start = std::chrono::steady_clock::now();
    while (queue->items.empty() == empty) {
        if (native_task && native_task->stop.load()) throw NativeTaskStopped{};
        if (!wait) return 0;
        if (wait != portMAX_DELAY && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(wait)) return 0;
        queue->changed.wait_for(lock, std::chrono::milliseconds(1));
    }
    return pdTRUE;
}
int xQueueSend(QueueHandle_t queue, const void* item, unsigned wait) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    if (!NativeQueueWait(queue, lock, false, wait)) return 0;
    const auto* bytes = static_cast<const uint8_t*>(item);
    queue->items.emplace_back(bytes, bytes + queue->size); queue->changed.notify_all(); return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void* item, unsigned wait) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    if (!NativeQueueWait(queue, lock, true, wait)) return 0;
    memcpy(item, queue->items.front().data(), queue->size); queue->items.pop_front(); queue->changed.notify_all(); return pdTRUE;
}
int xTaskCreatePinnedToCore(void (*entry)(void*), const char*, unsigned, void* context, unsigned, TaskHandle_t* out, int) {
    auto* task = new NativeTask;
    task->thread = std::thread([=]() { native_task = task; try { entry(context); } catch (NativeTaskStopped&) {} });
    *out = task; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { task->stop.store(true); task->thread.join(); delete task; }
