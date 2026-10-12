#include "Config.h"

#include "Console.h"

#include <fstream>
#include <map>
#include <sstream>

namespace {

const char* kDefaultConfig = R"(; Конфигурация консольного SoundPad.
; Комментарии: строки, начинающиеся с ';' или '#', и всё после " ;".

[settings]
; Файл базы данных (путь относительно этого конфига).
database = soundpad.db

; Устройства вывода через ';'. "default" — системное по умолчанию.
; Остальное — часть имени устройства (см. команду "devices").
;
; Чтобы звуки слышали в игре / Discord, нужен виртуальный кабель
; (VB-Audio Virtual Cable, https://vb-audio.com/Cable/):
;   outputs    = CABLE Input; default   <- в кабель и себе в наушники
;   microphone = default                <- ваш настоящий микрофон
; а в Discord / игре выберите устройством ввода "CABLE Output".
outputs = default

; Настоящий микрофон, который подмешивается в кабель вместе со звуками,
; чтобы собеседники слышали и вас, и саундпад.
; none — не подмешивать, default — микрофон по умолчанию, иначе часть имени.
microphone = none

; Устройство из outputs, в которое идёт микрофон (по умолчанию — первое в outputs).
; mic_output = CABLE Input

; Громкость микрофона 0..200.
mic_volume = 100

; Общая громкость 0..100.
volume = 80

; true — звуки накладываются друг на друга, false — новый звук обрывает старый.
overlap = true

[binds]
; Сочетание = имя звука из базы (или #id).
; Модификаторы: Ctrl, Alt, Shift, Win. Клавиши: A-Z, 0-9, F1-F24, Num0-Num9,
; Space, Enter, Tab, Esc, Insert, Delete, Home, End, PageUp, PageDown,
; Up, Down, Left, Right, Pause, NumAdd, NumSub, NumMul, NumDiv, NumDecimal.
; Специальные действия: !stop, !volume+, !volume-, !mic (выкл/вкл микрофон)
;
; Ctrl+Alt+1 = airhorn
; Ctrl+Alt+2 = "sad trombone"
; Ctrl+Alt+3 = #5
Ctrl+Alt+0 = !stop
Ctrl+Alt+Up = !volume+
Ctrl+Alt+Down = !volume-
Ctrl+Alt+M = !mic
)";

std::string unquote(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

// Комментарии: строка, начинающаяся с ';' или '#', либо " ;" в конце строки.
// '#' внутри строки не комментарий — им записываются ссылки на id ("#5").
std::string stripComment(const std::string& line)
{
    std::string t = console::trim(line);
    if (!t.empty() && (t[0] == ';' || t[0] == '#')) return {};
    bool inQuotes = false;
    for (size_t i = 1; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') inQuotes = !inQuotes;
        if (!inQuotes && c == ';' && (line[i - 1] == ' ' || line[i - 1] == '\t')) return line.substr(0, i);
    }
    return line;
}

bool parseBool(const std::string& v, bool& out)
{
    std::string s = console::toLower(v);
    if (s == "true" || s == "yes" || s == "on" || s == "1") return out = true, true;
    if (s == "false" || s == "no" || s == "off" || s == "0") return out = false, true;
    return false;
}

} // namespace

bool Config::load(const std::filesystem::path& file, Config& out,
                  std::vector<std::string>& warnings, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "не удалось открыть " + pathToUtf8(file);
        return false;
    }

    Config cfg;
    cfg.path = file;
    std::map<Hotkey, int> seen;
    std::string section, raw;
    int lineNo = 0;
    auto warn = [&](const std::string& msg) {
        warnings.push_back(pathToUtf8(file.filename()) + ":" + std::to_string(lineNo) + ": " + msg);
    };

    while (std::getline(in, raw)) {
        ++lineNo;
        if (lineNo == 1 && raw.rfind("\xEF\xBB\xBF", 0) == 0) raw.erase(0, 3);  // UTF-8 BOM
        std::string line = console::trim(stripComment(raw));
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                warn("незакрытая секция");
                continue;
            }
            section = console::toLower(console::trim(line.substr(1, line.size() - 2)));
            if (section != "settings" && section != "binds") warn("неизвестная секция [" + section + "]");
            continue;
        }

        auto eq = line.find('=', 1);  // с 1: клавиша "=" допустима как "Ctrl+="
        // Для "Ctrl+= = sound" первым '=' будет клавиша, ищем разделитель с пробелами.
        auto spaced = line.find(" = ");
        if (spaced != std::string::npos) eq = spaced + 1;
        if (eq == std::string::npos) {
            warn("ожидалось \"ключ = значение\"");
            continue;
        }
        std::string key = console::trim(line.substr(0, eq));
        std::string value = unquote(console::trim(line.substr(eq + 1)));

        if (section == "settings") {
            std::string k = console::toLower(key);
            if (k == "database") {
                cfg.database = pathFromUtf8(value);
            } else if (k == "outputs" || k == "output") {
                cfg.outputs.clear();
                std::stringstream ss(value);
                std::string item;
                while (std::getline(ss, item, ';'))
                    if (!console::trim(item).empty()) cfg.outputs.push_back(unquote(console::trim(item)));
                if (cfg.outputs.empty()) cfg.outputs.push_back("default");
            } else if (k == "volume") {
                try {
                    int v = std::stoi(value);
                    if (v < 0 || v > 100) throw std::out_of_range("volume");
                    cfg.volume = v;
                } catch (...) {
                    warn("volume должен быть числом 0..100");
                }
            } else if (k == "microphone" || k == "mic") {
                cfg.microphone = value.empty() ? "none" : value;
            } else if (k == "mic_output") {
                cfg.micOutput = value;
            } else if (k == "mic_volume") {
                try {
                    int v = std::stoi(value);
                    if (v < 0 || v > 200) throw std::out_of_range("mic_volume");
                    cfg.micVolume = v;
                } catch (...) {
                    warn("mic_volume должен быть числом 0..200");
                }
            } else if (k == "overlap") {
                if (!parseBool(value, cfg.overlap)) warn("overlap должен быть true/false");
            } else {
                warn("неизвестный параметр \"" + key + "\"");
            }
        } else if (section == "binds") {
            Bind b;
            b.line = lineNo;
            std::string err;
            if (!Hotkey::parse(key, b.hotkey, err)) {
                warn(err);
                continue;
            }
            if (value.empty()) {
                warn("не указан звук для " + b.hotkey.toString());
                continue;
            }
            std::string lv = console::toLower(value);
            if (lv == "!stop") b.action = Bind::Action::StopAll;
            else if (lv == "!volume+") b.action = Bind::Action::VolumeUp;
            else if (lv == "!volume-") b.action = Bind::Action::VolumeDown;
            else if (lv == "!mic") b.action = Bind::Action::MicToggle;
            else if (lv[0] == '!') {
                warn("неизвестное действие " + value);
                continue;
            } else b.sound = value;

            auto [it, inserted] = seen.emplace(b.hotkey, lineNo);
            if (!inserted) {
                warn(b.hotkey.toString() + " уже назначено в строке " + std::to_string(it->second));
                continue;
            }
            cfg.binds.push_back(b);
        } else {
            warn("параметр вне секции");
        }
    }

    if (cfg.micOutput.empty()) cfg.micOutput = cfg.outputs.front();
    if (cfg.database.is_relative()) cfg.database = file.parent_path() / cfg.database;
    out = std::move(cfg);
    return true;
}

bool Config::writeDefault(const std::filesystem::path& file, std::string& error)
{
    std::ofstream o(file, std::ios::binary);
    if (!o) {
        error = "не удалось создать " + pathToUtf8(file);
        return false;
    }
    o << kDefaultConfig;
    return static_cast<bool>(o);
}
