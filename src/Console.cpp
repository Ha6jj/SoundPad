#include "Console.h"

#include <algorithm>
#include <cctype>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

std::filesystem::path pathFromUtf8(const std::string& s)
{
#ifdef _WIN32
    return std::filesystem::path(widen(s));
#else
    return std::filesystem::path(s);
#endif
}

std::string pathToUtf8(const std::filesystem::path& p)
{
#ifdef _WIN32
    return narrow(p.wstring());
#else
    return p.string();
#endif
}

namespace console {

void init()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::ios::sync_with_stdio(false);
}

bool readLine(std::string& line)
{
#ifdef _WIN32
    // std::getline при CP_UTF8 ломает кириллицу на вводе — читаем UTF-16 напрямую.
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        std::wstring buf;
        wchar_t chunk[512];
        for (;;) {
            DWORD read = 0;
            if (!ReadConsoleW(in, chunk, 512, &read, nullptr) || read == 0)
                return !buf.empty() ? (line = narrow(buf), true) : false;
            buf.append(chunk, read);
            if (!buf.empty() && buf.back() == L'\n') break;
        }
        while (!buf.empty() && (buf.back() == L'\n' || buf.back() == L'\r')) buf.pop_back();
        // Ctrl+Z в начале строки — EOF.
        if (!buf.empty() && buf[0] == 0x1A) return false;
        line = narrow(buf);
        return true;
    }
#endif
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

std::vector<std::string> splitArgs(const std::string& line)
{
    std::vector<std::string> args;
    std::string cur;
    bool inQuotes = false, hasToken = false;
    for (char c : line) {
        if (c == '"') {
            inQuotes = !inQuotes;
            hasToken = true;
        } else if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
            if (hasToken) {
                args.push_back(cur);
                cur.clear();
                hasToken = false;
            }
        } else {
            cur += c;
            hasToken = true;
        }
    }
    if (hasToken) args.push_back(cur);
    return args;
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

} // namespace console
