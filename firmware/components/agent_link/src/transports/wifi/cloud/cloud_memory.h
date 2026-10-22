#pragma once

#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32C3
#include <mutex>
#endif

namespace agentlink::cloud {

// A C3 cannot afford another HTTPS handshake alongside the live Opus call.
// HTTP callers wait for voice teardown; periodic polling tries once and defers.
// Recursive ownership lets the session poll guard call the guarded HTTP client.
class NetworkMemoryLock {
public:
    explicit NetworkMemoryLock(bool wait = true)
#if CONFIG_IDF_TARGET_ESP32C3
        : lock_(Mutex(), std::defer_lock)
#endif
    {
#if CONFIG_IDF_TARGET_ESP32C3
        if (wait) lock_.lock();
        else (void)lock_.try_lock();
#else
        (void)wait;
#endif
    }
    explicit operator bool() const {
#if CONFIG_IDF_TARGET_ESP32C3
        return lock_.owns_lock();
#else
        return true;
#endif
    }
private:
#if CONFIG_IDF_TARGET_ESP32C3
    static std::recursive_mutex& Mutex() {
        static std::recursive_mutex mutex;
        return mutex;
    }
    std::unique_lock<std::recursive_mutex> lock_;
#endif
};

}  // namespace agentlink::cloud
