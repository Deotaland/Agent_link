#include "recording_files.h"

#include <cerrno>
#include <cctype>
#include <dirent.h>

namespace {
bool Consume(const char*& name, const char* literal) {
    while (*literal) {
        if (std::tolower(static_cast<unsigned char>(*name)) != *literal++) return false;
        ++name;
    }
    return true;
}

bool Sequence(const char* name, uint32_t* result) {
    if (!Consume(name, "rec-")) return false;
    const char* digits = name;
    uint32_t number = 0;
    while (*name >= '0' && *name <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*name++ - '0');
        if (number > (UINT32_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    if (name == digits || number == 0 || !Consume(name, ".wav") || *name) return false;
    *result = number;
    return true;
}
}

esp_err_t recording_files::ScanLastSequence(const char* directory, uint32_t* last) {
    if (!directory || !last) return ESP_ERR_INVALID_ARG;
    DIR* dir = opendir(directory);
    if (!dir) return ESP_FAIL;
    uint32_t maximum = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        const dirent* entry = readdir(dir);
        if (!entry) {
            error = errno;
            break;
        }
        uint32_t number = 0;
        if (Sequence(entry->d_name, &number) && number > maximum) maximum = number;
    }
    if (closedir(dir) != 0) error = errno;
    if (error) return ESP_FAIL;
    *last = maximum;
    return ESP_OK;
}
