#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Поддерживаемые форматы аудиофайлов. Каждый формат описан в FormatRegistry:
// расширения, сигнатура (magic bytes) и декодер miniaudio, которым его читать.
enum class AudioFormat {
    Unknown,
    Wav,
    Mp3,
    Flac,
    OggVorbis,
};

struct FormatInfo {
    AudioFormat format;
    const char* id;           // ключ для БД: "wav", "mp3", ...
    const char* displayName;  // для вывода пользователю
    std::vector<std::string> extensions;  // в нижнем регистре, с точкой
    // Проверка заголовка файла. header содержит первые байты (до 64).
    bool (*probe)(const std::vector<uint8_t>& header);
};

class FormatRegistry {
public:
    static const std::vector<FormatInfo>& all();
    static const FormatInfo* find(AudioFormat f);
    static const FormatInfo* findById(const std::string& id);
    static const FormatInfo* findByExtension(const std::string& ext);
    static bool isSupportedExtension(const std::filesystem::path& p);

    // Определяет формат файла: сначала по содержимому, затем по расширению.
    // Возвращает Unknown, если файл не открылся или формат не поддерживается.
    static AudioFormat detect(const std::filesystem::path& p);
};
