// Extracts ear-model features from labelled WAV stems for training (training/ear/train_ear.py).
//   smix_ear_features <out.csv> --dir <root>              (each sub-folder of root is one class; any depth below it)
//   smix_ear_features <out.csv> --list ear-tracks.csv           (label,path lines from smix_learn_mix)
//   smix_ear_features <out.csv> <label>=<wav> [...]
// Every file is cut into 3 s windows (hop 1.5 s). Windows that are (mostly) silent - long rests in
// multitrack stems - are skipped, so the model only learns from the instrument actually playing.
// CSV row: label,file_index,f0,...,fN    (file_index groups windows of one file for a leak-free split)
// <out.csv>.files lists file_index -> path.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <string>
#include <vector>

#include <smix/WavFile.h>
#include <smix/ear/EarModel.h>

namespace fs = std::filesystem;

namespace
{
struct Counts { int windows = 0, silent = 0; };

Counts addFile (std::ofstream& out, std::ofstream& files, int index, const std::string& label, const std::string& path, float silenceDb)
{
    Counts c;
    smix::WavData wav;
    std::string err;
    if (! smix::readWav (path, wav, err))
    {
        std::cerr << "skip " << path << ": " << err << " (convert to WAV: ffmpeg -i in.flac out.wav)\n";
        return c;
    }
    files << index << "\t" << label << "\t" << path << "\n";
    std::vector<float> mono (wav.samples[0].size());
    for (size_t i = 0; i < mono.size(); ++i)
    {
        float s = 0;
        for (auto& ch : wav.samples) s += ch[i];
        mono[i] = s / static_cast<float> (wav.channels);
    }

    const size_t win = static_cast<size_t> (wav.sampleRate * 3.0), hop = win / 2;
    for (size_t start = 0; start < mono.size(); start += hop)
    {
        const size_t n = std::min (win, mono.size() - start);
        if (n < win / 2)
            break;
        double sq = 0;
        for (size_t i = 0; i < n; ++i)
            sq += static_cast<double> (mono[start + i]) * mono[start + i];
        const double rmsDb = 10.0 * std::log10 (sq / static_cast<double> (n) + 1.0e-20);
        const auto f = smix::ear::EarFeatures::extract (mono.data() + start, n, wav.sampleRate);
        // Too quiet overall, or hardly any of the window actually sounding.
        if (rmsDb < silenceDb || f.back() < smix::ear::EarFeatures::kMinActive)
        {
            ++c.silent;
            continue;
        }
        out << label << "," << index;
        for (auto v : f) out << "," << v;
        out << "\n";
        ++c.windows;
    }
    return c;
}

bool isWav (const fs::path& p)
{
    auto e = p.extension().string();
    for (auto& ch : e) ch = static_cast<char> (std::tolower (static_cast<unsigned char> (ch)));
    return e == ".wav";
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: smix_ear_features out.csv --dir root [--silence-db -50] | out.csv label=file.wav ...\n";
        return 2;
    }
    const std::string outPath = argv[1];
    std::ofstream out (outPath), files (outPath + ".files");
    float silenceDb = -50.0f;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string (argv[i]) == "--silence-db")
            silenceDb = std::stof (argv[i + 1]);

    int index = 0;
    std::map<std::string, Counts> perLabel;
    auto add = [&] (const std::string& label, const std::string& path) {
        const auto c = addFile (out, files, index++, label, path, silenceDb);
        perLabel[label].windows += c.windows;
        perLabel[label].silent += c.silent;
    };

    if (std::string (argv[2]) == "--list" && argc > 3)
    {
        // label,path per line (written by smix_learn_mix as ear-tracks.csv)
        std::ifstream list (argv[3]);
        std::string line;
        while (std::getline (list, line))
        {
            const auto comma = line.find (',');
            if (comma != std::string::npos)
                add (line.substr (0, comma), line.substr (comma + 1));
        }
    }
    else if (std::string (argv[2]) == "--dir" && argc > 3)
    {
        for (auto& labelDir : fs::directory_iterator (fs::u8path (argv[3])))
            if (labelDir.is_directory())
                for (auto& f : fs::recursive_directory_iterator (labelDir.path()))
                    if (f.is_regular_file() && isWav (f.path()))
                        add (labelDir.path().filename().u8string(), f.path().u8string());
    }
    else
    {
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            const auto eq = a.find ('=');
            if (eq != std::string::npos)
                add (a.substr (0, eq), a.substr (eq + 1));
        }
    }

    int total = 0;
    for (auto& [label, c] : perLabel)
    {
        std::cerr << "  " << label << ": " << c.windows << " windows (" << c.silent << " silent skipped)\n";
        total += c.windows;
    }
    std::cerr << total << " feature rows from " << index << " files, dim " << smix::ear::EarFeatures::kDim << "\n";
    return total > 0 ? 0 : 1;
}
