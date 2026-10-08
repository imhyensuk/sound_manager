#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include <smix/MixAction.h>
#include <smix/MixSession.h>
#include <smix/PluginCatalog.h>

namespace smix::test
{

/** Builds a fake EQ slot with 4 bands, like most parametric EQs expose. */
inline SlotState makeEq (const std::string& uid = "eq")
{
    SlotState s;
    s.pluginUid = uid;
    s.pluginName = "Test EQ";
    s.category = PluginCategory::EQ;
    int idx = 0;
    const double defaultsHz[] = { 80.0, 400.0, 2500.0, 10000.0 };
    for (int band = 1; band <= 4; ++band)
    {
        for (auto* what : { "Frequency", "Gain", "Q" })
        {
            ParamInfo p;
            p.index = idx++;
            p.name = "Band " + std::to_string (band) + " " + what;
            p.semantic = classifyParameter (p.name, s.category);
            ValueMapper m;
            for (auto n : ValueMapper::probePoints (33))
            {
                char buf[64];
                if (std::string (what) == "Frequency")
                    std::snprintf (buf, sizeof (buf), "%.1f Hz", 20.0 * std::pow (1000.0, n));
                else if (std::string (what) == "Gain")
                    std::snprintf (buf, sizeof (buf), "%.1f dB", -18.0 + 36.0 * n);
                else
                    std::snprintf (buf, sizeof (buf), "%.2f", 0.1 + 9.9 * n);
                m.addSample (n, buf);
            }
            if (std::string (what) == "Frequency")
                p.value = *m.toNormalised (defaultsHz[band - 1], "Hz");
            else if (std::string (what) == "Gain")
                p.value = 0.5f;
            else
                p.value = 0.1f;
            s.mappers[p.index] = m;
            s.params.push_back (p);
        }
    }
    return s;
}

inline SlotState makeCompressor (const std::string& uid = "comp")
{
    SlotState s;
    s.pluginUid = uid;
    s.pluginName = "Test Comp";
    s.category = PluginCategory::Compressor;
    struct Def { const char* name; double lo, hi; const char* unit; float value; };
    const Def defs[] = { { "Threshold", -60, 0, "dB", 0.8f }, { "Ratio", 1, 20, ":1", 0.1f },
                         { "Attack", 0.1, 100, "ms", 0.1f },  { "Release", 10, 1000, "ms", 0.1f },
                         { "Makeup Gain", 0, 24, "dB", 0.0f } };
    int idx = 0;
    for (auto& d : defs)
    {
        ParamInfo p;
        p.index = idx++;
        p.name = d.name;
        p.value = d.value;
        p.semantic = classifyParameter (p.name, s.category);
        ValueMapper m;
        for (auto n : ValueMapper::probePoints (33))
        {
            char buf[64];
            const double v = d.lo + (d.hi - d.lo) * n;
            if (std::string (d.unit) == ":1")
                std::snprintf (buf, sizeof (buf), "%.1f:1", v);
            else
                std::snprintf (buf, sizeof (buf), "%.1f %s", v, d.unit);
            m.addSample (n, buf);
        }
        s.mappers[p.index] = m;
        s.params.push_back (p);
    }
    return s;
}

/** Records what the executor asked the host to do. */
struct FakeController : MixController
{
    int paramCalls = 0, gainCalls = 0, chainCalls = 0;
    float lastValue = -1.0f, lastGain = 0.0f;
    std::vector<std::string> lastChain;

    bool setParameter (const std::string&, int, int, float v) override { ++paramCalls; lastValue = v; return true; }
    bool setGain (const std::string&, float g) override { ++gainCalls; lastGain = g; return true; }
    bool setBypass (const std::string&, int, bool) override { return true; }
    bool setChain (const std::string&, const std::vector<std::string>& uids) override { ++chainCalls; lastChain = uids; return true; }
    bool moveSlot (const std::string&, int, int) override { return true; }
};

inline ChannelState makeChannel (const std::string& id, const std::string& name, InstrumentRole role,
                                 ChannelKind kind = ChannelKind::Track, const std::string& parent = {})
{
    ChannelState c;
    c.id = id;
    c.name = name;
    c.role = role;
    c.kind = kind;
    c.parentId = parent;
    return c;
}

/** master <- drum bus <- (kick, snare); master <- vocal; master <- bass */
inline MixSession makeSession()
{
    MixSession s;
    s.upsert (makeChannel ("m", "Master", InstrumentRole::Master, ChannelKind::Master));
    s.upsert (makeChannel ("db", "Drum Bus", InstrumentRole::DrumBus, ChannelKind::Bus));
    auto kick = makeChannel ("k", "Kick In", InstrumentRole::Kick, ChannelKind::Track, "db");
    kick.chain.push_back (makeEq());
    kick.chain.push_back (makeCompressor());
    s.upsert (kick);
    s.upsert (makeChannel ("s", "Snare Top", InstrumentRole::Snare, ChannelKind::Track, "db"));
    s.upsert (makeChannel ("v", "Lead Vox", InstrumentRole::LeadVocal));
    s.upsert (makeChannel ("b", "Bass DI", InstrumentRole::Bass));
    return s;
}

} // namespace smix::test
