#include <doctest/doctest.h>

#include <cmath>

#include <smix/knowledge/KnowledgeBase.h>

using namespace smix;
using namespace smix::knowledge;

namespace
{
/** A one-band peaking EQ with an obscure parameter name, plus a dummy parameter. */
struct FakeEq : ProbeTarget
{
    float gain = 0.5f, freq = 0.5f;
    double z1[2] {}, z2[2] {};
    double sampleRate() const override { return 48000.0; }
    int numParams() const override { return 3; }
    std::string paramName (int i) const override { return i == 0 ? "P1 Amount" : i == 1 ? "P1 Freq" : "GUI Zoom"; }
    std::string paramLabel (int i) const override { return i == 0 ? "dB" : i == 1 ? "Hz" : ""; }
    float paramDefault (int i) const override { return i == 2 ? 0.0f : 0.5f; }
    std::string paramText (int i, float v) const override
    {
        if (i == 0) return std::to_string (static_cast<int> (-15 + 30 * v)) + " dB";
        if (i == 1) return std::to_string (static_cast<int> (100 * std::pow (100.0, v))) + " Hz";
        return "x";
    }
    bool paramIsDiscrete (int) const override { return false; }
    void setParam (int i, float v) override { if (i == 0) gain = v; if (i == 1) freq = v; }
    void reset() override { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    void process (float* l, float* r, int n) override
    {
        const double f = 100 * std::pow (100.0, freq), g = -15 + 30 * gain;
        const double A = std::pow (10.0, g / 40), w = 2 * M_PI * f / 48000.0, alpha = std::sin (w) / 2.0, c = std::cos (w), a0 = 1 + alpha / A;
        const double b0 = (1 + alpha * A) / a0, b1 = -2 * c / a0, b2 = (1 - alpha * A) / a0, a1 = -2 * c / a0, a2 = (1 - alpha / A) / a0;
        float* ch[2] = { l, r };
        for (int k = 0; k < 2; ++k)
            for (int i = 0; i < n; ++i)
            {
                const double x = ch[k][i], y = b0 * x + z1[k];
                z1[k] = b1 * x - a1 * y + z2[k];
                z2[k] = b2 * x - a2 * y;
                ch[k][i] = static_cast<float> (y);
            }
    }
};

/** A simple feed-forward compressor with "Thresh" and "Amt" parameters. */
struct FakeComp : ProbeTarget
{
    float thr = 0.5f, ratio = 0.5f, env = 0;
    double sampleRate() const override { return 48000.0; }
    int numParams() const override { return 2; }
    std::string paramName (int i) const override { return i == 0 ? "Thresh" : "Amt"; }
    std::string paramLabel (int i) const override { return i == 0 ? "dB" : ""; }
    float paramDefault (int) const override { return 0.5f; }
    std::string paramText (int i, float v) const override { return i == 0 ? std::to_string (-40 + 40 * v) + " dB" : std::to_string (1 + 9 * v); }
    bool paramIsDiscrete (int) const override { return false; }
    void setParam (int i, float v) override { (i == 0 ? thr : ratio) = v; }
    void reset() override { env = 0; }
    void process (float* l, float* r, int n) override
    {
        const float t = -40 + 40 * (1 - thr);  // the high setting lowers the threshold: more compression
        const float rt = 1 + 9 * ratio;
        for (int i = 0; i < n; ++i)
        {
            const float level = 20 * std::log10 (std::max (1e-6f, std::max (std::abs (l[i]), std::abs (r[i]))));
            const float over = std::max (0.0f, level - t);
            const float target = over * (1 - 1 / rt);
            env = target > env ? 0.5f * env + 0.5f * target : 0.9995f * env + 0.0005f * target;
            const float g = std::pow (10.0f, -env / 20);
            l[i] *= g;
            r[i] *= g;
        }
    }
};
} // namespace

TEST_CASE ("profiler measures what obscure parameters do and what the plugin really is")
{
    FakeEq eq;
    PluginInfo info { "uid-eq", "Mystery Box", "Vendor", "VST3", "" };
    int lastDone = 0, total = 0;
    const auto p = PluginProfiler().profile (eq, info, {}, [&] (int d, int t, const std::string&) { lastDone = d; total = t; });
    CHECK (lastDone == total);
    CHECK ((p.measuredCategory == PluginCategory::EQ));
    REQUIRE_FALSE (p.effects.empty());
    CHECK (p.effects.front().name == "P1 Amount");  // most audible first
    CHECK (p.effects.front().sensitivity > 0.5f);
    CHECK (p.effects.front().summary.find ("EQ") != std::string::npos);
    for (auto& e : p.effects)
        if (e.name == "GUI Zoom")
            CHECK (e.sensitivity == 0.0f);

    FakeComp comp;
    const auto c = PluginProfiler().profile (comp, { "uid-comp", "Squasher", "Vendor", "VST3", "" });
    CHECK ((c.measuredCategory == PluginCategory::Compressor));
    CHECK (c.document().find ("compressor") != std::string::npos);

    // Remembered in the knowledge base (memopro-backed) and found by Korean questions.
    mem::Runtime rt (8u << 20);
    KnowledgeBase kb (&rt);
    kb.upsert (p);
    kb.upsert (c);
    CHECK (kb.size() == 2);
    auto hits = kb.search ("소리를 압축하는 플러그인", 2);
    REQUIRE_FALSE (hits.empty());
    CHECK (hits[0].uid == "uid-comp");
    hits = kb.search ("eq 대역", 2);
    REQUIRE_FALSE (hits.empty());
    CHECK (hits[0].uid == "uid-eq");
    CHECK (kb.contextFor ("압축", 1).find ("Squasher") != std::string::npos);
    CHECK (kb.isCurrent ("uid-eq"));

    REQUIRE (kb.save ("smix-kb-test.jsonl"));
    KnowledgeBase loaded (&rt);
    REQUIRE (loaded.load ("smix-kb-test.jsonl"));
    CHECK (loaded.size() == 2);
    CHECK (loaded.get ("uid-eq")->effects.size() == p.effects.size());
    std::remove ("smix-kb-test.jsonl");
}

TEST_CASE ("tokenizer handles Korean inflection and English words")
{
    const auto t = KnowledgeBase::tokenize ("압축하는 Compressor-2");
    CHECK (std::find (t.begin(), t.end(), "압축") != t.end());
    CHECK (std::find (t.begin(), t.end(), "compressor") != t.end());
}
