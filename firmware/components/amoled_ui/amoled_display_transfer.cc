#include "amoled_display_transfer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

esp_err_t AmoledDisplayTransfer::Start(Co5300Panel* panel, size_t capacity, int rotation,
                                       int width) {
    if (Active() || !panel || !capacity || width <= 0 || (rotation != 0 && rotation != 90))
        return ESP_ERR_INVALID_ARG;
    panel_ = panel;
    capacity_ = capacity;
    rotation_ = rotation;
    width_ = width;
    staging_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, capacity, MALLOC_CAP_SPIRAM));
    jobs_ = xQueueCreate(1, sizeof(Job));
    results_ = xQueueCreate(1, sizeof(Result));
    if (!staging_ || !jobs_ || !results_ ||
        xTaskCreatePinnedToCore(Entry, "amoled_flush", 4096, this, 4, &task_, 0) != pdPASS) {
        Stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t AmoledDisplayTransfer::Submit(amoled_ui::DisplayRect area, const uint8_t* source) {
    if (!Active() || pending_)
        return ESP_ERR_INVALID_STATE;
    if (!source || area.x < 0 || area.y < 0 || area.w <= 0 || area.h <= 0 || area.w > width_ ||
        area.h > width_ || area.x > width_ - area.w || area.y > width_ - area.h ||
        static_cast<size_t>(area.w) * area.h > capacity_ / 2)
        return ESP_ERR_INVALID_ARG;
    const Job job{area, source, esp_timer_get_time()};
    if (xQueueSend(jobs_, &job, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    pending_ = true;
    return ESP_OK;
}
bool AmoledDisplayTransfer::Finish(bool wait, Result* result) {
    if (!pending_ || !result || xQueueReceive(results_, result, wait ? portMAX_DELAY : 0) != pdTRUE)
        return false;
    pending_ = false;
    return true;
}
void AmoledDisplayTransfer::Stop() {
    Result result;
    (void)Finish(true, &result);
    // A received result means DrawBitmap returned: the worker no longer borrows
    // the LVGL source or staging pixels. Its next operation is waiting for a job.
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (jobs_) {
        vQueueDelete(jobs_);
        jobs_ = nullptr;
    }
    if (results_) {
        vQueueDelete(results_);
        results_ = nullptr;
    }
    heap_caps_free(staging_);
    staging_ = nullptr;
}
void AmoledDisplayTransfer::Entry(void* context) {
    static_cast<AmoledDisplayTransfer*>(context)->Run();
}
void AmoledDisplayTransfer::Run() {
    Job job;
    while (xQueueReceive(jobs_, &job, portMAX_DELAY) == pdTRUE) {
        auto area = job.area;
        Result result;
        if (rotation_ == 90) {
            if (!amoled_ui::RotateRgb565Clockwise90(job.source, staging_, area.w, area.h, capacity_,
                                                    true))
                result.error = ESP_ERR_INVALID_SIZE;
            area = amoled_ui::Clockwise90Rect(area, width_);
        } else {
            const size_t bytes = static_cast<size_t>(area.w) * area.h * 2;
            for (size_t i = 0; i < bytes; i += 2) {
                staging_[i] = job.source[i + 1];
                staging_[i + 1] = job.source[i];
            }
        }
        if (result.error == ESP_OK)
            result.error = panel_->DrawBitmap(area.x, area.y, area.w, area.h, staging_);
        result.elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - job.started);
        (void)xQueueSend(results_, &result, portMAX_DELAY);
    }
}
