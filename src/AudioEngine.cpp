#include "AudioEngine.h"

#include "Console.h"

#include "miniaudio.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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
    std::string requested;   // как указано в конфиге
    std::string deviceName;  // как называется в системе
    ma_engine engine{};
};

// Источник данных miniaudio, который отдаёт звук с микрофона из кольцевого
// буфера. Пишет в буфер поток захвата, читает — аудиопоток устройства вывода.
// Часы микрофона и кабеля немного расходятся, поэтому задержка держится в
// коридоре: при опустошении буфера выдаётся тишина, при переполнении лишнее
// отбрасывается. Источник «бесконечный» — звук никогда не заканчивается.
struct AudioEngine::MicLink {
    ma_data_source_base base{};  // обязан быть первым полем
    ma_device capture{};
    ma_pcm_rb rb{};
    ma_sound sound{};
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_uint32 targetLatency = 0;  // кадры, с которых начинаем читать
    ma_uint32 maxLatency = 0;     // больше — догоняем, отбрасывая старое
    bool primed = false;          // только из аудиопотока вывода
    std::string description;
    bool rbReady = false, sourceReady = false, captureReady = false, soundReady = false;

    ~MicLink()
    {
        // Сначала останавливаем захват и звук, потом освобождаем буфер.
        if (captureReady) ma_device_uninit(&capture);
        if (soundReady) ma_sound_uninit(&sound);
        if (sourceReady) ma_data_source_uninit(&base);
        if (rbReady) ma_pcm_rb_uninit(&rb);
    }

    static void onCapture(ma_device* dev, void*, const void* input, ma_uint32 frameCount)
    {
        auto* self = static_cast<MicLink*>(dev->pUserData);
        auto* src = static_cast<const float*>(input);
        while (frameCount > 0) {
            ma_uint32 n = frameCount;
            void* dst = nullptr;
            if (ma_pcm_rb_acquire_write(&self->rb, &n, &dst) != MA_SUCCESS || n == 0) return;  // буфер полон
            std::memcpy(dst, src, size_t(n) * self->channels * sizeof(float));
            ma_pcm_rb_commit_write(&self->rb, n);
            src += size_t(n) * self->channels;
            frameCount -= n;
        }
    }

    static ma_result onRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* framesRead)
    {
        auto* self = static_cast<MicLink*>(ds);
        auto* dst = static_cast<float*>(out);
        ma_uint32 avail = ma_pcm_rb_available_read(&self->rb);

        if (avail > self->maxLatency) {
            ma_pcm_rb_seek_read(&self->rb, avail - self->targetLatency);
            avail = self->targetLatency;
        }
        if (!self->primed && avail >= self->targetLatency) self->primed = true;

        ma_uint64 done = 0;
        while (self->primed && done < frameCount) {
            ma_uint32 n = static_cast<ma_uint32>(std::min<ma_uint64>(frameCount - done, 0xFFFFFFFFu));
            void* src = nullptr;
            if (ma_pcm_rb_acquire_read(&self->rb, &n, &src) != MA_SUCCESS || n == 0) {
                self->primed = false;  // опустели — копим заново, чтобы не трещало
                break;
            }
            std::memcpy(dst + done * self->channels, src, size_t(n) * self->channels * sizeof(float));
            ma_pcm_rb_commit_read(&self->rb, n);
            done += n;
        }
        if (done < frameCount)
            std::memset(dst + done * self->channels, 0, size_t(frameCount - done) * self->channels * sizeof(float));
        if (framesRead) *framesRead = frameCount;
        return MA_SUCCESS;
    }

    static ma_result onSeek(ma_data_source*, ma_uint64) { return MA_SUCCESS; }

    static ma_result onGetDataFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels,
                                     ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCap)
    {
        auto* self = static_cast<MicLink*>(ds);
        *format = ma_format_f32;
        *channels = self->channels;
        *sampleRate = self->sampleRate;
        if (channelMap) ma_channel_map_init_standard(ma_standard_channel_map_default, channelMap, channelMapCap, self->channels);
        return MA_SUCCESS;
    }

    static ma_result onGetCursor(ma_data_source*, ma_uint64* cursor)
    {
        *cursor = 0;
        return MA_SUCCESS;
    }

    static ma_result onGetLength(ma_data_source*, ma_uint64* length)
    {
        *length = 0;  // поток без конца
        return MA_NOT_IMPLEMENTED;
    }
};

// Один экземпляр проигрываемого звука на одном устройстве.
struct AudioEngine::Voice {
    Output* output = nullptr;
    ma_decoder decoder{};
    ma_sound sound{};
    float baseVolume = 1.0f;  // громкость звука без учёта общей
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
        out->requested = want;
        out->deviceName = label;
        ma_engine_config ec = ma_engine_config_init();
        ec.pContext = context_.get();
        ec.pPlaybackDeviceID = id;
        if (ma_engine_init(&ec, &out->engine) != MA_SUCCESS) {
            warnings.push_back("не удалось открыть устройство: " + label);
            continue;
        }
        outputs_.push_back(std::move(out));
    }
    return !outputs_.empty();
}

void AudioEngine::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopMicLocked();
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

std::vector<DeviceInfo> AudioEngine::listPlaybackDevices()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceInfo> result;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (contextReady_ && ma_context_get_devices(context_.get(), &infos, &count, nullptr, nullptr) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < count; ++i) result.push_back({infos[i].name, infos[i].isDefault != 0});
    return result;
}

std::vector<DeviceInfo> AudioEngine::listCaptureDevices()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceInfo> result;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (contextReady_ && ma_context_get_devices(context_.get(), nullptr, nullptr, &infos, &count) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < count; ++i) result.push_back({infos[i].name, infos[i].isDefault != 0});
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
        v->baseVolume = volume;
        ma_sound_set_volume(&v->sound, volume * masterVolume_);
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
    // Общая громкость касается только звуков саундпада, не голоса.
    for (auto& v : voices_) ma_sound_set_volume(&v->sound, v->baseVolume * masterVolume_);
}

float AudioEngine::masterVolume() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return masterVolume_;
}

bool AudioEngine::startMic(const std::string& micName, const std::string& target, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopMicLocked();
    if (!contextReady_ || outputs_.empty()) {
        error = "нет активных устройств вывода";
        return false;
    }

    // Куда подмешивать голос: устройство из outputs, совпадающее с target.
    Output* out = nullptr;
    std::string needle = console::toLower(target);
    for (auto& o : outputs_)
        if (console::toLower(o->requested) == needle ||
            console::toLower(o->deviceName).find(needle) != std::string::npos) {
            out = o.get();
            break;
        }
    if (!out) {
        error = "mic_output \"" + target + "\" не входит в outputs";
        return false;
    }

    // Микрофон.
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(context_.get(), nullptr, nullptr, &infos, &count) != MA_SUCCESS) count = 0;
    ma_device_id* micId = nullptr;
    std::string micLabel = "микрофон по умолчанию";
    if (console::toLower(micName) != "default") {
        std::string m = console::toLower(micName);
        for (ma_uint32 i = 0; i < count; ++i)
            if (console::toLower(infos[i].name).find(m) != std::string::npos) {
                micId = &infos[i].id;
                micLabel = infos[i].name;
                break;
            }
        if (!micId) {
            error = "микрофон не найден: \"" + micName + "\" (см. devices)";
            return false;
        }
    } else {
        for (ma_uint32 i = 0; i < count; ++i)
            if (infos[i].isDefault) micLabel = infos[i].name;
    }
    // Частая ошибка: выбрать микрофоном выход самого кабеля — получится петля.
    if (console::toLower(micLabel).find("cable output") != std::string::npos) {
        error = "\"" + micLabel + "\" — это выход виртуального кабеля, укажите настоящий микрофон";
        return false;
    }

    static const ma_data_source_vtable vtable = {
        &MicLink::onRead, &MicLink::onSeek, &MicLink::onGetDataFormat,
        &MicLink::onGetCursor, &MicLink::onGetLength, nullptr, 0,
    };

    auto link = std::make_unique<MicLink>();
    link->channels = ma_engine_get_channels(&out->engine);
    link->sampleRate = ma_engine_get_sample_rate(&out->engine);
    link->targetLatency = link->sampleRate * 30 / 1000;
    link->maxLatency = link->sampleRate * 120 / 1000;

    ma_result r = ma_pcm_rb_init(ma_format_f32, link->channels, link->sampleRate / 2, nullptr, nullptr, &link->rb);
    if (r != MA_SUCCESS) {
        error = std::string("буфер микрофона: ") + ma_result_description(r);
        return false;
    }
    link->rbReady = true;

    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &vtable;
    if ((r = ma_data_source_init(&dsc, &link->base)) != MA_SUCCESS) {
        error = std::string("источник микрофона: ") + ma_result_description(r);
        return false;
    }
    link->sourceReady = true;

    if ((r = ma_sound_init_from_data_source(&out->engine, &link->base, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                            nullptr, &link->sound)) != MA_SUCCESS) {
        error = std::string("микшер микрофона: ") + ma_result_description(r);
        return false;
    }
    link->soundReady = true;

    // Захват сразу в формате устройства вывода: miniaudio сам сконвертирует
    // частоту и число каналов микрофона.
    ma_device_config dc = ma_device_config_init(ma_device_type_capture);
    dc.capture.pDeviceID = micId;
    dc.capture.format = ma_format_f32;
    dc.capture.channels = link->channels;
    dc.sampleRate = link->sampleRate;
    dc.periodSizeInMilliseconds = 10;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.dataCallback = &MicLink::onCapture;
    dc.pUserData = link.get();
    if ((r = ma_device_init(context_.get(), &dc, &link->capture)) != MA_SUCCESS) {
        error = "не удалось открыть микрофон \"" + micLabel + "\": " + ma_result_description(r);
        return false;
    }
    link->captureReady = true;

    link->description = micLabel + " -> " + out->deviceName;
    mic_ = std::move(link);
    applyMicVolumeLocked();
    ma_sound_start(&mic_->sound);
    if ((r = ma_device_start(&mic_->capture)) != MA_SUCCESS) {
        error = std::string("микрофон не запустился: ") + ma_result_description(r);
        mic_.reset();
        return false;
    }
    return true;
}

void AudioEngine::stopMic()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopMicLocked();
}

void AudioEngine::stopMicLocked()
{
    mic_.reset();
}

bool AudioEngine::micActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mic_ != nullptr;
}

std::string AudioEngine::micDescription() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mic_ ? mic_->description : std::string();
}

void AudioEngine::setMicMuted(bool muted)
{
    std::lock_guard<std::mutex> lock(mutex_);
    micMuted_ = muted;
    applyMicVolumeLocked();
}

bool AudioEngine::micMuted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return micMuted_;
}

void AudioEngine::setMicVolume(float v)
{
    std::lock_guard<std::mutex> lock(mutex_);
    micVolume_ = std::clamp(v, 0.0f, 2.0f);
    applyMicVolumeLocked();
}

float AudioEngine::micVolume() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return micVolume_;
}

void AudioEngine::applyMicVolumeLocked()
{
    if (mic_) ma_sound_set_volume(&mic_->sound, micMuted_ ? 0.0f : micVolume_);
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
