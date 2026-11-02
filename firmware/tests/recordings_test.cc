#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "recordings.h"
#include <dirent.h>
#include <sys/stat.h>

std::vector<std::string> files;
size_t cursor = 0;
bool available = true;
DIR directory;
DIR* opendir(const char*) { cursor = 0; return available ? &directory : nullptr; }
dirent* readdir(DIR*) {
    static dirent item;
    if (cursor == files.size()) return nullptr;
    std::strcpy(item.d_name, files[cursor++].c_str()); return &item;
}
int closedir(DIR*) { return 0; }
int stat(const char*, struct stat* out) { out->st_size = 32044; out->st_mtime = 42; return 0; }
uint32_t WavRecorder::DurationMsFromSize(uint32_t, uint32_t, Format) { return 1000; }
unsigned Read16(const uint8_t* p) { return p[0] | (p[1] << 8); }

int main() {
    Recordings recordings; recordings.Init({"records", "messages", 16000, WavRecorder::Format::kPcm16});
    // Serialized entry sizes are 100, 80, 20: an unfit middle entry must not be skipped.
    files = {std::string(83, 'a') + ".wav", std::string(63, 'b') + ".wav", "ccc.wav"};
    uint8_t response[128], request[4]{}; size_t length; uint16_t error;
    assert(recordings.HandleList(request, 3, response, sizeof response, &length, &error));
    assert(error == 0 && Read16(response) == 1 && response[length - 5] == 1);
    assert(Read16(response + length - 4) == 3 && Read16(response + length - 2) == 1);
    request[0] = 1;
    assert(recordings.HandleList(request, 3, response, sizeof response, &length, &error));
    assert(Read16(response) == 2 && response[length - 5] == 0 && response[3] == 'b');
    assert(!recordings.HandleList(request, 2, response, sizeof response, &length, &error) && error == 1004);
    request[3] = 2;
    assert(!recordings.HandleList(request, 4, response, sizeof response, &length, &error) && error == 1004);
    available = false;
    assert(!recordings.HandleList(nullptr, 0, response, sizeof response, &length, &error) && error == 1003);
    available = true; files = {std::string(120, 'a') + ".wav"};
    assert(!recordings.HandleList(nullptr, 0, response, sizeof response, &length, &error) && error == 1005);
    response[0] = 0x55;
    assert(!recordings.HandleList(nullptr, 0, response, 6, &length, &error) && error == 1005 && response[0] == 0x55);
    files.clear();
    assert(recordings.HandleList(nullptr, 0, response, sizeof response, &length, &error));
    assert(error == 0 && length == 7 && Read16(response) == 0);
    puts("Recording pages: contiguous offsets, missing storage, malformed requests, capacity guards and empty directory passed.");
}
