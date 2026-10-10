// Learns a genre profile from the user's own songs: how loud every instrument sits in the mix, how it
// was processed (gain, gate, EQ, de-esser, compressor, saturation, envelope, reverb) and what the
// finished mixes sound like.
//
//   smix_learn_mix --genre CCM --out CCM.smxgenre songs/
//
// One folder per song (see training/DATA.md):
//
//   songs/곡1/raw/        01_Kick.wav 02_Snare.wav 여성 보컬.wav ...   dry recordings (no plugins)
//   songs/곡1/processed/  same file names, after each track's insert plugins, fader and pan (no sends)
//   songs/곡1/returns/    (optional) reverb/delay aux returns: Reverb Hall.wav ...
//   songs/곡1/곡1_Mix.wav        finished mix (before mastering if a master is also given)
//   songs/곡1/곡1_Master.wav     (optional) mastered version -> the master chain is learned too
//
// Without raw/ and processed/ every WAV in the song folder except the mix is taken as a raw track
// (levels and tone only).
//
// - Click, guide and cue tracks are left out by name.
// - Instrument labels come from the file names; to correct them, edit tracks.csv (written next to the
//   output: file,label - label "skip" leaves a track out) and run again.
// - Also writes ear-tracks.csv (label,path) for: smix_ear_features features.csv --list ear-tracks.csv

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <smix/WavFile.h>
#include <smix/style/GenreProfile.h>
#include <smix/style/ProcessingLearner.h>

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

std::vector<float> mono (const style::Audio& a)
{
    std::vector<float> m (a.empty() ? 0 : a[0].size());
    for (size_t i = 0; i < m.size(); ++i)
    {
        float s = 0;
        for (auto& ch : a) s += ch[i];
        m[i] = s / static_cast<float> (a.size());
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

std::string key (const fs::path& p)
{
    return toLowerAscii (p.stem().u8string());
}

InstrumentRole roleFor (const std::string& label)
{
    auto r = guessRoleFromTrackName (label);
    if (r == InstrumentRole::DrumBus)  // "drum" alone in a raw track name: most likely a room/overhead mic
        r = InstrumentRole::Overheads;
    return r;
}

bool contains (const std::string& lower, const std::vector<std::string>& words)
{
    for (auto& w : words)
        if (lower.find (toLowerAscii (w)) != std::string::npos)
            return true;
    return false;
}

std::vector<fs::path> wavsIn (const fs::path& dir)
{
    std::vector<fs::path> out;
    if (fs::is_directory (dir))
        for (auto& f : fs::directory_iterator (dir))
            if (f.is_regular_file() && isWav (f.path()))
                out.push_back (f.path());
    std::sort (out.begin(), out.end());
    return out;
}

bool load (const fs::path& p, style::Audio& out, double& sr)
{
    WavData w;
    std::string err;
    if (! readWav (p.u8string(), w, err))
    {
        std::cerr << "    cannot read " << p.filename().u8string() << ": " << err << " (convert to WAV first)\n";
        return false;
    }
    sr = w.sampleRate;
    out = std::move (w.samples);
    return true;
}

double secondsSince (std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - t).count();
}
} // namespace

int main (int argc, char** argv)
{
    std::string genre = "Genre", outPath, root;
    std::vector<std::string> mixWords { "premaster", "pre-master", "프리마스터", "mix", "믹스" };
    std::vector<std::string> masterWords { "master", "final", "마스터", "완성" };
    bool learnChains = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--genre" && i + 1 < argc) genre = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--mix-word" && i + 1 < argc) mixWords = { argv[++i] };
        else if (a == "--master-word" && i + 1 < argc) masterWords = { argv[++i] };
        else if (a == "--levels-only") learnChains = false;
        else root = a;
    }
    if (root.empty() || ! fs::is_directory (fs::u8path (root)))
    {
        std::cerr << "usage: smix_learn_mix --genre CCM --out CCM.smxgenre [--levels-only] <folder with one sub-folder per song>\n";
        return 2;
    }
    if (outPath.empty())
        outPath = genre + ".smxgenre";
    const auto outDir = fs::u8path (outPath).parent_path().empty() ? fs::path (".") : fs::u8path (outPath).parent_path();
    const auto started = std::chrono::steady_clock::now();

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

    std::vector<fs::path> songDirs;
    for (auto& d : fs::directory_iterator (fs::u8path (root)))
        if (d.is_directory())
            songDirs.push_back (d.path());
    std::sort (songDirs.begin(), songDirs.end());

    std::vector<style::LearnedSong> songs;
    std::vector<style::StyleProfile> mixes;
    std::vector<style::ProcessingSettings> masterChains;
    int songIndex = 0;
    for (auto& songDir : songDirs)
    {
        ++songIndex;
        const auto songName = songDir.filename().u8string();

        // The finished mix (and optionally the master) in the song folder itself.
        fs::path mixFile, masterFile;
        std::vector<fs::path> looseTracks;
        for (auto& f : wavsIn (songDir))
        {
            const auto lower = toLowerAscii (f.filename().u8string());
            if (contains (lower, { "premaster", "pre-master", "프리마스터" }) && mixFile.empty())
                mixFile = f;
            else if (contains (lower, masterWords) && masterFile.empty())
                masterFile = f;
            else if (contains (lower, mixWords) && mixFile.empty())
                mixFile = f;
            else
                looseTracks.push_back (f);
        }
        if (mixFile.empty() && ! masterFile.empty())
            std::swap (mixFile, masterFile);  // only one finished file: it is "the mix"
        if (mixFile.empty())
        {
            std::cerr << "[" << songName << "] no finished mix found (name must contain mix/master/final/믹스/마스터/완성) - skipped\n";
            continue;
        }

        const bool structured = fs::is_directory (songDir / "raw");
        const auto rawFiles = structured ? wavsIn (songDir / "raw") : looseTracks;
        std::map<std::string, fs::path> processedFiles;
        for (auto& f : wavsIn (songDir / "processed"))
            processedFiles[key (f)] = f;
        const auto returnFiles = wavsIn (songDir / "returns");

        std::cerr << "[" << songIndex << "/" << songDirs.size() << " " << songName << "] mix: " << mixFile.filename().u8string()
                  << (masterFile.empty() ? "" : ", master: " + masterFile.filename().u8string()) << ", " << rawFiles.size() << " tracks, "
                  << processedFiles.size() << " processed, " << returnFiles.size() << " returns\n";

        style::Audio mixAudio;
        double sr = 0;
        if (! load (mixFile, mixAudio, sr))
            continue;
        const auto mixMono = mono (mixAudio);
        const auto mixFrames = style::bandFrames (mixMono.data(), mixMono.size(), sr);
        {
            style::StyleAnalyzer analyzer (sr);
            analyzer.process (mixAudio[0].data(), mixAudio.size() > 1 ? mixAudio[1].data() : nullptr, static_cast<int> (mixAudio[0].size()));
            mixes.push_back (analyzer.finish (songName, mixFile.u8string()));
        }

        // Master chain: premaster -> master
        if (learnChains && ! masterFile.empty())
        {
            style::Audio master;
            double msr = 0;
            if (load (masterFile, master, msr) && std::abs (msr - sr) < 1.0)
            {
                style::LearnOptions opt;
                opt.learnEnvelope = false;
                opt.learnReverb = false;
                auto chain = style::learnProcessing (mixAudio, master, sr, opt);
                std::cerr << "  master chain: " << chain.describe() << " (fit " << chain.fitDb << " dB)\n";
                masterChains.push_back (chain);
            }
        }
        mixAudio.clear();
        mixAudio.shrink_to_fit();

        // Tracks: levels from the processed version when there is one, processing from raw vs processed.
        std::vector<style::LearnTrack> tracks;
        std::vector<style::ProcessingSettings> chains;
        std::vector<bool> hasChain;
        std::vector<style::SendSource> sends;
        size_t trackNo = 0;
        for (auto& tf : rawFiles)
        {
            ++trackNo;
            const auto rel = songName + "/" + tf.filename().u8string();
            std::string label = labelFromFile (tf);
            if (auto it = labels.find (rel); it != labels.end())
                label = it->second;
            else if (style::isAuxiliaryTrackName (tf.filename().u8string()))
                label = "skip";
            labelOut << rel << "," << label << "\n";
            if (label == "skip")
                continue;

            style::Audio raw;
            double tsr = 0;
            if (! load (tf, raw, tsr))
                continue;
            if (std::abs (tsr - sr) > 1.0)
            {
                std::cerr << "    skip " << tf.filename().u8string() << ": sample rate differs from the mix\n";
                continue;
            }
            const auto role = roleFor (label);
            if (role != InstrumentRole::Unknown)
                earList << label << "," << tf.u8string() << "\n";

            style::Audio processed;
            bool haveProcessed = false;
            if (auto it = processedFiles.find (key (tf)); it != processedFiles.end())
            {
                double psr = 0;
                haveProcessed = load (it->second, processed, psr) && std::abs (psr - sr) < 1.0;
                processedFiles.erase (it);
            }

            const auto& levelSource = haveProcessed ? processed : raw;
            const auto m = mono (levelSource);
            tracks.push_back ({ tf.filename().u8string(), label, role, style::bandFrames (m.data(), m.size(), sr) });
            sends.push_back (style::makeSendSource (levelSource, sr));

            style::ProcessingSettings chain;
            if (learnChains && haveProcessed)
            {
                const auto t0 = std::chrono::steady_clock::now();
                chain = style::learnProcessing (raw, processed, sr);
                std::cerr << "    [" << trackNo << "/" << rawFiles.size() << "] " << label << ": " << chain.describe() << "  (fit "
                          << static_cast<int> (chain.fitDb * 10) / 10.0 << " dB, " << static_cast<int> (secondsSince (t0)) << " s)\n";
            }
            chains.push_back (chain);
            hasChain.push_back (learnChains && haveProcessed);
        }
        for (auto& [k, f] : processedFiles)
            std::cerr << "    note: processed/" << f.filename().u8string() << " has no raw/ file with the same name - ignored\n";

        // Returns join the mix fit as extra sources (they explain the reverb in the mix).
        const size_t numTracks = tracks.size();
        std::vector<style::Audio> returns;
        for (auto& rf : returnFiles)
        {
            style::Audio r;
            double rsr = 0;
            if (! load (rf, r, rsr) || std::abs (rsr - sr) > 1.0)
                continue;
            const auto m = mono (r);
            tracks.push_back ({ rf.filename().u8string(), "return:" + labelFromFile (rf), InstrumentRole::FX,
                                style::bandFrames (m.data(), m.size(), sr) });
            returns.push_back (std::move (r));
        }

        auto learned = style::learnSong (songName, tracks, mixFrames);
        float fit = 0;
        for (auto f : learned.fit) fit += f / static_cast<float> (learned.fit.size());
        std::cerr << "  reconstruction fit " << static_cast<int> (fit * 100) << "% (low = processing missing from the files: bus effects, returns), anchor "
                  << learned.anchor << "\n";

        // Sends: each return against all processed tracks. Processed tracks and returns are exported
        // post-fader, so both are already at mix scale (no mix gains needed).
        std::vector<float> dryGain (numTracks, 0.0f);
        std::vector<style::ProcessingSettings*> outs;
        for (size_t t = 0; t < numTracks; ++t)
            outs.push_back (hasChain[t] ? &chains[t] : nullptr);
        for (size_t r = 0; r < returns.size(); ++r)
        {
            const auto& rt = learned.tracks[numTracks + r];
            if (! rt.present)
                continue;
            style::learnReturn (returns[r], sends, sr, dryGain, 0.0f, outs);
            std::cerr << "    return " << rt.label.substr (7) << " learned (sends per track below)\n";
        }

        learned.tracks.resize (numTracks);  // returns only helped the fit
        for (size_t t = 0; t < numTracks; ++t)
        {
            auto& lt = learned.tracks[t];
            lt.hasProcessing = hasChain[t];
            lt.processing = chains[t];
            std::ostringstream os;
            os.precision (1);
            os << std::fixed << lt.levelLu;
            std::cerr << "    " << (lt.present ? "" : "(unused) ") << lt.label << " [" << toString (lt.role) << "] " << os.str() << " LU";
            if (lt.hasProcessing && lt.processing.reverb.fromReturn)
                std::cerr << ", send -> reverb " << lt.processing.reverb.decayS << " s, mix " << static_cast<int> (lt.processing.reverb.mixPct) << "%";
            std::cerr << "\n";
        }
        songs.push_back (std::move (learned));
    }

    if (songs.empty())
    {
        std::cerr << "no songs learned\n";
        return 1;
    }
    const auto profile = style::combineSongs (genre, songs, mixes, 2, masterChains);
    std::ofstream (outPath) << profile.toJson().dump (1);

    std::cerr << "\n" << genre << ": " << songs.size() << " songs (" << static_cast<int> (secondsSince (started)) << " s)\nbalance vs lead vocal (LU):\n";
    for (auto& [role, lu] : profile.balanceLu)
        std::cerr << "  " << koreanName (role) << " " << lu << "  (" << profile.samples.at (role) << " tracks)\n";
    if (! profile.processing.empty())
    {
        std::cerr << "processing per instrument:\n";
        for (auto& [role, p] : profile.processing)
            std::cerr << "  " << koreanName (role) << " (" << p.tracks << "): " << p.describe() << "\n";
    }
    if (profile.hasMasterProcessing)
        std::cerr << "  master chain: " << profile.masterProcessing.describe() << "\n";
    std::cerr << "master: " << profile.master.integratedLufs << " LUFS, PLR " << profile.master.plrDb << " dB\n";
    std::cerr << "\nwrote " << outPath << " (copy it to SoundManagerAI/models/), " << labelFile.u8string() << (labels.empty() ? "" : ".new")
              << " (edit labels, rerun), ear-tracks.csv\n";
    return 0;
}
