#pragma once

#include <string>
#include <vector>

namespace smix
{

/** Minimal WAV reader (PCM 16/24/32-bit, float 32) for the offline tools and tests. */
struct WavData
{
    double sampleRate = 0;
    int channels = 0;
    std::vector<std::vector<float>> samples;  // [channel][frame]
};

bool readWav (const std::string& path, WavData& out, std::string& error);
bool writeWav (const std::string& path, const WavData& data);

} // namespace smix
