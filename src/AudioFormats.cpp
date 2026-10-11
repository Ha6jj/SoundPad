#include "AudioFormats.h"

#include "Console.h"

#include <cstring>
#include <fstream>

namespace {

bool startsWith(const std::vector<uint8_t>& h, size_t off, const char* sig)
{
    size_t n = std::strlen(sig);
    return h.size() >= off + n && std::memcmp(h.data() + off, sig, n) == 0;
}

bool probeWav(const std::vector<uint8_t>& h)
{
    return (startsWith(h, 0, "RIFF") || startsWith(h, 0, "RF64")) && startsWith(h, 8, "WAVE");
}

bool probeMp3(const std::vector<uint8_t>& h)
{
    if (startsWith(h, 0, "ID3")) return true;
    // MPEG frame sync: 11 единичных бит, layer != 0.
    return h.size() >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && (h[1] & 0x06) != 0;
}

bool probeFlac(const std::vector<uint8_t>& h)
{
    return startsWith(h, 0, "fLaC");
}

bool probeOggVorbis(const std::vector<uint8_t>& h)
{
    // Ogg-страница, в первом пакете заголовок "\x01vorbis".
    if (!startsWith(h, 0, "OggS")) return false;
    for (size_t i = 0; i + 7 <= h.size(); ++i)
        if (h[i] == 0x01 && std::memcmp(h.data() + i + 1, "vorbis", 6) == 0) return true;
    return false;
}

} // namespace

const std::vector<FormatInfo>& FormatRegistry::all()
{
    static const std::vector<FormatInfo> formats = {
        {AudioFormat::Wav,       "wav",  "WAV (PCM / IEEE float)", {".wav", ".wave"}, probeWav},
        {AudioFormat::Mp3,       "mp3",  "MPEG Layer III",         {".mp3"},          probeMp3},
        {AudioFormat::Flac,      "flac", "FLAC (lossless)",        {".flac"},         probeFlac},
        {AudioFormat::OggVorbis, "ogg",  "Ogg Vorbis",             {".ogg", ".oga"},  probeOggVorbis},
    };
    return formats;
}

const FormatInfo* FormatRegistry::find(AudioFormat f)
{
    for (const auto& fi : all())
        if (fi.format == f) return &fi;
    return nullptr;
}

const FormatInfo* FormatRegistry::findById(const std::string& id)
{
    for (const auto& fi : all())
        if (id == fi.id) return &fi;
    return nullptr;
}

const FormatInfo* FormatRegistry::findByExtension(const std::string& ext)
{
    std::string e = console::toLower(ext);
    for (const auto& fi : all())
        for (const auto& x : fi.extensions)
            if (x == e) return &fi;
    return nullptr;
}

bool FormatRegistry::isSupportedExtension(const std::filesystem::path& p)
{
    return findByExtension(pathToUtf8(p.extension())) != nullptr;
}

AudioFormat FormatRegistry::detect(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return AudioFormat::Unknown;

    std::vector<uint8_t> header(64);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<size_t>(in.gcount()));

    for (const auto& fi : all())
        if (fi.probe(header)) return fi.format;

    // Сигнатура не распознана (например, MP3 без ID3 с мусором в начале) —
    // доверяем расширению, декодер всё равно проверит файл при открытии.
    if (const FormatInfo* fi = findByExtension(pathToUtf8(p.extension()))) return fi->format;
    return AudioFormat::Unknown;
}
