#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace console {

// Переключает консоль Windows в UTF-8. На других ОС ничего не делает.
void init();

// Читает строку из stdin в UTF-8. Возвращает false на EOF.
bool readLine(std::string& line);

// Разбивает строку на аргументы; поддерживаются "кавычки с пробелами".
std::vector<std::string> splitArgs(const std::string& line);

std::string toLower(std::string s);
std::string trim(const std::string& s);

} // namespace console

// UTF-8 <-> std::filesystem::path без потерь на Windows.
std::filesystem::path pathFromUtf8(const std::string& s);
std::string pathToUtf8(const std::filesystem::path& p);

#ifdef _WIN32
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
#endif
