#include "smix/style/GenreProfile.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>

#include "smix/FFT.h"

namespace smix::style
{
namespace
{
std::shared_ptr<const GenreProfile> g_active;

// Approximate ITU-R BS.1770 K-weighting per band (dB), to turn band energies into loudness.
constexpr std::array<double, SpectrumBands::kNumBands> kKWeightDb { -4.0, -1.0, 0.0, 0.0, 0.0, 0.5, 2.5, 3.8, 4.0, 4.0 };

float median (std::vector<float> v)
{
    if (v.empty())
        return 0.0f;
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/** min ||A x - y||^2, x >= 0. A is rows x cols (row-major). Projected gradient with a 1/L step. */
std::vector<double> nnls (const std::vector<double>& A, const std::vector<double>& y, size_t rows, size_t cols)
{
    // Column scaling makes the problem well conditioned.
    std::vector<double> scale (cols, 0.0);
    for (size_t c = 0; c < cols; ++c)
    {
        double s = 0;
        for (size_t r = 0; r < rows; ++r) s += A[r * cols + c] * A[r * cols + c];
        scale[c] = s > 0 ? 1.0 / std::sqrt (s) : 0.0;
    }
    std::vector<double> AtA (cols * cols, 0.0), Aty (cols, 0.0);
    for (size_t r = 0; r < rows; ++r)
        for (size_t i = 0; i < cols; ++i)
        {
            const double ai = A[r * cols + i] * scale[i];
            Aty[i] += ai * y[r];
            for (size_t j = 0; j < cols; ++j)
                AtA[i * cols + j] += ai * A[r * cols + j] * scale[j];
        }
    // Largest eigenvalue (power iteration) -> step size.
    std::vector<double> v (cols, 1.0), w (cols);
    double L = 1.0;
    for (int it = 0; it < 50; ++it)
    {
        double norm = 0;
        for (size_t i = 0; i < cols; ++i)
        {
            w[i] = 0;
            for (size_t j = 0; j < cols; ++j) w[i] += AtA[i * cols + j] * v[j];
            norm += w[i] * w[i];
        }
        norm = std::sqrt (norm);
        if (norm <= 0) break;
        L = norm;
        for (size_t i = 0; i < cols; ++i) v[i] = w[i] / norm;
    }
    std::vector<double> x (cols, 0.0), g (cols);
    for (int it = 0; it < 4000; ++it)
    {
        for (size_t i = 0; i < cols; ++i)
        {
            g[i] = -Aty[i];
            for (size_t j = 0; j < cols; ++j) g[i] += AtA[i * cols + j] * x[j];
        }
        for (size_t i = 0; i < cols; ++i)
            x[i] = std::max (0.0, x[i] - g[i] / L);
    }
    for (size_t i = 0; i < cols; ++i)
        x[i] *= scale[i];
    return x;
}
} // namespace

bool isAuxiliaryTrackName (const std::string& fileName)
{
    const auto n = toLowerAscii (fileName);
    return containsAny (n, { "click", "metronome", "metro", "guide", "cue", "talkback", "count-in", "countin", "timecode", "smpte",
                             "클릭", "메트로놈", "가이드", "카운트" });
}

void setActiveGenre (std::shared_ptr<const GenreProfile> p)
{
    std::atomic_store (&g_active, std::move (p));
}

std::shared_ptr<const GenreProfile> activeGenre()
{
    return std::atomic_load (&g_active);
}

nlohmann::json GenreProfile::toJson() const
{
    nlohmann::json balance = nlohmann::json::object(), curvesJson = nlohmann::json::object(), n = nlohmann::json::object();
    for (auto& [r, v] : balanceLu) balance[toString (r)] = std::round (v * 10.0f) / 10.0f;
    for (auto& [r, c] : curves)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto v : c) arr.push_back (std::round (v * 10.0f) / 10.0f);
        curvesJson[toString (r)] = arr;
    }
    for (auto& [r, k] : samples) n[toString (r)] = k;
    return { { "format", "smix-genre-1" }, { "name", name }, { "songs", songs }, { "balance_lu", balance },
             { "curves_db", curvesJson }, { "tracks_per_role", n }, { "master", master.toJson() } };
}

GenreProfile GenreProfile::fromJson (const nlohmann::json& j)
{
    GenreProfile g;
    g.name = j.value ("name", std::string {});
    g.songs = j.value ("songs", 0);
    // Keep each sub-object alive while iterating it (items() of a temporary would dangle).
    const auto balance = j.value ("balance_lu", nlohmann::json::object());
    const auto curvesJson = j.value ("curves_db", nlohmann::json::object());
    const auto counts = j.value ("tracks_per_role", nlohmann::json::object());
    for (auto& [k, v] : balance.items())
        if (auto r = roleFromString (k)) g.balanceLu[*r] = v.get<float>();
    for (auto& [k, v] : curvesJson.items())
        if (auto r = roleFromString (k); r && v.size() == SpectrumBands::kNumBands)
            for (size_t b = 0; b < SpectrumBands::kNumBands; ++b) g.curves[*r][b] = v[b].get<float>();
    for (auto& [k, v] : counts.items())
        if (auto r = roleFromString (k)) g.samples[*r] = v.get<int>();
    if (j.contains ("master"))
        g.master = StyleProfile::fromJson (j["master"]);
    return g;
}

BandFrames bandFrames (const float* x, std::size_t n, double sr, double frameSeconds)
{
    constexpr int order = 12, size = 1 << order;
    FFT fft (order);
    std::vector<std::complex<float>> buf (size);
    std::vector<int> binBand (size / 2, -1);
    for (int b = 1; b < size / 2; ++b)
    {
        const float hz = static_cast<float> (b * sr / size);
        if (hz >= SpectrumBands::kEdgesHz.front() && hz < SpectrumBands::kEdgesHz.back())
            binBand[static_cast<size_t> (b)] = SpectrumBands::bandForFrequency (hz);
    }
    const size_t frameLen = static_cast<size_t> (sr * frameSeconds);
    BandFrames out;
    for (size_t start = 0; start + frameLen <= n; start += frameLen)
    {
        std::array<double, SpectrumBands::kNumBands> e {};
        for (size_t s = start; s + size <= start + frameLen + size / 2 && s + size <= n; s += size / 2)
        {
            for (int i = 0; i < size; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (6.2831853f * static_cast<float> (i) / (size - 1));
                buf[static_cast<size_t> (i)] = { x[s + static_cast<size_t> (i)] * w, 0.0f };
            }
            fft.perform (buf.data());
            for (int b = 1; b < size / 2; ++b)
                if (binBand[static_cast<size_t> (b)] >= 0)
                    e[static_cast<size_t> (binBand[static_cast<size_t> (b)])] += std::norm (buf[static_cast<size_t> (b)]);
        }
        out.push_back (e);
    }
    return out;
}

LearnedSong learnSong (const std::string& name, const std::vector<LearnTrack>& tracks, const BandFrames& mix)
{
    LearnedSong song;
    song.name = name;
    size_t frames = mix.size();
    for (auto& t : tracks)
        frames = std::min (frames, t.frames.size());
    const size_t T = tracks.size();
    if (frames < 8 || T == 0)
        return song;

    // Frames where the mix is playing.
    std::vector<size_t> rowsUsed;
    double mixPeak = 0;
    for (size_t f = 0; f < frames; ++f)
    {
        double s = 0;
        for (auto v : mix[f]) s += v;
        mixPeak = std::max (mixPeak, s);
    }
    for (size_t f = 0; f < frames; ++f)
    {
        double s = 0;
        for (auto v : mix[f]) s += v;
        if (s > mixPeak * 1.0e-4)
            rowsUsed.push_back (f);
    }
    const size_t R = rowsUsed.size();

    std::vector<std::array<double, SpectrumBands::kNumBands>> gain (T);  // power gain per track and band
    for (size_t b = 0; b < SpectrumBands::kNumBands; ++b)
    {
        std::vector<double> A (R * T), y (R);
        for (size_t r = 0; r < R; ++r)
        {
            y[r] = mix[rowsUsed[r]][b];
            for (size_t t = 0; t < T; ++t)
                A[r * T + t] = tracks[t].frames[rowsUsed[r]][b];
        }
        const auto x = nnls (A, y, R, T);
        double ssRes = 0, ssTot = 0, mean = 0;
        for (auto v : y) mean += v;
        mean /= std::max<size_t> (1, R);
        for (size_t r = 0; r < R; ++r)
        {
            double pred = 0;
            for (size_t t = 0; t < T; ++t) pred += A[r * T + t] * x[t];
            ssRes += (y[r] - pred) * (y[r] - pred);
            ssTot += (y[r] - mean) * (y[r] - mean);
        }
        song.fit[b] = ssTot > 0 ? static_cast<float> (1.0 - ssRes / ssTot) : 0.0f;
        for (size_t t = 0; t < T; ++t)
            gain[t][b] = x[t];
    }

    // In-mix energy and loudness of each track.
    std::vector<double> loud (T, 0.0);
    double totalMix = 0;
    for (size_t t = 0; t < T; ++t)
    {
        LearnedTrack lt;
        lt.name = tracks[t].name;
        lt.label = tracks[t].label;
        lt.role = tracks[t].role;
        std::array<double, SpectrumBands::kNumBands> inMix {}, raw {};
        double sum = 0;
        for (size_t b = 0; b < SpectrumBands::kNumBands; ++b)
        {
            for (auto f : rowsUsed) raw[b] += tracks[t].frames[f][b];
            inMix[b] = gain[t][b] * raw[b];
            sum += inMix[b];
            loud[t] += inMix[b] * std::pow (10.0, kKWeightDb[b] / 10.0);
        }
        totalMix += loud[t];
        // Band gains relative to the track's overall gain: the shape of the engineer's EQ.
        double rawSum = 0;
        for (auto v : raw) rawSum += v;
        const double overall = rawSum > 0 ? sum / rawSum : 0.0;
        for (size_t b = 0; b < SpectrumBands::kNumBands; ++b)
        {
            lt.curveDb[b] = static_cast<float> (10.0 * std::log10 (std::max (inMix[b] / std::max (sum, 1.0e-20), 1.0e-6)));
            lt.bandGainDb[b] = raw[b] > 0 && overall > 0 ? static_cast<float> (10.0 * std::log10 (std::max (gain[t][b] / overall, 1.0e-6))) : 0.0f;
        }
        song.tracks.push_back (lt);
    }
    for (size_t t = 0; t < T; ++t)
        song.tracks[t].present = totalMix > 0 && loud[t] / totalMix > 1.0e-3;  // above -30 dB of the mix

    // Anchor: the loudest lead vocal, else the loudest kick, else the loudest track.
    int anchor = -1;
    for (auto role : { InstrumentRole::LeadVocal, InstrumentRole::Kick })
    {
        for (size_t t = 0; t < T; ++t)
            if (song.tracks[t].role == role && song.tracks[t].present && (anchor < 0 || loud[t] > loud[static_cast<size_t> (anchor)]))
                anchor = static_cast<int> (t);
        if (anchor >= 0)
            break;
    }
    if (anchor < 0)
        anchor = static_cast<int> (std::max_element (loud.begin(), loud.end()) - loud.begin());
    song.anchor = song.tracks[static_cast<size_t> (anchor)].name;
    const double ref = std::max (loud[static_cast<size_t> (anchor)], 1.0e-20);
    for (size_t t = 0; t < T; ++t)
        song.tracks[t].levelLu = static_cast<float> (10.0 * std::log10 (std::max (loud[t], 1.0e-20) / ref));
    return song;
}

GenreProfile combineSongs (const std::string& genre, const std::vector<LearnedSong>& songs, const std::vector<StyleProfile>& mixes, int minTracks)
{
    GenreProfile g;
    g.name = genre;
    g.songs = static_cast<int> (songs.size());
    std::map<InstrumentRole, std::vector<float>> levels;
    std::map<InstrumentRole, std::array<std::vector<float>, SpectrumBands::kNumBands>> bands;
    for (auto& s : songs)
        for (auto& t : s.tracks)
        {
            if (! t.present || t.role == InstrumentRole::Unknown)
                continue;
            levels[t.role].push_back (t.levelLu);
            for (size_t b = 0; b < SpectrumBands::kNumBands; ++b)
                bands[t.role][b].push_back (t.curveDb[b]);
        }
    for (auto& [role, v] : levels)
    {
        if (static_cast<int> (v.size()) < minTracks)
            continue;
        g.samples[role] = static_cast<int> (v.size());
        g.balanceLu[role] = role == InstrumentRole::LeadVocal ? 0.0f : median (v);
        for (size_t b = 0; b < SpectrumBands::kNumBands; ++b)
            g.curves[role][b] = median (bands[role][b]);
    }

    // Master: mean of the finished mixes (tone in dB, loudness, dynamics, width).
    if (! mixes.empty())
    {
        g.master = mixes.front();
        g.master.name = genre + " (" + std::to_string (mixes.size()) + "곡 평균)";
        g.master.sourceFile.clear();
        auto avg = [&mixes] (auto get) { float s = 0; for (auto& m : mixes) s += get (m); return s / static_cast<float> (mixes.size()); };
        for (size_t b = 0; b < g.master.bandLevelDb.size(); ++b)
            g.master.bandLevelDb[b] = avg ([b] (const StyleProfile& m) { return m.bandLevelDb[b]; });
        for (size_t b = 0; b < g.master.thirdOctaveDb.size(); ++b)
            g.master.thirdOctaveDb[b] = avg ([b] (const StyleProfile& m) { return m.thirdOctaveDb[b]; });
        g.master.integratedLufs = avg ([] (const StyleProfile& m) { return m.integratedLufs; });
        g.master.loudnessRangeLu = avg ([] (const StyleProfile& m) { return m.loudnessRangeLu; });
        g.master.peakDb = avg ([] (const StyleProfile& m) { return m.peakDb; });
        g.master.plrDb = avg ([] (const StyleProfile& m) { return m.plrDb; });
        g.master.crestDb = avg ([] (const StyleProfile& m) { return m.crestDb; });
        g.master.widthLow = avg ([] (const StyleProfile& m) { return m.widthLow; });
        g.master.widthMid = avg ([] (const StyleProfile& m) { return m.widthMid; });
        g.master.widthHigh = avg ([] (const StyleProfile& m) { return m.widthHigh; });
        g.master.correlation = avg ([] (const StyleProfile& m) { return m.correlation; });
        g.master.transientDensity = avg ([] (const StyleProfile& m) { return m.transientDensity; });
        g.master.tiltDbPerOctave = avg ([] (const StyleProfile& m) { return m.tiltDbPerOctave; });
        g.master.centroidHz = avg ([] (const StyleProfile& m) { return m.centroidHz; });
        double secs = 0;
        for (auto& m : mixes) secs += m.seconds;
        g.master.seconds = secs;
    }
    return g;
}

} // namespace smix::style
