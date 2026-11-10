#pragma once
#include "amoled_orientation.h"
#include "round_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// One outstanding transfer owns its LVGL source until Finish returns it.
// Only the UI task submits/finishes jobs; the worker never calls LVGL.
class AmoledDisplayTransfer {
   public:
    AmoledDisplayTransfer() = default;
    AmoledDisplayTransfer(const AmoledDisplayTransfer&) = delete;
    AmoledDisplayTransfer& operator=(const AmoledDisplayTransfer&) = delete;
    struct Result {
        esp_err_t error = ESP_OK;
        uint32_t elapsed_us = 0;
    };
    ~AmoledDisplayTransfer() {
        Stop();
    }
    esp_err_t Start(RoundDisplay* panel, size_t capacity, int rotation, int width);
    bool Active() const {
        return task_ != nullptr;
    }
    esp_err_t Submit(amoled_ui::DisplayRect area, const uint8_t* source);
    bool Finish(bool wait, Result* result);
    void Stop();

   private:
    struct Job {
        amoled_ui::DisplayRect area;
        const uint8_t* source;
        int64_t started;
    };
    static void Entry(void* context);
    void Run();
    RoundDisplay* panel_ = nullptr;
    uint8_t* staging_ = nullptr;
    size_t capacity_ = 0;
    int rotation_ = 0, width_ = 0;
    QueueHandle_t jobs_ = nullptr, results_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool pending_ = false;
};
