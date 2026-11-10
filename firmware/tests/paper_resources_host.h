#pragma once
#include "paper_resources.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

inline std::vector<uint8_t>& LoadPaperResources(const char* path) {
    static std::vector<uint8_t> bytes;
    std::ifstream file(path,std::ios::binary);
    assert(file);
    bytes.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
    assert(paper_resources::Install(bytes.data(),bytes.size()));
    return bytes;
}
