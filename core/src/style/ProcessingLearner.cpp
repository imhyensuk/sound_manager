#include "smix/style/ProcessingLearner.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <chrono>
#include <cstdlib>

#include "smix/AudioAnalyzer.h"
#include "smix/FFT.h"
#include "smix/dsp/Builtin.h"
#include "smix/style/GenreProfile.h"

namespace smix::style
{
namespace
{
using dsp::Kind;
constexpr double kPi = 3.14159265358979323846;
constexpr float kFloorDb = -100.0f;

//==============================================================================
// small helpers

std::vector<float> toMono (const Audio& a)
{
    if (a.empty())
        return {};
    std::vector<float> m (a[0].size(), 0.0f);
    for (auto& c : a)
        for (size_t i = 0; i < m.size() && i < c.size(); ++i)
            m[i] += c[i];
    const float k = 1.0f / static_cast<float> (a.size());
    for (auto& v : m) v *= k;
    return m;
}

float median (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

float percentile (std::vector<float> v, float p)
{
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    const size_t i = std::min (v.size() - 1, static_cast<size_t> (p * static_cast<float> (v.size() - 1)));
    return v[i];
}

float geoMedian (std::vector<float> v)  // median in the log domain (times, frequencies)
{
    for (auto& x : v) x = std::log (std::max (x, 1.0e-6f));
    return std::exp (median (std::move (v)));
}

/** RMS level per frame, dB. */
std::vector<float> frameLevels (const std::vector<float>& x, size_t frame)
{
    std::vector<float> out (x.size() / frame);
    for (size_t f = 0; f < out.size(); ++f)
    {
        double s = 0;
        for (size_t i = f * frame; i < (f + 1) * frame; ++i)
            s += static_cast<double> (x[i]) * x[i];
        out[f] = std::max (kFloorDb, static_cast<float> (10.0 * std::log10 (s / static_cast<double> (frame) + 1.0e-12)));
    }
    return out;
}

std::vector<float> frameLevels (const Audio& a, size_t frame) { return frameLevels (toMono (a), frame); }

int fftOrderFor (size_t n)
{
    int order = 1;
    while ((static_cast<size_t> (1) << order) < n) ++order;
    return order;
}

//==============================================================================
// alignment

/** Samples by which `y` lags `x` (positive: y is late), searched within +-maxLag. */
int findLag (const std::vector<float>& x, const std::vector<float>& y, int maxLag)
{
    const size_t n = std::min (x.size(), y.size());
    if (n < 4096)
        return 0;
    // The most energetic ~2.7 s (at 48 kHz) of the raw track.
    const size_t seg = std::min<size_t> (n, 1 << 17);
    size_t bestStart = 0;
    {
        const size_t hop = seg / 4;
        double best = -1;
        for (size_t s = 0; s + seg <= n; s += hop)
        {
            double e = 0;
            for (size_t i = s; i < s + seg; i += 4) e += static_cast<double> (x[i]) * x[i];
            if (e > best) { best = e; bestStart = s; }
        }
    }
    const int order = fftOrderFor (seg + static_cast<size_t> (2 * maxLag) + 1);
    FFT fft (order);
    const size_t N = static_cast<size_t> (fft.size());
    std::vector<std::complex<float>> X (N), Y (N);
    for (size_t i = 0; i < seg; ++i)
        X[i] = x[bestStart + i];
    // y window extends maxLag on both sides
    for (size_t i = 0; i < seg + static_cast<size_t> (2 * maxLag) && i < N; ++i)
    {
        const long idx = static_cast<long> (bestStart) - maxLag + static_cast<long> (i);
        if (idx >= 0 && static_cast<size_t> (idx) < y.size())
            Y[i] = y[static_cast<size_t> (idx)];
    }
    fft.perform (X.data());
    fft.perform (Y.data());
    for (size_t i = 0; i < N; ++i)
        X[i] = std::conj (std::conj (X[i]) * Y[i]);  // conj for the inverse via forward FFT
    fft.perform (X.data());
    // r[k] corresponds to y shifted by (k - maxLag)
    int best = 0;
    float bestVal = -1;
    for (int k = 0; k <= 2 * maxLag && static_cast<size_t> (k) < N; ++k)
    {
        const float v = std::abs (X[static_cast<size_t> (k)].real());
        if (v > bestVal) { bestVal = v; best = k; }
    }
    return best - maxLag;
}

Audio shift (const Audio& a, int lag, size_t length)
{
    Audio out (a.size(), std::vector<float> (length, 0.0f));
    for (size_t c = 0; c < a.size(); ++c)
        for (size_t i = 0; i < length; ++i)
        {
            const long j = static_cast<long> (i) + lag;
            if (j >= 0 && static_cast<size_t> (j) < a[c].size())
                out[c][i] = a[c][static_cast<size_t> (j)];
        }
    return out;
}

//==============================================================================
// spectra

std::array<std::pair<float, float>, kEqCurveBands> curveEdges()
{
    std::array<std::pair<float, float>, kEqCurveBands> e {};
    for (int b = 0; b < kEqCurveBands; ++b)
    {
        const float c = eqCurveCentres()[static_cast<size_t> (b)];
        e[static_cast<size_t> (b)] = { c * std::pow (2.0f, -1.0f / 6.0f), c * std::pow (2.0f, 1.0f / 6.0f) };
    }
    return e;
}

/** 1/3-octave band energies per STFT frame (Hann, 50 % overlap). */
std::vector<std::array<double, kEqCurveBands>> bandFrames31 (const std::vector<float>& x, double sr, int order)
{
    FFT fft (order);
    const size_t N = static_cast<size_t> (fft.size()), hop = N / 2;
    std::vector<float> win (N);
    for (size_t i = 0; i < N; ++i) win[i] = static_cast<float> (0.5 - 0.5 * std::cos (2 * kPi * static_cast<double> (i) / static_cast<double> (N)));
    const auto edges = curveEdges();
    std::vector<int> binBand (N / 2, -1);
    for (size_t k = 1; k < N / 2; ++k)
    {
        const double hz = static_cast<double> (k) * sr / static_cast<double> (N);
        for (int b = 0; b < kEqCurveBands; ++b)
            if (hz >= edges[static_cast<size_t> (b)].first && hz < edges[static_cast<size_t> (b)].second)
                binBand[k] = b;
    }
    std::vector<std::array<double, kEqCurveBands>> out;
    std::vector<std::complex<float>> buf (N);
    for (size_t s = 0; s + N <= x.size(); s += hop)
    {
        for (size_t i = 0; i < N; ++i) buf[i] = x[s + i] * win[i];
        fft.perform (buf.data());
        std::array<double, kEqCurveBands> e {};
        for (size_t k = 1; k < N / 2; ++k)
            if (binBand[k] >= 0)
                e[static_cast<size_t> (binBand[k])] += std::norm (buf[k]);
        out.push_back (e);
    }
    return out;
}

/** Tonal change raw -> processed, per 1/3 octave, independent of level changes (frame by frame). */
void measureEqCurve (const std::vector<float>& x, const std::vector<float>& y, double sr, std::array<float, kEqCurveBands>& curve,
                     std::array<bool, kEqCurveBands>& valid)
{
    const int order = sr > 60000 ? 14 : 13;
    const auto X = bandFrames31 (x, sr, order);
    const auto Y = bandFrames31 (y, sr, order);
    const size_t F = std::min (X.size(), Y.size());
    std::vector<double> totals (F, 0.0);
    double maxTotal = 0;
    for (size_t f = 0; f < F; ++f)
    {
        for (auto v : X[f]) totals[f] += v;
        maxTotal = std::max (maxTotal, totals[f]);
    }
    std::array<std::vector<float>, kEqCurveBands> d;
    std::array<double, kEqCurveBands> share {};
    double shareTotal = 0;
    for (size_t f = 0; f < F; ++f)
    {
        if (totals[f] < maxTotal * 1.0e-3)  // only frames where the raw track plays
            continue;
        double ty = 0;
        for (auto v : Y[f]) ty += v;
        if (ty <= 0)
            continue;
        for (int b = 0; b < kEqCurveBands; ++b)
        {
            const double xs = X[f][static_cast<size_t> (b)] / totals[f];
            const double ys = Y[f][static_cast<size_t> (b)] / ty;
            share[static_cast<size_t> (b)] += xs;
            if (xs > 1.0e-6)
                d[static_cast<size_t> (b)].push_back (static_cast<float> (10.0 * std::log10 ((ys + 1.0e-12) / xs)));
        }
        shareTotal += 1.0;
    }
    for (int b = 0; b < kEqCurveBands; ++b)
    {
        const auto i = static_cast<size_t> (b);
        // A band is only measurable where the raw track has a meaningful share of its energy.
        valid[i] = shareTotal > 0 && share[i] / shareTotal > 1.0e-4 && d[i].size() >= 4;  // > -40 dB of the energy
        curve[i] = valid[i] ? std::max (-40.0f, median (d[i])) : 0.0f;
    }
    // 0 dB around 1 kHz (the level change goes to the gain stage, not the EQ)
    std::vector<float> mids;
    for (int b = 0; b < kEqCurveBands; ++b)
    {
        const float c = eqCurveCentres()[static_cast<size_t> (b)];
        if (valid[static_cast<size_t> (b)] && c >= 400.0f && c <= 2500.0f)
            mids.push_back (curve[static_cast<size_t> (b)]);
    }
    const float ref = mids.empty() ? 0.0f : median (mids);
    for (int b = 0; b < kEqCurveBands; ++b)
        if (valid[static_cast<size_t> (b)])
            curve[static_cast<size_t> (b)] -= ref;
}

//==============================================================================
// biquad magnitude (same RBJ formulas as the built-in EQ)

struct Coefs { double b0, b1, b2, a1, a2; };

Coefs rbj (int type, double sr, double f, double q, double g)  // 0 peak, 1 low shelf, 2 high shelf, 3 HP, 4 LP
{
    f = std::clamp (f, 10.0, 0.49 * sr);
    const double A = std::pow (10.0, g / 40.0), w = 2 * kPi * f / sr, cw = std::cos (w), sw = std::sin (w), al = sw / (2 * q);
    double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
    switch (type)
    {
        case 0: B0 = 1 + al * A; B1 = -2 * cw; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * cw; A2 = 1 - al / A; break;
        case 1: { const double s = 2 * std::sqrt (A) * al;
                  B0 = A * ((A + 1) - (A - 1) * cw + s); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - s);
                  A0 = (A + 1) + (A - 1) * cw + s; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - s; break; }
        case 2: { const double s = 2 * std::sqrt (A) * al;
                  B0 = A * ((A + 1) + (A - 1) * cw + s); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - s);
                  A0 = (A + 1) - (A - 1) * cw + s; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - s; break; }
        case 3: B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = (1 + cw) / 2; A0 = 1 + al; A1 = -2 * cw; A2 = 1 - al; break;
        default: B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = (1 - cw) / 2; A0 = 1 + al; A1 = -2 * cw; A2 = 1 - al; break;
    }
    return { B0 / A0, B1 / A0, B2 / A0, A1 / A0, A2 / A0 };
}

double magDb (const Coefs& c, double f, double sr)
{
    const double w = 2 * kPi * f / sr;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2 * w);
    const auto num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const auto den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return 20.0 * std::log10 (std::max (std::abs (num / den), 1.0e-9));
}

double sectionDb (int type, double f0, double q, double g, double f, double sr = 48000.0)
{
    return magDb (rbj (type, sr, f0, q, g), f, sr);
}

//==============================================================================
// simulation scaffolding

struct Excerpt
{
    Audio x;           // raw (with pre-roll)
    Audio y;           // processed, aligned
    size_t preroll = 0;  // samples excluded from errors (processor warm-up)
};

Excerpt pickExcerpt (const Audio& x, const Audio& y, double sr, double seconds)
{
    const auto mono = toMono (x);
    const size_t frame = static_cast<size_t> (sr * 0.01);
    const auto L = frameLevels (mono, frame);
    const float peak = L.empty() ? kFloorDb : *std::max_element (L.begin(), L.end());
    const size_t want = std::min (L.size(), static_cast<size_t> (seconds * 100.0));
    size_t bestStart = 0, bestCount = 0, count = 0;
    for (size_t f = 0; f < L.size(); ++f)
    {
        if (L[f] > peak - 40.0f) ++count;
        if (f >= want && L[f - want] > peak - 40.0f) --count;
        if (f + 1 >= want && count > bestCount) { bestCount = count; bestStart = f + 1 - want; }
    }
    Excerpt e;
    const size_t pre = std::min (bestStart * frame, static_cast<size_t> (sr * 1.0));
    const size_t start = bestStart * frame - pre;
    const size_t len = std::min (want * frame + pre, mono.size() - start);
    e.preroll = pre;
    for (auto* src : { &x, &y })
    {
        Audio out (src->size(), std::vector<float> (len));
        for (size_t c = 0; c < src->size(); ++c)
            std::copy ((*src)[c].begin() + static_cast<long> (start), (*src)[c].begin() + static_cast<long> (start + len), out[c].begin());
        (src == &x ? e.x : e.y) = std::move (out);
    }
    return e;
}

void render (Kind k, const std::vector<std::pair<std::string, float>>& params, Audio& a, double sr)
{
    auto p = dsp::create (k);
    for (auto& [id, v] : params) p->set (id, v);
    dsp::renderOffline (*p, a, sr);
}

std::vector<std::pair<std::string, float>> eqParams (const EqSettings& eq)
{
    std::vector<std::pair<std::string, float>> p;
    p.emplace_back ("lowcut", eq.lowCutHz > 20.0f ? eq.lowCutHz : 20.0f);
    p.emplace_back ("lowcut_slope", eq.steep ? 1.0f : 0.0f);
    for (int b = 0; b < 6; ++b)
    {
        const std::string pre = "b" + std::to_string (b + 1) + "_";
        p.emplace_back (pre + "freq", eq.bands[static_cast<size_t> (b)].freq);
        p.emplace_back (pre + "gain", eq.bands[static_cast<size_t> (b)].gain);
        if (b >= 1 && b <= 4)
            p.emplace_back (pre + "q", eq.bands[static_cast<size_t> (b)].q);
    }
    p.emplace_back ("highcut", eq.highCutHz > 0.0f ? eq.highCutHz : 22000.0f);
    return p;
}

/** Level error between two frame-level series over a mask, with the mean difference removed. */
float levelError (const std::vector<float>& a, const std::vector<float>& b, const std::vector<char>& mask, float* meanOut = nullptr)
{
    double sum = 0, sq = 0;
    size_t n = 0;
    for (size_t i = 0; i < std::min ({ a.size(), b.size(), mask.size() }); ++i)
    {
        if (! mask[i]) continue;
        const double d = a[i] - b[i];
        sum += d; sq += d * d; ++n;
    }
    if (n == 0) return 0.0f;
    const double mean = sum / static_cast<double> (n);
    if (meanOut) *meanOut = static_cast<float> (mean);
    return static_cast<float> (std::sqrt (std::max (0.0, sq / static_cast<double> (n) - mean * mean)));
}

/**
    Fraction of `a`'s energy that a linear filter of `ref` cannot explain (harmonics added by
    saturation), measured on loud blocks with each block's level normalised.
*/
float nonlinearity (const std::vector<float>& a, const std::vector<float>& ref, double sr, size_t from)
{
    const int order = 11;
    FFT fft (order);
    const size_t N = static_cast<size_t> (fft.size()), hop = N / 2;
    std::vector<float> win (N);
    for (size_t i = 0; i < N; ++i) win[i] = static_cast<float> (0.5 - 0.5 * std::cos (2 * kPi * static_cast<double> (i) / static_cast<double> (N)));
    const size_t kLo = static_cast<size_t> (300.0 * static_cast<double> (N) / sr), kHi = std::min (N / 2, static_cast<size_t> (12000.0 * static_cast<double> (N) / sr));

    struct Block { size_t start; double energy; };
    std::vector<Block> blocks;
    for (size_t s = from; s + N <= std::min (a.size(), ref.size()); s += hop)
    {
        double e = 0;
        for (size_t i = 0; i < N; i += 4) e += static_cast<double> (ref[s + i]) * ref[s + i];
        blocks.push_back ({ s, e });
    }
    if (blocks.empty()) return 0.0f;
    std::sort (blocks.begin(), blocks.end(), [] (auto& p, auto& q) { return p.energy > q.energy; });
    blocks.resize (std::max<size_t> (1, blocks.size() * 4 / 10));  // loud 40 %

    std::vector<std::complex<double>> Sab (N / 2);
    std::vector<double> Saa (N / 2), Sbb (N / 2);
    std::vector<std::complex<float>> A (N), B (N);
    for (auto& blk : blocks)
    {
        double ea = 0, eb = 0;
        for (size_t i = 0; i < N; ++i)
        {
            A[i] = a[blk.start + i] * win[i];
            B[i] = ref[blk.start + i] * win[i];
            ea += std::norm (A[i]); eb += std::norm (B[i]);
        }
        if (ea <= 0 || eb <= 0) continue;
        const float ga = static_cast<float> (1.0 / std::sqrt (ea)), gb = static_cast<float> (1.0 / std::sqrt (eb));
        for (size_t i = 0; i < N; ++i) { A[i] *= ga; B[i] *= gb; }
        fft.perform (A.data());
        fft.perform (B.data());
        for (size_t k = kLo; k < kHi; ++k)
        {
            Sab[k] += std::complex<double> (A[k]) * std::conj (std::complex<double> (B[k]));
            Saa[k] += std::norm (A[k]);
            Sbb[k] += std::norm (B[k]);
        }
    }
    double unexplained = 0, total = 0;
    for (size_t k = kLo; k < kHi; ++k)
    {
        if (Saa[k] <= 0) continue;
        total += Saa[k];
        unexplained += Sbb[k] > 0 ? std::max (0.0, Saa[k] - std::norm (Sab[k]) / Sbb[k]) : Saa[k];
    }
    return total > 0 ? static_cast<float> (unexplained / total) : 0.0f;
}

/** Chain of the built-in processors, stage by stage (the order the planner uses). */
struct Stages
{
    bool gate = false, eq = false, deesser = false, comp = false, env = false, sat = false, reverb = false;
};

void renderStages (const ProcessingSettings& s, Audio& a, double sr, Stages st)
{
    const float ref = s.inputLevelDb;
    if (st.gate && s.gate.used)
        render (Kind::Gate, { { "threshold", ref + s.gate.thresholdRel }, { "range", s.gate.rangeDb }, { "attack", s.gate.attackMs },
                              { "hold", s.gate.holdMs }, { "release", s.gate.releaseMs } }, a, sr);
    if (st.eq && s.eq.used)
        render (Kind::EQ, eqParams (s.eq), a, sr);
    if (st.deesser && s.deesser.used)
        render (Kind::DeEsser, { { "freq", s.deesser.freqHz }, { "threshold", ref + s.deesser.thresholdRel }, { "range", s.deesser.rangeDb } }, a, sr);
    if (st.comp && s.comp.used)
        render (Kind::Compressor, { { "threshold", ref + s.comp.thresholdRel }, { "ratio", s.comp.ratio }, { "attack", s.comp.attackMs },
                                    { "release", s.comp.releaseMs }, { "knee", s.comp.kneeDb } }, a, sr);
    if (st.env && s.envelope.used)
        render (Kind::Enveloper, { { "attack", s.envelope.attackDb }, { "sustain", s.envelope.sustainDb } }, a, sr);
    if (st.sat && s.saturation.used)
        render (Kind::Saturation, { { "drive", s.saturation.driveDb }, { "type", static_cast<float> (s.saturation.type) } }, a, sr);
    if (st.reverb && s.reverb.used)
        render (Kind::Reverb, { { "predelay", s.reverb.predelayMs }, { "decay", s.reverb.decayS }, { "size", s.reverb.sizePct },
                                { "damping", s.reverb.dampingHz }, { "lowcut", s.reverb.lowCutHz }, { "mix", s.reverb.mixPct } }, a, sr);
}

Stages all() { Stages s; s.gate = s.eq = s.deesser = s.comp = s.env = s.sat = s.reverb = true; return s; }

//==============================================================================
// onsets / offsets on 5 ms frames

std::vector<size_t> onsets (const std::vector<float>& L, float minLevel)
{
    std::vector<size_t> out;
    for (size_t f = 10; f < L.size(); ++f)
    {
        float lo = L[f];
        for (size_t k = f - 10; k < f; ++k) lo = std::min (lo, L[k]);
        if (L[f] > minLevel && L[f] - lo > 9.0f && (out.empty() || f - out.back() > 20))
            out.push_back (f);
    }
    return out;
}

std::vector<size_t> offsets (const std::vector<float>& L, float minLevel)
{
    // frame (10 ms) where the level falls 25 dB below the recent peak and stays down for 300 ms
    std::vector<size_t> out;
    for (size_t f = 20; f + 30 < L.size(); ++f)
    {
        float pk = kFloorDb;
        for (size_t k = f - 20; k < f; ++k) pk = std::max (pk, L[k]);
        if (pk < minLevel) continue;
        bool down = true;
        for (size_t k = f; k < f + 30 && down; ++k) down = L[k] < pk - 25.0f;
        if (down && (out.empty() || f - out.back() > 100))
            out.push_back (f);
    }
    return out;
}
} // namespace

//==============================================================================
const std::array<float, kEqCurveBands>& eqCurveCentres()
{
    static const std::array<float, kEqCurveBands> c = [] {
        std::array<float, kEqCurveBands> v {};
        for (int b = 0; b < kEqCurveBands; ++b) v[static_cast<size_t> (b)] = 20.0f * std::pow (2.0f, static_cast<float> (b) / 3.0f);
        return v;
    }();
    return c;
}

float eqResponseDb (const EqSettings& eq, double f, double sr)
{
    double db = 0;
    if (eq.lowCutHz > 20.5f)
    {
        db += sectionDb (3, eq.lowCutHz, eq.steep ? 0.541 : 0.707, 0, f, sr);
        if (eq.steep) db += sectionDb (3, eq.lowCutHz, 1.307, 0, f, sr);
    }
    if (std::abs (eq.bands[0].gain) > 0.01f) db += sectionDb (1, eq.bands[0].freq, 0.707, eq.bands[0].gain, f, sr);
    for (size_t b = 1; b <= 4; ++b)
        if (std::abs (eq.bands[b].gain) > 0.01f) db += sectionDb (0, eq.bands[b].freq, eq.bands[b].q, eq.bands[b].gain, f, sr);
    if (std::abs (eq.bands[5].gain) > 0.01f) db += sectionDb (2, eq.bands[5].freq, 0.707, eq.bands[5].gain, f, sr);
    if (eq.highCutHz > 0.0f && eq.highCutHz < 21900.0f) db += sectionDb (4, eq.highCutHz, 0.707, 0, f, sr);
    return static_cast<float> (db);
}

EqSettings fitEq (const std::array<float, kEqCurveBands>& curve, const std::array<bool, kEqCurveBands>& valid)
{
    const auto& centres = eqCurveCentres();
    EqSettings eq;
    eq.bands[0] = { 100.0f, 0.0f, 0.707f };
    eq.bands[1] = { 250.0f, 0.0f, 1.0f };
    eq.bands[2] = { 800.0f, 0.0f, 1.0f };
    eq.bands[3] = { 2500.0f, 0.0f, 1.0f };
    eq.bands[4] = { 6000.0f, 0.0f, 1.0f };
    eq.bands[5] = { 10000.0f, 0.0f, 0.707f };

    auto error = [&] (const EqSettings& e) {
        double s = 0; int n = 0;
        for (int b = 0; b < kEqCurveBands; ++b)
        {
            if (! valid[static_cast<size_t> (b)]) continue;
            const double d = curve[static_cast<size_t> (b)] - eqResponseDb (e, centres[static_cast<size_t> (b)]);
            s += d * d; ++n;
        }
        return n > 0 ? std::sqrt (s / n) : 0.0;
    };
    double err = error (eq);

    // 1) low cut (only when the lows really fall away)
    {
        EqSettings best = eq;
        double bestErr = err;
        for (float f = 25.0f; f <= 500.0f; f *= std::pow (2.0f, 1.0f / 6.0f))
            for (bool steep : { false, true })
            {
                EqSettings t = eq;
                t.lowCutHz = f;
                t.steep = steep;
                const double e = error (t);
                if (e < bestErr - 0.3) { bestErr = e; best = t; }
            }
        eq = best;
        err = bestErr;
    }
    // 2) high cut
    {
        EqSettings best = eq;
        double bestErr = err;
        for (float f = 2000.0f; f <= 20000.0f; f *= std::pow (2.0f, 1.0f / 6.0f))
        {
            EqSettings t = eq;
            t.highCutHz = f;
            const double e = error (t);
            if (e < bestErr - 0.3) { bestErr = e; best = t; }
        }
        eq = best;
        err = bestErr;
    }
    // 3) shelves
    for (size_t sb : { size_t (0), size_t (5) })
    {
        EqSettings best = eq;
        double bestErr = err;
        const float lo = sb == 0 ? 50.0f : 2500.0f, hi = sb == 0 ? 500.0f : 14000.0f;
        for (float f = lo; f <= hi; f *= std::pow (2.0f, 1.0f / 6.0f))
            for (float g = -12.0f; g <= 12.0f; g += 0.5f)
            {
                if (std::abs (g) < 0.5f) continue;
                EqSettings t = eq;
                t.bands[sb] = { f, g, 0.707f };
                const double e = error (t);
                if (e < bestErr - 0.25) { bestErr = e; best = t; }
            }
        eq = best;
        err = bestErr;
    }
    // 4) up to four peaks, greedily on the largest remaining deviation
    std::vector<EqSettings::Band> peaks;
    for (int round = 0; round < 4; ++round)
    {
        // residual
        int at = -1;
        float worst = 0.0f;
        for (int b = 1; b < kEqCurveBands - 1; ++b)
        {
            if (! valid[static_cast<size_t> (b)]) continue;
            float r = 0; int n = 0;
            for (int k = b - 1; k <= b + 1; ++k)
                if (valid[static_cast<size_t> (k)])
                {
                    r += curve[static_cast<size_t> (k)] - eqResponseDb (eq, centres[static_cast<size_t> (k)]);
                    ++n;
                }
            r /= static_cast<float> (std::max (1, n));
            if (std::abs (r) > std::abs (worst)) { worst = r; at = b; }
        }
        if (at < 0 || std::abs (worst) < 1.0f)
            break;
        const size_t slot = static_cast<size_t> (1 + round);
        EqSettings best = eq;
        double bestErr = err;
        const float c = centres[static_cast<size_t> (at)];
        for (float f = c * 0.63f; f <= c * 1.59f; f *= std::pow (2.0f, 1.0f / 12.0f))
            for (float q : { 0.5f, 0.7f, 1.0f, 1.4f, 2.0f, 3.0f, 4.5f })
                for (float g = worst - 4.0f; g <= worst + 4.0f; g += 0.5f)
                {
                    if (std::abs (g) < 0.5f || std::abs (g) > 18.0f) continue;
                    EqSettings t = eq;
                    t.bands[slot] = { f, g, q };
                    const double e = error (t);
                    if (e < bestErr) { bestErr = e; best = t; }
                }
        if (bestErr > err - 0.2)
            break;
        eq = best;
        err = bestErr;
    }
    // peaks sorted by frequency (bands 2..5 low -> high, like the defaults)
    std::sort (eq.bands.begin() + 1, eq.bands.begin() + 5, [] (auto& a, auto& b) {
        if (std::abs (a.gain) < 0.01f) return false;
        if (std::abs (b.gain) < 0.01f) return true;
        return a.freq < b.freq;
    });
    eq.errorDb = static_cast<float> (err);
    eq.used = eq.lowCutHz > 20.5f || (eq.highCutHz > 0.0f && eq.highCutHz < 21900.0f);
    for (auto& b : eq.bands)
        eq.used = eq.used || std::abs (b.gain) >= 0.75f;
    return eq;
}

float activeLoudness (const Audio& a, double sr)
{
    if (a.empty() || a[0].empty())
        return -70.0f;
    AudioAnalyzer an;
    an.prepare (sr);
    const size_t block = static_cast<size_t> (sr * 0.1);
    std::vector<float> st;
    const float* L = a[0].data();
    const float* R = a.size() > 1 ? a[1].data() : nullptr;
    for (size_t s = 0, k = 0; s + block <= a[0].size(); s += block, ++k)
    {
        an.process (L + s, R ? R + s : nullptr, static_cast<int> (block));
        if (k % 10 == 9)
        {
            const auto f = an.snapshot();
            if (f.valid && f.secondsAnalysed >= 3.0f && f.shortTermLufs > -70.0f)
                st.push_back (f.shortTermLufs);
        }
    }
    if (st.empty())
        return -70.0f;
    // the parts that play: the upper half of the short-term values
    std::sort (st.begin(), st.end());
    return median (std::vector<float> (st.begin() + static_cast<long> (st.size() / 2), st.end()));
}

//==============================================================================
ProcessingSettings learnProcessing (const Audio& raw, const Audio& processed, double sr, const LearnOptions& opt)
{
    ProcessingSettings s;
    const bool trace = std::getenv ("SMIX_LEARN_TRACE") != nullptr;
    auto lap = std::chrono::steady_clock::now();
    auto mark = [&] (const char* what) {
        if (! trace) return;
        const auto now = std::chrono::steady_clock::now();
        std::fprintf (stderr, "      %-10s %.2f s\n", what, std::chrono::duration<double> (now - lap).count());
        lap = now;
    };
    if (raw.empty() || processed.empty() || raw[0].size() < static_cast<size_t> (sr))
        return s;

    const auto xm = toMono (raw);
    const auto ymRaw = toMono (processed);
    s.latencySamples = findLag (xm, ymRaw, static_cast<int> (sr * 0.1));
    const size_t n = xm.size();
    const Audio y = shift (processed, s.latencySamples, n);
    const auto ym = toMono (y);
    s.inputLevelDb = activeLoudness (raw, sr);

    // Stereo image of the processed track.
    if (y.size() >= 2)
    {
        double el = 0, er = 0, em = 0, es = 0;
        for (size_t i = 0; i < n; ++i)
        {
            el += static_cast<double> (y[0][i]) * y[0][i];
            er += static_cast<double> (y[1][i]) * y[1][i];
            const double m = 0.5 * (y[0][i] + y[1][i]), d = 0.5 * (y[0][i] - y[1][i]);
            em += m * m; es += d * d;
        }
        if (el + er > 0)
        {
            // constant-power law: L = cos, R = sin of (pan+1) pi/4
            const double angle = std::atan2 (std::sqrt (er), std::sqrt (el));
            s.pan = static_cast<float> (std::clamp ((angle / (kPi / 4.0) - 1.0) * 100.0, -100.0, 100.0));
        }
        if (raw.size() >= 2 && em > 0)
        {
            double rm = 0, rs = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double m = 0.5 * (raw[0][i] + raw[1][i]), d = 0.5 * (raw[0][i] - raw[1][i]);
                rm += m * m; rs += d * d;
            }
            if (rm > 0 && rs / rm > 1.0e-4)
                s.widthPct = static_cast<float> (std::clamp (100.0 * std::sqrt ((es / em) / (rs / rm)), 0.0, 200.0));
        }
    }

    // 1) EQ: tonal change, fitted with the built-in EQ
    measureEqCurve (xm, ym, sr, s.eqCurveDb, s.eqCurveValid);
    s.eq = fitEq (s.eqCurveDb, s.eqCurveValid);

    mark ("eq");
    // Simulations run on the most active excerpt.
    auto ex = pickExcerpt (raw, y, sr, opt.maxAnalysisSeconds);
    const size_t f10 = static_cast<size_t> (sr * 0.01);
    const size_t skip10 = ex.preroll / f10;
    const auto Ly = frameLevels (ex.y, f10);
    const auto Lx = frameLevels (ex.x, f10);
    const float xPeak = *std::max_element (Lx.begin(), Lx.end());
    std::vector<char> active (Ly.size(), 0), loud (Ly.size(), 0), quiet (Ly.size(), 0);
    for (size_t f = skip10; f < Ly.size(); ++f)
    {
        active[f] = Lx[f] > xPeak - 45.0f;
        loud[f] = Lx[f] > s.inputLevelDb - 18.0f;  // where a compressor works
        quiet[f] = Lx[f] < s.inputLevelDb - 20.0f && Lx[f] > xPeak - 70.0f;  // where a gate works
    }

    auto simulate = [&] (const ProcessingSettings& t, Stages st) {
        Audio a = ex.x;
        renderStages (t, a, sr, st);
        return a;
    };
    Stages upToEq; upToEq.gate = upToEq.eq = true;

    // 2) Compressor: coordinate search by simulation on the EQ'd signal
    {
        Audio x1 = ex.x;
        renderStages (s, x1, sr, Stages { false, true });
        const auto Lx1 = frameLevels (x1, f10);
        auto compErr = [&] (const CompSettings& c) {
            auto t = s;
            t.comp = c;
            t.comp.used = true;
            Audio a = x1;
            renderStages (t, a, sr, Stages { false, false, false, true });
            return levelError (Ly, frameLevels (a, f10), loud);
        };
        const float noComp = levelError (Ly, Lx1, loud);
        CompSettings c;
        c.kneeDb = 6.0f;
        float best = std::numeric_limits<float>::max();
        for (float thr = -30.0f; thr <= 6.0f; thr += 3.0f)  // relative to the input level
        {
            CompSettings t = c; t.thresholdRel = thr; t.ratio = 4.0f;
            const float e = compErr (t);
            if (e < best) { best = e; c = t; }
        }
        for (float r : { 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 20.0f })
        {
            CompSettings t = c; t.ratio = r;
            const float e = compErr (t);
            if (e < best) { best = e; c = t; }
        }
        for (float at : { 0.3f, 1.0f, 3.0f, 10.0f, 30.0f, 80.0f })
            for (float rl : { 30.0f, 60.0f, 120.0f, 250.0f, 500.0f, 1000.0f })
            {
                CompSettings t = c; t.attackMs = at; t.releaseMs = rl;
                const float e = compErr (t);
                if (e < best) { best = e; c = t; }
            }
        for (float d = -3.0f; d <= 3.0f; d += 1.0f)
        {
            CompSettings t = c; t.thresholdRel = c.thresholdRel + d;
            const float e = compErr (t);
            if (e < best) { best = e; c = t; }
        }
        for (float r : { c.ratio * 0.7f, c.ratio * 1.4f })
        {
            CompSettings t = c; t.ratio = std::clamp (r, 1.1f, 20.0f);
            const float e = compErr (t);
            if (e < best) { best = e; c = t; }
        }
        // Worth a compressor only if it explains the dynamics clearly better than none.
        c.used = c.ratio >= 1.3f && best < noComp - 0.4f && best < noComp * 0.8f;
        if (c.used)
        {
            auto t = s; t.comp = c;
            Audio a = x1;
            renderStages (t, a, sr, Stages { false, false, false, true });
            const auto La = frameLevels (a, f10);
            std::vector<float> red;
            for (size_t f = skip10; f < La.size(); ++f)
                if (loud[f]) red.push_back (Lx1[f] - La[f]);
            c.avgReductionDb = median (red);
        }
        s.comp = c;
    }

    mark ("comp");
    // 3) Gate: level of the quiet parts (bleed, noise) relative to the loud parts
    {
        size_t quietCount = 0;
        for (auto q : quiet) quietCount += q ? 1 : 0;
        if (quietCount > 50)
        {
            Stages withComp; withComp.eq = withComp.comp = true;
            const auto Lbase = frameLevels (simulate (s, withComp), f10);
            std::vector<char> both (active.size());
            for (size_t f = 0; f < both.size(); ++f) both[f] = active[f] || quiet[f];
            // the quiet frames are lower in the processed track than the chain explains?
            std::vector<float> dq, dl;
            for (size_t f = skip10; f < Ly.size(); ++f)
            {
                if (quiet[f]) dq.push_back (Ly[f] - Lbase[f]);
                else if (loud[f]) dl.push_back (Ly[f] - Lbase[f]);
            }
            const float drop = median (dl) - median (dq);
            if (drop > 6.0f)
            {
                auto gateErr = [&] (const GateSettings& g) {
                    auto t = s; t.gate = g; t.gate.used = true;
                    return levelError (Ly, frameLevels (simulate (t, Stages { true, true, false, true }), f10), both);
                };
                GateSettings g;
                float best = std::numeric_limits<float>::max();
                for (float thr = -45.0f; thr <= -5.0f; thr += 5.0f)
                    for (float range : { 10.0f, 20.0f, 40.0f, 70.0f })
                    {
                        GateSettings t = g; t.thresholdRel = thr; t.rangeDb = std::min (range, drop + 10.0f);
                        const float e = gateErr (t);
                        if (e < best) { best = e; g = t; }
                    }
                for (float rel : { 50.0f, 150.0f, 400.0f })
                {
                    GateSettings t = g; t.releaseMs = rel;
                    const float e = gateErr (t);
                    if (e < best) { best = e; g = t; }
                }
                g.used = true;
                s.gate = g;
            }
        }
    }

    mark ("gate");
    // 4) De-esser: the sibilant band is turned down only when it is loud
    {
        Stages base; base.gate = base.eq = true;
        Audio pre = simulate (s, base);
        const auto preMono = toMono (pre);
        auto bandShare = [&] (const std::vector<float>& sig) {
            // per 10.7 ms frame: (energy 4.5-12 kHz) - (total), dB
            const auto B = bandFrames31 (sig, sr, 10);
            std::vector<float> d (B.size());
            for (size_t f = 0; f < B.size(); ++f)
            {
                double hi = 0, all = 0;
                for (int b = 0; b < kEqCurveBands; ++b)
                {
                    all += B[f][static_cast<size_t> (b)];
                    if (eqCurveCentres()[static_cast<size_t> (b)] >= 4500.0f) hi += B[f][static_cast<size_t> (b)];
                }
                d[f] = static_cast<float> (10.0 * std::log10 ((hi + 1e-12) / (all + 1e-12)));
            }
            return d;
        };
        const auto Dx = bandShare (preMono);
        const auto Dy = bandShare (toMono (ex.y));
        const size_t F = std::min (Dx.size(), Dy.size());
        const size_t skipF = ex.preroll / 512;
        std::vector<float> sib;
        for (size_t f = skipF; f < F; ++f) sib.push_back (Dx[f]);
        const float sibThreshold = percentile (sib, 0.9f);
        std::vector<float> eS, eO;
        for (size_t f = skipF; f < F; ++f)
            (Dx[f] >= sibThreshold ? eS : eO).push_back (Dy[f] - Dx[f]);
        const float medianOthers = median (eO);
        const float extra = medianOthers - median (eS);  // how much more the sibilant frames lost
        if (sib.size() > 100 && extra > 3.0f && sibThreshold > -25.0f)
        {
            auto deErr = [&] (const DeEsserSettings& d) {
                auto t = s; t.deesser = d; t.deesser.used = true;
                Audio a = pre;
                renderStages (t, a, sr, Stages { false, false, true });
                const auto Da = bandShare (toMono (a));
                double err = 0; size_t k = 0;
                for (size_t f = skipF; f < std::min (F, Da.size()); ++f)
                {
                    // compare the change each frame gets, relative to the overall tonal offset
                    const double obs = (Dy[f] - Dx[f]) - medianOthers;
                    const double sim = Da[f] - Dx[f];
                    err += (obs - sim) * (obs - sim); ++k;
                }
                return k ? err / static_cast<double> (k) : 0.0;
            };
            DeEsserSettings d;
            DeEsserSettings none; none.rangeDb = 0.0f;
            const double noDe = deErr (none);
            double best = std::numeric_limits<double>::max();
            for (float f : { 5000.0f, 6500.0f, 8000.0f })
                for (float thr = -40.0f; thr <= -5.0f; thr += 5.0f)
                    for (float range : { 4.0f, 8.0f, 12.0f })
                    {
                        DeEsserSettings t; t.freqHz = f; t.thresholdRel = thr; t.rangeDb = range;
                        const double e = deErr (t);
                        if (e < best) { best = e; d = t; }
                    }
            d.used = best < noDe * 0.6;  // clearly explains the sibilant frames, not just any tonal change
            if (d.used)
                s.deesser = d;
        }
    }

    mark ("deesser");
    // 5) Saturation: harmonics the linear chain cannot explain
    if (opt.learnSaturation)
    {
        Stages lin; lin.gate = lin.eq = lin.deesser = lin.comp = true;
        const Audio base = simulate (s, lin);
        const auto baseMono = toMono (base);
        const float observed = nonlinearity (toMono (ex.y), baseMono, sr, ex.preroll);
        float bestDrive = 0.0f, bestDiff = std::numeric_limits<float>::max();
        float nlSmallest = 1.0f;
        for (float drive : { 3.0f, 6.0f, 9.0f, 12.0f, 18.0f, 24.0f, 30.0f })
        {
            auto t = s;
            t.saturation = { true, drive, 0 };
            Audio a = base;
            renderStages (t, a, sr, Stages { false, false, false, false, false, true });
            const float nl = nonlinearity (toMono (a), baseMono, sr, ex.preroll);
            if (drive == 3.0f) nlSmallest = nl;
            if (std::abs (std::log (std::max (nl, 1e-7f)) - std::log (std::max (observed, 1e-7f))) < bestDiff)
            {
                bestDiff = std::abs (std::log (std::max (nl, 1e-7f)) - std::log (std::max (observed, 1e-7f)));
                bestDrive = drive;
            }
        }
        // at least as nonlinear as a light drive, and clearly above numerical noise (0.2 % of the energy)
        if (observed > nlSmallest && observed > 0.002f)
            s.saturation = { true, bestDrive, 0 };
    }

    mark ("saturation");
    // 6) Envelope (transient shaper): attack/sustain changes the chain does not explain
    if (opt.learnEnvelope)
    {
        const size_t f5 = static_cast<size_t> (sr * 0.005);
        Stages chain; chain.gate = chain.eq = chain.deesser = chain.comp = chain.sat = true;
        const Audio baseAll = simulate (s, chain);
        // A window of at most 8 s (plus 0.5 s warm-up) starting at the first hit keeps the search fast.
        const auto L5all = frameLevels (ex.x, f5);
        const auto onAll = onsets (L5all, s.inputLevelDb - 20.0f);
        size_t subStart = ex.preroll;
        for (auto o : onAll)
            if (o * f5 > ex.preroll + static_cast<size_t> (sr * 0.5)) { subStart = o * f5 - static_cast<size_t> (sr * 0.5); break; }
        const size_t subLen = std::min (static_cast<size_t> (sr * 8.5), baseAll[0].size() - std::min (subStart, baseAll[0].size()));
        auto slice = [&] (const Audio& a) {
            Audio out (a.size(), std::vector<float> (subLen));
            for (size_t c = 0; c < a.size(); ++c)
                std::copy (a[c].begin() + static_cast<long> (subStart), a[c].begin() + static_cast<long> (subStart + subLen), out[c].begin());
            return out;
        };
        const Audio base = slice (baseAll);
        const auto L5x = frameLevels (slice (ex.x), f5);
        const auto L5y = frameLevels (slice (ex.y), f5);
        const auto L5b = frameLevels (base, f5);
        const size_t warm = static_cast<size_t> (0.5 * sr) / f5;
        std::vector<size_t> use;
        for (auto o : onsets (L5x, s.inputLevelDb - 20.0f))
            if (o >= warm && o + 40 < L5x.size()) use.push_back (o);
        auto shape = [&] (const std::vector<float>& Lsig, const std::vector<float>& Lref) {
            // attack 0-15 ms and sustain 60-200 ms after each hit, relative to the overall offset
            double a = 0, su = 0, g = 0; size_t na = 0, ns = 0, ng = 0;
            for (size_t f = warm; f < std::min (Lsig.size(), Lref.size()); ++f)
                if (Lref[f] > s.inputLevelDb - 30.0f) { g += Lsig[f] - Lref[f]; ++ng; }
            g = ng ? g / static_cast<double> (ng) : 0.0;
            for (auto o : use)
            {
                for (size_t k = o; k < o + 3; ++k) { a += Lsig[k] - Lref[k] - g; ++na; }
                for (size_t k = o + 12; k < o + 40; ++k) { su += Lsig[k] - Lref[k] - g; ++ns; }
            }
            return std::pair<float, float> (na ? static_cast<float> (a / static_cast<double> (na)) : 0.0f,
                                            ns ? static_cast<float> (su / static_cast<double> (ns)) : 0.0f);
        };
        if (use.size() >= 6)
        {
            const auto obs = shape (L5y, L5b);
            if (std::abs (obs.first) > 1.5f || std::abs (obs.second) > 1.5f)
            {
                float best = std::numeric_limits<float>::max();
                EnvelopeSettings bestEnv;
                for (float at = -9.0f; at <= 12.0f; at += 3.0f)
                    for (float su = -12.0f; su <= 6.0f; su += 3.0f)
                    {
                        if (at == 0.0f && su == 0.0f) continue;
                        auto t = s; t.envelope = { true, at, su };
                        Audio a = base;
                        renderStages (t, a, sr, Stages { false, false, false, false, true });
                        const auto sim = shape (frameLevels (a, f5), L5b);
                        const float e = (sim.first - obs.first) * (sim.first - obs.first) + (sim.second - obs.second) * (sim.second - obs.second);
                        if (e < best) { best = e; bestEnv = t.envelope; }
                    }
                s.envelope = bestEnv;
            }
        }
    }

    mark ("envelope");
    // 7) Reverb (insert): tails after the raw track stops that the dry chain does not have
    if (opt.learnReverb)
    {
        Stages dry = all(); dry.reverb = false;
        const Audio base = simulate (s, dry);
        const auto Lb = frameLevels (base, f10);
        const auto off = offsets (Lx, s.inputLevelDb - 15.0f);
        std::vector<size_t> use;
        for (auto o : off)
            if (o > skip10 + 5 && o + 30 < Ly.size()) use.push_back (o);
        auto tails = [&] (const std::vector<float>& L) {
            // energy 30-100, 100-200 and 200-300 ms after the source stopped, relative to just before
            std::array<float, 3> t {};
            const std::array<std::pair<size_t, size_t>, 3> w { { { 3, 10 }, { 10, 20 }, { 20, 30 } } };
            for (auto o : use)
            {
                float pre = kFloorDb;
                for (size_t k = o - 5; k < o; ++k) pre = std::max (pre, L[k]);
                for (size_t i = 0; i < 3; ++i)
                {
                    double e = 0;
                    for (size_t k = o + w[i].first; k < o + w[i].second; ++k) e += std::pow (10.0, L[k] / 10.0);
                    t[i] += static_cast<float> (10.0 * std::log10 (e / static_cast<double> (w[i].second - w[i].first) + 1e-12)) - pre;
                }
            }
            for (auto& v : t) v /= static_cast<float> (std::max<size_t> (1, use.size()));
            return t;
        };
        if (use.size() >= 3)
        {
            const auto ty = tails (Ly);
            const auto tb = tails (Lb);
            if (ty[0] - tb[0] > 6.0f || ty[1] - tb[1] > 6.0f)
            {
                float best = std::numeric_limits<float>::max();
                ReverbSettings bestRv;
                for (float decay : { 0.6f, 1.0f, 1.5f, 2.2f, 3.2f, 4.5f })
                    for (float mix : { 5.0f, 10.0f, 15.0f, 25.0f, 40.0f })
                    {
                        auto t = s;
                        t.reverb = {};
                        t.reverb.used = true;
                        t.reverb.decayS = decay;
                        t.reverb.mixPct = mix;
                        t.reverb.lowCutHz = 20.0f;
                        t.reverb.dampingHz = 12000.0f;
                        Audio a = base;
                        renderStages (t, a, sr, Stages { false, false, false, false, false, false, true });
                        const auto ta = tails (frameLevels (a, f10));
                        float e = 0;
                        for (size_t i = 0; i < 3; ++i) e += (ta[i] - ty[i]) * (ta[i] - ty[i]);
                        if (e < best) { best = e; bestRv = t.reverb; }
                    }
                s.reverb = bestRv;
            }
        }
    }

    mark ("reverb");
    // 8) Static gain and the overall quality of the re-created chain
    {
        const auto Ls = frameLevels (simulate (s, all()), f10);
        float mean = 0;
        s.fitDb = levelError (Ly, Ls, active, &mean);
        s.gainDb = mean;
    }
    mark ("final");
    return s;
}

void renderChain (const ProcessingSettings& s, Audio& audio, double sr)
{
    renderStages (s, audio, sr, all());
    if (std::abs (s.gainDb) > 0.01f)
    {
        const float g = std::pow (10.0f, s.gainDb / 20.0f);
        for (auto& c : audio)
            for (auto& v : c) v *= g;
    }
}

//==============================================================================
namespace
{
std::vector<double> sendEnergies (const Audio& a, double sr)
{
    const size_t frame = static_cast<size_t> (sr * 0.05);
    {
        // 400 Hz - 6 kHz: where reverb returns are rarely filtered, so the energy follows the sends
        Audio band { toMono (a) };
        render (Kind::EQ, { { "lowcut", 400.0f }, { "lowcut_slope", 1.0f }, { "highcut", 6000.0f } }, band, sr);
        const auto& m = band[0];
        std::vector<double> e (m.size() / frame);
        for (size_t f = 0; f < e.size(); ++f)
            for (size_t i = f * frame; i < (f + 1) * frame; ++i) e[f] += static_cast<double> (m[i]) * m[i];
        return e;
    }
}
} // namespace

SendSource makeSendSource (const Audio& a, double sr)
{
    SendSource s;
    s.energy = sendEnergies (a, sr);
    const auto mono = toMono (a);
    s.bands = bandFrames (mono.data(), mono.size(), sr);
    return s;
}

void learnReturn (const Audio& ret, const std::vector<SendSource>& sources, double sr, const std::vector<float>& dryMixGainDb,
                  float returnMixGainDb, std::vector<ProcessingSettings*>& out)
{
    if (ret.empty() || sources.empty())
        return;
    const double dt = 0.05;
    const auto Er = sendEnergies (ret, sr);
    std::vector<std::vector<double>> Et;
    for (auto& src : sources) Et.push_back (src.energy);
    size_t F = Er.size();
    for (auto& e : Et) F = std::min (F, e.size());
    if (F < 40)
        return;

    const size_t T = sources.size();

    // Decay time straight from the return: how fast it falls while every source is silent.
    double measuredRt = 0.0;
    {
        std::vector<double> S (F, 0.0);
        for (size_t f = 0; f < F; ++f)
            for (auto& e : Et) S[f] += e[f];
        std::vector<float> rts;
        for (size_t f = 10; f + 4 < F; ++f)
        {
            double recent = 0;
            for (size_t k = f - 10; k < f; ++k) recent = std::max (recent, S[k]);
            if (recent <= 0 || S[f] > recent * 1.0e-3 || S[f - 1] <= recent * 1.0e-3)
                continue;  // not the first silent frame after sound
            // regression of the return level while the sources stay silent (skip the first frame)
            std::vector<std::pair<double, double>> pts;
            for (size_t k = f + 1; k < F && S[k] <= recent * 1.0e-3 && pts.size() < 40; ++k)
                if (Er[k] > 0) pts.emplace_back (static_cast<double> (k - f) * dt, 10.0 * std::log10 (Er[k]));
            if (pts.size() < 4)
                continue;
            double mx = 0, my = 0;
            for (auto& [x, y] : pts) { mx += x; my += y; }
            mx /= static_cast<double> (pts.size()); my /= static_cast<double> (pts.size());
            double sxy = 0, sxx = 0;
            for (auto& [x, y] : pts) { sxy += (x - mx) * (y - my); sxx += (x - mx) * (x - mx); }
            const double slope = sxx > 0 ? sxy / sxx : 0.0;  // dB per second
            if (slope < -3.0)
                rts.push_back (static_cast<float> (-60.0 / slope));
        }
        if (rts.size() >= 2)
            measuredRt = std::clamp (static_cast<double> (median (rts)), 0.2, 12.0);
    }

    // Send amounts: sources smeared by the reverb's energy decay, NNLS onto the return.
    double bestRes = std::numeric_limits<double>::max(), bestRt = 1.5;
    std::vector<double> bestW, bestShare;
    std::vector<double> candidates { 0.5, 0.8, 1.2, 1.6, 2.2, 3.0, 4.5 };
    if (measuredRt > 0.0)
        candidates = { measuredRt };
    for (double rt : candidates)
    {
        const size_t K = static_cast<size_t> (rt * 1.2 / dt) + 1;
        std::vector<double> kernel (K);
        for (size_t k = 0; k < K; ++k) kernel[k] = std::exp (-13.8155 * static_cast<double> (k) * dt / rt);
        std::vector<double> A (F * T, 0.0), y (Er.begin(), Er.begin() + static_cast<long> (F));
        for (size_t t = 0; t < T; ++t)
            for (size_t f = 0; f < F; ++f)
            {
                double acc = 0;
                for (size_t k = 0; k < K && k <= f; ++k) acc += Et[t][f - k] * kernel[k];
                A[f * T + t] = acc;
            }
        // NNLS (projected gradient, as in the genre learner)
        std::vector<double> AtA (T * T, 0.0), Aty (T, 0.0);
        for (size_t f = 0; f < F; ++f)
            for (size_t i = 0; i < T; ++i)
            {
                Aty[i] += A[f * T + i] * y[f];
                for (size_t j = 0; j < T; ++j) AtA[i * T + j] += A[f * T + i] * A[f * T + j];
            }
        double L = 0;
        for (size_t i = 0; i < T; ++i) { double row = 0; for (size_t j = 0; j < T; ++j) row += std::abs (AtA[i * T + j]); L = std::max (L, row); }
        if (L <= 0) continue;
        std::vector<double> w (T, 0.0), g (T);
        for (int it = 0; it < 3000; ++it)
        {
            for (size_t i = 0; i < T; ++i) { g[i] = -Aty[i]; for (size_t j = 0; j < T; ++j) g[i] += AtA[i * T + j] * w[j]; }
            for (size_t i = 0; i < T; ++i) w[i] = std::max (0.0, w[i] - g[i] / L);
        }
        double res = 0;
        for (size_t f = 0; f < F; ++f)
        {
            double p = 0;
            for (size_t t = 0; t < T; ++t) p += A[f * T + t] * w[t];
            res += (y[f] - p) * (y[f] - p);
        }
        if (res < bestRes)
        {
            bestRes = res; bestRt = rt; bestW = w;
            // share of the return each source explains: tiny sources get huge weights for nothing
            bestShare.assign (T, 0.0);
            double total = 0;
            for (size_t f = 0; f < F; ++f) total += y[f];
            for (size_t t = 0; t < T; ++t)
            {
                double c = 0;
                for (size_t f = 0; f < F; ++f) c += A[f * T + t] * w[t];
                bestShare[t] = total > 0 ? c / total : 0.0;
            }
        }
    }
    if (bestW.empty())
        return;

    // Our reverb's own energy gain at this decay (mix 100 %), measured on noise.
    double ownGain = 1.0;
    {
        Audio noise (1, std::vector<float> (static_cast<size_t> (sr * 4)));
        uint32_t seed = 1234567;
        for (auto& v : noise[0]) { seed = seed * 1664525u + 1013904223u; v = (static_cast<float> (seed >> 8) / 16777216.0f - 0.5f) * 0.5f; }
        double ein = 0;
        for (size_t i = static_cast<size_t> (sr); i < noise[0].size(); ++i) ein += static_cast<double> (noise[0][i]) * noise[0][i];
        render (Kind::Reverb, { { "decay", static_cast<float> (bestRt) }, { "mix", 100.0f }, { "lowcut", 20.0f }, { "damping", 20000.0f },
                                { "predelay", 0.0f } }, noise, sr);
        double eout = 0;
        for (size_t i = static_cast<size_t> (sr); i < noise[0].size(); ++i) eout += static_cast<double> (noise[0][i]) * noise[0][i];
        ownGain = ein > 0 ? std::max (1e-6, eout / ein) : 1.0;
    }
    // The kernel integrates the decay: a unit impulse of source energy gives sum(kernel) return energy.
    double kernelSum = 0;
    for (size_t k = 0; k < static_cast<size_t> (bestRt * 1.2 / dt) + 1; ++k) kernelSum += std::exp (-13.8155 * static_cast<double> (k) * dt / bestRt);

    // Tone of the return relative to its sources (low cut / damping)
    float lowCut = 150.0f, damping = 7000.0f;
    {
        const auto Br = bandFrames (toMono (ret).data(), toMono (ret).size(), sr);
        std::array<double, SpectrumBands::kNumBands> er {}, es {};
        for (auto& fr : Br) for (int b = 0; b < SpectrumBands::kNumBands; ++b) er[static_cast<size_t> (b)] += fr[static_cast<size_t> (b)];
        for (size_t t = 0; t < T; ++t)
            for (auto& fr : sources[t].bands)
                for (int b = 0; b < SpectrumBands::kNumBands; ++b) es[static_cast<size_t> (b)] += bestW[t] * fr[static_cast<size_t> (b)];
        std::array<float, SpectrumBands::kNumBands> ratio {};
        for (int b = 0; b < SpectrumBands::kNumBands; ++b)
            ratio[static_cast<size_t> (b)] = static_cast<float> (10.0 * std::log10 ((er[static_cast<size_t> (b)] + 1e-12) / (es[static_cast<size_t> (b)] + 1e-12)));
        const float mid = 0.5f * (ratio[4] + ratio[5]);
        for (int b = 3; b >= 0; --b)
            if (ratio[static_cast<size_t> (b)] < mid - 6.0f) { lowCut = SpectrumBands::kEdgesHz[static_cast<size_t> (b + 1)]; break; }
        for (int b = 6; b < SpectrumBands::kNumBands; ++b)
            if (ratio[static_cast<size_t> (b)] < mid - 6.0f) { damping = SpectrumBands::kEdgesHz[static_cast<size_t> (b)]; break; }
    }

    for (size_t t = 0; t < T && t < out.size(); ++t)
    {
        if (out[t] == nullptr || out[t]->reverb.used || bestW[t] <= 0 || bestShare[t] < 0.05)
            continue;
        // wet/dry energy in the mix, then the insert mix that gives the same ratio
        const double sendEnergy = bestW[t] * kernelSum;  // return energy per unit of source energy
        const double rho = sendEnergy * std::pow (10.0, (returnMixGainDb - (t < dryMixGainDb.size() ? dryMixGainDb[t] : 0.0f)) / 10.0);
        if (rho < 1.0e-4)  // more than 40 dB below the dry track: not really sent
            continue;
        const double q = std::sqrt (rho / ownGain);
        ReverbSettings rv;
        rv.used = true;
        rv.fromReturn = true;
        rv.decayS = static_cast<float> (bestRt);
        rv.mixPct = static_cast<float> (std::clamp (100.0 * q / (1.0 + q), 1.0, 80.0));
        rv.lowCutHz = lowCut;
        rv.dampingHz = damping;
        out[t]->reverb = rv;
    }
}

//==============================================================================
ProcessingSettings combineProcessing (const std::vector<ProcessingSettings>& list)
{
    ProcessingSettings c;
    if (list.empty())
        return c;
    c.tracks = static_cast<int> (list.size());
    auto med = [&] (auto get, bool geometric = false) {
        std::vector<float> v;
        for (auto& p : list) v.push_back (get (p));
        return geometric ? geoMedian (v) : median (v);
    };
    auto medIf = [&] (auto used, auto get, bool geometric, float fallback) {
        std::vector<float> v;
        for (auto& p : list) if (used (p)) v.push_back (get (p));
        if (v.empty()) return fallback;
        return geometric ? geoMedian (v) : median (v);
    };
    auto majority = [&] (auto used) {
        size_t k = 0;
        for (auto& p : list) k += used (p) ? 1 : 0;
        return 2 * k >= list.size();
    };
    c.inputLevelDb = med ([] (auto& p) { return p.inputLevelDb; });
    c.gainDb = med ([] (auto& p) { return p.gainDb; });
    c.pan = med ([] (auto& p) { return p.pan; });
    c.widthPct = med ([] (auto& p) { return p.widthPct; });
    c.fitDb = med ([] (auto& p) { return p.fitDb; });

    for (int b = 0; b < kEqCurveBands; ++b)
    {
        std::vector<float> v;
        for (auto& p : list)
            if (p.eqCurveValid[static_cast<size_t> (b)]) v.push_back (p.eqCurveDb[static_cast<size_t> (b)]);
        c.eqCurveValid[static_cast<size_t> (b)] = 2 * v.size() >= list.size() && ! v.empty();
        c.eqCurveDb[static_cast<size_t> (b)] = median (v);
    }
    c.eq = fitEq (c.eqCurveDb, c.eqCurveValid);

    c.gate.used = majority ([] (auto& p) { return p.gate.used; });
    auto gu = [] (auto& p) { return p.gate.used; };
    c.gate.thresholdRel = medIf (gu, [] (auto& p) { return p.gate.thresholdRel; }, false, c.gate.thresholdRel);
    c.gate.rangeDb = medIf (gu, [] (auto& p) { return p.gate.rangeDb; }, false, c.gate.rangeDb);
    c.gate.releaseMs = medIf (gu, [] (auto& p) { return p.gate.releaseMs; }, true, c.gate.releaseMs);

    c.deesser.used = majority ([] (auto& p) { return p.deesser.used; });
    auto du = [] (auto& p) { return p.deesser.used; };
    c.deesser.freqHz = medIf (du, [] (auto& p) { return p.deesser.freqHz; }, true, c.deesser.freqHz);
    c.deesser.thresholdRel = medIf (du, [] (auto& p) { return p.deesser.thresholdRel; }, false, c.deesser.thresholdRel);
    c.deesser.rangeDb = medIf (du, [] (auto& p) { return p.deesser.rangeDb; }, false, c.deesser.rangeDb);

    c.comp.used = majority ([] (auto& p) { return p.comp.used; });
    auto cu = [] (auto& p) { return p.comp.used; };
    c.comp.thresholdRel = medIf (cu, [] (auto& p) { return p.comp.thresholdRel; }, false, c.comp.thresholdRel);
    c.comp.ratio = medIf (cu, [] (auto& p) { return p.comp.ratio; }, true, c.comp.ratio);
    c.comp.attackMs = medIf (cu, [] (auto& p) { return p.comp.attackMs; }, true, c.comp.attackMs);
    c.comp.releaseMs = medIf (cu, [] (auto& p) { return p.comp.releaseMs; }, true, c.comp.releaseMs);
    c.comp.kneeDb = medIf (cu, [] (auto& p) { return p.comp.kneeDb; }, false, c.comp.kneeDb);
    c.comp.avgReductionDb = medIf (cu, [] (auto& p) { return p.comp.avgReductionDb; }, false, 0.0f);

    c.saturation.used = majority ([] (auto& p) { return p.saturation.used; });
    c.saturation.driveDb = medIf ([] (auto& p) { return p.saturation.used; }, [] (auto& p) { return p.saturation.driveDb; }, false, 0.0f);

    c.envelope.used = majority ([] (auto& p) { return p.envelope.used; });
    auto eu = [] (auto& p) { return p.envelope.used; };
    c.envelope.attackDb = medIf (eu, [] (auto& p) { return p.envelope.attackDb; }, false, 0.0f);
    c.envelope.sustainDb = medIf (eu, [] (auto& p) { return p.envelope.sustainDb; }, false, 0.0f);

    c.reverb.used = majority ([] (auto& p) { return p.reverb.used; });
    auto ru = [] (auto& p) { return p.reverb.used; };
    c.reverb.decayS = medIf (ru, [] (auto& p) { return p.reverb.decayS; }, true, c.reverb.decayS);
    c.reverb.mixPct = medIf (ru, [] (auto& p) { return p.reverb.mixPct; }, true, c.reverb.mixPct);
    c.reverb.predelayMs = medIf (ru, [] (auto& p) { return p.reverb.predelayMs; }, false, c.reverb.predelayMs);
    c.reverb.lowCutHz = medIf (ru, [] (auto& p) { return p.reverb.lowCutHz; }, true, c.reverb.lowCutHz);
    c.reverb.dampingHz = medIf (ru, [] (auto& p) { return p.reverb.dampingHz; }, true, c.reverb.dampingHz);
    c.reverb.fromReturn = majority ([] (auto& p) { return p.reverb.fromReturn; });
    return c;
}

//==============================================================================
nlohmann::json ProcessingSettings::toJson() const
{
    auto r1 = [] (float v) { return std::round (v * 10.0f) / 10.0f; };
    nlohmann::json bands = nlohmann::json::array();
    for (auto& b : eq.bands) bands.push_back ({ r1 (b.freq), r1 (b.gain), std::round (b.q * 100.0f) / 100.0f });
    nlohmann::json curve = nlohmann::json::array();
    for (int b = 0; b < kEqCurveBands; ++b)
        curve.push_back (eqCurveValid[static_cast<size_t> (b)] ? nlohmann::json (r1 (eqCurveDb[static_cast<size_t> (b)])) : nlohmann::json());
    return {
        { "input_level_db", r1 (inputLevelDb) }, { "gain_db", r1 (gainDb) }, { "pan", r1 (pan) }, { "width_pct", r1 (widthPct) },
        { "fit_db", r1 (fitDb) }, { "latency_samples", latencySamples }, { "tracks", tracks },
        { "eq_curve_db", curve },
        { "eq", { { "used", eq.used }, { "low_cut_hz", r1 (eq.lowCutHz) }, { "steep", eq.steep }, { "bands", bands },
                  { "high_cut_hz", r1 (eq.highCutHz) }, { "error_db", r1 (eq.errorDb) } } },
        { "gate", { { "used", gate.used }, { "threshold_rel", r1 (gate.thresholdRel) }, { "range_db", r1 (gate.rangeDb) },
                    { "attack_ms", r1 (gate.attackMs) }, { "hold_ms", r1 (gate.holdMs) }, { "release_ms", r1 (gate.releaseMs) } } },
        { "deesser", { { "used", deesser.used }, { "freq_hz", r1 (deesser.freqHz) }, { "threshold_rel", r1 (deesser.thresholdRel) },
                       { "range_db", r1 (deesser.rangeDb) } } },
        { "comp", { { "used", comp.used }, { "threshold_rel", r1 (comp.thresholdRel) }, { "ratio", r1 (comp.ratio) },
                    { "attack_ms", r1 (comp.attackMs) }, { "release_ms", r1 (comp.releaseMs) }, { "knee_db", r1 (comp.kneeDb) },
                    { "avg_reduction_db", r1 (comp.avgReductionDb) } } },
        { "saturation", { { "used", saturation.used }, { "drive_db", r1 (saturation.driveDb) }, { "type", saturation.type } } },
        { "envelope", { { "used", envelope.used }, { "attack_db", r1 (envelope.attackDb) }, { "sustain_db", r1 (envelope.sustainDb) } } },
        { "reverb", { { "used", reverb.used }, { "predelay_ms", r1 (reverb.predelayMs) }, { "decay_s", r1 (reverb.decayS) },
                      { "size_pct", r1 (reverb.sizePct) }, { "damping_hz", r1 (reverb.dampingHz) }, { "low_cut_hz", r1 (reverb.lowCutHz) },
                      { "mix_pct", r1 (reverb.mixPct) }, { "from_return", reverb.fromReturn } } },
    };
}

ProcessingSettings ProcessingSettings::fromJson (const nlohmann::json& j)
{
    ProcessingSettings s;
    if (! j.is_object())
        return s;
    s.inputLevelDb = j.value ("input_level_db", s.inputLevelDb);
    s.gainDb = j.value ("gain_db", 0.0f);
    s.pan = j.value ("pan", 0.0f);
    s.widthPct = j.value ("width_pct", 100.0f);
    s.fitDb = j.value ("fit_db", 0.0f);
    s.latencySamples = j.value ("latency_samples", 0);
    s.tracks = j.value ("tracks", 1);
    if (j.contains ("eq_curve_db") && j["eq_curve_db"].is_array())
    {
        const auto curve = j["eq_curve_db"];
        for (size_t b = 0; b < curve.size() && b < static_cast<size_t> (kEqCurveBands); ++b)
            if (curve[b].is_number())
            {
                s.eqCurveDb[b] = curve[b].get<float>();
                s.eqCurveValid[b] = true;
            }
    }
    const auto eq = j.value ("eq", nlohmann::json::object());
    s.eq.used = eq.value ("used", false);
    s.eq.lowCutHz = eq.value ("low_cut_hz", 0.0f);
    s.eq.steep = eq.value ("steep", false);
    s.eq.highCutHz = eq.value ("high_cut_hz", 0.0f);
    s.eq.errorDb = eq.value ("error_db", 0.0f);
    if (eq.contains ("bands") && eq["bands"].is_array())
    {
        const auto bands = eq["bands"];
        for (size_t b = 0; b < bands.size() && b < 6; ++b)
            if (bands[b].is_array() && bands[b].size() == 3)
                s.eq.bands[b] = { bands[b][0].get<float>(), bands[b][1].get<float>(), bands[b][2].get<float>() };
    }
    const auto g = j.value ("gate", nlohmann::json::object());
    s.gate = { g.value ("used", false), g.value ("threshold_rel", -30.0f), g.value ("range_db", 20.0f), g.value ("attack_ms", 0.5f),
               g.value ("hold_ms", 40.0f), g.value ("release_ms", 150.0f) };
    const auto d = j.value ("deesser", nlohmann::json::object());
    s.deesser = { d.value ("used", false), d.value ("freq_hz", 6500.0f), d.value ("threshold_rel", -20.0f), d.value ("range_db", 6.0f) };
    const auto c = j.value ("comp", nlohmann::json::object());
    s.comp = { c.value ("used", false), c.value ("threshold_rel", -10.0f), c.value ("ratio", 3.0f), c.value ("attack_ms", 10.0f),
               c.value ("release_ms", 120.0f), c.value ("knee_db", 6.0f), c.value ("avg_reduction_db", 0.0f) };
    const auto sa = j.value ("saturation", nlohmann::json::object());
    s.saturation = { sa.value ("used", false), sa.value ("drive_db", 0.0f), sa.value ("type", 0) };
    const auto e = j.value ("envelope", nlohmann::json::object());
    s.envelope = { e.value ("used", false), e.value ("attack_db", 0.0f), e.value ("sustain_db", 0.0f) };
    const auto r = j.value ("reverb", nlohmann::json::object());
    s.reverb.used = r.value ("used", false);
    s.reverb.predelayMs = r.value ("predelay_ms", 20.0f);
    s.reverb.decayS = r.value ("decay_s", 1.6f);
    s.reverb.sizePct = r.value ("size_pct", 60.0f);
    s.reverb.dampingHz = r.value ("damping_hz", 7000.0f);
    s.reverb.lowCutHz = r.value ("low_cut_hz", 150.0f);
    s.reverb.mixPct = r.value ("mix_pct", 15.0f);
    s.reverb.fromReturn = r.value ("from_return", false);
    return s;
}

std::string ProcessingSettings::describe() const
{
    std::string out;
    char buf[160];
    auto add = [&out] (const char* t) { if (! out.empty()) out += " · "; out += t; };
    if (eq.used)
    {
        std::string e = "EQ";
        if (eq.lowCutHz > 20.5f) { std::snprintf (buf, sizeof (buf), " 로우컷 %.0f Hz", eq.lowCutHz); e += buf; }
        for (auto& b : eq.bands)
            if (std::abs (b.gain) >= 0.75f) { std::snprintf (buf, sizeof (buf), " %+.1f dB@%.0f Hz", b.gain, b.freq); e += buf; }
        add (e.c_str());
    }
    if (gate.used) { std::snprintf (buf, sizeof (buf), "게이트 %.0f dB(입력 대비) 레인지 %.0f dB", gate.thresholdRel, gate.rangeDb); add (buf); }
    if (deesser.used) { std::snprintf (buf, sizeof (buf), "디에서 %.1f kHz 최대 %.0f dB", deesser.freqHz / 1000.0f, deesser.rangeDb); add (buf); }
    if (comp.used)
    {
        std::snprintf (buf, sizeof (buf), "컴프 %.1f:1 어택 %.1f ms 릴리즈 %.0f ms (평균 %.1f dB 압축)", comp.ratio, comp.attackMs, comp.releaseMs,
                       comp.avgReductionDb);
        add (buf);
    }
    if (envelope.used) { std::snprintf (buf, sizeof (buf), "엔벨로프 어택 %+.0f dB 서스테인 %+.0f dB", envelope.attackDb, envelope.sustainDb); add (buf); }
    if (saturation.used) { std::snprintf (buf, sizeof (buf), "새츄레이션 %.0f dB", saturation.driveDb); add (buf); }
    if (reverb.used)
    {
        std::snprintf (buf, sizeof (buf), "리버브%s %.1f s 믹스 %.0f%%", reverb.fromReturn ? "(센드)" : "", reverb.decayS, reverb.mixPct);
        add (buf);
    }
    if (out.empty())
        out = "처리 없음(또는 레벨만)";
    return out;
}

} // namespace smix::style
