#pragma once

namespace agent_system {
// Service results use the board's default language, independently of any renderer.
constexpr const char* DefaultUiText(const char* chinese, const char* english) {
#if defined(AGENT_LINK_UI_DEFAULT_ENGLISH) && AGENT_LINK_UI_DEFAULT_ENGLISH
    (void)chinese;
    return english;
#else
    (void)english;
    return chinese;
#endif
}
}  // namespace agent_system
