#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

struct SoundRecord {
    int64_t id = 0;
    std::string name;
    std::string path;    // UTF-8, абсолютный
    std::string format;  // FormatInfo::id
    int64_t durationMs = 0;
    int channels = 0;
    int sampleRate = 0;
    int volume = 100;    // громкость конкретного звука, 0..200 %
    int64_t playCount = 0;
    std::string createdAt;
    std::string lastPlayedAt;
};

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Хранилище звуков на SQLite. Потокобезопасно: вызывается и из консоли,
// и из потока глобальных хоткеев.
class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open(const std::filesystem::path& file);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    int64_t add(const SoundRecord& rec);  // бросает DbError, если имя занято
    std::vector<SoundRecord> list();
    std::optional<SoundRecord> findById(int64_t id);
    std::optional<SoundRecord> findByName(const std::string& name);
    std::optional<SoundRecord> findByPath(const std::string& path);
    // "#12" или "12" — id, иначе имя (без учёта регистра для ASCII).
    std::optional<SoundRecord> resolve(const std::string& ref);

    bool remove(int64_t id);
    bool rename(int64_t id, const std::string& newName);
    bool setVolume(int64_t id, int volume);
    void markPlayed(int64_t id);

private:
    void exec(const char* sql);
    void migrate();

    sqlite3* db_ = nullptr;
    std::recursive_mutex mutex_;
};
