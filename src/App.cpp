#include "App.h"

#include "AudioFormats.h"
#include "Console.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::mutex g_outMutex;

// Вывод из разных потоков (консоль и хоткеи) не перемешивается.
void say(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::cout << text << '\n' << std::flush;
}

void sayErr(const std::string& text)
{
    say("! " + text);
}

size_t utf8Width(const std::string& s)
{
    return static_cast<size_t>(std::count_if(s.begin(), s.end(),
                                             [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string pad(const std::string& s, size_t width)
{
    size_t w = utf8Width(s);
    return w >= width ? s : s + std::string(width - w, ' ');
}

std::string formatDuration(int64_t ms)
{
    std::ostringstream o;
    o << ms / 60000 << ':' << std::setw(2) << std::setfill('0') << (ms / 1000) % 60 << '.'
      << (ms / 100) % 10;
    return o.str();
}

bool parseInt(const std::string& s, int& out)
{
    try {
        size_t pos = 0;
        int v = std::stoi(s, &pos);
        if (pos != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

App::App(fs::path configPath) : configPath_(std::move(configPath))
{
    commands_ = {
        {"help",    {"help", "список команд", &App::cmdHelp}},
        {"list",    {"list", "все звуки в базе", &App::cmdList}},
        {"add",     {"add <файл> [имя]", "добавить звук в базу", &App::cmdAdd}},
        {"import",  {"import <папка>", "добавить все поддерживаемые файлы из папки", &App::cmdImport}},
        {"remove",  {"remove <имя|id>", "удалить звук из базы (файл не трогается)", &App::cmdRemove}},
        {"rename",  {"rename <имя|id> <новое имя>", "переименовать звук", &App::cmdRename}},
        {"info",    {"info <имя|id>", "подробности о звуке", &App::cmdInfo}},
        {"play",    {"play <имя|id>", "проиграть звук", &App::cmdPlay}},
        {"stop",    {"stop", "остановить все звуки", &App::cmdStop}},
        {"volume",  {"volume [0-100]", "общая громкость", &App::cmdVolume}},
        {"gain",    {"gain <имя|id> <0-200>", "громкость отдельного звука, %", &App::cmdGain}},
        {"binds",   {"binds", "активные горячие клавиши", &App::cmdBinds}},
        {"reload",  {"reload", "перечитать конфиг и бинды", &App::cmdReload}},
        {"devices", {"devices", "устройства вывода звука", &App::cmdDevices}},
        {"formats", {"formats", "поддерживаемые форматы", &App::cmdFormats}},
    };
    aliases_ = {{"?", "help"}, {"ls", "list"}, {"rm", "remove"}, {"del", "remove"},
                {"p", "play"}, {"s", "stop"}, {"vol", "volume"}, {"mv", "rename"}};
}

int App::run()
{
    say("SoundPad — консольный саундпад. Введите \"help\" для списка команд.");

    if (!fs::exists(configPath_)) {
        std::string err;
        if (Config::writeDefault(configPath_, err))
            say("Создан конфиг по умолчанию: " + pathToUtf8(fs::absolute(configPath_)));
        else
            sayErr(err);
    }
    if (!loadConfig()) return 1;

    db_.open(config_.database);
    say("База данных: " + pathToUtf8(fs::absolute(config_.database)) + " (звуков: " +
        std::to_string(db_.list().size()) + ")");

    std::vector<std::string> warnings;
    if (audio_.init(config_.outputs, warnings)) {
        audio_.setMasterVolume(config_.volume / 100.0f);
        for (const auto& name : audio_.activeDeviceNames()) say("Вывод: " + name);
    }
    for (const auto& w : warnings) sayErr(w);
    if (!audio_.ready()) sayErr("звук недоступен — работают только команды управления базой");

    applyBinds();

    std::string line;
    while (running_) {
        {
            std::lock_guard<std::mutex> lock(g_outMutex);
            std::cout << "> " << std::flush;
        }
        if (!console::readLine(line)) break;
        auto args = console::splitArgs(line);
        if (args.empty()) continue;

        std::string name = console::toLower(args[0]);
        if (name == "exit" || name == "quit" || name == "q") break;
        if (auto a = aliases_.find(name); a != aliases_.end()) name = a->second;

        auto it = commands_.find(name);
        if (it == commands_.end()) {
            sayErr("неизвестная команда \"" + args[0] + "\", см. help");
            continue;
        }
        args.erase(args.begin());
        try {
            (this->*it->second.handler)(args);
        } catch (const DbError& e) {
            sayErr(std::string("БД: ") + e.what());
        } catch (const std::exception& e) {
            sayErr(e.what());
        }
    }

    hotkeys_.stop();
    audio_.shutdown();
    db_.close();
    say("Пока!");
    return 0;
}

bool App::loadConfig()
{
    std::vector<std::string> warnings;
    std::string err;
    Config cfg;
    if (!Config::load(configPath_, cfg, warnings, err)) {
        sayErr(err);
        return false;
    }
    for (const auto& w : warnings) sayErr(w);
    config_ = std::move(cfg);
    say("Конфиг: " + pathToUtf8(fs::absolute(configPath_)) + " (биндов: " +
        std::to_string(config_.binds.size()) + ")");
    return true;
}

void App::applyBinds()
{
    hotkeys_.stop();
    {
        std::lock_guard<std::mutex> lock(bindsMutex_);
        activeBinds_ = config_.binds;
    }
    // Заранее предупреждаем о биндах на несуществующие звуки: звук могут
    // добавить позже, поэтому бинд всё равно регистрируется.
    for (const auto& b : config_.binds)
        if (b.action == Bind::Action::PlaySound && !db_.resolve(b.sound))
            sayErr(b.hotkey.toString() + ": звука \"" + b.sound + "\" нет в базе");

    if (config_.binds.empty()) return;
    if (!HotkeyListener::supported()) {
        sayErr("глобальные хоткеи доступны только в Windows-сборке");
        return;
    }
    std::vector<Hotkey> keys;
    for (const auto& b : config_.binds) keys.push_back(b.hotkey);
    std::vector<std::string> failed;
    hotkeys_.start(keys, [this](size_t i) { onHotkey(i); }, failed);
    for (const auto& f : failed) sayErr(f);
}

void App::onHotkey(size_t index)
{
    Bind b;
    {
        std::lock_guard<std::mutex> lock(bindsMutex_);
        if (index >= activeBinds_.size()) return;
        b = activeBinds_[index];
    }
    try {
        switch (b.action) {
        case Bind::Action::StopAll:
            audio_.stopAll();
            say("[" + b.hotkey.toString() + "] стоп");
            break;
        case Bind::Action::VolumeUp:
        case Bind::Action::VolumeDown: {
            float step = b.action == Bind::Action::VolumeUp ? 0.1f : -0.1f;
            audio_.setMasterVolume(audio_.masterVolume() + step);
            say("[" + b.hotkey.toString() + "] громкость " +
                std::to_string(static_cast<int>(audio_.masterVolume() * 100 + 0.5f)) + "%");
            break;
        }
        case Bind::Action::PlaySound: {
            auto rec = db_.resolve(b.sound);
            if (!rec) {
                sayErr("[" + b.hotkey.toString() + "] звука \"" + b.sound + "\" нет в базе");
                break;
            }
            std::string err;
            if (playRecord(*rec, err))
                say("[" + b.hotkey.toString() + "] ▶ " + rec->name);
            else
                sayErr("[" + b.hotkey.toString() + "] " + rec->name + ": " + err);
            break;
        }
        }
    } catch (const std::exception& e) {
        sayErr(std::string("[hotkey] ") + e.what());
    }
}

bool App::playRecord(const SoundRecord& rec, std::string& error)
{
    fs::path file = pathFromUtf8(rec.path);
    if (!fs::exists(file)) {
        error = "файл не найден: " + rec.path;
        return false;
    }
    const FormatInfo* fi = FormatRegistry::findById(rec.format);
    AudioFormat fmt = fi ? fi->format : AudioFormat::Unknown;
    if (!audio_.play(file, fmt, rec.volume / 100.0f, config_.overlap, error)) return false;
    db_.markPlayed(rec.id);
    return true;
}

std::optional<SoundRecord> App::resolveOrComplain(const std::string& ref)
{
    auto rec = db_.resolve(ref);
    if (!rec) sayErr("звук \"" + ref + "\" не найден (см. list)");
    return rec;
}

bool App::importFile(const fs::path& file, std::string name, bool quiet)
{
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(file, ec), ec);
    if (ec || !fs::is_regular_file(abs)) {
        sayErr("нет такого файла: " + pathToUtf8(file));
        return false;
    }
    std::string absUtf8 = pathToUtf8(abs);

    if (auto existing = db_.findByPath(absUtf8)) {
        if (!quiet) sayErr("этот файл уже в базе как \"" + existing->name + "\" (#" +
                           std::to_string(existing->id) + ")");
        return false;
    }

    AudioFormat fmt = FormatRegistry::detect(abs);
    if (fmt == AudioFormat::Unknown) {
        if (!quiet) sayErr("формат не поддерживается: " + absUtf8 + " (см. formats)");
        return false;
    }
    const FormatInfo* fi = FormatRegistry::find(fmt);

    DecodedInfo info;
    std::string err;
    if (!AudioEngine::probe(abs, fmt, info, err)) {
        sayErr(pathToUtf8(abs.filename()) + ": " + err);
        return false;
    }

    if (name.empty()) name = pathToUtf8(abs.stem());
    // Имя должно быть уникальным: при импорте папки добавляем суффикс.
    if (db_.findByName(name)) {
        if (!quiet) {
            sayErr("имя \"" + name + "\" уже занято, укажите другое: add <файл> <имя>");
            return false;
        }
        std::string base = name;
        for (int i = 2; db_.findByName(name); ++i) name = base + " (" + std::to_string(i) + ")";
    }
    if (name.find_first_not_of("0123456789#") == std::string::npos) {
        sayErr("имя \"" + name + "\" похоже на id; выберите имя с буквами");
        return false;
    }

    SoundRecord rec;
    rec.name = name;
    rec.path = absUtf8;
    rec.format = fi->id;
    rec.durationMs = static_cast<int64_t>(info.durationMs);
    rec.channels = static_cast<int>(info.channels);
    rec.sampleRate = static_cast<int>(info.sampleRate);
    int64_t id = db_.add(rec);
    say("+ #" + std::to_string(id) + " " + name + "  [" + fi->id + ", " +
        formatDuration(rec.durationMs) + "]");
    return true;
}

void App::cmdHelp(const Args&)
{
    std::ostringstream o;
    o << "Команды:\n";
    for (const auto& [_, c] : commands_) o << "  " << pad(c.usage, 30) << c.help << '\n';
    o << "  " << pad("exit", 30) << "выход\n";
    o << "Сокращения: ls, rm, mv, p (play), s (stop), vol. "
         "Имена с пробелами берите в кавычки: play \"sad trombone\"";
    say(o.str());
}

void App::cmdList(const Args&)
{
    auto all = db_.list();
    if (all.empty()) {
        say("База пуста. Добавьте звук: add <файл> [имя]");
        return;
    }
    // Хоткеи для каждого звука, чтобы видеть их прямо в списке.
    std::map<int64_t, std::string> keys;
    for (const auto& b : config_.binds) {
        if (b.action != Bind::Action::PlaySound) continue;
        if (auto r = db_.resolve(b.sound)) {
            auto& k = keys[r->id];
            k += (k.empty() ? "" : ", ") + b.hotkey.toString();
        }
    }
    size_t nameW = 4;
    for (const auto& r : all) nameW = std::max(nameW, utf8Width(r.name));

    std::ostringstream o;
    o << pad("id", 5) << pad("имя", nameW + 2) << pad("формат", 8) << pad("длит.", 9)
      << pad("громк.", 8) << pad("игр", 6) << "хоткей\n";
    for (const auto& r : all) {
        bool missing = !fs::exists(pathFromUtf8(r.path));
        o << pad(std::to_string(r.id), 5) << pad(r.name, nameW + 2) << pad(r.format, 8)
          << pad(formatDuration(r.durationMs), 9) << pad(std::to_string(r.volume) + "%", 8)
          << pad(std::to_string(r.playCount), 6) << keys[r.id]
          << (missing ? "  (файл не найден!)" : "") << '\n';
    }
    std::string s = o.str();
    s.pop_back();
    say(s);
}

void App::cmdAdd(const Args& a)
{
    if (a.empty() || a.size() > 2) {
        sayErr("использование: add <файл> [имя]");
        return;
    }
    importFile(pathFromUtf8(a[0]), a.size() > 1 ? a[1] : "", false);
}

void App::cmdImport(const Args& a)
{
    if (a.size() != 1) {
        sayErr("использование: import <папка>");
        return;
    }
    fs::path dir = pathFromUtf8(a[0]);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        sayErr("нет такой папки: " + a[0]);
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        if (e.is_regular_file() && FormatRegistry::isSupportedExtension(e.path())) files.push_back(e.path());
    std::sort(files.begin(), files.end());

    int added = 0;
    for (const auto& f : files)
        if (importFile(f, "", true)) ++added;
    say("Добавлено " + std::to_string(added) + " из " + std::to_string(files.size()) +
        " найденных файлов (уже существующие пропущены).");
}

void App::cmdRemove(const Args& a)
{
    if (a.size() != 1) {
        sayErr("использование: remove <имя|id>");
        return;
    }
    if (auto r = resolveOrComplain(a[0]); r && db_.remove(r->id)) say("- #" + std::to_string(r->id) + " " + r->name);
}

void App::cmdRename(const Args& a)
{
    if (a.size() != 2) {
        sayErr("использование: rename <имя|id> <новое имя>");
        return;
    }
    if (a[1].find_first_not_of("0123456789#") == std::string::npos) {
        sayErr("имя не может состоять только из цифр");
        return;
    }
    if (auto r = resolveOrComplain(a[0]); r && db_.rename(r->id, a[1]))
        say(r->name + " -> " + a[1] + "  (не забудьте обновить бинды, если они ссылались на старое имя)");
}

void App::cmdInfo(const Args& a)
{
    if (a.size() != 1) {
        sayErr("использование: info <имя|id>");
        return;
    }
    auto r = resolveOrComplain(a[0]);
    if (!r) return;
    const FormatInfo* fi = FormatRegistry::findById(r->format);
    std::ostringstream o;
    o << "#" << r->id << " " << r->name << '\n'
      << "  файл:       " << r->path << (fs::exists(pathFromUtf8(r->path)) ? "" : "  (не найден!)") << '\n'
      << "  формат:     " << (fi ? fi->displayName : r->format) << '\n'
      << "  длительность: " << formatDuration(r->durationMs) << ", " << r->channels << " кан., "
      << r->sampleRate << " Гц\n"
      << "  громкость:  " << r->volume << "%\n"
      << "  проигран:   " << r->playCount << " раз"
      << (r->lastPlayedAt.empty() ? "" : ", последний — " + r->lastPlayedAt) << '\n'
      << "  добавлен:   " << r->createdAt;
    say(o.str());
}

void App::cmdPlay(const Args& a)
{
    if (a.size() != 1) {
        sayErr("использование: play <имя|id>");
        return;
    }
    auto r = resolveOrComplain(a[0]);
    if (!r) return;
    std::string err;
    if (playRecord(*r, err))
        say("▶ " + r->name);
    else
        sayErr(err);
}

void App::cmdStop(const Args&)
{
    audio_.stopAll();
    say("■ стоп");
}

void App::cmdVolume(const Args& a)
{
    if (!a.empty()) {
        int v;
        if (!parseInt(a[0], v) || v < 0 || v > 100) {
            sayErr("громкость — число от 0 до 100");
            return;
        }
        audio_.setMasterVolume(v / 100.0f);
    }
    say("Громкость: " + std::to_string(static_cast<int>(audio_.masterVolume() * 100 + 0.5f)) + "%");
}

void App::cmdGain(const Args& a)
{
    int v;
    if (a.size() != 2 || !parseInt(a[1], v) || v < 0 || v > 200) {
        sayErr("использование: gain <имя|id> <0-200>");
        return;
    }
    if (auto r = resolveOrComplain(a[0]); r && db_.setVolume(r->id, v))
        say(r->name + ": громкость " + std::to_string(v) + "%");
}

void App::cmdBinds(const Args&)
{
    if (config_.binds.empty()) {
        say("Биндов нет. Добавьте их в секцию [binds] файла " + pathToUtf8(configPath_) + " и выполните reload.");
        return;
    }
    std::ostringstream o;
    for (const auto& b : config_.binds) {
        o << "  " << pad(b.hotkey.toString(), 22);
        switch (b.action) {
        case Bind::Action::StopAll: o << "[остановить всё]"; break;
        case Bind::Action::VolumeUp: o << "[громкость +10%]"; break;
        case Bind::Action::VolumeDown: o << "[громкость -10%]"; break;
        case Bind::Action::PlaySound: {
            auto r = db_.resolve(b.sound);
            o << b.sound << (r ? "" : "  (нет в базе!)");
            break;
        }
        }
        o << '\n';
    }
    std::string s = o.str();
    s.pop_back();
    say(s);
}

void App::cmdReload(const Args&)
{
    fs::path oldDb = config_.database;
    auto oldOutputs = config_.outputs;
    if (!loadConfig()) return;

    if (config_.database != oldDb) {
        db_.open(config_.database);
        say("База данных: " + pathToUtf8(fs::absolute(config_.database)));
    }
    if (config_.outputs != oldOutputs || !audio_.ready()) {
        std::vector<std::string> warnings;
        audio_.init(config_.outputs, warnings);
        for (const auto& w : warnings) sayErr(w);
        for (const auto& name : audio_.activeDeviceNames()) say("Вывод: " + name);
    }
    audio_.setMasterVolume(config_.volume / 100.0f);
    applyBinds();
    say("Конфиг перечитан.");
}

void App::cmdDevices(const Args&)
{
    auto devices = audio_.listDevices();
    if (devices.empty()) {
        say("Устройства вывода не найдены.");
        return;
    }
    auto active = audio_.activeDeviceNames();
    std::ostringstream o;
    o << "Устройства вывода (в конфиге outputs = часть имени):\n";
    for (const auto& d : devices) {
        bool used = std::any_of(active.begin(), active.end(), [&](const std::string& n) {
            return n == d.name || n == "default (" + d.name + ")";
        });
        o << (used ? "  * " : "    ") << d.name << (d.isDefault ? "  [по умолчанию]" : "") << '\n';
    }
    std::string s = o.str();
    s.pop_back();
    say(s);
}

void App::cmdFormats(const Args&)
{
    std::ostringstream o;
    o << "Поддерживаемые форматы (определяются по содержимому файла, затем по расширению):";
    for (const auto& f : FormatRegistry::all()) {
        std::string exts;
        for (const auto& e : f.extensions) exts += (exts.empty() ? "" : " ") + e;
        o << "\n  " << pad(f.id, 6) << pad(f.displayName, 26) << exts;
    }
    say(o.str());
}
