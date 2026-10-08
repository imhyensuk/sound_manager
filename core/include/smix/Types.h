#pragma once

#include <optional>
#include <string>

namespace smix
{

/** What kind of mixer channel an instance sits on. Determines its mixing scope. */
enum class ChannelKind
{
    Track,  // a single source: only this channel is mixed
    Bus,    // a group/bus: this channel plus every linked child channel is mixed
    Master  // master: everything in the session is mixed
};

/** What a channel carries. Drives chain planning, gain targets and chat targeting. */
enum class InstrumentRole
{
    Unknown,
    Kick, Snare, HiHat, Toms, Overheads, Percussion, DrumBus,
    Bass,
    LeadVocal, BackingVocal,
    AcousticGuitar, ElectricGuitar,
    Piano, Keys, Synth, Pad, Strings, Brass,
    FX,
    MixBus,
    Master
};

/** Functional category of a (third-party) plugin. */
enum class PluginCategory
{
    Unknown,
    EQ, Compressor, Gate, DeEsser, Saturation, TransientShaper,
    Reverb, Delay, Modulation, StereoImager, Limiter, Utility, PitchCorrection
};

std::string toString (ChannelKind);
std::string toString (InstrumentRole);
std::string toString (PluginCategory);

std::optional<ChannelKind>    channelKindFromString (const std::string&);
std::optional<InstrumentRole> roleFromString (const std::string&);
std::optional<PluginCategory> categoryFromString (const std::string&);

bool isDrumRole (InstrumentRole);
bool isVocalRole (InstrumentRole);

/** Guesses the role from a DAW track name ("Kick In", "킥", "Lead Vox", "Master"...). */
InstrumentRole guessRoleFromTrackName (const std::string& trackName);

/** Guesses whether a DAW track name denotes the master or a bus/group. */
ChannelKind guessKindFromTrackName (const std::string& trackName);

/** ASCII lower-casing that leaves UTF-8 multibyte sequences untouched. */
std::string toLowerAscii (std::string);

bool containsAny (const std::string& haystackLower, std::initializer_list<const char*> needles);

} // namespace smix
