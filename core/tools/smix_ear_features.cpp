// Extracts ear-model features from labelled WAV files for training (training/ear/train_ear.py).
//   smix_ear_features <out.csv> <label>=<wav> [<label>=<wav> ...]
//   smix_ear_features <out.csv> --dir <root>     (sub-folder name = label)
// Every file is cut into 3 s windows (hop 1.5 s); one CSV row per window: label,f0,...,fN

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <smix/WavFile.h>
#include <smix/ear/EarModel.h>

namespace fs = std::filesystem;

static int addFile (std::ofstream& out, const std::string& label, const std::string& path)
{
    smix::WavData wav;
    std::string err;
    if (! smix::readWav (path, wav, err))
    {
        std::cerr << "skip " << path << ": " << err << "\n";
        return 0;
    }
    std::vector<float> mono (wav.samples[0].size());
    for (size_t i = 0; i < mono.size(); ++i)
    {
        float s = 0;
        for (auto& ch : wav.samples) s += ch[i];
        mono[i] = s / static_cast<float> (wav.channels);
    }
    const size_t win = static_cast<size_t> (wav.sampleRate * 3.0), hop = win / 2;
    int rows = 0;
    for (size_t start = 0; start + win <= mono.size() || (start == 0 && ! mono.empty()); start += hop)
    {
        const size_t n = std::min (win, mono.size() - start);
        const auto f = smix::ear::EarFeatures::extract (mono.data() + start, n, wav.sampleRate);
        out << label;
        for (auto v : f) out << "," << v;
        out << "\n";
        ++rows;
        if (start + win >= mono.size())
            break;
    }
    return rows;
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: smix_ear_features out.csv label=file.wav ... | out.csv --dir root\n";
        return 2;
    }
    std::ofstream out (argv[1]);
    int rows = 0;
    if (std::string (argv[2]) == "--dir" && argc > 3)
    {
        for (auto& labelDir : fs::directory_iterator (argv[3]))
            if (labelDir.is_directory())
                for (auto& f : fs::recursive_directory_iterator (labelDir.path()))
                    if (f.path().extension() == ".wav" || f.path().extension() == ".WAV")
                        rows += addFile (out, labelDir.path().filename().string(), f.path().string());
    }
    else
    {
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            const auto eq = a.find ('=');
            if (eq != std::string::npos)
                rows += addFile (out, a.substr (0, eq), a.substr (eq + 1));
        }
    }
    std::cerr << rows << " feature rows, dim " << smix::ear::EarFeatures::kDim << "\n";
    return rows > 0 ? 0 : 1;
}
