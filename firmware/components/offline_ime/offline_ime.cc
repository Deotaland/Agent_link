#include "offline_ime.h"
#include <algorithm>
#include <cstring>

namespace offline_ime {
namespace {
void Pop(std::string& value) {
    if (value.empty()) return;
    size_t last = value.size() - 1;
    while (last && (static_cast<unsigned char>(value[last]) & 0xc0) == 0x80) --last;
    value.resize(last);
}
}
const std::string& Ime::Pending() const {
    static const std::string empty;
    return mode_ == InputMode::Nine || mode_ == InputMode::Full ? pending_[int(mode_)] : empty;
}
std::string Ime::Digits(const std::string& pinyin) {
    static constexpr char map[] = "22233344455566677778889999";
    std::string result;
    for (char c : pinyin) result += c >= 'a' && c <= 'z' ? map[c - 'a'] : c;
    return result;
}
void Ime::SetMode(InputMode mode) {
    if (mode < InputMode::Nine || mode > InputMode::Symbols) return;
    mode_ = mode; Update();
}
bool Ime::Append(const std::string& text) {
    if (text_.size() + text.size() > kMaxBytes) return false;
    text_ += text;
    return true;
}
bool Ime::Input(const std::string& key) {
    if (key.empty()) return false;
    if (mode_ == InputMode::English || mode_ == InputMode::Symbols) {
        if (!Append(key)) return false;
    } else {
        if (key.size() != 1 || Pending().size() >= 32) return false;
        const char c = key[0];
        if (mode_ == InputMode::Nine ? (c < '2' || c > '9') : (c < 'a' || c > 'z')) return false;
        pending_[int(mode_)] += c;
    }
    Update(); return true;
}
void Ime::Update() {
    ++revision_; candidate_page_ = 0; candidates_.clear();
    const auto& query = Pending();
    if (query.empty()) return;
    std::vector<Candidate> matches;
    const auto& dictionary = paper_resources::Dictionary();
    for (size_t i = 0; i < dictionary.dictionary_count; ++i) {
        const auto entry = dictionary.Entry(i);
        const std::string key = mode_ == InputMode::Nine ? Digits(entry.pinyin) : entry.pinyin;
        if (key == query) matches.push_back({entry, query.size(), 3});
        else if (key.size() < query.size() && query.compare(0, key.size(), key) == 0)
            matches.push_back({entry, key.size(), 2});
        else if (key.size() > query.size() && key.compare(0, query.size(), query) == 0)
            matches.push_back({entry, query.size(), 1});
    }
    std::sort(matches.begin(), matches.end(), [](const Candidate& a, const Candidate& b) {
        if (a.quality != b.quality) return a.quality > b.quality;
        if (a.quality == 2 && a.consumed != b.consumed) return a.consumed > b.consumed;
        return a.entry.frequency > b.entry.frequency;
    });
    // Preserve all matching characters, including uncommon ones, while collapsing polyphonic duplicates.
    for (const auto& match : matches) {
        const bool duplicate = std::any_of(candidates_.begin(), candidates_.end(), [&](const Candidate& c) {
            return std::strcmp(c.entry.text, match.entry.text) == 0 && c.consumed == match.consumed;
        });
        if (!duplicate) candidates_.push_back(match);
    }
}
bool Ime::Select(size_t index) {
    if (index >= candidates_.size()) return false;
    const Candidate selected = candidates_[index];
    if (!Append(selected.entry.text)) return false;
    pending_[int(mode_)].erase(0, selected.consumed);
    Update(); return true;
}
void Ime::Page(int direction) {
    const size_t pages = (candidates_.size() + 3) / 4;
    if (pages) { candidate_page_ = (candidate_page_ + pages + direction) % pages; ++revision_; }
}
bool Ime::Space() {
    if (!Pending().empty()) return Select(0);
    if (!Append(" ")) return false;
    Update(); return true;
}
bool Ime::Literal() {
    if (Pending().empty() || !Append(Pending())) return false;
    pending_[int(mode_)].clear(); Update(); return true;
}
void Ime::Backspace() {
    if (!Pending().empty()) Pop(pending_[int(mode_)]);
    else Pop(text_);
    Update();
}
void Ime::Clear() { text_.clear(); pending_[0].clear(); pending_[1].clear(); Update(); }
}
