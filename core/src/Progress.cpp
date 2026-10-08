#include "smix/Progress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace smix
{

std::string formatDuration (double seconds)
{
    if (! std::isfinite (seconds) || seconds < 0)
        return "알 수 없음";
    if (seconds < 1.0)
        return "곧 완료";
    const long s = std::lround (seconds);
    char buf[64];
    if (s < 60)
        std::snprintf (buf, sizeof (buf), "약 %ld초", s);
    else if (s < 3600)
        std::snprintf (buf, sizeof (buf), s % 60 == 0 ? "약 %ld분" : "약 %ld분 %ld초", s / 60, s % 60);
    else
        std::snprintf (buf, sizeof (buf), "약 %ld시간 %ld분", s / 3600, (s % 3600) / 60);
    return buf;
}

void ProgressEstimator::start (std::string l, double totalUnits, double priorSecondsPerUnit, double now)
{
    label = std::move (l);
    total = totalUnits;
    done = 0;
    prior = std::max (1.0e-3, priorSecondsPerUnit);
    measured = 0;
    samples = 0;
    startTime = lastUnitTime = now;
    active = true;
}

void ProgressEstimator::advance (double units, double now)
{
    if (! active || units <= 0)
        return;
    const double perUnit = (now - lastUnitTime) / units;
    measured = samples == 0 ? perUnit : 0.7 * measured + 0.3 * perUnit;
    samples += static_cast<int> (std::ceil (units));
    done = std::min (total, done + units);
    lastUnitTime = now;
}

void ProgressEstimator::finish (double now)
{
    done = total;
    endTime = now;
    active = false;
}

double ProgressEstimator::fraction() const noexcept
{
    return total > 0 ? std::clamp (done / total, 0.0, 1.0) : (active ? 0.0 : 1.0);
}

double ProgressEstimator::secondsPerUnit() const noexcept
{
    // The prior counts like three measurements, so a single slow or fast unit does not swing the ETA.
    const double w = static_cast<double> (samples);
    return samples == 0 ? prior : (3.0 * prior + w * measured) / (3.0 + w);
}

double ProgressEstimator::etaSeconds (double now) const
{
    if (! active)
        return 0.0;
    const double remaining = std::max (0.0, total - done);
    const double inFlight = std::min (now - lastUnitTime, secondsPerUnit());  // credit for the unit in progress
    return std::max (0.0, remaining * secondsPerUnit() - inFlight);
}

std::string ProgressEstimator::describe (double now) const
{
    char counts[64];
    std::snprintf (counts, sizeof (counts), " %.0f/%.0f", done, total);
    if (! active)
        return label + counts + " · 완료";
    return label + counts + " · " + formatDuration (etaSeconds (now)) + " 남음";
}

nlohmann::json ProgressEstimator::toJson (double now) const
{
    return { { "label", label }, { "done", done }, { "total", total }, { "fraction", fraction() },
             { "eta_seconds", etaSeconds (now) }, { "eta_text", formatDuration (etaSeconds (now)) },
             { "running", active } };
}

} // namespace smix
