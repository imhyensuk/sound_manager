#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace smix
{

/** "약 2분 30초" style duration text. */
std::string formatDuration (double seconds);

/**
    Estimated time remaining for long jobs (plugin profiling, reference analysis, model
    loading, a mixing pass). Starts from a prior per-unit estimate and blends in the measured
    rate as units complete.
*/
class ProgressEstimator
{
public:
    void start (std::string label, double totalUnits, double priorSecondsPerUnit, double nowSeconds);
    void advance (double units, double nowSeconds);
    void setTotal (double totalUnits) { total = totalUnits; }
    void finish (double nowSeconds);

    bool running() const noexcept { return active; }
    double fraction() const noexcept;
    double etaSeconds (double nowSeconds) const;
    double elapsedSeconds (double nowSeconds) const { return active ? nowSeconds - startTime : endTime - startTime; }
    double secondsPerUnit() const noexcept;

    /** "플러그인 분석 12/40 · 약 3분 남음" */
    std::string describe (double nowSeconds) const;
    nlohmann::json toJson (double nowSeconds) const;

private:
    std::string label;
    double total = 0, done = 0;
    double prior = 1.0, measured = 0.0;
    int samples = 0;
    double startTime = 0, lastUnitTime = 0, endTime = 0;
    bool active = false;
};

} // namespace smix
