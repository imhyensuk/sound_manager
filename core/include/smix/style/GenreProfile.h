#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/AudioFeatures.h"
#include "smix/Types.h"
#include "smix/style/ProcessingLearner.h"
#include "smix/style/StyleProfile.h"

namespace smix::style
{

/**
    What a genre's finished mixes sound like, learned from the user's own songs
    (raw multitracks + the final mix of each song):
      - balance: level of each instrument in the mix relative to the lead vocal (LU)
      - curves:  tonal balance of each instrument as it sits in the mix (after the engineer's EQ)
      - master:  the average sound of the finished mixes (used as the default reference)
    When active it replaces the built-in rules of thumb in the perception, the level balancer
    and the default reference style.
*/
struct GenreProfile
{
    std::string name;
    int songs = 0;
    std::map<InstrumentRole, float> balanceLu;
    std::map<InstrumentRole, std::array<float, SpectrumBands::kNumBands>> curves;
    std::map<InstrumentRole, int> samples;  // how many tracks each role was learned from
    StyleProfile master;
    /** How each instrument was processed (gain, gate, EQ, de-esser, compressor, saturation, envelope, reverb). */
    std::map<InstrumentRole, ProcessingSettings> processing;
    bool hasMasterProcessing = false;
    ProcessingSettings masterProcessing;  // premaster -> master, when both were given

    const ProcessingSettings* processingFor (InstrumentRole r) const
    {
        if (r == InstrumentRole::Master && hasMasterProcessing) return &masterProcessing;
        const auto it = processing.find (r);
        return it != processing.end() ? &it->second : nullptr;
    }

    nlohmann::json toJson() const;
    static GenreProfile fromJson (const nlohmann::json&);
};

/** The genre profile in use (process-wide; safe to read from any thread). nullptr = built-in rules. */
void setActiveGenre (std::shared_ptr<const GenreProfile>);
std::shared_ptr<const GenreProfile> activeGenre();

/** Energy of a signal per analysis frame and band (what the learner works with). */
using BandFrames = std::vector<std::array<double, SpectrumBands::kNumBands>>;
BandFrames bandFrames (const float* mono, std::size_t numSamples, double sampleRate, double frameSeconds = 0.4);

struct LearnTrack
{
    std::string name;   // file name
    std::string label;  // user label ("여성 보컬", "Tom2"...)
    InstrumentRole role = InstrumentRole::Unknown;
    BandFrames frames;
};

struct LearnedTrack
{
    std::string name, label;
    InstrumentRole role = InstrumentRole::Unknown;
    std::array<float, SpectrumBands::kNumBands> bandGainDb {};  // how the engineer changed each band (relative)
    std::array<float, SpectrumBands::kNumBands> curveDb {};     // tonal balance in the mix
    float levelLu = 0.0f;   // level in the mix relative to the anchor (lead vocal)
    float mixGainDb = 0.0f; // broadband energy gain from the file to the mix (fader, bus, master)
    bool present = false;   // false: (almost) not used in the mix
    bool hasProcessing = false;  // a processed stem was given: its insert chain was learned
    ProcessingSettings processing;
};

struct LearnedSong
{
    std::string name;
    std::string anchor;                                  // track used as 0 LU
    std::array<float, SpectrumBands::kNumBands> fit {};  // R^2 of the reconstruction per band
    std::vector<LearnedTrack> tracks;
};

/**
    Estimates how each raw track sits in the final mix: per band, mix energy over time is
    explained as a non-negative sum of the tracks' energies (sources are roughly uncorrelated),
    solved by projected-gradient NNLS. EQ shows up as band-dependent gains; reverb returns and
    bus processing that are not in the raw tracks lower the fit (reported).
*/
LearnedSong learnSong (const std::string& name, const std::vector<LearnTrack>& tracks, const BandFrames& mix);

/**
    Session helper tracks that are never part of the mix (click, guide vocal, cue, talkback...).
    They must be left out: a steady click acts like a constant that soaks up modelling errors.
*/
bool isAuxiliaryTrackName (const std::string& fileName);

/** Median over songs/tracks -> genre profile. Roles seen in fewer than `minTracks` tracks are left out. */
GenreProfile combineSongs (const std::string& genre, const std::vector<LearnedSong>&, const std::vector<StyleProfile>& mixes,
                           int minTracks = 2, const std::vector<ProcessingSettings>& masterChains = {});

} // namespace smix::style
