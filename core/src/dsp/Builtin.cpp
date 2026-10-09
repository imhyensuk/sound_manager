#include "smix/dsp/Builtin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace smix::dsp
{
namespace
{
ParamSpec spec (std::string id, std::string name, std::string unit, float mn, float mx, float def, float centre, std::string help)
{
    ParamSpec s;
    s.id = std::move (id);
    s.name = std::move (name);
    s.unit = std::move (unit);
    s.min = mn;
    s.max = mx;
    s.def = def;
    s.centre = centre;
    s.help = std::move (help);
    return s;
}

ParamSpec choice (std::string id, std::string name, std::vector<std::string> options, int def, std::string help)
{
    ParamSpec s = spec (std::move (id), std::move (name), "", 0.0f, static_cast<float> (options.size() - 1), static_cast<float> (def), 0.0f,
                        std::move (help));
    s.choices = std::move (options);
    return s;
}

ParamSpec toggle (std::string id, std::string name, bool def, std::string help)
{
    ParamSpec s = spec (std::move (id), std::move (name), "", 0.0f, 1.0f, def ? 1.0f : 0.0f, 0.0f, std::move (help));
    s.toggle = true;
    return s;
}

std::vector<ParamSpec> makeSpecs (Kind k)
{
    switch (k)
    {
        case Kind::Gain:
            return { spec ("gain", "Gain", "dB", -48.0f, 24.0f, 0.0f, 0.0f, "채널 레벨. 페이더 앞에서 소리 크기를 맞춥니다."),
                     spec ("pan", "Pan", "%", -100.0f, 100.0f, 0.0f, 0.0f, "좌우 위치. -100 왼쪽, +100 오른쪽."),
                     spec ("width", "Stereo Width", "%", 0.0f, 200.0f, 100.0f, 0.0f, "스테레오 폭. 0은 모노, 100은 원본, 200은 두 배로 넓게."),
                     toggle ("invert", "Phase Invert", false, "위상 반전. 여러 마이크(킥 인/아웃, 스네어 탑/바텀)의 위상을 맞출 때 씁니다.") };

        case Kind::Gate:
            return { spec ("threshold", "Threshold", "dB", -90.0f, 0.0f, -50.0f, -30.0f, "이 레벨보다 작으면 게이트가 닫힙니다. 올릴수록 더 많이 잘립니다."),
                     spec ("range", "Range", "dB", 0.0f, 90.0f, 40.0f, 20.0f, "닫혔을 때 줄이는 양. 작게 하면 자연스럽게 줄어듭니다(익스팬더처럼)."),
                     spec ("attack", "Attack", "ms", 0.05f, 50.0f, 0.5f, 2.0f, "열리는 속도. 짧을수록 어택이 살지만 클릭이 날 수 있습니다."),
                     spec ("hold", "Hold", "ms", 0.0f, 500.0f, 40.0f, 60.0f, "열린 뒤 최소 유지 시간."),
                     spec ("release", "Release", "ms", 5.0f, 2000.0f, 150.0f, 150.0f, "닫히는 속도. 길수록 잔향이 자연스럽게 남습니다."),
                     spec ("key", "Key Filter", "Hz", 20.0f, 4000.0f, 20.0f, 200.0f, "감지 신호의 저역을 잘라 다른 악기의 저음(번짐)에 열리지 않게 합니다.") };

        case Kind::EQ:
            return { spec ("lowcut", "Low Cut Freq", "Hz", 20.0f, 1000.0f, 20.0f, 120.0f, "이 주파수 아래를 자릅니다(하이패스). 20 Hz는 꺼짐."),
                     choice ("lowcut_slope", "Low Cut Slope", { "12 dB/oct", "24 dB/oct" }, 0, "로우컷 기울기."),
                     spec ("b1_freq", "Band 1 Freq", "Hz", 20.0f, 1000.0f, 100.0f, 150.0f, "로우 셸프 주파수."),
                     spec ("b1_gain", "Band 1 Gain", "dB", -18.0f, 18.0f, 0.0f, 0.0f, "로우 셸프 게인: 저음 전체를 올리거나 내립니다."),
                     spec ("b2_freq", "Band 2 Freq", "Hz", 20.0f, 20000.0f, 250.0f, 1000.0f, "피크 밴드 2 주파수(저중역)."),
                     spec ("b2_gain", "Band 2 Gain", "dB", -18.0f, 18.0f, 0.0f, 0.0f, "피크 밴드 2 게인."),
                     spec ("b2_q", "Band 2 Q", "", 0.1f, 10.0f, 1.0f, 1.5f, "피크 밴드 2 폭. 클수록 좁습니다."),
                     spec ("b3_freq", "Band 3 Freq", "Hz", 20.0f, 20000.0f, 800.0f, 1000.0f, "피크 밴드 3 주파수(중역)."),
                     spec ("b3_gain", "Band 3 Gain", "dB", -18.0f, 18.0f, 0.0f, 0.0f, "피크 밴드 3 게인."),
                     spec ("b3_q", "Band 3 Q", "", 0.1f, 10.0f, 1.0f, 1.5f, "피크 밴드 3 폭."),
                     spec ("b4_freq", "Band 4 Freq", "Hz", 20.0f, 20000.0f, 2500.0f, 1000.0f, "피크 밴드 4 주파수(중고역)."),
                     spec ("b4_gain", "Band 4 Gain", "dB", -18.0f, 18.0f, 0.0f, 0.0f, "피크 밴드 4 게인."),
                     spec ("b4_q", "Band 4 Q", "", 0.1f, 10.0f, 1.0f, 1.5f, "피크 밴드 4 폭."),
                     spec ("b5_freq", "Band 5 Freq", "Hz", 20.0f, 20000.0f, 6000.0f, 1000.0f, "피크 밴드 5 주파수(고역)."),
                     spec ("b5_gain", "Band 5 Gain", "dB", -18.0f, 18.0f, 0.0f, 0.0f, "피크 밴드 5 게인."),
                     spec ("b5_q", "Band 5 Q", "", 0.1f, 10.0f, 1.0f, 1.5f, "피크 밴드 5 폭."),
                     spec ("b6_freq", "Band 6 Freq", "Hz", 1000.0f, 20000.0f, 10000.0f, 6000.0f, "하이 셸프 주파수."),
                     spec ("b6_gain", "Band 6 Gain", "dB", -18.0f, 18.0f, 0.0f, 0.0f, "하이 셸프 게인: 고음(에어) 전체를 올리거나 내립니다."),
                     spec ("highcut", "High Cut Freq", "Hz", 1000.0f, 22000.0f, 22000.0f, 8000.0f, "이 주파수 위를 자릅니다(로우패스). 22 kHz는 꺼짐."),
                     spec ("output", "Output Gain", "dB", -24.0f, 24.0f, 0.0f, 0.0f, "EQ 뒤 출력 레벨.") };

        case Kind::DeEsser:
            return { spec ("freq", "Frequency", "Hz", 2000.0f, 12000.0f, 6000.0f, 5500.0f, "치찰음을 감지하고 줄일 주파수(이 위쪽 대역)."),
                     spec ("threshold", "Threshold", "dB", -60.0f, 0.0f, -30.0f, -25.0f, "치찰 대역이 이 레벨을 넘으면 줄입니다."),
                     spec ("range", "Range", "dB", 0.0f, 24.0f, 8.0f, 8.0f, "최대로 줄이는 양.") };

        case Kind::Compressor:
            return { spec ("threshold", "Threshold", "dB", -60.0f, 0.0f, -18.0f, -20.0f, "이 레벨을 넘는 소리를 누릅니다."),
                     spec ("ratio", "Ratio", "ratio", 1.0f, 20.0f, 4.0f, 4.0f, "누르는 비율. 2:1은 부드럽게, 8:1 이상은 강하게."),
                     spec ("attack", "Attack", "ms", 0.05f, 200.0f, 10.0f, 10.0f, "누르기 시작하는 속도. 길게 하면 어택(펀치)이 살아납니다."),
                     spec ("release", "Release", "ms", 5.0f, 2000.0f, 120.0f, 150.0f, "다시 놓는 속도. 짧으면 밀도가 커지고, 길면 자연스럽습니다."),
                     spec ("knee", "Knee", "dB", 0.0f, 24.0f, 6.0f, 6.0f, "스레숄드 주변을 부드럽게 넘기는 폭."),
                     spec ("makeup", "Makeup", "dB", -12.0f, 24.0f, 0.0f, 4.0f, "누른 만큼 다시 올리는 게인."),
                     spec ("mix", "Mix", "%", 0.0f, 100.0f, 100.0f, 0.0f, "원음과 섞는 비율(병렬 컴프레션).") };

        case Kind::Saturation:
            return { spec ("drive", "Drive", "dB", 0.0f, 36.0f, 6.0f, 9.0f, "얼마나 세게 밀어 넣을지. 클수록 배음과 밀도가 커집니다."),
                     choice ("type", "Type", { "Tape", "Tube", "Clip" }, 0, "테이프: 부드러운 압축감, 튜브: 짝수 배음의 따뜻함, 클립: 단단하고 거친 질감."),
                     spec ("tone", "Tone", "dB", -12.0f, 12.0f, 0.0f, 0.0f, "새츄레이션 뒤 고역 기울기. 음수는 어둡게, 양수는 밝게."),
                     spec ("mix", "Mix", "%", 0.0f, 100.0f, 100.0f, 0.0f, "원음과 섞는 비율."),
                     spec ("output", "Output Gain", "dB", -24.0f, 12.0f, 0.0f, 0.0f, "출력 레벨.") };

        case Kind::Enveloper:
            return { spec ("attack", "Attack", "dB", -24.0f, 24.0f, 0.0f, 0.0f, "어택(트랜지언트)만 키우거나 줄입니다. 킥·스네어를 단단하게 하려면 올립니다."),
                     spec ("sustain", "Sustain", "dB", -24.0f, 24.0f, 0.0f, 0.0f, "어택 뒤 울림(서스테인)을 키우거나 줄입니다. 룸·잔향을 줄이려면 내립니다."),
                     spec ("output", "Output Gain", "dB", -24.0f, 12.0f, 0.0f, 0.0f, "출력 레벨.") };

        case Kind::Reverb:
            return { spec ("predelay", "Pre-Delay", "ms", 0.0f, 250.0f, 20.0f, 40.0f, "원음과 잔향 사이 간격. 길게 하면 보컬이 앞에 남습니다."),
                     spec ("decay", "Decay", "s", 0.2f, 12.0f, 1.8f, 2.0f, "잔향 길이(RT60)."),
                     spec ("size", "Size", "%", 0.0f, 100.0f, 60.0f, 0.0f, "공간 크기(반사 간격)."),
                     spec ("damping", "Damping", "Hz", 1000.0f, 20000.0f, 7000.0f, 6000.0f, "잔향의 고역이 이 위에서 빨리 사라집니다. 낮으면 어둡고 따뜻합니다."),
                     spec ("lowcut", "Low Cut", "Hz", 20.0f, 1000.0f, 150.0f, 200.0f, "잔향의 저역을 잘라 탁해지지 않게 합니다."),
                     spec ("mix", "Mix", "%", 0.0f, 100.0f, 20.0f, 25.0f, "잔향의 양."),
                     spec ("width", "Width", "%", 0.0f, 100.0f, 100.0f, 0.0f, "잔향의 스테레오 폭.") };

        case Kind::Limiter:
            return { spec ("input", "Input Gain", "dB", 0.0f, 24.0f, 0.0f, 6.0f, "리미터로 밀어 넣는 양. 라우드니스를 올립니다."),
                     spec ("ceiling", "Ceiling", "dB", -12.0f, 0.0f, -1.0f, -3.0f, "출력 최고 레벨."),
                     spec ("release", "Release", "ms", 1.0f, 1000.0f, 80.0f, 80.0f, "리미팅 뒤 회복 속도.") };
    }
    return {};
}
} // namespace

std::string toString (Kind k)
{
    switch (k)
    {
        case Kind::Gain:       return "gain";
        case Kind::Gate:       return "gate";
        case Kind::EQ:         return "eq";
        case Kind::DeEsser:    return "deesser";
        case Kind::Compressor: return "compressor";
        case Kind::Saturation: return "saturation";
        case Kind::Enveloper:  return "enveloper";
        case Kind::Reverb:     return "reverb";
        case Kind::Limiter:    return "limiter";
    }
    return "?";
}

std::string displayName (Kind k)
{
    switch (k)
    {
        case Kind::Gain:       return "SM Gain";
        case Kind::Gate:       return "SM Gate";
        case Kind::EQ:         return "SM EQ";
        case Kind::DeEsser:    return "SM De-Esser";
        case Kind::Compressor: return "SM Compressor";
        case Kind::Saturation: return "SM Saturation";
        case Kind::Enveloper:  return "SM Enveloper";
        case Kind::Reverb:     return "SM Reverb";
        case Kind::Limiter:    return "SM Limiter";
    }
    return "?";
}

std::string koreanName (Kind k)
{
    switch (k)
    {
        case Kind::Gain:       return "게인";
        case Kind::Gate:       return "게이트";
        case Kind::EQ:         return "EQ";
        case Kind::DeEsser:    return "디에서";
        case Kind::Compressor: return "컴프레서";
        case Kind::Saturation: return "새츄레이션";
        case Kind::Enveloper:  return "엔벨로퍼(트랜지언트)";
        case Kind::Reverb:     return "리버브";
        case Kind::Limiter:    return "리미터";
    }
    return "?";
}

std::string description (Kind k)
{
    switch (k)
    {
        case Kind::Gain:       return "레벨, 팬, 스테레오 폭, 위상 반전. 볼륨 밸런스의 기본입니다.";
        case Kind::Gate:       return "작은 소리(마이크 번짐, 잡음, 드럼 사이의 울림)를 줄이거나 막습니다. 탐, 킥, 스네어, 잡음이 있는 보컬에 씁니다.";
        case Kind::EQ:         return "로우컷, 로우/하이 셸프, 피크 4밴드, 하이컷. 탁함·부밍·쏘는 소리를 줄이고 존재감과 공기감을 더합니다.";
        case Kind::DeEsser:    return "보컬의 ㅅ, ㅊ 같은 치찰음이 셀 때 그 대역만 순간적으로 줄입니다.";
        case Kind::Compressor: return "큰 소리를 눌러 다이내믹을 고르게 하고 밀도와 펀치를 만듭니다. 어택이 길면 펀치가 살고, 짧으면 단단하게 붙습니다.";
        case Kind::Saturation: return "배음을 더해 따뜻함, 밀도, 존재감을 줍니다. 테이프는 부드럽게, 튜브는 따뜻하게, 클립은 거칠게.";
        case Kind::Enveloper:  return "트랜지언트 셰이퍼. 어택과 서스테인을 따로 키우거나 줄여 단단함, 펀치, 울림을 조절합니다.";
        case Kind::Reverb:     return "공간감과 깊이를 더합니다. 프리딜레이, 길이, 크기, 댐핑, 로우컷, 양을 조절합니다.";
        case Kind::Limiter:    return "최고 레벨을 막고 라우드니스를 올립니다. 마스터와 버스의 마지막에 씁니다.";
    }
    return {};
}

std::string uidFor (Kind k)
{
    return "SoundManager-smix." + toString (k);
}

std::optional<Kind> kindFromString (const std::string& sIn)
{
    std::string s = toLowerAscii (sIn);
    for (const char* prefix : { "soundmanager-", "smix." })
        if (s.rfind (prefix, 0) == 0)
            s = s.substr (std::string (prefix).size());
    for (int i = 0; i < kNumKinds; ++i)
        if (toString (static_cast<Kind> (i)) == s)
            return static_cast<Kind> (i);
    return std::nullopt;
}

PluginCategory categoryOf (Kind k)
{
    switch (k)
    {
        case Kind::Gain:       return PluginCategory::Utility;
        case Kind::Gate:       return PluginCategory::Gate;
        case Kind::EQ:         return PluginCategory::EQ;
        case Kind::DeEsser:    return PluginCategory::DeEsser;
        case Kind::Compressor: return PluginCategory::Compressor;
        case Kind::Saturation: return PluginCategory::Saturation;
        case Kind::Enveloper:  return PluginCategory::TransientShaper;
        case Kind::Reverb:     return PluginCategory::Reverb;
        case Kind::Limiter:    return PluginCategory::Limiter;
    }
    return PluginCategory::Unknown;
}

const std::vector<ParamSpec>& specsFor (Kind k)
{
    static const std::vector<std::vector<ParamSpec>> all = [] {
        std::vector<std::vector<ParamSpec>> v;
        for (int i = 0; i < kNumKinds; ++i)
            v.push_back (makeSpecs (static_cast<Kind> (i)));
        return v;
    }();
    return all[static_cast<size_t> (k)];
}

namespace
{
// Same skew law as juce::NormalisableRange::setSkewForCentre, so knobs and the AI agree.
double skewOf (const ParamSpec& s)
{
    if (s.centre <= s.min || s.centre >= s.max || ! s.choices.empty() || s.toggle)
        return 1.0;
    return std::log (0.5) / std::log ((s.centre - s.min) / (s.max - s.min));
}
} // namespace

float toNormalised (const ParamSpec& s, float value)
{
    const double prop = std::clamp ((static_cast<double> (value) - s.min) / (s.max - s.min), 0.0, 1.0);
    const double k = skewOf (s);
    return static_cast<float> (k == 1.0 ? prop : std::pow (prop, k));
}

float fromNormalised (const ParamSpec& s, float normalised)
{
    double prop = std::clamp (static_cast<double> (normalised), 0.0, 1.0);
    const double k = skewOf (s);
    if (k != 1.0)
        prop = std::exp (std::log (std::max (prop, 1.0e-12)) / k);
    double v = s.min + (s.max - s.min) * prop;
    if (! s.choices.empty() || s.toggle)
        v = std::round (v);
    return static_cast<float> (v);
}

std::string formatValue (const ParamSpec& s, float v)
{
    char buf[64];
    if (s.toggle)
        return v >= 0.5f ? "On" : "Off";
    if (! s.choices.empty())
        return s.choices[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (v)), 0, static_cast<int> (s.choices.size()) - 1))];
    if (s.unit == "dB")
        std::snprintf (buf, sizeof (buf), "%.1f dB", v);
    else if (s.unit == "Hz")
    {
        if (v >= 1000.0f)
            std::snprintf (buf, sizeof (buf), "%.2f kHz", v / 1000.0f);
        else
            std::snprintf (buf, sizeof (buf), "%.0f Hz", v);
    }
    else if (s.unit == "ms")
    {
        if (v >= 1000.0f)
            std::snprintf (buf, sizeof (buf), "%.2f s", v / 1000.0f);
        else
            std::snprintf (buf, sizeof (buf), v < 10.0f ? "%.2f ms" : "%.1f ms", v);
    }
    else if (s.unit == "s")
        std::snprintf (buf, sizeof (buf), "%.2f s", v);
    else if (s.unit == "%")
        std::snprintf (buf, sizeof (buf), "%.0f %%", v);
    else if (s.unit == "ratio")
        std::snprintf (buf, sizeof (buf), "%.1f:1", v);
    else
        std::snprintf (buf, sizeof (buf), "%.2f", v);
    return buf;
}

//==============================================================================
Processor::Processor (Kind k) : processorKind (k)
{
    const auto& sp = specs();
    values.reset (new std::atomic<float>[sp.size()]);
    cached.resize (sp.size());
    resetToDefaults();
}

int Processor::indexOf (const std::string& id) const
{
    const auto& sp = specs();
    for (size_t i = 0; i < sp.size(); ++i)
        if (sp[i].id == id || sp[i].name == id)
            return static_cast<int> (i);
    return -1;
}

void Processor::set (int index, float value)
{
    if (index < 0 || index >= numParams())
        return;
    const auto& s = specs()[static_cast<size_t> (index)];
    value = std::clamp (value, s.min, s.max);
    if (! s.choices.empty() || s.toggle)
        value = std::round (value);
    values[static_cast<size_t> (index)].store (value, std::memory_order_relaxed);
    dirty.store (true, std::memory_order_release);
}

void Processor::set (const std::string& id, float value)
{
    set (indexOf (id), value);
}

float Processor::get (int index) const
{
    if (index < 0 || index >= numParams())
        return 0.0f;
    return values[static_cast<size_t> (index)].load (std::memory_order_relaxed);
}

float Processor::get (const std::string& id) const
{
    return get (indexOf (id));
}

void Processor::resetToDefaults()
{
    const auto& sp = specs();
    for (size_t i = 0; i < sp.size(); ++i)
        values[i].store (sp[i].def, std::memory_order_relaxed);
    dirty.store (true, std::memory_order_release);
}

void Processor::prepare (double sr, int block, int channels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = std::max (1, block);
    channelCount = std::clamp (channels, 1, 8);
    for (size_t i = 0; i < cached.size(); ++i)
        cached[i] = values[i].load (std::memory_order_relaxed);
    dirty.store (false);
    onPrepare();
    onParams();
    onReset();
}

void Processor::reset()
{
    meter.store (0.0f);
    onReset();
}

void Processor::process (float* const* ch, int numChannels, int numSamples) noexcept
{
    if (dirty.exchange (false, std::memory_order_acq_rel))
    {
        for (size_t i = 0; i < cached.size(); ++i)
            cached[i] = values[i].load (std::memory_order_relaxed);
        onParams();
    }
    numChannels = std::min (numChannels, channelCount);
    if (numChannels > 0 && numSamples > 0)
        onProcess (ch, numChannels, numSamples);
}

void renderOffline (Processor& proc, std::vector<std::vector<float>>& channels, double sampleRate, int blockSize)
{
    if (channels.empty())
        return;
    const int n = static_cast<int> (channels[0].size());
    proc.prepare (sampleRate, blockSize, static_cast<int> (channels.size()));
    std::vector<float*> ptrs (channels.size());
    for (int start = 0; start < n; start += blockSize)
    {
        const int len = std::min (blockSize, n - start);
        for (size_t c = 0; c < channels.size(); ++c)
            ptrs[c] = channels[c].data() + start;
        proc.process (ptrs.data(), static_cast<int> (ptrs.size()), len);
    }
    // Latency compensation, like a DAW's plugin delay compensation.
    if (const int lat = proc.latencySamples(); lat > 0 && lat < n)
        for (auto& c : channels)
        {
            std::move (c.begin() + lat, c.end(), c.begin());
            std::fill (c.end() - lat, c.end(), 0.0f);
        }
}

} // namespace smix::dsp
