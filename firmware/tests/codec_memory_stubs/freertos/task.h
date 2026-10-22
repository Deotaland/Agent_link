#pragma once
extern thread_local void* codec_test_task;
inline void* xTaskGetCurrentTaskHandle() { return codec_test_task; }
