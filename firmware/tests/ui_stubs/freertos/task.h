#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
inline int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t* task) {
    *task = reinterpret_cast<void*>(1); return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 2048; }
inline void vTaskDelay(unsigned) {}
unsigned ulTaskNotifyTake(int, unsigned);
