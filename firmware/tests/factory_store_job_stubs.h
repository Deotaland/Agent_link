#pragma once
#include "agent_system_stubs.h"
#include <condition_variable>
#include <memory>
#include <chrono>

struct StopFactoryWorker {};
struct FactoryTask {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned notifications = 0;
    bool stop = false;
    std::thread thread;
};
using TaskHandle_t = FactoryTask*;
constexpr unsigned pdTRUE = 1, pdPASS = 1, portMAX_DELAY = ~0u;
inline thread_local FactoryTask* current_factory_task = nullptr;
inline std::vector<std::unique_ptr<FactoryTask>> factory_tasks;
inline int xTaskCreate(void (*entry)(void*), const char*, unsigned, void* context, unsigned,
                       TaskHandle_t* out) {
    auto task = std::make_unique<FactoryTask>();
    *out = task.get();
    task->thread = std::thread([entry, context, worker = task.get()] {
        current_factory_task = worker;
        try {
            entry(context);
        } catch (const StopFactoryWorker&) {
        }
    });
    factory_tasks.push_back(std::move(task));
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(task->mutex);
    ++task->notifications;
    task->changed.notify_one();
}
inline unsigned ulTaskNotifyTake(unsigned, unsigned) {
    auto& task = *current_factory_task;
    std::unique_lock<std::mutex> lock(task.mutex);
    task.changed.wait(lock, [&] { return task.stop || task.notifications; });
    if (task.stop)
        throw StopFactoryWorker{};
    const auto count = task.notifications;
    task.notifications = 0;
    return count;
}
inline void StopFactoryTasks() {
    for (auto& task : factory_tasks) {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->stop = true;
            task->changed.notify_one();
        }
        task->thread.join();
    }
    factory_tasks.clear();
}
