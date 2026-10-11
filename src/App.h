#pragma once

#include "AudioEngine.h"
#include "Config.h"
#include "Database.h"
#include "HotkeyListener.h"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class App {
public:
    explicit App(std::filesystem::path configPath);
    int run();

private:
    using Args = std::vector<std::string>;
    struct Command {
        std::string usage;
        std::string help;
        void (App::*handler)(const Args&);
    };

    bool loadConfig();
    void applyBinds();
    void onHotkey(size_t index);
    bool playRecord(const SoundRecord& rec, std::string& error);
    std::optional<SoundRecord> resolveOrComplain(const std::string& ref);
    bool importFile(const std::filesystem::path& file, std::string name, bool quiet);

    void cmdHelp(const Args&);
    void cmdList(const Args&);
    void cmdAdd(const Args&);
    void cmdImport(const Args&);
    void cmdRemove(const Args&);
    void cmdRename(const Args&);
    void cmdInfo(const Args&);
    void cmdPlay(const Args&);
    void cmdStop(const Args&);
    void cmdVolume(const Args&);
    void cmdGain(const Args&);
    void cmdBinds(const Args&);
    void cmdReload(const Args&);
    void cmdDevices(const Args&);
    void cmdFormats(const Args&);

    std::filesystem::path configPath_;
    Config config_;
    Database db_;
    AudioEngine audio_;
    HotkeyListener hotkeys_;
    std::vector<Bind> activeBinds_;
    std::mutex bindsMutex_;
    std::map<std::string, Command> commands_;
    std::map<std::string, std::string> aliases_;
    bool running_ = true;
};
