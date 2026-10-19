#pragma once
extern unsigned delayed_ms;
inline void vTaskDelay(unsigned ms) { delayed_ms+=ms; }
