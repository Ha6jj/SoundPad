#include "HotkeyListener.h"

#include <windows.h>

#include <future>
#include <thread>

struct HotkeyListener::Impl {
    std::thread thread;
    DWORD threadId = 0;
};

HotkeyListener::HotkeyListener() : impl_(std::make_unique<Impl>()) {}

HotkeyListener::~HotkeyListener()
{
    stop();
}

bool HotkeyListener::supported()
{
    return true;
}

void HotkeyListener::start(const std::vector<Hotkey>& hotkeys, Callback cb, std::vector<std::string>& failed)
{
    stop();

    // RegisterHotKey привязывает хоткей к потоку, который его вызвал, поэтому
    // регистрация и цикл сообщений живут в одном потоке. Результат регистрации
    // передаём обратно через promise.
    std::promise<std::pair<DWORD, std::vector<std::string>>> ready;
    auto readyFuture = ready.get_future();

    impl_->thread = std::thread([hotkeys, cb = std::move(cb), &ready]() {
        MSG msg;
        // Создаём очередь сообщений потока до того, как кто-то пришлёт WM_QUIT.
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        std::vector<std::string> errors;
        std::vector<int> registered;
        for (size_t i = 0; i < hotkeys.size(); ++i) {
            int id = static_cast<int>(i) + 1;
            if (RegisterHotKey(nullptr, id, hotkeys[i].mods | MOD_NOREPEAT, hotkeys[i].vk))
                registered.push_back(id);
            else
                errors.push_back(hotkeys[i].toString() + ": " +
                                 (GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED
                                      ? "уже занято другой программой"
                                      : "ошибка RegisterHotKey " + std::to_string(GetLastError())));
        }
        ready.set_value({GetCurrentThreadId(), std::move(errors)});

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_HOTKEY && msg.wParam >= 1 && msg.wParam <= hotkeys.size())
                cb(static_cast<size_t>(msg.wParam - 1));
        }

        for (int id : registered) UnregisterHotKey(nullptr, id);
    });

    auto [tid, errors] = readyFuture.get();
    impl_->threadId = tid;
    failed.insert(failed.end(), errors.begin(), errors.end());
}

void HotkeyListener::stop()
{
    if (!impl_->thread.joinable()) return;
    PostThreadMessageW(impl_->threadId, WM_QUIT, 0, 0);
    impl_->thread.join();
    impl_->threadId = 0;
}
