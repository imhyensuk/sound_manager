// Implementations of Sound Manager's built-in processors (see smix/dsp/Builtin.h).
// All real-time paths are allocation-free; coefficients are recomputed only when a parameter changes.

#include <algorithm>
#include <array>
#include <cmath>

#include "smix/dsp/Builtin.h"

namespace smix::dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
inline float gainToDb (float g) { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

/** One-pole smoothing coefficient for a time constant in ms. */
inline float coef (double ms, double sr)
{
    return ms <= 0.0 ? 0.0f : static_cast<float> (std::exp (-1.0 / (ms * 0.001 * sr)));
}

// RBJ cookbook biquad, transposed direct form II.
struct Biquad
{
    enum class Type { Peak, LowShelf, HighShelf, HighPass, LowPass, BandPass };
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    void set (Type t, double sr, double freq, double q, double gainDb)
    {
        freq = std::clamp (freq, 10.0, 0.49 * sr);
        q = std::max (q, 0.05);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * freq / sr;
        const double cw = std::cos (w), sw = std::sin (w);
        const double alpha = sw / (2.0 * q);
        double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
        switch (t)
        {
            case Type::Peak:
                B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A;
                A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A;
                break;
            case Type::LowShelf:
            {
                const double s = 2 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) - (A - 1) * cw + s); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - s);
                A0 = (A + 1) + (A - 1) * cw + s; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case Type::HighShelf:
            {
                const double s = 2 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) + (A - 1) * cw + s); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - s);
                A0 = (A + 1) - (A - 1) * cw + s; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
            case Type::HighPass:
                B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = (1 + cw) / 2;
                A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
                break;
            case Type::LowPass:
                B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = (1 - cw) / 2;
                A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
                break;
            case Type::BandPass:
                B0 = alpha; B1 = 0; B2 = -alpha;
                A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
                break;
        }
        b0 = static_cast<float> (B0 / A0); b1 = static_cast<float> (B1 / A0); b2 = static_cast<float> (B2 / A0);
        a1 = static_cast<float> (A1 / A0); a2 = static_cast<float> (A2 / A0);
    }

    void setIdentity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }
};

struct BiquadState
{
    float z1 = 0, z2 = 0;
    inline float run (const Biquad& c, float x) noexcept
    {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void clear() { z1 = z2 = 0; }
};

constexpr int kMaxCh = 8;

//==============================================================================
class GainProc final : public Processor
{
public:
    GainProc() : Processor (Kind::Gain) {}

private:
    void onParams() override
    {
        target = dbToGain (p (0));
        const float pan = p (1) / 100.0f;  // constant-power pan law, unity at centre
        const double angle = (pan + 1.0) * kPi / 4.0;
        panL = static_cast<float> (std::cos (angle) * std::sqrt (2.0));
        panR = static_cast<float> (std::sin (angle) * std::sqrt (2.0));
        width = p (2) / 100.0f;
        sign = p (3) >= 0.5f ? -1.0f : 1.0f;
    }
    void onReset() override { current = target; }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        const float step = (target - current) / static_cast<float> (n);
        for (int i = 0; i < n; ++i)
        {
            current += step;
            const float g = current * sign;
            if (nc >= 2)
            {
                float l = ch[0][i], r = ch[1][i];
                const float m = 0.5f * (l + r), s = 0.5f * (l - r) * width;
                l = (m + s) * g * panL;
                r = (m - s) * g * panR;
                ch[0][i] = l;
                ch[1][i] = r;
                for (int c = 2; c < nc; ++c)
                    ch[c][i] *= g;
            }
            else
            {
                ch[0][i] *= g;
            }
        }
        current = target;
    }

    float target = 1, current = 1, panL = 1, panR = 1, width = 1, sign = 1;
};

//==============================================================================
class GateProc final : public Processor
{
public:
    GateProc() : Processor (Kind::Gate) {}

private:
    void onParams() override
    {
        threshold = p (0);
        floorGain = dbToGain (-p (1));
        attackCoef = coef (p (2), sampleRate);
        holdSamples = static_cast<int> (p (3) * 0.001 * sampleRate);
        releaseCoef = coef (p (4), sampleRate);
        key.set (Biquad::Type::HighPass, sampleRate, p (5), 0.707, 0.0);
        if (p (5) <= 20.5f)
            key.setIdentity();
        levelCoef = coef (1.0, sampleRate);
    }
    void onReset() override
    {
        for (auto& k : keyState) k.clear();
        open = 1.0f;
        level = 0.0f;
        holdLeft = 0;
        isOpen = true;
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        float minGain = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < nc; ++c)
                peak = std::max (peak, std::abs (keyState[static_cast<size_t> (c)].run (key, ch[c][i])));
            // Fast peak follower (instant attack, 1 ms release) for the open/close decision.
            level = peak > level ? peak : levelCoef * level + (1.0f - levelCoef) * peak;
            const float db = gainToDb (level);
            if (db > threshold)
            {
                isOpen = true;
                holdLeft = holdSamples;
            }
            else if (db < threshold - 4.0f)  // hysteresis: avoids chattering around the threshold
            {
                if (holdLeft > 0)
                    --holdLeft;
                else
                    isOpen = false;
            }
            const float targetOpen = isOpen ? 1.0f : 0.0f;
            const float k = targetOpen > open ? attackCoef : releaseCoef;
            open = k * open + (1.0f - k) * targetOpen;
            const float g = floorGain + (1.0f - floorGain) * open;
            minGain = std::min (minGain, g);
            for (int c = 0; c < nc; ++c)
                ch[c][i] *= g;
        }
        meter.store (-gainToDb (minGain), std::memory_order_relaxed);
    }

    Biquad key;
    std::array<BiquadState, kMaxCh> keyState {};
    float threshold = -50, floorGain = 0.01f, attackCoef = 0, releaseCoef = 0, levelCoef = 0;
    float open = 1, level = 0;
    int holdSamples = 0, holdLeft = 0;
    bool isOpen = true;
};

//==============================================================================
class EqProc final : public Processor
{
public:
    EqProc() : Processor (Kind::EQ) {}

private:
    enum { kLowCut1, kLowCut2, kB1, kB2, kB3, kB4, kB5, kB6, kHighCut, kNumFilters };

    void onParams() override
    {
        const double sr = sampleRate;
        active.fill (false);
        if (p (0) > 20.5f)
        {
            const bool steep = p (1) >= 0.5f;
            // 24 dB/oct: two Butterworth sections (Q 0.54 and 1.31); 12 dB/oct: one with Q 0.707.
            f[kLowCut1].set (Biquad::Type::HighPass, sr, p (0), steep ? 0.541 : 0.707, 0);
            active[kLowCut1] = true;
            if (steep)
            {
                f[kLowCut2].set (Biquad::Type::HighPass, sr, p (0), 1.307, 0);
                active[kLowCut2] = true;
            }
        }
        auto band = [&] (int idx, Biquad::Type t, float freq, float gain, float q) {
            if (std::abs (gain) < 0.01f)
                return;
            f[static_cast<size_t> (idx)].set (t, sr, freq, q, gain);
            active[static_cast<size_t> (idx)] = true;
        };
        band (kB1, Biquad::Type::LowShelf, p (2), p (3), 0.707f);
        band (kB2, Biquad::Type::Peak, p (4), p (5), p (6));
        band (kB3, Biquad::Type::Peak, p (7), p (8), p (9));
        band (kB4, Biquad::Type::Peak, p (10), p (11), p (12));
        band (kB5, Biquad::Type::Peak, p (13), p (14), p (15));
        band (kB6, Biquad::Type::HighShelf, p (16), p (17), 0.707f);
        if (p (18) < 21900.0f)
        {
            f[kHighCut].set (Biquad::Type::LowPass, sr, p (18), 0.707, 0);
            active[kHighCut] = true;
        }
        out = dbToGain (p (19));
    }
    void onReset() override
    {
        for (auto& c : state)
            for (auto& s : c) s.clear();
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        for (int c = 0; c < nc; ++c)
        {
            auto& st = state[static_cast<size_t> (c)];
            float* x = ch[c];
            for (size_t k = 0; k < kNumFilters; ++k)
            {
                if (! active[k])
                    continue;
                for (int i = 0; i < n; ++i)
                    x[i] = st[k].run (f[k], x[i]);
            }
            if (out != 1.0f)
                for (int i = 0; i < n; ++i)
                    x[i] *= out;
        }
    }

    std::array<Biquad, kNumFilters> f {};
    std::array<bool, kNumFilters> active {};
    std::array<std::array<BiquadState, kNumFilters>, kMaxCh> state {};
    float out = 1;
};

//==============================================================================
class DeEsserProc final : public Processor
{
public:
    DeEsserProc() : Processor (Kind::DeEsser) {}

private:
    // Linkwitz-Riley 4th-order split: low + high sum to an all-pass, so only the sibilant band moves.
    void onParams() override
    {
        lp.set (Biquad::Type::LowPass, sampleRate, p (0), 0.7071, 0);
        hp.set (Biquad::Type::HighPass, sampleRate, p (0), 0.7071, 0);
        threshold = p (1);
        range = p (2);
        attackCoef = coef (0.5, sampleRate);
        releaseCoef = coef (60.0, sampleRate);
    }
    void onReset() override
    {
        for (auto& c : st) for (auto& s : c) s.clear();
        env = 0;
        gr = 0;
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        float maxGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            float lo[kMaxCh], hi[kMaxCh];
            float peak = 0.0f;
            for (int c = 0; c < nc; ++c)
            {
                auto& s = st[static_cast<size_t> (c)];
                const float x = ch[c][i];
                lo[c] = s[1].run (lp, s[0].run (lp, x));
                hi[c] = s[3].run (hp, s[2].run (hp, x));
                peak = std::max (peak, std::abs (hi[c]));
            }
            env = peak > env ? attackCoef * env + (1.0f - attackCoef) * peak : releaseCoef * env + (1.0f - releaseCoef) * peak;
            const float over = gainToDb (env) - threshold;
            const float target = over > 0.0f ? std::min (range, over * 0.75f) : 0.0f;  // 4:1 above the threshold
            gr = target > gr ? target : releaseCoef * gr + (1.0f - releaseCoef) * target;
            maxGr = std::max (maxGr, gr);
            const float keep = dbToGain (-gr);
            for (int c = 0; c < nc; ++c)
                ch[c][i] = lo[c] + keep * hi[c];
        }
        meter.store (maxGr, std::memory_order_relaxed);
    }

    Biquad lp, hp;
    std::array<std::array<BiquadState, 4>, kMaxCh> st {};
    float threshold = -30, range = 8, attackCoef = 0, releaseCoef = 0, env = 0, gr = 0;
};

//==============================================================================
/** Feed-forward, log-domain compressor with a soft knee and smooth attack/release branching. */
class CompressorProc final : public Processor
{
public:
    CompressorProc() : Processor (Kind::Compressor) {}

private:
    void onParams() override
    {
        threshold = p (0);
        ratio = std::max (1.0f, p (1));
        attackCoef = coef (p (2), sampleRate);
        releaseCoef = coef (p (3), sampleRate);
        knee = p (4);
        makeup = p (5);
        wet = p (6) / 100.0f;
    }
    void onReset() override { gr = 0; }

    float staticReduction (float xDb) const noexcept
    {
        const float over = xDb - threshold;
        const float slope = 1.0f - 1.0f / ratio;
        if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
            return slope * (over + knee / 2.0f) * (over + knee / 2.0f) / (2.0f * knee);
        return over > 0.0f ? slope * over : 0.0f;
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        float maxGr = 0.0f;
        const float mk = dbToGain (makeup);
        for (int i = 0; i < n; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < nc; ++c)
                peak = std::max (peak, std::abs (ch[c][i]));
            const float target = staticReduction (gainToDb (peak));
            const float k = target > gr ? attackCoef : releaseCoef;
            gr = k * gr + (1.0f - k) * target;
            maxGr = std::max (maxGr, gr);
            const float g = dbToGain (-gr) * mk;
            const float mixG = wet * g + (1.0f - wet);
            for (int c = 0; c < nc; ++c)
                ch[c][i] *= mixG;
        }
        meter.store (maxGr, std::memory_order_relaxed);
    }

    float threshold = -18, ratio = 4, attackCoef = 0, releaseCoef = 0, knee = 6, makeup = 0, wet = 1, gr = 0;
};

//==============================================================================
/** Waveshaper at 2x oversampling (31-tap half-band style FIR up and down) to keep aliasing low. */
class SaturationProc final : public Processor
{
public:
    SaturationProc() : Processor (Kind::Saturation)
    {
        // Windowed-sinc low-pass at a quarter of the oversampled rate.
        double sum = 0;
        for (int i = 0; i < kTaps; ++i)
        {
            const double m = i - (kTaps - 1) / 2.0;
            const double sinc = m == 0 ? 0.5 : std::sin (kPi * 0.5 * m) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos (2 * kPi * i / (kTaps - 1)) + 0.08 * std::cos (4 * kPi * i / (kTaps - 1));
            taps[static_cast<size_t> (i)] = static_cast<float> (sinc * w);
            sum += sinc * w;
        }
        for (auto& t : taps)
            t = static_cast<float> (t / sum);
    }

    // (taps-1)/2 per filter at the 2x rate, two filters -> (taps-1)/2 at the base rate.
    int latencySamples() const override { return kLatency; }

private:
    static constexpr int kTaps = 31;
    static constexpr int kLatency = (kTaps - 1) / 2;

    void onParams() override
    {
        drive = dbToGain (p (0));
        type = static_cast<int> (std::lround (p (1)));
        tone.set (Biquad::Type::HighShelf, sampleRate, 3000.0, 0.707, p (2));
        toneActive = std::abs (p (2)) > 0.01f;
        wet = p (3) / 100.0f;
        out = dbToGain (p (4));
        dcBlock.set (Biquad::Type::HighPass, sampleRate, 10.0, 0.707, 0);
    }
    void onReset() override
    {
        for (auto& c : upHist) c.fill (0);
        for (auto& c : downHist) c.fill (0);
        for (auto& c : dryDelay) c.fill (0);
        for (auto& s : toneState) s.clear();
        for (auto& s : dcState) s.clear();
        upPos.fill (0);
        downPos.fill (0);
        dryPos = 0;
    }

    inline float shape (float x) const noexcept
    {
        switch (type)
        {
            case 1:  // tube: biased tanh -> even harmonics (DC removed afterwards)
                return std::tanh (x + 0.25f) - 0.2449187f;
            case 2:  // clip: cubic soft clip, hard beyond
                if (x > 1.0f) return 2.0f / 3.0f;
                if (x < -1.0f) return -2.0f / 3.0f;
                return x - x * x * x / 3.0f;
            default:  // tape
                return std::tanh (x);
        }
    }

    inline float fir (const std::array<float, kTaps>& hist, int pos) const noexcept
    {
        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k)
            acc += taps[static_cast<size_t> (k)] * hist[static_cast<size_t> ((pos + k) % kTaps)];
        return acc;
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        // Small-signal gain of every shaper is 1 (tube ~0.94): divide by the drive so quiet parts keep their level.
        const float norm = 1.0f / (drive * (type == 1 ? 0.9388f : 1.0f));
        for (int c = 0; c < nc; ++c)
        {
            auto& up = upHist[static_cast<size_t> (c)];
            auto& down = downHist[static_cast<size_t> (c)];
            int& upP = upPos[static_cast<size_t> (c)];
            int& dnP = downPos[static_cast<size_t> (c)];
            auto& dry = dryDelay[static_cast<size_t> (c)];
            float* x = ch[c];
            for (int i = 0; i < n; ++i)
            {
                const float in = x[i];
                float result = 0.0f;
                for (int phase = 0; phase < 2; ++phase)
                {
                    upP = (upP + kTaps - 1) % kTaps;
                    up[static_cast<size_t> (upP)] = phase == 0 ? 2.0f * in : 0.0f;  // zero stuffing, gain 2
                    const float s = shape (fir (up, upP) * drive) * norm;
                    dnP = (dnP + kTaps - 1) % kTaps;
                    down[static_cast<size_t> (dnP)] = s;
                    if (phase == 0)
                        result = fir (down, dnP);  // decimate on the even phase: exactly kLatency samples late
                }
                result = dcState[static_cast<size_t> (c)].run (dcBlock, result);
                if (toneActive)
                    result = toneState[static_cast<size_t> (c)].run (tone, result);
                // Dry path delayed by the same latency, so the mix control does not comb-filter.
                const int dIdx = (dryPos + i) % kLatency;
                const float delayedDry = dry[static_cast<size_t> (dIdx)];
                dry[static_cast<size_t> (dIdx)] = in;
                x[i] = (wet * result + (1.0f - wet) * delayedDry) * out;
            }
        }
        dryPos = (dryPos + n) % kLatency;
    }

    std::array<float, kTaps> taps {};
    std::array<std::array<float, kTaps>, kMaxCh> upHist {}, downHist {};
    std::array<std::array<float, kLatency>, kMaxCh> dryDelay {};
    std::array<int, kMaxCh> upPos {}, downPos {};
    int dryPos = 0;
    Biquad tone, dcBlock;
    std::array<BiquadState, kMaxCh> toneState {}, dcState {};
    bool toneActive = false;
    float drive = 1, wet = 1, out = 1;
    int type = 0;
};

//==============================================================================
/** Transient shaper: fast vs slow envelopes separate the attack from the sustain. */
class EnveloperProc final : public Processor
{
public:
    EnveloperProc() : Processor (Kind::Enveloper) {}

private:
    void onParams() override
    {
        attackDb = p (0);
        sustainDb = p (1);
        out = dbToGain (p (2));
        fastA = coef (0.5, sampleRate);
        slowA = coef (25.0, sampleRate);
        rel = coef (40.0, sampleRate);  // faster than a drum's decay: both detectors fall back onto the level
        msCoef = coef (4.0, sampleRate);
        sFastR = coef (25.0, sampleRate);
        sSlowR = coef (400.0, sampleRate);
        smooth = coef (1.0, sampleRate);
    }
    void onReset() override { ef = es = rf = rs = ms = 0; g = 0; }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            float sq = 0.0f;
            for (int c = 0; c < nc; ++c)
                sq = std::max (sq, ch[c][i] * ch[c][i]);
            // short-term RMS: smooth enough that a sustained tone gives a steady level
            ms = msCoef * ms + (1 - msCoef) * sq;
            const float x = std::sqrt (ms);
            // attack detector: fast vs slow rising envelope
            ef = x > ef ? fastA * ef + (1 - fastA) * x : rel * ef + (1 - rel) * x;
            es = x > es ? slowA * es + (1 - slowA) * x : rel * es + (1 - rel) * x;
            // sustain detector: fast vs slow falling envelope
            rf = x > rf ? x : sFastR * rf + (1 - sFastR) * x;
            rs = x > rs ? x : sSlowR * rs + (1 - sSlowR) * x;
            const float transient = std::clamp ((gainToDb (ef + 1e-9f) - gainToDb (es + 1e-9f)) / 6.0f, 0.0f, 1.0f);
            const float tail = std::clamp ((gainToDb (rs + 1e-9f) - gainToDb (rf + 1e-9f)) / 12.0f, 0.0f, 1.0f);
            const float targetDb = attackDb * transient + sustainDb * tail;
            g = smooth * g + (1 - smooth) * targetDb;
            const float gain = dbToGain (g) * out;
            for (int c = 0; c < nc; ++c)
                ch[c][i] *= gain;
        }
        meter.store (std::max (0.0f, -g), std::memory_order_relaxed);
    }

    float attackDb = 0, sustainDb = 0, out = 1;
    float fastA = 0, slowA = 0, rel = 0, sFastR = 0, sSlowR = 0, smooth = 0, msCoef = 0;
    float ef = 0, es = 0, rf = 0, rs = 0, ms = 0, g = 0;
};

//==============================================================================
/** 8-line feedback delay network with a Householder matrix, input diffusion and in-loop damping. */
class ReverbProc final : public Processor
{
public:
    ReverbProc() : Processor (Kind::Reverb) {}

    double tailSeconds() const override { return p (1) * 1.2 + 0.3; }

private:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;

    void onPrepare() override
    {
        const double sr = sampleRate;
        preBuf.assign (static_cast<size_t> (0.26 * sr) + 2, 0.0f);
        for (int l = 0; l < kLines; ++l)
            lines[static_cast<size_t> (l)].assign (static_cast<size_t> (kBaseMs[l] * 0.0015 * sr) + 2, 0.0f);
        for (int d = 0; d < kDiffusers; ++d)
            diff[static_cast<size_t> (d)].assign (static_cast<size_t> (kDiffMs[d] * 0.001 * sr) + 2, 0.0f);
    }

    void onParams() override
    {
        const double sr = sampleRate;
        preSamples = std::clamp (static_cast<int> (p (0) * 0.001 * sr), 0, static_cast<int> (preBuf.size()) - 2);
        const double rt60 = std::max (0.1f, p (1));
        const double scale = 0.5 + p (2) / 100.0;  // size 0..100 % -> 0.5..1.5 of the base lengths
        for (int l = 0; l < kLines; ++l)
        {
            const int len = std::clamp (static_cast<int> (kBaseMs[l] * 0.001 * scale * sr), 8,
                                        static_cast<int> (lines[static_cast<size_t> (l)].size()) - 1);
            length[static_cast<size_t> (l)] = len;
            loopGain[static_cast<size_t> (l)] = static_cast<float> (std::pow (10.0, -3.0 * len / (rt60 * sr)));
        }
        dampCoef = static_cast<float> (std::exp (-2.0 * kPi * std::min<double> (p (3), 0.45 * sr) / sr));
        lowCut.set (Biquad::Type::HighPass, sr, p (4), 0.707, 0);
        wet = p (5) / 100.0f;
        width = p (6) / 100.0f;
        for (int d = 0; d < kDiffusers; ++d)
            diffLen[static_cast<size_t> (d)] = std::clamp (static_cast<int> (kDiffMs[d] * 0.001 * sr), 4,
                                                            static_cast<int> (diff[static_cast<size_t> (d)].size()) - 1);
    }

    void onReset() override
    {
        std::fill (preBuf.begin(), preBuf.end(), 0.0f);
        for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
        for (auto& d : diff) std::fill (d.begin(), d.end(), 0.0f);
        pos.fill (0);
        dpos.fill (0);
        damp.fill (0);
        prePos = 0;
        lcState.clear();
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            float in = 0.0f;
            for (int c = 0; c < nc; ++c)
                in += ch[c][i];
            in = lcState.run (lowCut, in / static_cast<float> (nc));

            // pre-delay
            preBuf[static_cast<size_t> (prePos)] = in;
            int readPre = prePos - preSamples;
            if (readPre < 0) readPre += static_cast<int> (preBuf.size());
            float x = preBuf[static_cast<size_t> (readPre)];
            prePos = (prePos + 1) % static_cast<int> (preBuf.size());

            // input diffusion (series all-passes)
            for (int d = 0; d < kDiffusers; ++d)
            {
                auto& buf = diff[static_cast<size_t> (d)];
                int& dp = dpos[static_cast<size_t> (d)];
                const int len = diffLen[static_cast<size_t> (d)];
                int rd = dp - len;
                if (rd < 0) rd += static_cast<int> (buf.size());
                const float delayed = buf[static_cast<size_t> (rd)];
                const float v = x + 0.6f * delayed;
                buf[static_cast<size_t> (dp)] = v;
                x = delayed - 0.6f * v;
                dp = (dp + 1) % static_cast<int> (buf.size());
            }

            // FDN
            float outs[kLines];
            float sum = 0.0f;
            for (int l = 0; l < kLines; ++l)
            {
                auto& buf = lines[static_cast<size_t> (l)];
                int rd = pos[static_cast<size_t> (l)] - length[static_cast<size_t> (l)];
                if (rd < 0) rd += static_cast<int> (buf.size());
                float o = buf[static_cast<size_t> (rd)];
                float& dz = damp[static_cast<size_t> (l)];
                dz = (1.0f - dampCoef) * o + dampCoef * dz;  // one-pole low-pass in the loop
                outs[l] = dz * loopGain[static_cast<size_t> (l)];
                sum += outs[l];
            }
            const float h = 2.0f / kLines * sum;
            for (int l = 0; l < kLines; ++l)
            {
                auto& buf = lines[static_cast<size_t> (l)];
                const float fb = outs[l] - h;  // Householder reflection: energy preserving
                buf[static_cast<size_t> (pos[static_cast<size_t> (l)])] = fb + ((l & 1) ? -x : x) * 0.5f;
                pos[static_cast<size_t> (l)] = (pos[static_cast<size_t> (l)] + 1) % static_cast<int> (buf.size());
            }
            const float wl = 0.5f * (outs[0] - outs[2] + outs[4] - outs[6]);
            const float wr = 0.5f * (outs[1] - outs[3] + outs[5] - outs[7]);
            const float m = 0.5f * (wl + wr), s = 0.5f * (wl - wr) * width;
            if (nc >= 2)
            {
                ch[0][i] = ch[0][i] * (1.0f - wet) + wet * (m + s);
                ch[1][i] = ch[1][i] * (1.0f - wet) + wet * (m - s);
            }
            else
            {
                ch[0][i] = ch[0][i] * (1.0f - wet) + wet * m;
            }
        }
    }

    static constexpr double kBaseMs[kLines] = { 29.7, 37.1, 41.1, 43.7, 47.9, 53.3, 59.1, 67.3 };
    static constexpr double kDiffMs[kDiffusers] = { 4.7, 3.6, 12.7, 9.3 };

    std::vector<float> preBuf;
    std::array<std::vector<float>, kLines> lines;
    std::array<std::vector<float>, kDiffusers> diff;
    std::array<int, kLines> pos {}, length {};
    std::array<int, kDiffusers> dpos {}, diffLen {};
    std::array<float, kLines> loopGain {}, damp {};
    int prePos = 0, preSamples = 0;
    float dampCoef = 0, wet = 0.2f, width = 1;
    Biquad lowCut;
    BiquadState lcState;
};

//==============================================================================
/** Look-ahead peak limiter (1.5 ms): the gain reaches its target before the peak arrives. */
class LimiterProc final : public Processor
{
public:
    LimiterProc() : Processor (Kind::Limiter) {}

    int latencySamples() const override { return look; }

private:
    void onPrepare() override
    {
        look = std::max (1, static_cast<int> (0.0015 * sampleRate));
        for (auto& d : delay) d.assign (static_cast<size_t> (look + 1), 0.0f);
        need.assign (static_cast<size_t> (look + 1), 1.0f);
    }
    void onParams() override
    {
        inGain = dbToGain (p (0));
        ceiling = dbToGain (p (1));
        releaseCoef = coef (p (2), sampleRate);
        attackCoef = coef (0.0015 * 1000.0 / 5.0, sampleRate);  // ~5 time constants within the look-ahead
    }
    void onReset() override
    {
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        std::fill (need.begin(), need.end(), 1.0f);
        wpos = 0;
        g = 1;
    }

    void onProcess (float* const* ch, int nc, int n) noexcept override
    {
        float minG = 1.0f;
        const int size = look + 1;
        for (int i = 0; i < n; ++i)
        {
            float peak = 0.0f;
            for (int c = 0; c < nc; ++c)
            {
                const float v = ch[c][i] * inGain;
                peak = std::max (peak, std::abs (v));
                delay[static_cast<size_t> (c)][static_cast<size_t> (wpos)] = v;
            }
            need[static_cast<size_t> (wpos)] = peak > ceiling ? ceiling / peak : 1.0f;
            float target = 1.0f;
            for (float v : need)
                target = std::min (target, v);
            const float k = target < g ? attackCoef : releaseCoef;
            g = k * g + (1.0f - k) * target;
            const int rpos = (wpos + 1) % size;  // oldest sample = look samples ago
            for (int c = 0; c < nc; ++c)
            {
                float y = delay[static_cast<size_t> (c)][static_cast<size_t> (rpos)] * g;
                ch[c][i] = std::clamp (y, -ceiling, ceiling);  // safety: never above the ceiling
            }
            minG = std::min (minG, g);
            wpos = rpos;
        }
        meter.store (-gainToDb (minG), std::memory_order_relaxed);
    }

    std::array<std::vector<float>, kMaxCh> delay;
    std::vector<float> need;
    int look = 72, wpos = 0;
    float inGain = 1, ceiling = 0.89f, releaseCoef = 0, attackCoef = 0, g = 1;
};
} // namespace

std::unique_ptr<Processor> create (Kind k)
{
    switch (k)
    {
        case Kind::Gain:       return std::make_unique<GainProc>();
        case Kind::Gate:       return std::make_unique<GateProc>();
        case Kind::EQ:         return std::make_unique<EqProc>();
        case Kind::DeEsser:    return std::make_unique<DeEsserProc>();
        case Kind::Compressor: return std::make_unique<CompressorProc>();
        case Kind::Saturation: return std::make_unique<SaturationProc>();
        case Kind::Enveloper:  return std::make_unique<EnveloperProc>();
        case Kind::Reverb:     return std::make_unique<ReverbProc>();
        case Kind::Limiter:    return std::make_unique<LimiterProc>();
    }
    return nullptr;
}

} // namespace smix::dsp
