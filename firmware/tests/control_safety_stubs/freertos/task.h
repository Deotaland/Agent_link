#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
enum eNotifyAction { eSetBits };
int xTaskCreate(void (*entry)(void*), const char*, unsigned, void*, int, TaskHandle_t*);
int xTaskNotify(TaskHandle_t, uint32_t, eNotifyAction);
int xTaskNotifyWait(uint32_t, uint32_t, uint32_t*, TickType_t);
