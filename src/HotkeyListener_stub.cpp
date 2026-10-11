// Заглушка для не-Windows сборок (используется для разработки и тестов
// остальных модулей). Глобальные хоткеи здесь не поддерживаются.
#include "HotkeyListener.h"

struct HotkeyListener::Impl {};

HotkeyListener::HotkeyListener() = default;
HotkeyListener::~HotkeyListener() = default;

bool HotkeyListener::supported()
{
    return false;
}

void HotkeyListener::start(const std::vector<Hotkey>& hotkeys, Callback, std::vector<std::string>& failed)
{
    for (const auto& hk : hotkeys) failed.push_back(hk.toString() + ": глобальные хоткеи доступны только на Windows");
}

void HotkeyListener::stop() {}
