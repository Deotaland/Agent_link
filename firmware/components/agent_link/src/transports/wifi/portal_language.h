#pragma once
#include <string>

inline void ApplyPortalLanguage(std::string& page) {
#if defined(AGENT_LINK_UI_DEFAULT_ENGLISH) && AGENT_LINK_UI_DEFAULT_ENGLISH
    constexpr const char* checked = "checked";
    constexpr const char* language = "en";
#else
    constexpr const char* checked = "";
    constexpr const char* language = "zh-CN";
#endif
    const auto replace = [&](const char* marker, const char* value) {
        auto at = page.find(marker);
        if (at != std::string::npos)
            page.replace(at, std::char_traits<char>::length(marker), value);
    };
    replace("{{LANG_CHECKED}}", checked);
    replace("{{LANGUAGE}}", language);
}
