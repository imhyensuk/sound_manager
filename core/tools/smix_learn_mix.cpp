// Learns a genre profile from the user's own songs: raw multitracks + the finished mix of each song.
//
//   smix_learn_mix --genre CCM --out CCM.smxgenre songs/
//
//   songs/곡1/  Kick.wav  Snare.wav  여성 보컬.wav ...  곡1_Mix.wav
//   songs/곡2/  ...
//
// - The finished mix is the WAV whose name contains mix / master / final / 믹스 / 마스터 / 완성
//   (or --mix-word <word>). Every other WAV is a raw track.
// - Click, guide and cue tracks are left out by name.
// - Instrument labels come from the file names; to correct them, edit tracks.csv (written on the first
//   run next to the output: file,label - label "skip" leaves a track out) and run again.
// - Also writes ear-tracks.csv (label,path) for: smix_ear_features features.csv --list ear-tracks.csv

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <smix/WavFile.h>
#include <smix/style/GenreProfile.h>

namespace fs = std::filesystem;
using namespace smix;

namespace
{
bool isWav (const fs::path& p)
{
    auto e = p.extension().string();
    for (auto& c : e) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return e == ".wav";
}

std::vector<float> mono (const WavData& w)
{
    std::vector<float> m (w.samples.empty() ? 0 : w.samples[0].size());
    for (size_t i = 0; i < m.size(); ++i)
    {
        float s = 0;
        for (auto& ch : w.samples) s += ch[i];
        m[i] = s / static_cast<float> (w.channels);
    }
    return m;
}

/** "Track 3 - 여성 보컬 (Take2)" -> "여성 보컬 (Take2)": strip extension and leading numbering. */
std::string labelFromFile (const fs::path& p)
{
    std::string s = p.stem().u8string();
    size_t i = 0;
    while (i < s.size() && (std::isdigit (static_cast<unsigned char> (s[i])) || s[i] == '_' || s[i] == '-' || s[i] == ' ' || s[i] == '.'))
        ++i;
    return i < s.size() ? s.substr (i) : s;
}

InstrumentRole roleFor (const std::string& label)
{
    auto r = guessRoleFromTrackName (label);
    if (r == InstrumentRole::DrumBus)  // "drum" alone in a raw track name: most likely a room/overhead mic
        r = InstrumentRole::Overheads;
    return r;
}
} // namespace

int main (int argc, char** argv)
{
    std::string genre = "Genre", outPath, root;
    std::vector<std::string> mixWords { "mix", "master", "final", "믹스", "마스터", "완성" };
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--genre" && i + 1 < argc) genre = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--mix-word" && i + 1 < argc) mixWords = { argv[++i] };
        else root = a;
    }
    if (root.empty() || ! fs::is_directory (fs::u8path (root)))
    {
        std::cerr << "usage: smix_learn_mix --genre CCM --out CCM.smxgenre <folder with one sub-folder per song>\n";
        return 2;
    }
    if (outPath.empty())
        outPath = genre + ".smxgenre";
    const auto outDir = fs::u8path (outPath).parent_path().empty() ? fs::path (".") : fs::u8path (outPath).parent_path();

    // User corrections: file,label (label "skip" = leave out).
    std::map<std::string, std::string> labels;
    const auto labelFile = outDir / "tracks.csv";
    if (std::ifstream in (labelFile); in)
    {
        std::string line;
        while (std::getline (in, line))
        {
            const auto comma = line.rfind (',');
            if (comma != std::string::npos && line.rfind ("file,", 0) != 0)
                labels[line.substr (0, comma)] = line.substr (comma + 1);
        }
        std::cerr << "using labels from " << labelFile.u8string() << "\n";
    }
    std::ofstream labelOut (labelFile.u8string() + (labels.empty() ? "" : ".new"));
    labelOut << "file,label\n";
    std::ofstream earList ((outDir / "ear-tracks.csv").u8string());

    std::vector<style::LearnedSong> songs;
    std::vector<style::StyleProfile> mixes;
    for (auto& songDir : fs::directory_iterator (fs::u8path (root)))
    {
        if (! songDir.is_directory())
            continue;
        const auto songName = songDir.path().filename().u8string();
        fs::path mixFile;
        std::vector<fs::path> trackFiles;
        for (auto& f : fs::directory_iterator (songDir.path()))
        {
            if (! f.is_regular_file() || ! isWav (f.path()))
                continue;
            const auto lower = toLowerAscii (f.path().filename().u8string());
            bool isMix = false;
            for (auto& w : mixWords)
                isMix = isMix || lower.find (toLowerAscii (w)) != std::string::npos;
            if (isMix && mixFile.empty())
                mixFile = f.path();
            else
                trackFiles.push_back (f.path());
        }
        if (mixFile.empty())
        {
            std::cerr << "[" << songName << "] no finished mix found (name must contain mix/master/final/믹스/마스터/완성) - skipped\n";
            continue;
        }

        std::cerr << "[" << songName << "] mix: " << mixFile.filename().u8string() << ", " << trackFiles.size() << " files\n";
        WavData wav;
        std::string err;
        if (! readWav (mixFile.u8string(), wav, err))
        {
            std::cerr << "  cannot read the mix: " << err << "\n";
            continue;
        }
        const double sr = wav.sampleRate;
        const auto mixMono = mono (wav);
        const auto mixFrames = style::bandFrames (mixMono.data(), mixMono.size(), sr);
        style::StyleAnalyzer analyzer (sr);
        analyzer.process (wav.samples[0].data(), wav.channels > 1 ? wav.samples[1].data() : nullptr, static_cast<int> (wav.samples[0].size()));
        mixes.push_back (analyzer.finish (songName, mixFile.u8string()));
        wav = {};

        std::vector<style::LearnTrack> tracks;
        for (auto& tf : trackFiles)
        {
            const auto rel = songName + "/" + tf.filename().u8string();
            std::string label = labelFromFile (tf);
            if (auto it = labels.find (rel); it != labels.end())
                label = it->second;
            else if (style::isAuxiliaryTrackName (tf.filename().u8string()))
                label = "skip";
            labelOut << rel << "," << label << "\n";
            if (label == "skip")
                continue;
            if (! readWav (tf.u8string(), wav, err))
            {
                std::cerr << "  skip " << tf.filename().u8string() << ": " << err << "\n";
                continue;
            }
            if (std::abs (wav.sampleRate - sr) > 1.0)
            {
                std::cerr << "  skip " << tf.filename().u8string() << ": sample rate differs from the mix\n";
                continue;
            }
            const auto m = mono (wav);
            tracks.push_back ({ tf.filename().u8string(), label, roleFor (label), style::bandFrames (m.data(), m.size(), sr) });
            if (tracks.back().role != InstrumentRole::Unknown)
                earList << label << "," << tf.u8string() << "\n";
        }

        auto learned = style::learnSong (songName, tracks, mixFrames);
        float fit = 0;
        for (auto f : learned.fit) fit += f / static_cast<float> (learned.fit.size());
        std::cerr << "  reconstruction fit " << static_cast<int> (fit * 100) << "% (low = reverb returns / bus processing not in the raw tracks), anchor " << learned.anchor << "\n";
        for (auto& t : learned.tracks)
        {
            std::ostringstream os;
            os.precision (1);
            os << std::fixed << t.levelLu;
            std::cerr << "    " << (t.present ? "" : "(unused) ") << t.label << " [" << toString (t.role) << "] " << os.str() << " LU\n";
        }
        songs.push_back (std::move (learned));
    }

    if (songs.empty())
    {
        std::cerr << "no songs learned\n";
        return 1;
    }
    const auto profile = style::combineSongs (genre, songs, mixes);
    std::ofstream (outPath) << profile.toJson().dump (1);

    std::cerr << "\n" << genre << ": " << songs.size() << " songs\nbalance vs lead vocal (LU):\n";
    for (auto& [role, lu] : profile.balanceLu)
        std::cerr << "  " << koreanName (role) << " " << lu << "  (" << profile.samples.at (role) << " tracks)\n";
    std::cerr << "master: " << profile.master.integratedLufs << " LUFS, PLR " << profile.master.plrDb << " dB\n";
    std::cerr << "\nwrote " << outPath << " (copy it to SoundManagerAI/models/), " << labelFile.u8string() << (labels.empty() ? "" : ".new")
              << " (edit labels, rerun), ear-tracks.csv\n";
    return 0;
}
