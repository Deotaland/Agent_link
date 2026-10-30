#pragma once
#include <atomic>
namespace amoled_voice {
enum class Language { kChinese, kEnglish };
inline std::atomic<Language> language{Language::kChinese};
inline const char* Tr(const char* chinese, const char* english) {
    return language.load() == Language::kEnglish ? english : chinese;
}
}  // namespace amoled_voice
