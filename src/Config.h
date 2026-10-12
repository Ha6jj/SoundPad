#pragma once

#include "Hotkey.h"

#include <filesystem>
#include <string>
#include <vector>

// Действие, назначенное на сочетание клавиш.
struct Bind {
    Hotkey hotkey;
    enum class Action { PlaySound, StopAll, VolumeUp, VolumeDown, MicToggle } action = Action::PlaySound;
    std::string sound;  // имя или #id записи в БД для PlaySound
    int line = 0;       // строка в конфиге, для сообщений об ошибках
};

struct Config {
    std::filesystem::path path;  // откуда загружен

    std::filesystem::path database = "soundpad.db";
    std::vector<std::string> outputs = {"default"};
    int volume = 80;           // 0..100
    bool overlap = true;       // разрешать одновременное проигрывание

    // Проброс микрофона в виртуальный кабель (для игр и Discord).
    std::string microphone = "none";  // none | default | часть имени
    std::string micOutput;            // пусто = первое устройство из outputs
    int micVolume = 100;              // 0..200
    std::vector<Bind> binds;

    // Загружает INI-файл. Ошибки в отдельных строках не фатальны:
    // строка пропускается, описание кладётся в warnings.
    static bool load(const std::filesystem::path& file, Config& out,
                     std::vector<std::string>& warnings, std::string& error);

    // Пишет конфиг-пример, если файла нет.
    static bool writeDefault(const std::filesystem::path& file, std::string& error);
};
