#pragma once

#include "Hotkey.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Глобальные (системные) хоткеи: срабатывают, даже когда окно консоли
// не в фокусе — например, во время игры. На Windows работает через
// RegisterHotKey в отдельном потоке с очередью сообщений.
class HotkeyListener {
public:
    // Вызывается из потока слушателя с индексом хоткея в переданном списке.
    using Callback = std::function<void(size_t index)>;

    HotkeyListener();
    ~HotkeyListener();
    HotkeyListener(const HotkeyListener&) = delete;
    HotkeyListener& operator=(const HotkeyListener&) = delete;

    static bool supported();

    // Регистрирует хоткеи (заменяя предыдущие). Для каждого хоткея, который
    // не удалось занять (например, он используется другой программой),
    // в failed кладётся описание.
    void start(const std::vector<Hotkey>& hotkeys, Callback cb, std::vector<std::string>& failed);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
