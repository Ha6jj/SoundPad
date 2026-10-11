#include "Hotkey.h"

#include "Console.h"

#include <cstdio>
#include <map>
#include <vector>

namespace {

// Имя клавиши -> virtual-key code. Коды заданы числами, чтобы модуль
// собирался и тестировался без windows.h.
const std::map<std::string, uint32_t>& keyTable()
{
    static const std::map<std::string, uint32_t> table = [] {
        std::map<std::string, uint32_t> t;
        for (char c = 'a'; c <= 'z'; ++c) t[std::string(1, c)] = static_cast<uint32_t>(c - 'a' + 'A');
        for (char c = '0'; c <= '9'; ++c) t[std::string(1, c)] = static_cast<uint32_t>(c);
        for (int i = 1; i <= 24; ++i) t["f" + std::to_string(i)] = 0x70 + i - 1;
        for (int i = 0; i <= 9; ++i) {
            t["num" + std::to_string(i)] = 0x60 + i;
            t["numpad" + std::to_string(i)] = 0x60 + i;
        }
        const std::vector<std::pair<const char*, uint32_t>> named = {
            {"space", 0x20}, {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09},
            {"esc", 0x1B}, {"escape", 0x1B}, {"backspace", 0x08},
            {"pause", 0x13}, {"capslock", 0x14}, {"scrolllock", 0x91}, {"numlock", 0x90},
            {"printscreen", 0x2C},
            {"pageup", 0x21}, {"pgup", 0x21}, {"pagedown", 0x22}, {"pgdn", 0x22},
            {"end", 0x23}, {"home", 0x24},
            {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
            {"insert", 0x2D}, {"ins", 0x2D}, {"delete", 0x2E}, {"del", 0x2E},
            {"nummul", 0x6A}, {"numadd", 0x6B}, {"numsub", 0x6D},
            {"numdecimal", 0x6E}, {"numdiv", 0x6F},
            {";", 0xBA}, {"=", 0xBB}, {"plus", 0xBB}, {",", 0xBC}, {"comma", 0xBC},
            {"-", 0xBD}, {"minus", 0xBD}, {".", 0xBE}, {"period", 0xBE},
            {"/", 0xBF}, {"`", 0xC0}, {"tilde", 0xC0},
            {"[", 0xDB}, {"\\", 0xDC}, {"]", 0xDD}, {"'", 0xDE},
        };
        for (const auto& [name, vk] : named) t[name] = vk;
        return t;
    }();
    return table;
}

// Каноническое имя клавиши для вывода.
std::string keyName(uint32_t vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    if (vk >= 0x60 && vk <= 0x69) return "Num" + std::to_string(vk - 0x60);
    static const std::map<uint32_t, const char*> pretty = {
        {0x20, "Space"}, {0x0D, "Enter"}, {0x09, "Tab"}, {0x1B, "Esc"}, {0x08, "Backspace"},
        {0x13, "Pause"}, {0x14, "CapsLock"}, {0x91, "ScrollLock"}, {0x90, "NumLock"},
        {0x2C, "PrintScreen"}, {0x21, "PageUp"}, {0x22, "PageDown"}, {0x23, "End"},
        {0x24, "Home"}, {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"}, {0x28, "Down"},
        {0x2D, "Insert"}, {0x2E, "Delete"}, {0x6A, "NumMul"}, {0x6B, "NumAdd"},
        {0x6D, "NumSub"}, {0x6E, "NumDecimal"}, {0x6F, "NumDiv"},
        {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."}, {0xBF, "/"},
        {0xC0, "`"}, {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"},
    };
    auto it = pretty.find(vk);
    if (it != pretty.end()) return it->second;
    char buf[16];
    std::snprintf(buf, sizeof buf, "VK_0x%02X", vk);
    return buf;
}

} // namespace

bool Hotkey::parse(const std::string& text, Hotkey& out, std::string& error)
{
    Hotkey hk;
    std::vector<std::string> parts;
    std::string cur;
    // '+' разделитель, но "Ctrl++" означает клавишу '+'/'='.
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '+' && !console::trim(cur).empty()) {
            parts.push_back(console::trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!console::trim(cur).empty()) parts.push_back(console::trim(cur));
    if (parts.empty()) {
        error = "пустое сочетание клавиш";
        return false;
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        std::string p = console::toLower(parts[i]);
        bool last = i + 1 == parts.size();
        if (!last) {
            if (p == "ctrl" || p == "control") hk.mods |= ModCtrl;
            else if (p == "alt") hk.mods |= ModAlt;
            else if (p == "shift") hk.mods |= ModShift;
            else if (p == "win" || p == "super" || p == "meta") hk.mods |= ModWin;
            else {
                error = "неизвестный модификатор \"" + parts[i] + "\" в \"" + text + "\"";
                return false;
            }
            continue;
        }
        if (p == "+") p = "plus";
        auto it = keyTable().find(p);
        if (it == keyTable().end()) {
            error = "неизвестная клавиша \"" + parts[i] + "\" в \"" + text + "\"";
            return false;
        }
        hk.vk = it->second;
    }
    out = hk;
    return true;
}

std::string Hotkey::toString() const
{
    std::string s;
    if (mods & ModCtrl) s += "Ctrl+";
    if (mods & ModAlt) s += "Alt+";
    if (mods & ModShift) s += "Shift+";
    if (mods & ModWin) s += "Win+";
    return s + keyName(vk);
}
