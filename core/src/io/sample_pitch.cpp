#include "sample_pitch.hpp"

#include <algorithm>
#include <cmath>

namespace midi_composer::io {

namespace {

// The range worth searching. Below 40Hz is longer than most loops; above 4kHz
// is above anything a sampled instrument is recorded at, and searching there
// only invites the detector to lock onto a harmonic.
constexpr double kLowestHz  = 40.0;
constexpr double kHighestHz = 4000.0;

// At least this periodic before an answer is offered at all.
//
// Low on purpose. The two mistakes are not the same size: a loosely measured
// pitch puts an instrument a semitone out, while refusing to answer leaves the
// caller's default in place -- and for a sample recorded two octaves from that
// default, that is what the listener notices. White noise still falls below
// this, which is the case worth refusing.
constexpr double kMinimumConfidence = 0.20;

// A peak this close to the best one, at a shorter lag, is taken instead. The
// octave error is the classic failure of autocorrelation: a signal that repeats
// every N samples also repeats every 2N, and the longer lag often correlates
// marginally better. Preferring the shortest lag that is nearly as good is the
// standard answer, and it is why a threshold rather than a maximum is used.
constexpr double kOctaveMargin = 0.85;

} // namespace

PitchEstimate estimate_pitch(const std::vector<float>& samples, int sample_rate,
                             int from, int to) {
    PitchEstimate estimate;
    if (sample_rate <= 0 || samples.empty()) return estimate;

    const int size = static_cast<int>(samples.size());
    from = std::clamp(from, 0, size);
    if (to < 0 || to > size) to = size;
    if (to <= from) return estimate;

    const int max_lag = std::min(static_cast<int>(sample_rate / kLowestHz), (to - from) / 2);
    const int min_lag = std::max(2, static_cast<int>(sample_rate / kHighestHz));
    if (max_lag <= min_lag) return estimate;

    // One window, twice the longest lag, taken from the end of the region: a
    // loop's later part is the most settled, and for a sample with no loop the
    // decay is still more periodic than the attack.
    const int window = max_lag;
    const int start = std::max(from, to - 2 * window);
    if (to - start < 2 * window) return estimate;

    // Silence has no pitch, and dividing by its energy would find one anyway.
    double energy = 0.0;
    for (int i = start; i < start + window; ++i) {
        energy += static_cast<double>(samples[i]) * samples[i];
    }
    if (energy <= 1e-9) return estimate;

    std::vector<double> correlation(static_cast<size_t>(max_lag) + 1, 0.0);
    double best = 0.0;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        double dot = 0.0;
        double lagged_energy = 0.0;
        for (int i = 0; i < window; ++i) {
            const double a = samples[start + i];
            const double b = samples[start + i + lag];
            dot += a * b;
            lagged_energy += b * b;
        }
        const double denominator = std::sqrt(energy * lagged_energy);
        const double r = denominator > 1e-12 ? dot / denominator : 0.0;
        correlation[static_cast<size_t>(lag)] = r;
        best = std::max(best, r);
    }
    if (best < kMinimumConfidence) return estimate;

    // The shortest lag that is within the margin of the best, and is a local
    // peak rather than the shoulder of one.
    int chosen = 0;
    const double threshold = best * kOctaveMargin;
    for (int lag = min_lag + 1; lag < max_lag; ++lag) {
        const double r = correlation[static_cast<size_t>(lag)];
        if (r < threshold) continue;
        if (r >= correlation[static_cast<size_t>(lag - 1)] &&
            r >= correlation[static_cast<size_t>(lag + 1)]) {
            chosen = lag;
            break;
        }
    }
    if (chosen == 0) return estimate;

    // Parabolic interpolation through the peak and its neighbours. Without it
    // the answer is quantised to whole samples, which at the top of the range
    // is most of a semitone.
    const double y0 = correlation[static_cast<size_t>(chosen - 1)];
    const double y1 = correlation[static_cast<size_t>(chosen)];
    const double y2 = correlation[static_cast<size_t>(chosen + 1)];
    const double denominator = 2.0 * (2.0 * y1 - y0 - y2);
    const double offset = std::abs(denominator) > 1e-12 ? (y2 - y0) / denominator : 0.0;
    const double period = chosen + std::clamp(offset, -0.5, 0.5);
    if (period <= 0.0) return estimate;

    estimate.frequency = sample_rate / period;
    estimate.confidence = std::clamp(y1, 0.0, 1.0);
    estimate.midi_note = 69.0 + 12.0 * std::log2(estimate.frequency / 440.0);
    return estimate;
}

} // namespace midi_composer::io
