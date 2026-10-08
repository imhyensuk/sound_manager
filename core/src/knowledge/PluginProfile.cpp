#include "smix/knowledge/PluginProfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <sstream>

#include "smix/FFT.h"

namespace smix::knowledge
{
namespace
{
float toDb (double power) { return static_cast<float> (10.0 * std::log10 (std::max (power, 1.0e-14))); }

/** Deterministic noise so that two measurements differ only because of the plugin. */
struct Rng
{
    std::uint32_t s = 0x12345678u;
    float next() { s = s * 1664525u + 1013904223u; return static_cast<float> (s >> 8) / 8388608.0f - 1.0f; }
};

struct Stimuli
{
    std::vector<float> noiseL, noiseR, burstL, burstR, impulse, sine;
};

Stimuli makeStimuli (double sr, double seconds)
{
    const auto n = static_cast<size_t> (sr * seconds);
    Stimuli s;
    s.noiseL.resize (n); s.noiseR.resize (n); s.burstL.resize (n); s.burstR.resize (n);
    s.impulse.assign (n, 0.0f); s.sine.resize (n);

    Rng rng;
    float b0 = 0, b1 = 0, b2 = 0, c0 = 0, c1 = 0, c2 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        // Pink noise (Paul Kellet), partly correlated between channels (known moderate width).
        const float w1 = rng.next(), w2 = rng.next();
        b0 = 0.99765f * b0 + w1 * 0.0990460f; b1 = 0.96300f * b1 + w1 * 0.2965164f; b2 = 0.57000f * b2 + w1 * 1.0526913f;
        c0 = 0.99765f * c0 + w2 * 0.0990460f; c1 = 0.96300f * c1 + w2 * 0.2965164f; c2 = 0.57000f * c2 + w2 * 1.0526913f;
        const float p1 = 0.05f * (b0 + b1 + b2 + w1 * 0.1848f);
        const float p2 = 0.05f * (c0 + c1 + c2 + w2 * 0.1848f);
        s.noiseL[i] = p1;
        s.noiseR[i] = 0.8f * p1 + 0.2f * p2;

        // Drum-like hits at 4 Hz: decaying 60 Hz body + noise click.
        const double t = std::fmod (static_cast<double> (i) / sr, 0.25);
        const float hit = static_cast<float> (0.6 * std::exp (-t * 25.0) * std::sin (2.0 * 3.141592653589793 * 60.0 * t)
                                              + 0.3 * std::exp (-t * 300.0) * rng.next());
        s.burstL[i] = s.burstR[i] = hit;

        s.sine[i] = static_cast<float> (0.5 * std::sin (2.0 * 3.141592653589793 * 1000.0 * static_cast<double> (i) / sr));
    }
    s.impulse[static_cast<size_t> (sr * 0.01)] = 0.7f;
    return s;
}

std::array<double, SpectrumBands::kNumBands> bandPowers (const std::vector<float>& l, const std::vector<float>& r, double sr, size_t skip)
{
    constexpr int order = 11, size = 1 << order;
    FFT fft (order);
    std::vector<std::complex<float>> buf (size);
    std::array<double, SpectrumBands::kNumBands> bands {};
    for (size_t start = skip; start + size <= l.size(); start += size / 2)
    {
        for (int i = 0; i < size; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (6.2831853f * static_cast<float> (i) / (size - 1));
            buf[static_cast<size_t> (i)] = { 0.5f * (l[start + static_cast<size_t> (i)] + r[start + static_cast<size_t> (i)]) * w, 0.0f };
        }
        fft.perform (buf.data());
        for (int bin = 1; bin < size / 2; ++bin)
        {
            const float hz = static_cast<float> (bin * sr / size);
            if (hz < SpectrumBands::kEdgesHz.front() || hz >= SpectrumBands::kEdgesHz.back())
                continue;
            bands[static_cast<size_t> (SpectrumBands::bandForFrequency (hz))] += std::norm (buf[static_cast<size_t> (bin)]);
        }
    }
    return bands;
}

std::string bandKorean (int b)
{
    static const char* names[] = { "초저역(sub)", "저역(bass)", "저중역(low-mid)", "탁한 대역(mud)", "중역(mid)",
                                   "중고역(upper-mid)", "존재감(presence)", "어택/선명도(bite)", "치찰음(sibilance)", "공기감(air)" };
    return names[std::clamp (b, 0, 9)];
}

std::string fmt1 (float v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%+.1f", v);
    return buf;
}

std::string describeEffect (const ParamEffect& e)
{
    std::vector<std::string> parts;
    int best = -1;
    float bestAbs = 0;
    float mean = 0;
    for (auto d : e.bandDeltaDb) mean += d;
    mean /= SpectrumBands::kNumBands;
    for (int b = 0; b < SpectrumBands::kNumBands; ++b)
    {
        const float rel = std::abs (e.bandDeltaDb[static_cast<size_t> (b)] - mean);
        if (rel > bestAbs) { bestAbs = rel; best = b; }
    }
    if (best >= 0 && bestAbs > 1.0f)
        parts.push_back (bandKorean (best) + " " + fmt1 (e.bandDeltaDb[static_cast<size_t> (best)] - mean) + " dB (EQ "
                         + SpectrumBands::kNames[static_cast<size_t> (best)] + ")");
    if (std::abs (e.levelDeltaDb) > 1.0f)
        parts.push_back ("전체 레벨 " + fmt1 (e.levelDeltaDb) + " dB (level/gain)");
    if (e.crestDeltaDb < -1.0f)
        parts.push_back ("더 압축됨, 크레스트 " + fmt1 (e.crestDeltaDb) + " dB (compression)");
    else if (e.crestDeltaDb > 1.0f)
        parts.push_back ("어택/다이내믹 증가, 크레스트 " + fmt1 (e.crestDeltaDb) + " dB (transient/punch)");
    if (std::abs (e.widthDelta) > 0.05f)
        parts.push_back (std::string (e.widthDelta > 0 ? "스테레오 넓어짐 (wider)" : "스테레오 좁아짐 (narrower)"));
    if (e.tailDeltaDb > 3.0f)
        parts.push_back ("잔향/에코 증가 " + fmt1 (e.tailDeltaDb) + " dB (reverb/delay mix, decay)");
    else if (e.tailDeltaDb < -3.0f)
        parts.push_back ("잔향/에코 감소 (shorter, drier)");
    if (e.distortionDelta > 0.02f)
        parts.push_back ("배음/왜곡 증가 (saturation, harmonics, drive)");
    if (parts.empty())
        return "단독으로는 들리는 변화가 거의 없음 (no measurable effect alone)";
    std::string s = "높이면: ";
    for (size_t i = 0; i < parts.size(); ++i)
        s += (i ? ", " : "") + parts[i];
    return s;
}
} // namespace

//==============================================================================
nlohmann::json ParamEffect::toJson() const
{
    nlohmann::json bands = nlohmann::json::array();
    for (auto d : bandDeltaDb) bands.push_back (std::round (d * 10.0f) / 10.0f);
    return { { "index", index }, { "name", name }, { "role", toString (role) }, { "unit", unit },
             { "low", lowText }, { "high", highText }, { "default", defaultText }, { "band_delta_db", bands },
             { "level_delta_db", levelDeltaDb }, { "crest_delta_db", crestDeltaDb }, { "width_delta", widthDelta },
             { "tail_delta_db", tailDeltaDb }, { "distortion_delta", distortionDelta }, { "sensitivity", sensitivity },
             { "summary", summary } };
}

ParamEffect ParamEffect::fromJson (const nlohmann::json& j)
{
    ParamEffect e;
    e.index = j.value ("index", -1);
    e.name = j.value ("name", std::string {});
    const auto role = j.value ("role", std::string ("unknown"));
    for (int r = 0; r <= static_cast<int> (ParamRole::Ceiling); ++r)
        if (toString (static_cast<ParamRole> (r)) == role)
            e.role = static_cast<ParamRole> (r);
    e.unit = j.value ("unit", std::string {});
    e.lowText = j.value ("low", std::string {});
    e.highText = j.value ("high", std::string {});
    e.defaultText = j.value ("default", std::string {});
    if (j.contains ("band_delta_db"))
        for (size_t b = 0; b < e.bandDeltaDb.size() && b < j["band_delta_db"].size(); ++b)
            e.bandDeltaDb[b] = j["band_delta_db"][b].get<float>();
    e.levelDeltaDb = j.value ("level_delta_db", 0.0f);
    e.crestDeltaDb = j.value ("crest_delta_db", 0.0f);
    e.widthDelta = j.value ("width_delta", 0.0f);
    e.tailDeltaDb = j.value ("tail_delta_db", 0.0f);
    e.distortionDelta = j.value ("distortion_delta", 0.0f);
    e.sensitivity = j.value ("sensitivity", 0.0f);
    e.summary = j.value ("summary", std::string {});
    return e;
}

std::string PluginProfile::document() const
{
    std::ostringstream os;
    os << name << " (" << vendor << ", " << format << ")\n";
    os << "종류/category: " << toString (declaredCategory);
    if (measuredCategory != PluginCategory::Unknown && measuredCategory != declaredCategory)
        os << " / 측정상 measured: " << toString (measuredCategory);
    os << "\n" << summary << "\n";
    os << "지연 latency " << latencySamples << " samples, CPU x" << realtimeFactor << "\n";
    for (auto& e : effects)
    {
        if (e.sensitivity <= 0.0f)
            continue;
        os << "- " << e.name;
        if (e.role != ParamRole::Unknown)
            os << " [" << toString (e.role) << "]";
        os << " (" << e.lowText << " ~ " << e.highText << "): " << e.summary << "\n";
    }
    return os.str();
}

nlohmann::json PluginProfile::toJson() const
{
    nlohmann::json fx = nlohmann::json::array();
    for (auto& e : effects) fx.push_back (e.toJson());
    return { { "uid", uid }, { "name", name }, { "vendor", vendor }, { "format", format },
             { "declared_category", toString (declaredCategory) }, { "measured_category", toString (measuredCategory) },
             { "num_params", numParams }, { "latency_samples", latencySamples }, { "realtime_factor", realtimeFactor },
             { "load_ms", loadMs }, { "memory_bytes", memoryBytes }, { "effects", fx }, { "summary", summary },
             { "profiled_at", profiledAt }, { "version", version }, { "failed", failed }, { "error", error } };
}

PluginProfile PluginProfile::fromJson (const nlohmann::json& j)
{
    PluginProfile p;
    p.uid = j.value ("uid", std::string {});
    p.name = j.value ("name", std::string {});
    p.vendor = j.value ("vendor", std::string {});
    p.format = j.value ("format", std::string {});
    p.declaredCategory = categoryFromString (j.value ("declared_category", std::string ("unknown"))).value_or (PluginCategory::Unknown);
    p.measuredCategory = categoryFromString (j.value ("measured_category", std::string ("unknown"))).value_or (PluginCategory::Unknown);
    p.numParams = j.value ("num_params", 0);
    p.latencySamples = j.value ("latency_samples", 0);
    p.realtimeFactor = j.value ("realtime_factor", 0.0);
    p.loadMs = j.value ("load_ms", 0.0);
    p.memoryBytes = j.value ("memory_bytes", static_cast<std::uint64_t> (0));
    for (auto& e : j.value ("effects", nlohmann::json::array()))
        p.effects.push_back (ParamEffect::fromJson (e));
    p.summary = j.value ("summary", std::string {});
    p.profiledAt = j.value ("profiled_at", 0.0);
    p.version = j.value ("version", 0);
    p.failed = j.value ("failed", false);
    p.error = j.value ("error", std::string {});
    return p;
}

//==============================================================================
PluginProfiler::Measurement PluginProfiler::measure (ProbeTarget& t, const ProfilerOptions& o) const
{
    const double sr = t.sampleRate();
    const auto s = makeStimuli (sr, o.stimulusSeconds);
    const size_t n = s.noiseL.size();
    const size_t skip = static_cast<size_t> (sr * 0.1);  // let filters and envelopes settle
    Measurement m;

    auto run = [&t, n] (std::vector<float>& l, std::vector<float>& r) {
        t.reset();
        constexpr size_t block = 512;
        for (size_t i = 0; i < n; i += block)
            t.process (l.data() + i, r.data() + i, static_cast<int> (std::min (block, n - i)));
    };

    // Pink noise: spectrum, level, width.
    {
        auto l = s.noiseL, r = s.noiseR;
        run (l, r);
        const auto bands = bandPowers (l, r, sr, skip);
        double total = 0;
        for (auto b : bands) total += b;
        for (size_t b = 0; b < bands.size(); ++b)
            m.bandDb[b] = toDb (bands[b]);  // absolute: deltas between settings are what matter
        double mm = 0, ss = 0, sq = 0;
        for (size_t i = skip; i < n; ++i)
        {
            const double mid = 0.5 * (l[i] + r[i]), side = 0.5 * (l[i] - r[i]);
            mm += mid * mid; ss += side * side; sq += 0.5 * (l[i] * l[i] + r[i] * r[i]);
        }
        m.levelDb = toDb (sq / static_cast<double> (n - skip));
        m.width = static_cast<float> (ss / std::max (mm, 1.0e-14));
        (void) total;
    }
    // Bursts: crest factor.
    {
        auto l = s.burstL, r = s.burstR;
        run (l, r);
        double peak = 0, sq = 0;
        for (size_t i = skip; i < n; ++i)
        {
            peak = std::max (peak, static_cast<double> (std::abs (l[i])));
            sq += static_cast<double> (l[i]) * l[i];
        }
        const double rms = std::sqrt (sq / static_cast<double> (n - skip));
        m.crestDb = static_cast<float> (20.0 * std::log10 (std::max (peak, 1.0e-7) / std::max (rms, 1.0e-9)));
    }
    // Impulse: energy that arrives later than 50 ms (reverb/delay).
    {
        auto l = s.impulse, r = s.impulse;
        run (l, r);
        const size_t start = static_cast<size_t> (sr * 0.01) + static_cast<size_t> (t.latencySamples()) + static_cast<size_t> (sr * 0.05);
        double tail = 0;
        for (size_t i = start; i < n; ++i)
            tail += static_cast<double> (l[i]) * l[i] + static_cast<double> (r[i]) * r[i];
        m.tailDb = toDb (tail);
    }
    // Sine: harmonics relative to the fundamental.
    {
        auto l = s.sine, r = s.sine;
        run (l, r);
        constexpr int order = 12, size = 1 << order;
        if (n > skip + size)
        {
            FFT fft (order);
            std::vector<std::complex<float>> buf (size);
            for (int i = 0; i < size; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (6.2831853f * static_cast<float> (i) / (size - 1));
                buf[static_cast<size_t> (i)] = { l[skip + static_cast<size_t> (i)] * w, 0.0f };
            }
            fft.perform (buf.data());
            auto powerAround = [&] (double hz) {
                const int c = static_cast<int> (std::lround (hz * size / sr));
                double p = 0;
                for (int b = std::max (1, c - 3); b <= std::min (size / 2 - 1, c + 3); ++b)
                    p += std::norm (buf[static_cast<size_t> (b)]);
                return p;
            };
            const double fundamental = powerAround (1000.0);
            double harmonics = 0;
            for (int h = 2; h <= 6; ++h)
                if (1000.0 * h < sr / 2)
                    harmonics += powerAround (1000.0 * h);
            m.distortion = static_cast<float> (std::sqrt (harmonics / std::max (fundamental, 1.0e-14)));
        }
    }
    return m;
}

PluginProfile PluginProfiler::profile (ProbeTarget& t, const PluginInfo& info, const ProfilerOptions& o, const Progress& progress) const
{
    PluginProfile p;
    p.uid = info.uid;
    p.name = info.name;
    p.vendor = info.manufacturer;
    p.format = info.format;
    p.declaredCategory = info.category;
    p.numParams = t.numParams();
    p.latencySamples = t.latencySamples();
    p.profiledAt = static_cast<double> (std::chrono::duration_cast<std::chrono::seconds> (
                                            std::chrono::system_clock::now().time_since_epoch()).count());

    // Which parameters to measure: those with a recognisable role first, then the rest.
    std::vector<ParamInfo> params;
    for (int i = 0; i < t.numParams(); ++i)
    {
        ParamInfo pi;
        pi.index = i;
        pi.name = t.paramName (i);
        pi.label = t.paramLabel (i);
        pi.defaultValue = t.paramDefault (i);
        pi.semantic = classifyParameter (pi.name, info.category);
        params.push_back (pi);
    }
    auto relevant = mixRelevantParameters (params, static_cast<size_t> (o.maxParams));

    const int total = static_cast<int> (relevant.size()) + 1;
    auto resetDefaults = [&] {
        for (auto& pi : params)
            t.setParam (pi.index, pi.defaultValue);
    };

    resetDefaults();
    const auto start = std::chrono::steady_clock::now();
    const auto base = measure (t, o);
    const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    p.realtimeFactor = elapsed / (4.0 * o.stimulusSeconds);
    if (progress) progress (1, total, "기본 설정 측정");

    int done = 1;
    float maxEqSpread = 0, maxCompression = 0, maxWidth = 0, maxTail = base.tailDb, maxDistortion = base.distortion;
    for (auto* pi : relevant)
    {
        ParamEffect e;
        e.index = pi->index;
        e.name = pi->name;
        e.role = pi->semantic.role;
        e.unit = pi->label;
        e.lowText = t.paramText (pi->index, o.lowSetting);
        e.highText = t.paramText (pi->index, o.highSetting);
        e.defaultText = t.paramText (pi->index, pi->defaultValue);

        resetDefaults();
        t.setParam (pi->index, o.lowSetting);
        const auto lo = measure (t, o);
        t.setParam (pi->index, o.highSetting);
        const auto hi = measure (t, o);
        t.setParam (pi->index, pi->defaultValue);

        float mean = 0, spread = 0;
        for (size_t b = 0; b < e.bandDeltaDb.size(); ++b)
        {
            e.bandDeltaDb[b] = std::clamp (hi.bandDb[b] - lo.bandDb[b], -60.0f, 60.0f);
            mean += e.bandDeltaDb[b];
        }
        mean /= static_cast<float> (e.bandDeltaDb.size());
        for (auto d : e.bandDeltaDb)
            spread = std::max (spread, std::abs (d - mean));
        e.levelDeltaDb = hi.levelDb - lo.levelDb;
        e.crestDeltaDb = hi.crestDb - lo.crestDb;
        e.widthDelta = hi.width - lo.width;
        e.tailDeltaDb = std::clamp (hi.tailDb - lo.tailDb, -60.0f, 60.0f);
        e.distortionDelta = hi.distortion - lo.distortion;
        e.sensitivity = spread / 6.0f + std::abs (e.levelDeltaDb) / 6.0f + std::abs (e.crestDeltaDb) / 4.0f
                        + std::abs (e.widthDelta) * 4.0f + std::abs (e.tailDeltaDb) / 12.0f + std::abs (e.distortionDelta) * 5.0f;
        if (e.sensitivity < 0.05f)
            e.sensitivity = 0.0f;
        e.summary = describeEffect (e);
        p.effects.push_back (e);

        maxEqSpread = std::max (maxEqSpread, spread);
        maxCompression = std::max (maxCompression, std::abs (e.crestDeltaDb));
        maxWidth = std::max (maxWidth, std::abs (e.widthDelta));
        maxTail = std::max ({ maxTail, hi.tailDb, lo.tailDb });
        maxDistortion = std::max ({ maxDistortion, hi.distortion, lo.distortion });

        if (progress) progress (++done, total, e.name);
    }
    resetDefaults();

    // What the plugin really is, judged from its behaviour.
    const float dryTail = -120.0f;
    if (maxTail - dryTail > 60.0f && maxTail > -45.0f)
        p.measuredCategory = info.category == PluginCategory::Delay ? PluginCategory::Delay : PluginCategory::Reverb;
    else if (maxCompression > 1.5f)
        p.measuredCategory = info.category == PluginCategory::Limiter ? PluginCategory::Limiter : PluginCategory::Compressor;
    else if (maxDistortion > 0.05f)
        p.measuredCategory = PluginCategory::Saturation;
    else if (maxEqSpread > 2.0f)
        p.measuredCategory = PluginCategory::EQ;
    else if (maxWidth > 0.1f)
        p.measuredCategory = PluginCategory::StereoImager;

    std::sort (p.effects.begin(), p.effects.end(), [] (auto& a, auto& b) { return a.sensitivity > b.sensitivity; });
    std::ostringstream os;
    os << "측정된 동작 measured behaviour: " << toString (p.measuredCategory) << ". 가장 영향이 큰 파라미터 most effective: ";
    for (size_t i = 0; i < std::min<size_t> (3, p.effects.size()); ++i)
        os << (i ? ", " : "") << p.effects[i].name;
    p.summary = os.str();
    return p;
}

} // namespace smix::knowledge
