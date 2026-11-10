#pragma once
#include "paper4_ime.h"
#include <algorithm>
#include <string>
#include <vector>

namespace paper4 {
enum class InputKind { None, Compose, Review, Calibrate };
enum class TouchCommand { None, Character, Mode, Candidate, Previous, Next, Space, Literal, Erase, Clear, Review, Send, Back, Row, MenuNext, Calibrate };
struct InputView {
    InputKind kind = InputKind::None;
    InputMode mode = InputMode::Nine;
    uint32_t revision = 0;
    std::string text, pending, notice;
    std::vector<std::string> candidates, pinyins;
    size_t candidate_start = 0;
    int calibration_step = 0;
    bool Same(const InputView& b) const {
        return kind == b.kind && mode == b.mode && revision == b.revision && text == b.text && pending == b.pending && notice == b.notice &&
            candidates == b.candidates && pinyins == b.pinyins && candidate_start == b.candidate_start && calibration_step == b.calibration_step;
    }
};
struct TouchButton {
    int x, y, w, h;
    std::string label, value, hint;
    TouchCommand command;
    int argument = 0;
    bool selected = false;
    bool Contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
inline std::vector<TouchButton> InputButtons(const InputView& input) {
    std::vector<TouchButton> keys;
    const auto add = [&](int x, int y, int w, int h, const std::string& label, TouchCommand cmd, int arg = 0,
                         const std::string& value = "", const std::string& hint = "", bool selected = false) {
        keys.push_back({x,y,w,h,label,value,hint,cmd,arg,selected});
    };
    if (input.kind == InputKind::Review) {
        add(24, 668, 224, 54, "继续编辑", TouchCommand::Back);
        add(272, 668, 232, 54, "确认发送", TouchCommand::Send);
        add(24,734,224,44,"上一页",TouchCommand::Previous);
        add(272,734,232,44,"下一页",TouchCommand::Next);
        return keys;
    }
    if (input.kind != InputKind::Compose) return keys;
    add(16,330,42,62,"<",TouchCommand::Previous);
    for (size_t i = 0; i < input.candidates.size() && i < 4; ++i)
        add(64 + int(i)*100,330,94,62,input.candidates[i],TouchCommand::Candidate,
            int(input.candidate_start + i),"",i < input.pinyins.size() ? input.pinyins[i] : "");
    add(470,330,42,62,">",TouchCommand::Next);
    constexpr const char* modes[] = {"九键", "全键", "英文", "符号"};
    for (int i = 0; i < 4; ++i)
        add(24 + i*122,410,114,48,modes[i],TouchCommand::Mode,i,"","",int(input.mode) == i);
    if (input.mode == InputMode::Nine) {
        constexpr const char* labels[] = {"原文", "2 ABC", "3 DEF", "4 GHI", "5 JKL", "6 MNO", "7 PQRS", "8 TUV", "9 WXYZ"};
        for (int i = 0; i < 9; ++i) add(24 + i%3*164,482 + i/3*62,152,52,labels[i],
            i ? TouchCommand::Character : TouchCommand::Literal,0,i ? std::to_string(i + 1) : "");
    } else {
        const std::vector<std::vector<std::string>> symbols = {
            {"1","2","3","4","5","6","7","8","9","0"},
            {"，","。","？","！",".",",","?","!",":",";"},
            {"(",")","-","_","/","@","#","+","=","'"}};
        constexpr const char* letters[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int row = 0; row < 3; ++row) {
            const int count = input.mode == InputMode::Symbols ? 10 : int(std::char_traits<char>::length(letters[row]));
            const int left = row == 1 && input.mode != InputMode::Symbols ? 48 : 24;
            for (int i = 0; i < count; ++i) {
                const std::string value = input.mode == InputMode::Symbols ? symbols[row][i] : std::string(1,letters[row][i]);
                add(left + i*48,482 + row*62,44,52,value,TouchCommand::Character,0,value);
            }
        }
        if (input.mode != InputMode::Symbols) add(360,606,144,52,"空格",TouchCommand::Space);
    }
    add(24,668,112,52,"退格",TouchCommand::Erase);
    add(146,668,112,52,"空格",TouchCommand::Space);
    add(268,668,112,52,"清空",TouchCommand::Clear);
    add(390,668,114,52,"发送",TouchCommand::Review);
    add(24,734,224,44,"返回",TouchCommand::Back);
    add(272,734,232,44,"拼音原文",TouchCommand::Literal);
    return keys;
}
inline std::vector<TouchButton> MenuButtons(size_t count, int selected) {
    std::vector<TouchButton> keys;
    const size_t start = selected >= 7 ? size_t(selected) / 7 * 7 : 0;
    for (size_t i = start; i < count && i < start + 7; ++i)
        keys.push_back({20,227 + int(i-start)*61,488,53,"","","",TouchCommand::Row,int(i),false});
    keys.push_back({24,730,224,50,"返回","","",TouchCommand::Back});
    if (count > 7) keys.push_back({272,730,232,50,"下一页","","",TouchCommand::MenuNext});
    return keys;
}
}
