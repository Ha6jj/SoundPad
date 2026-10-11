#pragma once

#include <cstdint>
#include <string>

// Модификаторы совпадают со значениями MOD_* из WinAPI.
enum HotkeyMod : uint32_t {
    ModAlt = 0x0001,
    ModCtrl = 0x0002,
    ModShift = 0x0004,
    ModWin = 0x0008,
};

struct Hotkey {
    uint32_t mods = 0;
    uint32_t vk = 0;  // Windows virtual-key code

    bool operator==(const Hotkey& o) const { return mods == o.mods && vk == o.vk; }
    bool operator<(const Hotkey& o) const { return mods != o.mods ? mods < o.mods : vk < o.vk; }

    // Разбирает строки вида "Ctrl+Alt+F1", "shift + num5", "Ctrl+Space".
    static bool parse(const std::string& text, Hotkey& out, std::string& error);
    std::string toString() const;
};
