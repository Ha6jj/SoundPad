#include "AudioEngine.h"

#include "Console.h"

#include "miniaudio.h"

#include <algorithm>
#include <cstdlib>

namespace {

ma_encoding_format toMaEncoding(AudioFormat f)
{
    switch (f) {
    case AudioFormat::Wav:       return ma_encoding_format_wav;
    case AudioFormat::Mp3:       return ma_encoding_format_mp3;
    case AudioFormat::Flac:      return ma_encoding_format_flac;
    case AudioFormat::OggVorbis: return ma_encoding_format_vorbis;
    case AudioFormat::Unknown:   break;
    }
    return ma_encoding_format_unknown;
}

ma_result openDecoder(const std::filesystem::path& file, const ma_decoder_config& cfg, ma_decoder* dec)
{
#ifdef _WIN32
    return ma_decoder_init_file_w(file.wstring().c_str(), &cfg, dec);
#else
    return ma_decoder_init_file(file.string().c_str(), &cfg, dec);
#endif
}

} // namespace

struct AudioEngine::Output {
    std::string deviceName;
    ma_engine engine{};
};

// Один экземпляр проигрываемого звука на одном устройстве.
struct AudioEngine::Voice {
    Output* output = nullptr;
    ma_decoder decoder{};
    ma_sound sound{};
    bool decoderReady = false;
    bool soundReady = false;

    ~Voice()
    {
        if (soundReady) ma_sound_uninit(&sound);
        if (decoderReady) ma_decoder_uninit(&decoder);
    }
};

AudioEngine::AudioEngine() : context_(std::make_unique<ma_context>()) {}

AudioEngine::~AudioEngine()
{
    shutdown();
}

bool AudioEngine::init(const std::vector<std::string>& wanted, std::vector<std::string>& warnings)
{
    shutdown();
    std::lock_guard<std::mutex> lock(mutex_);

    // SOUNDPAD_NULL_AUDIO=1 — «немое» устройство для тестов без звуковой карты.
    const char* nullAudio = std::getenv("SOUNDPAD_NULL_AUDIO");
    ma_backend nullBackend = ma_backend_null;
    bool useNull = nullAudio && *nullAudio && *nullAudio != '0';
    if (ma_context_init(useNull ? &nullBackend : nullptr, useNull ? 1 : 0, nullptr, context_.get()) != MA_SUCCESS) {
        warnings.push_back("не удалось инициализировать аудиоподсистему");
        return false;
    }
    contextReady_ = true;

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context_.get(), &infos, &count, nullptr, nullptr) != MA_SUCCESS)
        count = 0;

    std::vector<std::string> names = wanted;
    if (names.empty()) names.push_back("default");

    for (const std::string& want : names) {
        ma_device_id* id = nullptr;
        std::string label = "default";
        if (console::toLower(want) != "default") {
            std::string needle = console::toLower(want);
            for (ma_uint32 i = 0; i < count; ++i) {
                if (console::toLower(infos[i].name).find(needle) != std::string::npos) {
                    id = &infos[i].id;
                    label = infos[i].name;
                    break;
                }
            }
            if (!id) {
                warnings.push_back("устройство вывода не найдено: \"" + want + "\"");
                continue;
            }
        } else {
            for (ma_uint32 i = 0; i < count; ++i)
                if (infos[i].isDefault) label = std::string("default (") + infos[i].name + ")";
        }

        bool duplicate = std::any_of(outputs_.begin(), outputs_.end(),
                                     [&](const auto& o) { return o->deviceName == label; });
        if (duplicate) continue;

        auto out = std::make_unique<Output>();
        out->deviceName = label;
        ma_engine_config ec = ma_engine_config_init();
        ec.pContext = context_.get();
        ec.pPlaybackDeviceID = id;
        if (ma_engine_init(&ec, &out->engine) != MA_SUCCESS) {
            warnings.push_back("не удалось открыть устройство: " + label);
            continue;
        }
        ma_engine_set_volume(&out->engine, masterVolume_);
        outputs_.push_back(std::move(out));
    }
    return !outputs_.empty();
}

void AudioEngine::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.clear();
    for (auto& o : outputs_) ma_engine_uninit(&o->engine);
    outputs_.clear();
    if (contextReady_) {
        ma_context_uninit(context_.get());
        contextReady_ = false;
    }
}

bool AudioEngine::ready() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !outputs_.empty();
}

std::vector<OutputDeviceInfo> AudioEngine::listDevices()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OutputDeviceInfo> result;
    if (!contextReady_) return result;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context_.get(), &infos, &count, nullptr, nullptr) != MA_SUCCESS)
        return result;
    for (ma_uint32 i = 0; i < count; ++i)
        result.push_back({infos[i].name, infos[i].isDefault != 0});
    return result;
}

std::vector<std::string> AudioEngine::activeDeviceNames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& o : outputs_) names.push_back(o->deviceName);
    return names;
}

bool AudioEngine::play(const std::filesystem::path& file, AudioFormat format, float volume,
                       bool allowOverlap, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (outputs_.empty()) {
        error = "нет активных устройств вывода";
        return false;
    }
    if (!allowOverlap) voices_.clear();
    reapFinishedLocked();

    std::vector<std::unique_ptr<Voice>> started;
    for (auto& out : outputs_) {
        auto v = std::make_unique<Voice>();
        v->output = out.get();

        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32,
                                                      ma_engine_get_channels(&out->engine),
                                                      ma_engine_get_sample_rate(&out->engine));
        dc.encodingFormat = toMaEncoding(format);
        ma_result r = openDecoder(file, dc, &v->decoder);
        if (r != MA_SUCCESS) {
            error = std::string("ошибка декодирования: ") + ma_result_description(r);
            return false;
        }
        v->decoderReady = true;

        r = ma_sound_init_from_data_source(&out->engine, &v->decoder,
                                           MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &v->sound);
        if (r != MA_SUCCESS) {
            error = std::string("ошибка запуска звука: ") + ma_result_description(r);
            return false;
        }
        v->soundReady = true;
        ma_sound_set_volume(&v->sound, volume);
        started.push_back(std::move(v));
    }
    // Стартуем только когда все устройства готовы, чтобы звук шёл синхронно.
    for (auto& v : started) {
        ma_sound_start(&v->sound);
        voices_.push_back(std::move(v));
    }
    return true;
}

void AudioEngine::stopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.clear();
}

size_t AudioEngine::playingCount()
{
    std::lock_guard<std::mutex> lock(mutex_);
    reapFinishedLocked();
    return voices_.size() / std::max<size_t>(1, outputs_.size());
}

void AudioEngine::setMasterVolume(float v)
{
    std::lock_guard<std::mutex> lock(mutex_);
    masterVolume_ = std::clamp(v, 0.0f, 1.0f);
    for (auto& o : outputs_) ma_engine_set_volume(&o->engine, masterVolume_);
}

float AudioEngine::masterVolume() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return masterVolume_;
}

void AudioEngine::reapFinishedLocked()
{
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const auto& v) { return ma_sound_at_end(&v->sound); }),
                  voices_.end());
}

bool AudioEngine::probe(const std::filesystem::path& file, AudioFormat format,
                        DecodedInfo& info, std::string& error)
{
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
    dc.encodingFormat = toMaEncoding(format);
    ma_decoder dec;
    ma_result r = openDecoder(file, dc, &dec);
    if (r != MA_SUCCESS) {
        const FormatInfo* fi = FormatRegistry::find(format);
        error = std::string("файл повреждён или не является ") + (fi ? fi->displayName : "аудиофайлом") +
                " (" + ma_result_description(r) + ")";
        return false;
    }
    ma_format fmt;
    ma_uint32 channels = 0, rate = 0;
    ma_decoder_get_data_format(&dec, &fmt, &channels, &rate, nullptr, 0);
    ma_uint64 frames = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &frames);
    ma_decoder_uninit(&dec);

    info.channels = channels;
    info.sampleRate = rate;
    info.durationMs = rate ? frames * 1000 / rate : 0;
    return true;
}
