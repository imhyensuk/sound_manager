#include "smix/RecipeEngine.h"

#include <cmath>
#include <limits>

namespace smix
{

std::string toString (SoundGoal g)
{
    switch (g)
    {
        case SoundGoal::Tighter:        return "tighter";
        case SoundGoal::Punchier:       return "punchier";
        case SoundGoal::MoreAttack:     return "more_attack";
        case SoundGoal::Warmer:         return "warmer";
        case SoundGoal::Brighter:       return "brighter";
        case SoundGoal::Darker:         return "darker";
        case SoundGoal::LessMuddy:      return "less_muddy";
        case SoundGoal::LessHarsh:      return "less_harsh";
        case SoundGoal::LessBoomy:      return "less_boomy";
        case SoundGoal::MoreBody:       return "more_body";
        case SoundGoal::Thinner:        return "thinner";
        case SoundGoal::MoreAir:        return "more_air";
        case SoundGoal::LessSibilance:  return "less_sibilance";
        case SoundGoal::MoreControlled: return "more_controlled";
        case SoundGoal::MoreDynamic:    return "more_dynamic";
        case SoundGoal::Wider:          return "wider";
        case SoundGoal::Narrower:       return "narrower";
        case SoundGoal::MoreSpace:      return "more_space";
        case SoundGoal::Drier:          return "drier";
        case SoundGoal::Louder:         return "louder";
        case SoundGoal::Quieter:        return "quieter";
        case SoundGoal::Forward:        return "forward";
        case SoundGoal::Back:           return "back";
        case SoundGoal::Smoother:       return "smoother";
    }
    return "unknown";
}

std::string koreanLabel (SoundGoal g)
{
    switch (g)
    {
        case SoundGoal::Tighter:        return "단단하게";
        case SoundGoal::Punchier:       return "펀치감 있게";
        case SoundGoal::MoreAttack:     return "어택/클릭 강조";
        case SoundGoal::Warmer:         return "따뜻하게";
        case SoundGoal::Brighter:       return "밝게";
        case SoundGoal::Darker:         return "어둡게";
        case SoundGoal::LessMuddy:      return "탁함 줄이기";
        case SoundGoal::LessHarsh:      return "쏘는 소리 줄이기";
        case SoundGoal::LessBoomy:      return "부밍 줄이기";
        case SoundGoal::MoreBody:       return "두껍게";
        case SoundGoal::Thinner:        return "가볍게";
        case SoundGoal::MoreAir:        return "공기감 추가";
        case SoundGoal::LessSibilance:  return "치찰음 줄이기";
        case SoundGoal::MoreControlled: return "더 일정하게(압축)";
        case SoundGoal::MoreDynamic:    return "다이내믹 살리기";
        case SoundGoal::Wider:          return "넓게";
        case SoundGoal::Narrower:       return "좁게";
        case SoundGoal::MoreSpace:      return "공간감 추가";
        case SoundGoal::Drier:          return "건조하게";
        case SoundGoal::Louder:         return "크게";
        case SoundGoal::Quieter:        return "작게";
        case SoundGoal::Forward:        return "앞으로";
        case SoundGoal::Back:           return "뒤로";
        case SoundGoal::Smoother:       return "부드럽게";
    }
    return "?";
}

namespace
{
int findSlot (const ChannelState& c, PluginCategory cat)
{
    for (size_t i = 0; i < c.chain.size(); ++i)
        if (c.chain[i].category == cat && ! c.chain[i].bypassed)
            return static_cast<int> (i);
    return -1;
}

const ValueMapper* mapperFor (const SlotState& s, const ParamInfo& p)
{
    auto it = s.mappers.find (p.index);
    return (it != s.mappers.end() && it->second.isUsable()) ? &it->second : nullptr;
}

double lowFocusHz (InstrumentRole r)
{
    switch (r)
    {
        case InstrumentRole::Kick:  return 60.0;
        case InstrumentRole::Bass:  return 80.0;
        case InstrumentRole::Toms:  return 100.0;
        case InstrumentRole::Snare: return 180.0;
        case InstrumentRole::LeadVocal:
        case InstrumentRole::BackingVocal: return 180.0;
        default: return 120.0;
    }
}

double mudHz (InstrumentRole r)
{
    switch (r)
    {
        case InstrumentRole::Kick:  return 350.0;
        case InstrumentRole::Snare: return 450.0;
        case InstrumentRole::Bass:  return 250.0;
        default: return 300.0;
    }
}

double attackHz (InstrumentRole r)
{
    switch (r)
    {
        case InstrumentRole::Kick:  return 4000.0;
        case InstrumentRole::Snare: return 5000.0;
        case InstrumentRole::Bass:  return 1500.0;
        default: return 3500.0;
    }
}

std::string fmt (double v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%+.1f", v);
    return buf;
}
} // namespace

void RecipeEngine::gainNudge (const ChannelState& c, double deltaDb, Result& r, const std::string& reason) const
{
    if (c.gainLocked)
    {
        r.notes.push_back ("level of '" + c.name + "' is locked by the user");
        return;
    }
    MixAction a;
    a.type = ActionType::NudgeGain;
    a.channelId = c.id;
    a.delta = deltaDb;
    a.unit = "dB";
    a.reason = reason;
    r.actions.push_back (std::move (a));
}

void RecipeEngine::paramNudge (const ChannelState& c, PluginCategory cat, ParamRole role, double delta, const std::string& unit,
                               Result& r, const std::string& reason) const
{
    const int slot = findSlot (c, cat);
    if (slot < 0)
    {
        r.notes.push_back ("no active " + toString (cat) + " on '" + c.name + "' (needed for: " + reason + ")");
        return;
    }
    const auto& s = c.chain[static_cast<size_t> (slot)];
    const auto* p = s.findByRole (role);
    if (p == nullptr)
    {
        r.notes.push_back (s.pluginName + " has no recognisable '" + toString (role) + "' parameter");
        return;
    }

    MixAction a;
    a.type = ActionType::NudgeParam;
    a.channelId = c.id;
    a.slot = slot;
    a.paramIndex = p->index;
    a.param = p->name;
    a.reason = reason;

    if (unit.empty() || mapperFor (s, *p) != nullptr)
    {
        a.delta = delta;
        a.unit = unit;
    }
    else
    {
        // No unit map: fall back to a conservative normalised move in the same direction.
        a.delta = delta > 0 ? 0.05 : -0.05;
    }
    r.actions.push_back (std::move (a));
}

void RecipeEngine::paramSet (const ChannelState& c, int slot, ParamRole role, double value, const std::string& unit, Result& r,
                             const std::string& reason) const
{
    const auto& s = c.chain[static_cast<size_t> (slot)];
    const auto* p = s.findByRole (role);
    if (p == nullptr || (! unit.empty() && mapperFor (s, *p) == nullptr))
        return;

    MixAction a;
    a.type = ActionType::SetParam;
    a.channelId = c.id;
    a.slot = slot;
    a.paramIndex = p->index;
    a.param = p->name;
    a.value = value;
    a.unit = unit;
    a.reason = reason;
    r.actions.push_back (std::move (a));
}

void RecipeEngine::paramSetNamed (const ChannelState& c, int slot, const std::string& name, double value, const std::string& unit,
                                  Result& r, const std::string& reason) const
{
    const auto& s = c.chain[static_cast<size_t> (slot)];
    for (auto& p : s.params)
    {
        if (toLowerAscii (p.name) != toLowerAscii (name))
            continue;
        if (! unit.empty() && mapperFor (s, p) == nullptr)
            return;
        MixAction a;
        a.type = ActionType::SetParam;
        a.channelId = c.id;
        a.slot = slot;
        a.paramIndex = p.index;
        a.param = p.name;
        a.value = value;
        a.unit = unit;
        a.reason = reason;
        r.actions.push_back (std::move (a));
        return;
    }
}

void RecipeEngine::eqMove (const ChannelState& c, EqMove move, Result& r) const
{
    const int slot = findSlot (c, PluginCategory::EQ);
    const auto reason = "EQ " + fmt (move.gainDb) + " dB @ " + std::to_string (static_cast<int> (move.freqHz)) + " Hz";
    if (slot < 0)
    {
        r.notes.push_back ("no active EQ on '" + c.name + "' (needed for: " + reason + ")");
        return;
    }

    const auto& s = c.chain[static_cast<size_t> (slot)];

    struct Band { int band; const ParamInfo* freq; const ParamInfo* gain; double hz; double db; };
    std::vector<Band> bands;
    for (auto* g : s.findAllByRole (ParamRole::BandGain))
    {
        const auto* f = s.findByRole (ParamRole::BandFreq, g->semantic.band);
        const auto* gm = mapperFor (s, *g);
        if (gm == nullptr)
            continue;
        Band b { g->semantic.band, f, g, 0.0, gm->toReal (g->value).value_or (0.0) };
        if (f != nullptr)
            if (const auto* fm = mapperFor (s, *f))
                b.hz = fm->toReal (f->value).value_or (0.0);
        bands.push_back (b);
    }

    if (bands.empty())
    {
        r.notes.push_back (s.pluginName + ": EQ bands could not be mapped (unknown parameter naming)");
        return;
    }

    // 1) A band already near the frequency (within ~2/3 octave): just move its gain.
    const Band* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();
    auto claimed = [&r, slot] (const Band& b) {
        for (auto& [cs, ci] : r.claimedBands)
            if (cs == slot && ci == b.gain->index)
                return true;
        return false;
    };

    for (auto& b : bands)
    {
        if (b.hz <= 0.0 || claimed (b))
            continue;
        const double d = std::abs (std::log2 (b.hz / move.freqHz));
        if (d < 0.66 && d < bestDistance)
        {
            best = &b;
            bestDistance = d;
        }
    }

    // 2) Otherwise an unused band (gain ~ 0) whose frequency we can set. Prefer inner bands
    //    for bell moves; the outer ones are often shelves or filters.
    bool retune = false;
    if (best == nullptr)
    {
        for (auto& b : bands)
        {
            if (b.freq == nullptr || mapperFor (s, *b.freq) == nullptr || std::abs (b.db) > 0.5 || claimed (b))
                continue;
            const bool outer = b.band == 0 || b.band == static_cast<int> (bands.size()) - 1;
            if (best == nullptr || (! outer && (best->band == 0 || best->band == static_cast<int> (bands.size()) - 1)))
                best = &b;
        }
        retune = best != nullptr;
    }

    if (best == nullptr)
    {
        r.notes.push_back (s.pluginName + ": no free EQ band for " + reason);
        return;
    }

    r.claimedBands.emplace_back (slot, best->gain->index);

    if (retune)
    {
        MixAction f;
        f.type = ActionType::SetParam;
        f.channelId = c.id;
        f.slot = slot;
        f.paramIndex = best->freq->index;
        f.param = best->freq->name;
        f.value = move.freqHz;
        f.unit = "Hz";
        f.reason = reason;
        r.actions.push_back (std::move (f));
    }

    MixAction g;
    g.type = ActionType::NudgeParam;
    g.channelId = c.id;
    g.slot = slot;
    g.paramIndex = best->gain->index;
    g.param = best->gain->name;
    g.delta = move.gainDb;
    g.unit = "dB";
    g.reason = reason;
    r.actions.push_back (std::move (g));
}

RecipeEngine::Result RecipeEngine::actionsFor (const ChannelState& c, const Goal& goal) const
{
    Result r;
    const double k = goal.amount;
    const auto role = c.role;
    const auto why = koreanLabel (goal.goal);
    using C = PluginCategory;
    using P = ParamRole;

    switch (goal.goal)
    {
        case SoundGoal::Tighter:
            eqMove (c, { mudHz (role), -2.5 * k }, r);
            eqMove (c, { lowFocusHz (role), 1.0 * k }, r);
            paramNudge (c, C::Compressor, P::Attack, 4.0 * k, "ms", r, why + ": let the transient through");
            paramNudge (c, C::Compressor, P::Release, -20.0 * k, "ms", r, why + ": faster recovery between hits");
            paramNudge (c, C::TransientShaper, P::TransientSustain, -0.08 * k, "", r, why + ": shorter tail");
            if (role == InstrumentRole::Kick)
                eqMove (c, { attackHz (role), 1.5 * k }, r);
            break;

        case SoundGoal::Punchier:
            paramNudge (c, C::TransientShaper, P::TransientAttack, 0.1 * k, "", r, why);
            paramNudge (c, C::Compressor, P::Attack, 6.0 * k, "ms", r, why + ": slower attack keeps the hit");
            eqMove (c, { attackHz (role), 1.5 * k }, r);
            eqMove (c, { lowFocusHz (role), 1.0 * k }, r);
            break;

        case SoundGoal::MoreAttack:
            paramNudge (c, C::TransientShaper, P::TransientAttack, 0.12 * k, "", r, why);
            eqMove (c, { attackHz (role), 2.0 * k }, r);
            break;

        case SoundGoal::Warmer:
            eqMove (c, { 220.0, 1.5 * k }, r);
            eqMove (c, { 4000.0, -1.5 * k }, r);
            paramNudge (c, C::Saturation, P::Drive, 0.06 * k, "", r, why);
            break;

        case SoundGoal::Brighter:
            eqMove (c, { 10000.0, 2.0 * k }, r);
            eqMove (c, { 4000.0, 1.0 * k }, r);
            break;

        case SoundGoal::Darker:
            eqMove (c, { 8000.0, -2.0 * k }, r);
            eqMove (c, { 3500.0, -1.0 * k }, r);
            break;

        case SoundGoal::LessMuddy:
            eqMove (c, { mudHz (role), -3.0 * k }, r);
            break;

        case SoundGoal::LessHarsh:
            eqMove (c, { 3000.0, -2.5 * k }, r);
            break;

        case SoundGoal::LessBoomy:
            eqMove (c, { lowFocusHz (role), -3.0 * k }, r);
            paramNudge (c, C::EQ, P::LowCutFreq, 15.0 * k, "Hz", r, why);
            break;

        case SoundGoal::MoreBody:
            eqMove (c, { isVocalRole (role) ? 200.0 : lowFocusHz (role) * 1.5, 2.0 * k }, r);
            paramNudge (c, C::Saturation, P::Drive, 0.05 * k, "", r, why);
            break;

        case SoundGoal::Thinner:
            eqMove (c, { isVocalRole (role) ? 200.0 : lowFocusHz (role) * 1.5, -2.0 * k }, r);
            break;

        case SoundGoal::MoreAir:
            eqMove (c, { 12000.0, 2.5 * k }, r);
            break;

        case SoundGoal::LessSibilance:
            if (findSlot (c, C::DeEsser) >= 0)
                paramNudge (c, C::DeEsser, P::Threshold, -3.0 * k, "dB", r, why);
            else
                eqMove (c, { 7000.0, -2.5 * k }, r);
            break;

        case SoundGoal::MoreControlled:
            paramNudge (c, C::Compressor, P::Threshold, -3.0 * k, "dB", r, why);
            paramNudge (c, C::Compressor, P::Ratio, 1.0 * k, "ratio", r, why);
            break;

        case SoundGoal::MoreDynamic:
            paramNudge (c, C::Compressor, P::Threshold, 3.0 * k, "dB", r, why);
            paramNudge (c, C::Compressor, P::Ratio, -1.0 * k, "ratio", r, why);
            break;

        case SoundGoal::Wider:
            paramNudge (c, C::StereoImager, P::Width, 0.08 * k, "", r, why);
            break;

        case SoundGoal::Narrower:
            paramNudge (c, C::StereoImager, P::Width, -0.08 * k, "", r, why);
            break;

        case SoundGoal::MoreSpace:
            if (findSlot (c, C::Reverb) >= 0)
                paramNudge (c, C::Reverb, P::Mix, 0.05 * k, "", r, why);
            else
                paramNudge (c, C::Delay, P::Mix, 0.05 * k, "", r, why);
            break;

        case SoundGoal::Drier:
            if (findSlot (c, C::Reverb) >= 0)
                paramNudge (c, C::Reverb, P::Mix, -0.05 * k, "", r, why);
            if (findSlot (c, C::Delay) >= 0)
                paramNudge (c, C::Delay, P::Mix, -0.05 * k, "", r, why);
            break;

        case SoundGoal::Louder:
            gainNudge (c, 1.5 * k, r, why);
            break;

        case SoundGoal::Quieter:
            gainNudge (c, -1.5 * k, r, why);
            break;

        case SoundGoal::Forward:
            eqMove (c, { 3000.0, 1.5 * k }, r);
            gainNudge (c, 1.0 * k, r, why);
            break;

        case SoundGoal::Back:
            eqMove (c, { 3000.0, -1.5 * k }, r);
            gainNudge (c, -1.0 * k, r, why);
            break;

        case SoundGoal::Smoother:
            eqMove (c, { 3000.0, -1.5 * k }, r);
            paramNudge (c, C::Compressor, P::Attack, -3.0 * k, "ms", r, why);
            break;
    }
    return r;
}

RecipeEngine::Result RecipeEngine::initialSettings (const ChannelState& c, int slotIndex) const
{
    Result r;
    if (slotIndex < 0 || slotIndex >= static_cast<int> (c.chain.size()))
        return r;

    const auto& s = c.chain[static_cast<size_t> (slotIndex)];
    const auto role = c.role;
    const bool bus = c.kind != ChannelKind::Track;
    const auto why = std::string ("initial ") + toString (s.category) + " setup for " + toString (role);

    switch (s.category)
    {
        case PluginCategory::Compressor:
        {
            double ratio = 3.0, attack = 10.0, release = 100.0;
            switch (role)
            {
                case InstrumentRole::Kick:      ratio = 4.0; attack = 20.0; release = 80.0; break;
                case InstrumentRole::Snare:     ratio = 4.0; attack = 10.0; release = 90.0; break;
                case InstrumentRole::Bass:      ratio = 4.0; attack = 15.0; release = 120.0; break;
                case InstrumentRole::LeadVocal: ratio = 3.0; attack = 5.0;  release = 60.0;  break;
                default: break;
            }
            if (bus) { ratio = c.kind == ChannelKind::Master ? 1.5 : 2.0; attack = 30.0; release = 150.0; }

            paramSet (c, slotIndex, ParamRole::Ratio, ratio, "ratio", r, why);
            paramSet (c, slotIndex, ParamRole::Attack, attack, "ms", r, why);
            paramSet (c, slotIndex, ParamRole::Release, release, "ms", r, why);
            if (c.features.valid)
            {
                // Aim for ~3-6 dB of gain reduction on peaks.
                const double threshold = c.features.peakDb - (bus ? 4.0 : 8.0);
                paramSet (c, slotIndex, ParamRole::Threshold, threshold, "dB", r, why);
            }
            break;
        }

        case PluginCategory::EQ:
        {
            double lowCut = 0.0;
            switch (role)
            {
                case InstrumentRole::LeadVocal:
                case InstrumentRole::BackingVocal:   lowCut = 90.0;  break;
                case InstrumentRole::ElectricGuitar:
                case InstrumentRole::AcousticGuitar: lowCut = 80.0;  break;
                case InstrumentRole::HiHat:          lowCut = 250.0; break;
                case InstrumentRole::Overheads:      lowCut = 150.0; break;
                case InstrumentRole::Snare:          lowCut = 70.0;  break;
                case InstrumentRole::Pad:
                case InstrumentRole::Strings:        lowCut = 120.0; break;
                case InstrumentRole::Keys:
                case InstrumentRole::Piano:
                case InstrumentRole::Synth:          lowCut = 60.0;  break;
                case InstrumentRole::Master:
                case InstrumentRole::MixBus:         lowCut = 25.0;  break;
                default: break;
            }
            if (lowCut > 0.0)
                paramSet (c, slotIndex, ParamRole::LowCutFreq, lowCut, "Hz", r, why + ": remove rumble below the instrument");
            break;
        }

        case PluginCategory::Limiter:
            paramSet (c, slotIndex, ParamRole::Ceiling, -1.0, "dB", r, why + ": true-peak safety");
            break;

        case PluginCategory::Gate:
        {
            // Close between hits; the range keeps some room sound instead of hard silence.
            double range = 20.0, release = 120.0, key = 20.0;
            switch (role)
            {
                case InstrumentRole::Toms:  range = 30.0; release = 220.0; key = 60.0; break;
                case InstrumentRole::Kick:  range = 20.0; release = 120.0; break;
                case InstrumentRole::Snare: range = 12.0; release = 100.0; key = 120.0; break;
                default: range = 10.0; release = 150.0; break;
            }
            paramSet (c, slotIndex, ParamRole::Range, range, "dB", r, why);
            paramSet (c, slotIndex, ParamRole::Release, release, "ms", r, why);
            paramSet (c, slotIndex, ParamRole::Attack, 0.5, "ms", r, why);
            paramSetNamed (c, slotIndex, "Key Filter", key, "Hz", r, why + ": ignore low-frequency bleed");
            if (c.features.valid)
                paramSet (c, slotIndex, ParamRole::Threshold, c.features.peakDb - 24.0, "dB", r, why + ": open on hits only");
            break;
        }

        case PluginCategory::DeEsser:
            paramSetNamed (c, slotIndex, "Frequency", role == InstrumentRole::BackingVocal ? 7000.0 : 6500.0, "Hz", r, why);
            paramSet (c, slotIndex, ParamRole::Range, 6.0, "dB", r, why);
            if (c.features.valid)
                paramSet (c, slotIndex, ParamRole::Threshold, c.features.peakDb - 22.0, "dB", r, why);
            break;

        case PluginCategory::Saturation:
        {
            double drive = 3.0;
            if (role == InstrumentRole::Bass) drive = 6.0;
            else if (isDrumRole (role)) drive = 4.0;
            if (c.kind == ChannelKind::Bus) drive = 2.0;
            if (c.kind == ChannelKind::Master) drive = 1.0;
            paramSet (c, slotIndex, ParamRole::Drive, drive, "dB", r, why + ": gentle harmonics");
            break;
        }

        case PluginCategory::TransientShaper:
            if (role == InstrumentRole::Kick || role == InstrumentRole::Snare)
                paramSet (c, slotIndex, ParamRole::TransientAttack, 3.0, "dB", r, why + ": firmer hit");
            if (role == InstrumentRole::Toms || role == InstrumentRole::Overheads)
                paramSet (c, slotIndex, ParamRole::TransientSustain, -3.0, "dB", r, why + ": less ring");
            break;

        case PluginCategory::Reverb:
        {
            double mix = 0.15, decay = 1.5, pre = 20.0;
            if (isVocalRole (role)) { mix = 0.15; decay = 1.8; pre = 30.0; }
            else if (role == InstrumentRole::Snare) { mix = 0.12; decay = 1.2; pre = 10.0; }
            else if (role == InstrumentRole::Pad || role == InstrumentRole::Strings) { mix = 0.2; decay = 2.5; pre = 20.0; }
            paramSet (c, slotIndex, ParamRole::Mix, mix, "", r, why + ": subtle insert level");
            paramSet (c, slotIndex, ParamRole::Decay, decay * 1000.0, "ms", r, why);
            paramSet (c, slotIndex, ParamRole::PreDelay, pre, "ms", r, why);
            break;
        }

        case PluginCategory::Delay:
            paramSet (c, slotIndex, ParamRole::Mix, 0.15, "", r, why + ": subtle insert level");
            break;

        default:
            break;
    }
    return r;
}

} // namespace smix
