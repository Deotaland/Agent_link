#pragma once
#include <atomic>
namespace amoled_ui {
enum class Language { kChinese, kEnglish };
#if defined(AGENT_LINK_UI_DEFAULT_ENGLISH) && AGENT_LINK_UI_DEFAULT_ENGLISH
inline constexpr Language kDefaultLanguage = Language::kEnglish;
#else
inline constexpr Language kDefaultLanguage = Language::kChinese;
#endif
constexpr Language SavedLanguage(int value) {
    return value == 0 ? Language::kChinese : value == 1 ? Language::kEnglish : kDefaultLanguage;
}
inline std::atomic<Language> language{kDefaultLanguage};
inline const char* Tr(const char* chinese, const char* english) {
    return language.load() == Language::kEnglish ? english : chinese;
}
}  // namespace amoled_ui
