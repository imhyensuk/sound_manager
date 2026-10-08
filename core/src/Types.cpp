#include "smix/Types.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace smix
{
namespace
{
const std::vector<std::pair<ChannelKind, const char*>> kKinds {
    { ChannelKind::Track, "track" }, { ChannelKind::Bus, "bus" }, { ChannelKind::Master, "master" }
};

const std::vector<std::pair<InstrumentRole, const char*>> kRoles {
    { InstrumentRole::Unknown, "unknown" },
    { InstrumentRole::Kick, "kick" }, { InstrumentRole::Snare, "snare" },
    { InstrumentRole::HiHat, "hihat" }, { InstrumentRole::Toms, "toms" },
    { InstrumentRole::Overheads, "overheads" }, { InstrumentRole::Percussion, "percussion" },
    { InstrumentRole::DrumBus, "drum_bus" }, { InstrumentRole::Bass, "bass" },
    { InstrumentRole::LeadVocal, "lead_vocal" }, { InstrumentRole::BackingVocal, "backing_vocal" },
    { InstrumentRole::AcousticGuitar, "acoustic_guitar" }, { InstrumentRole::ElectricGuitar, "electric_guitar" },
    { InstrumentRole::Piano, "piano" }, { InstrumentRole::Keys, "keys" }, { InstrumentRole::Synth, "synth" },
    { InstrumentRole::Pad, "pad" }, { InstrumentRole::Strings, "strings" }, { InstrumentRole::Brass, "brass" },
    { InstrumentRole::FX, "fx" }, { InstrumentRole::MixBus, "mix_bus" }, { InstrumentRole::Master, "master" }
};

const std::vector<std::pair<PluginCategory, const char*>> kCategories {
    { PluginCategory::Unknown, "unknown" }, { PluginCategory::EQ, "eq" },
    { PluginCategory::Compressor, "compressor" }, { PluginCategory::Gate, "gate" },
    { PluginCategory::DeEsser, "deesser" }, { PluginCategory::Saturation, "saturation" },
    { PluginCategory::TransientShaper, "transient_shaper" }, { PluginCategory::Reverb, "reverb" },
    { PluginCategory::Delay, "delay" }, { PluginCategory::Modulation, "modulation" },
    { PluginCategory::StereoImager, "stereo_imager" }, { PluginCategory::Limiter, "limiter" },
    { PluginCategory::Utility, "utility" }, { PluginCategory::PitchCorrection, "pitch_correction" }
};

template <typename E>
std::string lookupName (const std::vector<std::pair<E, const char*>>& table, E value)
{
    for (auto& [e, n] : table)
        if (e == value)
            return n;
    return "unknown";
}

template <typename E>
std::optional<E> lookupValue (const std::vector<std::pair<E, const char*>>& table, const std::string& name)
{
    auto lower = toLowerAscii (name);
    for (auto& [e, n] : table)
        if (lower == n)
            return e;
    return std::nullopt;
}
} // namespace

std::string toString (ChannelKind k)    { return lookupName (kKinds, k); }
std::string toString (InstrumentRole r) { return lookupName (kRoles, r); }
std::string toString (PluginCategory c) { return lookupName (kCategories, c); }

std::optional<ChannelKind>    channelKindFromString (const std::string& s) { return lookupValue (kKinds, s); }
std::optional<InstrumentRole> roleFromString (const std::string& s)        { return lookupValue (kRoles, s); }
std::optional<PluginCategory> categoryFromString (const std::string& s)    { return lookupValue (kCategories, s); }

bool isDrumRole (InstrumentRole r)
{
    switch (r)
    {
        case InstrumentRole::Kick: case InstrumentRole::Snare: case InstrumentRole::HiHat:
        case InstrumentRole::Toms: case InstrumentRole::Overheads: case InstrumentRole::Percussion:
        case InstrumentRole::DrumBus:
            return true;
        default:
            return false;
    }
}

bool isVocalRole (InstrumentRole r)
{
    return r == InstrumentRole::LeadVocal || r == InstrumentRole::BackingVocal;
}

std::string toLowerAscii (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) {
        return (c < 128) ? static_cast<char> (std::tolower (c)) : static_cast<char> (c);
    });
    return s;
}

bool containsAny (const std::string& haystackLower, std::initializer_list<const char*> needles)
{
    for (auto* n : needles)
        if (haystackLower.find (n) != std::string::npos)
            return true;
    return false;
}

InstrumentRole guessRoleFromTrackName (const std::string& trackName)
{
    const auto n = toLowerAscii (trackName);

    if (containsAny (n, { "master", "stereo out", "마스터" }))                    return InstrumentRole::Master;
    if (containsAny (n, { "drum bus", "drums bus", "drum grp", "드럼 버스", "드럼버스" })) return InstrumentRole::DrumBus;
    if (containsAny (n, { "kick", "킥" }))                                   return InstrumentRole::Kick;
    if (containsAny (n, { "snare", "snr", "스네어" }))                        return InstrumentRole::Snare;
    if (containsAny (n, { "hat", "hh", "하이햇" }))                                 return InstrumentRole::HiHat;
    if (containsAny (n, { "tom", "탐" }))                                           return InstrumentRole::Toms;
    if (containsAny (n, { "overhead", "room", "cymbal", "오버헤드" }))         return InstrumentRole::Overheads;
    if (containsAny (n, { "perc", "shaker", "clap", "tamb", "퍼커션" }))            return InstrumentRole::Percussion;
    if (containsAny (n, { "bass", "808", "sub", "베이스" }))                        return InstrumentRole::Bass;
    if (containsAny (n, { "synth", "lead syn", "pluck", "arp", "신스" }))            return InstrumentRole::Synth;
    if (containsAny (n, { "bv", "bgv", "backing", "choir", "harmony", "코러스", "화음" })) return InstrumentRole::BackingVocal;
    if (containsAny (n, { "vox", "vocal", "voc", "lead", "보컬", "목소리" }))        return InstrumentRole::LeadVocal;
    if (containsAny (n, { "acoustic", "ac gtr", "어쿠스틱", "통기타" }))             return InstrumentRole::AcousticGuitar;
    if (containsAny (n, { "gtr", "guitar", "기타" }))                               return InstrumentRole::ElectricGuitar;
    if (containsAny (n, { "piano", "pno", "피아노" }))                              return InstrumentRole::Piano;
    if (containsAny (n, { "pad", "패드" }))                                         return InstrumentRole::Pad;
    if (containsAny (n, { "key", "rhodes", "organ", "epiano", "건반" }))            return InstrumentRole::Keys;
    if (containsAny (n, { "string", "violin", "cello", "스트링", "현악" }))          return InstrumentRole::Strings;
    if (containsAny (n, { "brass", "horn", "trumpet", "sax", "브라스", "관악" }))    return InstrumentRole::Brass;
    if (containsAny (n, { "fx", "sfx", "riser", "impact", "효과" }))                return InstrumentRole::FX;
    if (containsAny (n, { "drum", "드럼" }))                                        return InstrumentRole::DrumBus;
    if (containsAny (n, { "bus", "group", "grp", "버스", "그룹" }))                 return InstrumentRole::MixBus;
    return InstrumentRole::Unknown;
}

ChannelKind guessKindFromTrackName (const std::string& trackName)
{
    const auto n = toLowerAscii (trackName);
    if (containsAny (n, { "master", "stereo out", "마스터" }))
        return ChannelKind::Master;
    if (containsAny (n, { "bus", "group", "grp", "sum", "버스", "그룹" }))
        return ChannelKind::Bus;
    return ChannelKind::Track;
}

} // namespace smix
