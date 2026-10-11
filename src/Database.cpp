#include "Database.h"

#include "Console.h"

#include <sqlite3.h>

#include <cctype>

namespace {

// RAII-обёртка над подготовленным запросом.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw DbError(std::string("SQL: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, int64_t v) { check(sqlite3_bind_int64(stmt_, i, v)); return *this; }
    Stmt& bind(int i, int v) { check(sqlite3_bind_int(stmt_, i, v)); return *this; }
    Stmt& bind(int i, const std::string& v)
    {
        check(sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }

    // true — есть строка, false — запрос завершён.
    bool step()
    {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(sqlite3_errmsg(db_));
    }

    int64_t i64(int col) const { return sqlite3_column_int64(stmt_, col); }
    int i32(int col) const { return sqlite3_column_int(stmt_, col); }
    std::string text(int col) const
    {
        auto p = sqlite3_column_text(stmt_, col);
        return p ? reinterpret_cast<const char*>(p) : "";
    }

private:
    void check(int rc)
    {
        if (rc != SQLITE_OK) throw DbError(sqlite3_errmsg(db_));
    }

    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

const char* kSelect =
    "SELECT id, name, path, format, duration_ms, channels, sample_rate, volume, play_count,"
    " created_at, COALESCE(last_played_at, '') FROM sounds ";

SoundRecord readRecord(const Stmt& s)
{
    SoundRecord r;
    r.id = s.i64(0);
    r.name = s.text(1);
    r.path = s.text(2);
    r.format = s.text(3);
    r.durationMs = s.i64(4);
    r.channels = s.i32(5);
    r.sampleRate = s.i32(6);
    r.volume = s.i32(7);
    r.playCount = s.i64(8);
    r.createdAt = s.text(9);
    r.lastPlayedAt = s.text(10);
    return r;
}

std::optional<SoundRecord> one(Stmt& s)
{
    if (s.step()) return readRecord(s);
    return std::nullopt;
}

} // namespace

Database::~Database()
{
    close();
}

void Database::open(const std::filesystem::path& file)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    close();
    // sqlite3_open_v2 принимает путь в UTF-8 на всех платформах.
    int rc = sqlite3_open_v2(pathToUtf8(file).c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        close();
        throw DbError("не удалось открыть БД " + pathToUtf8(file) + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 2000);
    exec("PRAGMA journal_mode=WAL;");
    migrate();
}

void Database::close()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Database::exec(const char* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DbError(msg);
    }
}

void Database::migrate()
{
    Stmt v(db_, "PRAGMA user_version;");
    v.step();
    int version = v.i32(0);

    if (version < 1) {
        exec("BEGIN;"
             "CREATE TABLE IF NOT EXISTS sounds ("
             "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  name           TEXT NOT NULL UNIQUE COLLATE NOCASE,"
             "  path           TEXT NOT NULL,"
             "  format         TEXT NOT NULL,"
             "  duration_ms    INTEGER NOT NULL DEFAULT 0,"
             "  channels       INTEGER NOT NULL DEFAULT 0,"
             "  sample_rate    INTEGER NOT NULL DEFAULT 0,"
             "  volume         INTEGER NOT NULL DEFAULT 100 CHECK (volume BETWEEN 0 AND 200),"
             "  play_count     INTEGER NOT NULL DEFAULT 0,"
             "  created_at     TEXT NOT NULL DEFAULT (datetime('now', 'localtime')),"
             "  last_played_at TEXT"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_sounds_path ON sounds(path);"
             "PRAGMA user_version = 1;"
             "COMMIT;");
    }
    // Новые версии схемы добавляются сюда: if (version < 2) { ... }
}

int64_t Database::add(const SoundRecord& rec)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (findByName(rec.name)) throw DbError("звук с именем \"" + rec.name + "\" уже есть");
    Stmt s(db_, "INSERT INTO sounds(name, path, format, duration_ms, channels, sample_rate, volume)"
                " VALUES (?, ?, ?, ?, ?, ?, ?);");
    s.bind(1, rec.name).bind(2, rec.path).bind(3, rec.format).bind(4, rec.durationMs)
        .bind(5, rec.channels).bind(6, rec.sampleRate).bind(7, rec.volume);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

std::vector<SoundRecord> Database::list()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, (std::string(kSelect) + "ORDER BY id;").c_str());
    std::vector<SoundRecord> out;
    while (s.step()) out.push_back(readRecord(s));
    return out;
}

std::optional<SoundRecord> Database::findById(int64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, (std::string(kSelect) + "WHERE id = ?;").c_str());
    s.bind(1, id);
    return one(s);
}

std::optional<SoundRecord> Database::findByName(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, (std::string(kSelect) + "WHERE name = ?;").c_str());
    s.bind(1, name);
    return one(s);
}

std::optional<SoundRecord> Database::findByPath(const std::string& path)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, (std::string(kSelect) + "WHERE path = ?;").c_str());
    s.bind(1, path);
    return one(s);
}

std::optional<SoundRecord> Database::resolve(const std::string& ref)
{
    std::string r = ref;
    bool forceId = !r.empty() && r[0] == '#';
    if (forceId) r.erase(0, 1);
    bool numeric = !r.empty() && r.size() < 18 &&
                   r.find_first_not_of("0123456789") == std::string::npos;
    if (numeric) {
        if (auto rec = findById(std::stoll(r))) return rec;
        if (forceId) return std::nullopt;
    }
    return findByName(ref);
}

bool Database::remove(int64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, "DELETE FROM sounds WHERE id = ?;");
    s.bind(1, id).step();
    return sqlite3_changes(db_) > 0;
}

bool Database::rename(int64_t id, const std::string& newName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto other = findByName(newName);
    if (other && other->id != id) throw DbError("имя \"" + newName + "\" уже занято");
    Stmt s(db_, "UPDATE sounds SET name = ? WHERE id = ?;");
    s.bind(1, newName).bind(2, id).step();
    return sqlite3_changes(db_) > 0;
}

bool Database::setVolume(int64_t id, int volume)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, "UPDATE sounds SET volume = ? WHERE id = ?;");
    s.bind(1, volume).bind(2, id).step();
    return sqlite3_changes(db_) > 0;
}

void Database::markPlayed(int64_t id)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Stmt s(db_, "UPDATE sounds SET play_count = play_count + 1,"
                " last_played_at = datetime('now', 'localtime') WHERE id = ?;");
    s.bind(1, id).step();
}
