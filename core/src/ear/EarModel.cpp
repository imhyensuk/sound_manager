#include "smix/ear/EarModel.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <numeric>

#include "smix/FFT.h"

namespace smix::ear
{
namespace
{
constexpr char kMagic[8] = { 'S', 'M', 'X', 'E', 'A', 'R', '0', '1' };

double hzToMel (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); }
double melToHz (double mel) { return 700.0 * (std::pow (10.0, mel / 2595.0) - 1.0); }
} // namespace

std::vector<float> EarFeatures::extract (const float* x, std::size_t n, double sr)
{
    constexpr int order = 11, size = 1 << order, hop = size / 2;
    std::vector<float> out (kDim, 0.0f);
    if (n < static_cast<std::size_t> (size) || sr <= 0)
        return out;

    // Triangular mel filters 30 Hz .. min(16 kHz, Nyquist).
    const double lo = hzToMel (30.0), hi = hzToMel (std::min (16000.0, sr / 2.0));
    std::vector<double> edges (kMelBands + 2);
    for (int i = 0; i < kMelBands + 2; ++i)
        edges[static_cast<size_t> (i)] = melToHz (lo + (hi - lo) * i / (kMelBands + 1)) * size / sr;

    FFT fft (order);
    std::vector<std::complex<float>> buf (size);
    std::vector<double> sum (kMelBands, 0.0), sumSq (kMelBands, 0.0);
    std::vector<float> prevMag (size / 2, 0.0f);
    double centroid = 0, flatness = 0, rolloff = 0, flux = 0, fluxSq = 0;
    int frames = 0, onsets = 0;
    double fluxMean = 0;

    for (std::size_t start = 0; start + size <= n; start += hop)
    {
        for (int i = 0; i < size; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (6.2831853f * static_cast<float> (i) / (size - 1));
            buf[static_cast<size_t> (i)] = { x[start + static_cast<size_t> (i)] * w, 0.0f };
        }
        fft.perform (buf.data());

        std::vector<double> power (size / 2);
        double total = 0, weighted = 0, logSum = 0, fl = 0;
        for (int b = 1; b < size / 2; ++b)
        {
            const double p = std::norm (buf[static_cast<size_t> (b)]);
            power[static_cast<size_t> (b)] = p;
            total += p;
            weighted += p * b;
            logSum += std::log (p + 1.0e-20);
            const float mag = static_cast<float> (std::sqrt (p));
            fl += std::max (0.0f, mag - prevMag[static_cast<size_t> (b)]);
            prevMag[static_cast<size_t> (b)] = mag;
        }
        if (total < 1.0e-10)
            continue;  // silence says nothing about the instrument

        for (int m = 0; m < kMelBands; ++m)
        {
            const double a = edges[static_cast<size_t> (m)], c = edges[static_cast<size_t> (m + 1)], e = edges[static_cast<size_t> (m + 2)];
            double energy = 0;
            for (int b = std::max (1, static_cast<int> (a)); b <= std::min (size / 2 - 1, static_cast<int> (e) + 1); ++b)
            {
                const double w = b < c ? (b - a) / std::max (1.0e-9, c - a) : (e - b) / std::max (1.0e-9, e - c);
                if (w > 0)
                    energy += w * power[static_cast<size_t> (b)];
            }
            const double l = std::log10 (energy / total + 1.0e-10);  // level-independent spectral shape
            sum[static_cast<size_t> (m)] += l;
            sumSq[static_cast<size_t> (m)] += l * l;
        }

        double acc = 0;
        int r = 1;
        for (; r < size / 2 && acc < 0.85 * total; ++r)
            acc += power[static_cast<size_t> (r)];
        centroid += weighted / total * sr / size;
        flatness += std::exp (logSum / (size / 2 - 1)) / (total / (size / 2 - 1));
        rolloff += r * sr / size;
        if (frames > 2 && fl > 1.6 * fluxMean)
            ++onsets;
        fluxMean += (frames == 0 ? 1.0 : 0.1) * (fl - fluxMean);
        flux += fl;
        fluxSq += fl * fl;
        ++frames;
    }
    if (frames == 0)
        return out;

    for (int m = 0; m < kMelBands; ++m)
    {
        const double mean = sum[static_cast<size_t> (m)] / frames;
        out[static_cast<size_t> (m)] = static_cast<float> (mean);
        out[static_cast<size_t> (kMelBands + m)] = static_cast<float> (std::sqrt (std::max (0.0, sumSq[static_cast<size_t> (m)] / frames - mean * mean)));
    }

    double peak = 0, sq = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        peak = std::max (peak, static_cast<double> (std::abs (x[i])));
        sq += static_cast<double> (x[i]) * x[i];
    }
    const double rms = std::sqrt (sq / static_cast<double> (n));
    const double seconds = static_cast<double> (n) / sr;
    const double fluxMeanAll = flux / frames;
    float* e = out.data() + 2 * kMelBands;
    e[0] = static_cast<float> (std::log10 (centroid / frames + 1.0));
    e[1] = static_cast<float> (flatness / frames);
    e[2] = static_cast<float> (std::log10 (rolloff / frames + 1.0));
    e[3] = static_cast<float> (20.0 * std::log10 (std::max (peak, 1.0e-6) / std::max (rms, 1.0e-9)) / 20.0);  // crest / 20 dB
    e[4] = static_cast<float> (onsets / std::max (0.1, seconds) / 10.0);
    e[5] = static_cast<float> (std::sqrt (std::max (0.0, fluxSq / frames - fluxMeanAll * fluxMeanAll)) / std::max (1.0e-9, fluxMeanAll));
    e[6] = static_cast<float> (sum[0] / frames - sum[kMelBands - 1] / frames);  // low/high balance
    e[7] = static_cast<float> (frames) / static_cast<float> (std::max (1.0, seconds * sr / hop));  // fraction of non-silent frames
    return out;
}

//==============================================================================
EarModel::~EarModel()
{
    unload();
}

void EarModel::unload()
{
    if (runtime != nullptr && weights != mem::kNoBuffer)
        runtime->free (weights);
    weights = mem::kNoBuffer;
    inlineWeights.clear();
    loaded = false;
}

bool EarModel::load (const std::string& path, std::string& error)
{
    unload();
    std::ifstream f (path, std::ios::binary | std::ios::ate);
    if (! f)
    {
        error = "ear model not found: " + path;
        return false;
    }
    const auto fileSize = static_cast<std::uint64_t> (f.tellg());
    f.seekg (0);
    char magic[8];
    std::uint32_t headerLen = 0;
    f.read (magic, 8);
    f.read (reinterpret_cast<char*> (&headerLen), 4);
    if (! f || std::memcmp (magic, kMagic, 8) != 0 || headerLen > (1u << 20))
    {
        error = "not a Sound Manager ear model";
        return false;
    }
    std::string header (headerLen, '\0');
    f.read (header.data(), headerLen);
    const auto j = nlohmann::json::parse (header, nullptr, false);
    if (j.is_discarded())
    {
        error = "broken model header";
        return false;
    }
    labels = j.value ("classes", std::vector<std::string> {});
    displayNames = j.value ("names", std::vector<std::string> {});  // optional (user folder names)
    dims = j.value ("dims", std::vector<int> {});
    roles.clear();
    for (auto& r : j.value ("roles", std::vector<std::string> {}))
        roles.push_back (roleFromString (r).value_or (guessRoleFromTrackName (r)));
    if (dims.size() < 2 || dims.front() != EarFeatures::kDim || dims.back() != static_cast<int> (labels.size()))
    {
        error = "model dimensions do not match the feature extractor";
        return false;
    }
    while (roles.size() < labels.size())
        roles.push_back (guessRoleFromTrackName (labels[roles.size()]));

    std::uint64_t floats = 2ull * static_cast<std::uint64_t> (dims.front());
    for (size_t l = 1; l < dims.size(); ++l)
        floats += static_cast<std::uint64_t> (dims[l]) * static_cast<std::uint64_t> (dims[l - 1] + 1);
    dataOffset = 12 + headerLen;
    bytes = floats * sizeof (float);
    if (dataOffset + bytes > fileSize)
    {
        error = "model file is truncated";
        return false;
    }

    filePath = path;
    if (runtime != nullptr)
        weights = runtime->addFile (path, dataOffset, static_cast<std::size_t> (bytes));
    if (weights == mem::kNoBuffer)
    {
        inlineWeights.resize (static_cast<size_t> (floats));
        f.seekg (static_cast<std::streamoff> (dataOffset));
        f.read (reinterpret_cast<char*> (inlineWeights.data()), static_cast<std::streamsize> (bytes));
    }
    loaded = true;
    return true;
}

std::vector<EarModel::Guess> EarModel::classify (const std::vector<float>& features, std::size_t topK) const
{
    // Mostly silence: there is nothing to recognise (guessing would only mislead the naming question).
    if (static_cast<int> (features.size()) == EarFeatures::kDim && features.back() < 0.4f)
        return {};
    if (! loaded || static_cast<int> (features.size()) != dims.front())
        return heuristicGuess (features);

    mem::Pin pin;
    const float* w = inlineWeights.data();
    if (weights != mem::kNoBuffer && runtime != nullptr)
    {
        pin = runtime->pin (weights);
        if (! pin)
            return heuristicGuess (features);
        w = pin.as<float>();
    }

    const int in = dims.front();
    std::vector<float> x (features);
    for (int i = 0; i < in; ++i)
        x[static_cast<size_t> (i)] = (x[static_cast<size_t> (i)] - w[i]) / std::max (1.0e-6f, w[in + i]);
    const float* p = w + 2 * in;

    for (size_t l = 1; l < dims.size(); ++l)
    {
        const int nIn = dims[l - 1], nOut = dims[l];
        std::vector<float> y (static_cast<size_t> (nOut));
        const float* W = p;
        const float* b = p + static_cast<size_t> (nOut) * static_cast<size_t> (nIn);
        for (int o = 0; o < nOut; ++o)
        {
            double acc = b[o];
            const float* row = W + static_cast<size_t> (o) * static_cast<size_t> (nIn);
            for (int i = 0; i < nIn; ++i)
                acc += static_cast<double> (row[i]) * x[static_cast<size_t> (i)];
            y[static_cast<size_t> (o)] = (l + 1 < dims.size()) ? std::max (0.0f, static_cast<float> (acc)) : static_cast<float> (acc);
        }
        p = b + nOut;
        x.swap (y);
    }

    const float mx = *std::max_element (x.begin(), x.end());
    double z = 0;
    for (auto& v : x) { v = std::exp (v - mx); z += v; }
    std::vector<Guess> out;
    for (size_t c = 0; c < x.size(); ++c)
        out.push_back ({ labels[c], c < displayNames.size() ? displayNames[c] : std::string(), roles[c], static_cast<float> (x[c] / z) });
    std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.probability > b.probability; });
    if (out.size() > topK)
        out.resize (topK);
    return out;
}

bool EarModel::write (const std::string& path, const std::vector<std::string>& classes, const std::vector<std::string>& roleNames,
                      const std::vector<int>& d, const std::vector<float>& mean, const std::vector<float>& stdev,
                      const std::vector<std::vector<float>>& w, const std::vector<std::vector<float>>& b)
{
    std::ofstream f (path, std::ios::binary | std::ios::trunc);
    if (! f)
        return false;
    const std::string header = nlohmann::json { { "classes", classes }, { "roles", roleNames }, { "dims", d } }.dump();
    const auto len = static_cast<std::uint32_t> (header.size());
    f.write (kMagic, 8);
    f.write (reinterpret_cast<const char*> (&len), 4);
    f.write (header.data(), static_cast<std::streamsize> (header.size()));
    auto put = [&f] (const std::vector<float>& v) { f.write (reinterpret_cast<const char*> (v.data()), static_cast<std::streamsize> (v.size() * sizeof (float))); };
    put (mean);
    put (stdev);
    for (size_t l = 0; l < w.size(); ++l)
    {
        put (w[l]);
        put (b[l]);
    }
    return static_cast<bool> (f);
}

std::vector<EarModel::Guess> heuristicGuess (const std::vector<float>& f)
{
    if (static_cast<int> (f.size()) != EarFeatures::kDim)
        return {};
    const float* e = f.data() + 2 * EarFeatures::kMelBands;
    const float centroidHz = std::pow (10.0f, e[0]) - 1.0f;
    const float crest = e[3] * 20.0f, onsetRate = e[4] * 10.0f, flat = e[1];
    std::vector<EarModel::Guess> g;
    auto add = [&g] (const char* label, InstrumentRole r, float p) { g.push_back ({ label, {}, r, p }); };
    if (onsetRate > 1.0f && crest > 12.0f)
    {
        if (centroidHz < 600.0f)       add ("kick", InstrumentRole::Kick, 0.5f);
        else if (centroidHz > 5000.0f) add ("hihat", InstrumentRole::HiHat, 0.5f);
        else                           add ("snare", InstrumentRole::Snare, 0.4f);
    }
    if (centroidHz < 400.0f && crest < 14.0f) add ("bass", InstrumentRole::Bass, 0.4f);
    if (centroidHz > 800.0f && centroidHz < 3500.0f && flat < 0.3f) add ("lead vocal", InstrumentRole::LeadVocal, 0.3f);
    if (flat < 0.2f && crest < 10.0f) add ("pad", InstrumentRole::Pad, 0.25f);
    std::sort (g.begin(), g.end(), [] (auto& a, auto& b) { return a.probability > b.probability; });
    return g;
}

} // namespace smix::ear
