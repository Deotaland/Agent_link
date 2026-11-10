#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "paper_resources.h"

namespace offline_ime {
enum class InputMode { Nine, Full, English, Symbols };
using DictionaryEntry = paper_resources::DictionaryEntry;
struct Candidate { DictionaryEntry entry; size_t consumed; int quality; };
class Ime {
public:
    static constexpr size_t kMaxBytes = 1024;
    InputMode Mode() const { return mode_; }
    uint32_t Revision() const { return revision_; }
    const std::string& Text() const { return text_; }
    const std::string& Pending() const;
    const std::vector<Candidate>& Candidates() const { return candidates_; }
    size_t CandidatePage() const { return candidate_page_; }
    void SetMode(InputMode mode);
    bool Input(const std::string& key);
    bool Select(size_t index);
    void Page(int direction);
    bool Space();
    bool Literal();
    void Backspace();
    void Clear();
    static std::string Digits(const std::string& pinyin);
private:
    bool Append(const std::string& text);
    void Update();
    InputMode mode_ = InputMode::Nine;
    std::string text_, pending_[2];
    std::vector<Candidate> candidates_;
    size_t candidate_page_ = 0;
    uint32_t revision_ = 1;
};
}
