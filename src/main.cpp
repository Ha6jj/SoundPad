#include "App.h"
#include "Console.h"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// Аргументы командной строки в UTF-8 (на Windows argv в ANSI-кодировке).
std::vector<std::string> utf8Args(int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; i < n; ++i) out.push_back(narrow(wargv[i]));
    LocalFree(wargv);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

} // namespace

int main(int argc, char** argv)
{
    console::init();

    std::filesystem::path config = "soundpad.ini";
    auto args = utf8Args(argc, argv);
    for (size_t i = 1; i < args.size(); ++i) {
        if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
            config = pathFromUtf8(args[++i]);
        } else if (args[i] == "-h" || args[i] == "--help") {
            std::cout << "Использование: soundpad [-c|--config путь_к_конфигу.ini]\n"
                         "По умолчанию используется soundpad.ini в текущей папке;\n"
                         "если его нет, он будет создан.\n";
            return 0;
        } else {
            std::cerr << "Неизвестный аргумент: " << args[i] << "\n";
            return 2;
        }
    }

    try {
        App app(config);
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "Фатальная ошибка: " << e.what() << "\n";
        return 1;
    }
}
