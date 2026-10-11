#pragma once

#include "AudioFormats.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct OutputDeviceInfo {
    std::string name;
    bool isDefault = false;
};

struct DecodedInfo {
    uint64_t durationMs = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
};

// Проигрывает звуки одновременно на одно или несколько устройств вывода
// (например, на виртуальный кабель для микрофона и в наушники для себя).
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // outputs: "default" или подстроки имён устройств. Пустой список = default.
    // Возвращает false, если не удалось открыть ни одного устройства;
    // описание проблем копится в warnings.
    bool init(const std::vector<std::string>& outputs, std::vector<std::string>& warnings);
    void shutdown();
    bool ready() const;

    std::vector<OutputDeviceInfo> listDevices();
    std::vector<std::string> activeDeviceNames() const;

    // volume — громкость конкретного звука (1.0 = 100%).
    // При allowOverlap=false перед запуском останавливаются все звуки.
    bool play(const std::filesystem::path& file, AudioFormat format, float volume,
              bool allowOverlap, std::string& error);
    void stopAll();
    size_t playingCount();

    void setMasterVolume(float v);
    float masterVolume() const;

    // Открывает файл декодером указанного формата и читает его параметры.
    // Не требует рабочего устройства вывода.
    static bool probe(const std::filesystem::path& file, AudioFormat format,
                      DecodedInfo& info, std::string& error);

private:
    struct Output;
    struct Voice;

    void reapFinishedLocked();

    mutable std::mutex mutex_;
    std::unique_ptr<struct ma_context> context_;
    bool contextReady_ = false;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::vector<std::unique_ptr<Voice>> voices_;
    float masterVolume_ = 1.0f;
};
