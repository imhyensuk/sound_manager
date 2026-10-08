#include "smix/style/StyleProfile.h"

#include <algorithm>
#include <cmath>

namespace smix::style
{
namespace
{
constexpr double kPi = 3.141592653589793;

float toDb (double p) { return static_cast<float> (10.0 * std::log10 (std::max (p, 1.0e-12))); }

double lufsOf (double meanSquare) { return -0.691 + 10.0 * std::log10 (std::max (meanSquare, 1.0e-12)); }

void appendGoal (std::vector<Goal>& goals, SoundGoal g, float amount)
{
    goals.push_back ({ g, amount });
}
} // namespace

float thirdOctaveCentreHz (int band)
{
    return static_cast<float> (1000.0 * std::pow (2.0, (band - 17) / 3.0));  // band 17 = 1 kHz, band 0 = 20 Hz
}

//==============================================================================
std::vector<std::string> StyleProfile::descriptors() const
{
    std::vector<std::string> d;
    if (tiltDbPerOctave > -3.0f)       d.push_back ("밝음(bright)");
    else if (tiltDbPerOctave < -5.0f)  d.push_back ("따뜻하고 어두움(warm/dark)");
    if (bandLevelDb[0] - bandLevelDb[4] > 1.0f) d.push_back ("서브가 풍부함(sub-heavy)");
    if (plrDb < 9.0f)                  d.push_back ("강하게 리미팅됨(loud/limited)");
    else if (plrDb > 14.0f)            d.push_back ("다이내믹이 큼(dynamic)");
    if (loudnessRangeLu < 4.0f)        d.push_back ("레벨이 일정함(dense)");
    if (widthMid > 0.35f)              d.push_back ("넓은 스테레오(wide)");
    else if (widthMid < 0.12f)         d.push_back ("센터 중심(narrow)");
    if (transientDensity > 3.0f && crestDb > 12.0f) d.push_back ("펀치감(punchy)");
    if (widthLow < 0.05f)              d.push_back ("저역 모노(mono low end)");
    return d;
}

nlohmann::json StyleProfile::toJson() const
{
    nlohmann::json third = nlohmann::json::array();
    for (auto v : thirdOctaveDb) third.push_back (std::round (v * 10.0f) / 10.0f);
    nlohmann::json bands = nlohmann::json::object();
    for (int b = 0; b < SpectrumBands::kNumBands; ++b)
        bands[SpectrumBands::kNames[static_cast<size_t> (b)]] = std::round (bandLevelDb[static_cast<size_t> (b)] * 10.0f) / 10.0f;
    return { { "name", name }, { "source", sourceFile }, { "seconds", seconds }, { "third_octave_db", third },
             { "band_level_db", bands }, { "integrated_lufs", integratedLufs }, { "loudness_range_lu", loudnessRangeLu },
             { "peak_db", peakDb }, { "plr_db", plrDb }, { "crest_db", crestDb },
             { "width", { widthLow, widthMid, widthHigh } }, { "correlation", correlation },
             { "transient_density", transientDensity }, { "tilt_db_per_octave", tiltDbPerOctave },
             { "centroid_hz", centroidHz }, { "descriptors", descriptors() } };
}

StyleProfile StyleProfile::fromJson (const nlohmann::json& j)
{
    StyleProfile p;
    p.name = j.value ("name", std::string {});
    p.sourceFile = j.value ("source", std::string {});
    p.seconds = j.value ("seconds", 0.0);
    if (j.contains ("third_octave_db"))
        for (int i = 0; i < kThirdOctaveBands && i < static_cast<int> (j["third_octave_db"].size()); ++i)
            p.thirdOctaveDb[static_cast<size_t> (i)] = j["third_octave_db"][static_cast<size_t> (i)].get<float>();
    if (j.contains ("band_level_db"))
        for (int b = 0; b < SpectrumBands::kNumBands; ++b)
            p.bandLevelDb[static_cast<size_t> (b)] = j["band_level_db"].value (SpectrumBands::kNames[static_cast<size_t> (b)], -60.0f);
    p.integratedLufs = j.value ("integrated_lufs", -70.0f);
    p.loudnessRangeLu = j.value ("loudness_range_lu", 0.0f);
    p.peakDb = j.value ("peak_db", -120.0f);
    p.plrDb = j.value ("plr_db", 0.0f);
    p.crestDb = j.value ("crest_db", 0.0f);
    if (j.contains ("width") && j["width"].size() == 3)
    {
        p.widthLow = j["width"][0].get<float>();
        p.widthMid = j["width"][1].get<float>();
        p.widthHigh = j["width"][2].get<float>();
    }
    p.correlation = j.value ("correlation", 1.0f);
    p.transientDensity = j.value ("transient_density", 0.0f);
    p.tiltDbPerOctave = j.value ("tilt_db_per_octave", 0.0f);
    p.centroidHz = j.value ("centroid_hz", 0.0f);
    return p;
}

//==============================================================================
StyleAnalyzer::StyleAnalyzer (double sr) : sampleRate (sr > 0 ? sr : 48000.0)
{
    window.resize (kSize);
    for (int i = 0; i < kSize; ++i)
        window[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * i / (kSize - 1)));
    fifoL.assign (kSize, 0.0f);
    fifoR.assign (kSize, 0.0f);
    bufMid.assign (kSize, {});
    bufSide.assign (kSize, {});
    prevMag.assign (kSize / 2, 0.0f);
    binThird.assign (kSize / 2, -1);
    binBand.assign (kSize / 2, -1);
    for (int bin = 1; bin < kSize / 2; ++bin)
    {
        const double hz = bin * sampleRate / kSize;
        if (hz < 17.8 || hz >= 22400.0)
            continue;
        binThird[static_cast<size_t> (bin)] = std::clamp (static_cast<int> (std::lround (17.0 + 3.0 * std::log2 (hz / 1000.0))), 0,
                                                          kThirdOctaveBands - 1);
        if (hz >= SpectrumBands::kEdgesHz.front() && hz < SpectrumBands::kEdgesHz.back())
            binBand[static_cast<size_t> (bin)] = SpectrumBands::bandForFrequency (static_cast<float> (hz));
    }

    // K-weighting (same derivation as AudioAnalyzer)
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / sampleRate), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& f : pre)
        {
            f.b0 = (Vh + Vb * K / Q + K * K) / a0; f.b1 = 2.0 * (K * K - Vh) / a0; f.b2 = (Vh - Vb * K / Q + K * K) / a0;
            f.a1 = 2.0 * (K * K - 1.0) / a0; f.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773, K = std::tan (kPi * f0 / sampleRate);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& f : rlb)
        {
            f.b0 = 1.0; f.b1 = -2.0; f.b2 = 1.0; f.a1 = 2.0 * (K * K - 1.0) / a0; f.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    blockLength = std::max (1L, static_cast<long> (sampleRate * 0.1));
}

void StyleAnalyzer::process (const float* left, const float* right, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const double l = left[i], r = right != nullptr ? right[i] : left[i];
        fifoL[static_cast<size_t> (fifoPos)] = static_cast<float> (l);
        fifoR[static_cast<size_t> (fifoPos)] = static_cast<float> (r);
        fifoPos = (fifoPos + 1) & (kSize - 1);
        if (++sinceHop >= kHop)
        {
            sinceHop = 0;
            analyseFrame();
        }

        peak = std::max ({ peak, std::abs (l), std::abs (r) });
        sumSquares += 0.5 * (l * l + r * r);
        lr += l * r; ll += l * l; rr += r * r;
        const double kl = rlb[0].process (pre[0].process (l)), kr = rlb[1].process (pre[1].process (r));
        blockPower += kl * kl + kr * kr;
        if (++blockSamples >= blockLength)
            finishBlock();
    }
    samplesTotal += n;
}

void StyleAnalyzer::finishBlock()
{
    blocks.push_back (blockPower / static_cast<double> (blockSamples));
    blockPower = 0;
    blockSamples = 0;
}

void StyleAnalyzer::analyseFrame()
{
    for (int i = 0; i < kSize; ++i)
    {
        const auto idx = static_cast<size_t> ((fifoPos + i) & (kSize - 1));
        const float w = window[static_cast<size_t> (i)];
        bufMid[static_cast<size_t> (i)] = { 0.5f * (fifoL[idx] + fifoR[idx]) * w, 0.0f };
        bufSide[static_cast<size_t> (i)] = { 0.5f * (fifoL[idx] - fifoR[idx]) * w, 0.0f };
    }
    fft.perform (bufMid.data());
    fft.perform (bufSide.data());

    double flux = 0, total = 0;
    for (int bin = 1; bin < kSize / 2; ++bin)
    {
        const auto b = static_cast<size_t> (bin);
        const double m = std::norm (bufMid[b]), s = std::norm (bufSide[b]);
        const double hz = bin * sampleRate / kSize;
        const double p = m + s;
        if (binThird[b] >= 0) thirdPower[static_cast<size_t> (binThird[b])] += p;
        if (binBand[b] >= 0)  bandPower[static_cast<size_t> (binBand[b])] += p;
        if (hz < 150)       { midLow += m; sideLow += s; }
        else if (hz < 4000) { midMid += m; sideMid += s; }
        else                { midHigh += m; sideHigh += s; }
        centroidNum += p * hz;
        centroidDen += p;
        total += p;
        const float mag = static_cast<float> (std::sqrt (m));
        flux += std::max (0.0f, mag - prevMag[b]);
        prevMag[b] = mag;
    }
    if (total < 1.0e-9)
        return;

    if (frames > 4 && hold == 0 && flux > 1.6 * fluxMean + 1.0e-6)
    {
        ++onsets;
        hold = static_cast<long> (sampleRate / kHop * 0.05) + 1;
    }
    else if (hold > 0)
    {
        --hold;
    }
    fluxMean += (frames == 0 ? 1.0 : 0.05) * (flux - fluxMean);
    ++frames;
}

StyleProfile StyleAnalyzer::finish (const std::string& name, const std::string& sourceFile)
{
    if (blockSamples > 0)
        finishBlock();

    StyleProfile p;
    p.name = name;
    p.sourceFile = sourceFile;
    p.seconds = samplesTotal / sampleRate;

    double thirdTotal = 0, bandTotal = 0;
    for (auto v : thirdPower) thirdTotal += v;
    for (auto v : bandPower) bandTotal += v;
    for (size_t i = 0; i < thirdPower.size(); ++i)
        p.thirdOctaveDb[i] = toDb (thirdPower[i] / std::max (thirdTotal, 1.0e-20));
    for (size_t i = 0; i < bandPower.size(); ++i)
        p.bandLevelDb[i] = toDb (bandPower[i] / std::max (bandTotal, 1.0e-20));

    // Integrated loudness: 400 ms blocks (75 % overlap), absolute then relative gate.
    std::vector<double> momentary;
    for (size_t i = 3; i < blocks.size(); ++i)
        momentary.push_back ((blocks[i] + blocks[i - 1] + blocks[i - 2] + blocks[i - 3]) / 4.0);
    double sum = 0;
    long count = 0;
    for (auto m : momentary)
        if (lufsOf (m) > -70.0) { sum += m; ++count; }
    if (count > 0)
    {
        const double relGate = lufsOf (sum / count) - 10.0;
        double s2 = 0;
        long c2 = 0;
        for (auto m : momentary)
            if (lufsOf (m) > -70.0 && lufsOf (m) > relGate) { s2 += m; ++c2; }
        p.integratedLufs = static_cast<float> (lufsOf (s2 / std::max (1L, c2)));
    }

    // LRA: 3 s short-term windows (every 100 ms), gated at -70 / -20 LU, p95 - p10.
    std::vector<double> shortTerm;
    for (size_t i = 29; i < blocks.size(); ++i)
    {
        double s = 0;
        for (size_t k = i - 29; k <= i; ++k) s += blocks[k];
        const double l = lufsOf (s / 30.0);
        if (l > -70.0 && l > p.integratedLufs - 20.0)
            shortTerm.push_back (l);
    }
    if (shortTerm.size() > 4)
    {
        std::sort (shortTerm.begin(), shortTerm.end());
        const auto at = [&shortTerm] (double q) { return shortTerm[static_cast<size_t> (q * (shortTerm.size() - 1))]; };
        p.loudnessRangeLu = static_cast<float> (at (0.95) - at (0.10));
    }

    p.peakDb = static_cast<float> (20.0 * std::log10 (std::max (peak, 1.0e-6)));
    p.plrDb = p.peakDb - p.integratedLufs;
    const double rms = std::sqrt (sumSquares / std::max (1.0, samplesTotal));
    p.crestDb = static_cast<float> (20.0 * std::log10 (std::max (peak, 1.0e-6) / std::max (rms, 1.0e-9)));
    p.widthLow = static_cast<float> (sideLow / std::max (midLow, 1.0e-12));
    p.widthMid = static_cast<float> (sideMid / std::max (midMid, 1.0e-12));
    p.widthHigh = static_cast<float> (sideHigh / std::max (midHigh, 1.0e-12));
    p.correlation = static_cast<float> (lr / std::max (std::sqrt (ll * rr), 1.0e-12));
    p.transientDensity = static_cast<float> (onsets / std::max (1.0e-3, p.seconds));
    p.centroidHz = static_cast<float> (centroidNum / std::max (centroidDen, 1.0e-12));

    // Tilt: least-squares slope of the 1/3-octave curve, 100 Hz .. 10 kHz, in dB per octave.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (int b = 0; b < kThirdOctaveBands; ++b)
    {
        const double hz = thirdOctaveCentreHz (b);
        if (hz < 100.0 || hz > 10000.0)
            continue;
        const double x = std::log2 (hz / 1000.0), y = p.thirdOctaveDb[static_cast<size_t> (b)];
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
    }
    if (n > 2)
        p.tiltDbPerOctave = static_cast<float> ((n * sxy - sx * sy) / std::max (1.0e-9, n * sxx - sx * sx));
    return p;
}

//==============================================================================
std::map<InstrumentRole, std::vector<Goal>> styleGoalsFor (const StyleProfile& r)
{
    std::map<InstrumentRole, std::vector<Goal>> g;
    const bool bright = r.tiltDbPerOctave > -3.0f;
    const bool dark = r.tiltDbPerOctave < -5.0f;
    const bool limited = r.plrDb < 9.0f;
    const bool punchy = r.transientDensity > 3.0f && r.crestDb > 12.0f;
    const bool wide = r.widthMid > 0.35f;
    const bool subHeavy = r.bandLevelDb[0] - r.bandLevelDb[4] > 1.0f;

    if (bright)
    {
        appendGoal (g[InstrumentRole::LeadVocal], SoundGoal::Brighter, 0.5f);
        appendGoal (g[InstrumentRole::LeadVocal], SoundGoal::MoreAir, 0.5f);
        appendGoal (g[InstrumentRole::HiHat], SoundGoal::Brighter, 0.4f);
        appendGoal (g[InstrumentRole::Overheads], SoundGoal::MoreAir, 0.4f);
    }
    if (dark)
    {
        appendGoal (g[InstrumentRole::LeadVocal], SoundGoal::Warmer, 0.5f);
        appendGoal (g[InstrumentRole::ElectricGuitar], SoundGoal::Warmer, 0.5f);
        appendGoal (g[InstrumentRole::Keys], SoundGoal::Warmer, 0.5f);
    }
    if (limited)
    {
        appendGoal (g[InstrumentRole::LeadVocal], SoundGoal::MoreControlled, 0.6f);
        appendGoal (g[InstrumentRole::Bass], SoundGoal::MoreControlled, 0.6f);
    }
    else
    {
        appendGoal (g[InstrumentRole::LeadVocal], SoundGoal::MoreDynamic, 0.3f);
    }
    if (punchy)
    {
        appendGoal (g[InstrumentRole::Kick], SoundGoal::Punchier, 0.6f);
        appendGoal (g[InstrumentRole::Snare], SoundGoal::Punchier, 0.6f);
    }
    if (subHeavy)
    {
        appendGoal (g[InstrumentRole::Kick], SoundGoal::MoreBody, 0.5f);
        appendGoal (g[InstrumentRole::Bass], SoundGoal::MoreBody, 0.5f);
    }
    else
    {
        appendGoal (g[InstrumentRole::Kick], SoundGoal::Tighter, 0.5f);
    }
    if (wide)
    {
        appendGoal (g[InstrumentRole::Pad], SoundGoal::Wider, 0.6f);
        appendGoal (g[InstrumentRole::BackingVocal], SoundGoal::Wider, 0.5f);
    }
    appendGoal (g[InstrumentRole::LeadVocal], SoundGoal::Forward, 0.4f);
    return g;
}

StyleMatch matchStyle (const StyleProfile& ref, const ChannelState& target, float amount)
{
    StyleMatch m;
    m.roleGoals = styleGoalsFor (ref);
    RecipeEngine recipes;

    const auto& f = target.features;
    if (! f.valid)
    {
        m.notes.push_back ("현재 믹스를 아직 듣지 못했어요. 재생한 뒤 다시 시도해 주세요.");
        return m;
    }

    auto add = [&m] (RecipeEngine::Result r) {
        m.actions.insert (m.actions.end(), r.actions.begin(), r.actions.end());
        m.notes.insert (m.notes.end(), r.notes.begin(), r.notes.end());
    };

    // 1) Tonal balance: the three largest band deviations, partially corrected (broad, gentle).
    std::vector<std::pair<float, int>> deviations;
    for (int b = 0; b < SpectrumBands::kNumBands; ++b)
    {
        const float d = f.bandLevelDb[static_cast<size_t> (b)] - ref.bandLevelDb[static_cast<size_t> (b)];
        if (std::abs (d) > 1.5f)
            deviations.emplace_back (std::abs (d), b);
    }
    std::sort (deviations.rbegin(), deviations.rend());
    if (deviations.size() > 3)
        deviations.resize (3);
    for (auto& [absDev, b] : deviations)
    {
        const float dev = f.bandLevelDb[static_cast<size_t> (b)] - ref.bandLevelDb[static_cast<size_t> (b)];
        const double gain = std::clamp (-0.6 * dev * amount, -4.0, 4.0);
        add (recipes.eq (target, SpectrumBands::centreHz (b), gain));
        char buf[160];
        std::snprintf (buf, sizeof (buf), "%s 대역이 레퍼런스보다 %+.1f dB → EQ %+.1f dB", SpectrumBands::kNames[static_cast<size_t> (b)],
                       dev, gain);
        m.notes.push_back (buf);
    }

    // 2) Density: crest factor relative to the reference.
    const float crestDiff = f.crestDb - ref.crestDb;
    if (crestDiff > 2.0f)
    {
        add (recipes.nudge (target, PluginCategory::Compressor, ParamRole::Threshold, -std::min (6.0, crestDiff * 0.75 * amount), "dB",
                            "레퍼런스만큼 촘촘하게"));
        m.notes.push_back ("레퍼런스보다 다이내믹이 커요(크레스트 +" + std::to_string (static_cast<int> (crestDiff)) + " dB) → 버스 압축 강화");
    }
    else if (crestDiff < -2.0f)
    {
        add (recipes.nudge (target, PluginCategory::Compressor, ParamRole::Threshold, std::min (6.0, -crestDiff * 0.75 * amount), "dB",
                            "레퍼런스만큼 다이내믹하게"));
        m.notes.push_back ("레퍼런스보다 많이 눌려 있어요 → 버스 압축 완화");
    }

    // 3) Loudness (master only): drive the limiter towards the reference level.
    if (target.kind == ChannelKind::Master && f.integratedLufs > -60.0f)
    {
        const float diff = ref.integratedLufs - f.integratedLufs;
        if (std::abs (diff) > 1.0f)
        {
            const double step = std::clamp (static_cast<double> (diff) * amount, -6.0, 6.0);
            auto r = recipes.nudge (target, PluginCategory::Limiter, ParamRole::InputGain, step, "dB", "레퍼런스 라우드니스");
            if (r.actions.empty())
                r = recipes.nudge (target, PluginCategory::Limiter, ParamRole::Threshold, -step, "dB", "레퍼런스 라우드니스");
            add (r);
            char buf[160];
            std::snprintf (buf, sizeof (buf), "레퍼런스 %.1f LUFS / 현재 %.1f LUFS", ref.integratedLufs, f.integratedLufs);
            m.notes.push_back (buf);
        }
    }

    // 4) Width.
    const float widthDiff = ref.widthMid - f.stereoWidth;
    if (std::abs (widthDiff) > 0.08f)
        add (recipes.nudge (target, PluginCategory::StereoImager, ParamRole::Width, std::clamp (widthDiff * 0.5, -0.1, 0.1) * amount, "",
                            widthDiff > 0 ? "레퍼런스처럼 넓게" : "레퍼런스처럼 좁게"));
    return m;
}

} // namespace smix::style
