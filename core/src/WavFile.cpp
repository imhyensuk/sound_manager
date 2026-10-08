#include "smix/WavFile.h"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace smix
{

bool readWav (const std::string& path, WavData& out, std::string& error)
{
    std::ifstream f (path, std::ios::binary);
    char riff[12];
    if (! f.read (riff, 12) || std::memcmp (riff, "RIFF", 4) != 0 || std::memcmp (riff + 8, "WAVE", 4) != 0)
    {
        error = "not a WAV file";
        return false;
    }
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    while (f)
    {
        char id[4];
        std::uint32_t size = 0;
        if (! f.read (id, 4) || ! f.read (reinterpret_cast<char*> (&size), 4))
            break;
        if (std::memcmp (id, "fmt ", 4) == 0)
        {
            std::vector<char> fmt (size);
            f.read (fmt.data(), size);
            std::memcpy (&format, fmt.data(), 2);
            std::memcpy (&channels, fmt.data() + 2, 2);
            std::memcpy (&rate, fmt.data() + 4, 4);
            std::memcpy (&bits, fmt.data() + 14, 2);
            if (format == 0xFFFE && size >= 26)
                std::memcpy (&format, fmt.data() + 24, 2);  // WAVE_FORMAT_EXTENSIBLE sub-format
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            if (channels == 0 || bits == 0)
                break;
            const int bytes = bits / 8;
            const std::uint32_t frames = size / (static_cast<std::uint32_t> (bytes) * channels);
            out.sampleRate = rate;
            out.channels = channels;
            out.samples.assign (channels, std::vector<float> (frames));
            std::vector<unsigned char> raw (size);
            f.read (reinterpret_cast<char*> (raw.data()), size);
            for (std::uint32_t i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c)
                {
                    const unsigned char* p = raw.data() + (static_cast<size_t> (i) * channels + static_cast<size_t> (c)) * static_cast<size_t> (bytes);
                    float v = 0;
                    if (format == 3 && bits == 32)        std::memcpy (&v, p, 4);
                    else if (bits == 16)                  v = static_cast<float> (static_cast<std::int16_t> (p[0] | (p[1] << 8))) / 32768.0f;
                    else if (bits == 24)                  v = static_cast<float> ((static_cast<std::int32_t> ((p[0] << 8) | (p[1] << 16) | (p[2] << 24))) >> 8) / 8388608.0f;
                    else if (bits == 32)                  v = static_cast<float> (static_cast<std::int32_t> (p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t> (p[3]) << 24))) / 2147483648.0f;
                    out.samples[static_cast<size_t> (c)][i] = v;
                }
            return true;
        }
        else
        {
            f.seekg (size + (size & 1), std::ios::cur);
        }
    }
    error = "unsupported or broken WAV";
    return false;
}

bool writeWav (const std::string& path, const WavData& d)
{
    std::ofstream f (path, std::ios::binary | std::ios::trunc);
    const std::uint32_t frames = d.samples.empty() ? 0 : static_cast<std::uint32_t> (d.samples[0].size());
    const std::uint16_t channels = static_cast<std::uint16_t> (d.channels), format = 3, bits = 32, align = channels * 4;
    const std::uint32_t rate = static_cast<std::uint32_t> (d.sampleRate), byteRate = rate * align, dataSize = frames * align;
    const std::uint32_t riffSize = 36 + dataSize, fmtSize = 16;
    f.write ("RIFF", 4); f.write (reinterpret_cast<const char*> (&riffSize), 4); f.write ("WAVE", 4);
    f.write ("fmt ", 4); f.write (reinterpret_cast<const char*> (&fmtSize), 4);
    f.write (reinterpret_cast<const char*> (&format), 2); f.write (reinterpret_cast<const char*> (&channels), 2);
    f.write (reinterpret_cast<const char*> (&rate), 4); f.write (reinterpret_cast<const char*> (&byteRate), 4);
    f.write (reinterpret_cast<const char*> (&align), 2); f.write (reinterpret_cast<const char*> (&bits), 2);
    f.write ("data", 4); f.write (reinterpret_cast<const char*> (&dataSize), 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < d.channels; ++c)
            f.write (reinterpret_cast<const char*> (&d.samples[static_cast<size_t> (c)][i]), 4);
    return static_cast<bool> (f);
}

} // namespace smix
