#pragma once

#include <cstddef>
#include <vector>

namespace mitm {

// Real-time loudness + beat-pulse detection, driven off the same
// per-frame data the bar visualizer already computes (the raw sample
// window and log-magnitude FFT buckets) -- no separate analysis pass.
//
// There's no dedicated beat-tracking library backing this on purpose: the
// well-established real-time-capable ones (aubio, BTrack) are GPLv3, and
// the broader MIR toolkit (Essentia) is AGPLv3 -- all incompatible with
// keeping this project's licensing permissive. Unlike FFT, though, beat
// detection has no single "correct" answer to get subtly wrong (no
// bit-reversal-style bug class); it's an inherently tunable heuristic, so
// implementing the standard technique ourselves is a comfortable choice
// here, not a compromise. See the README for the fuller writeup.
//
// This intentionally stops at discrete beat pulses rather than a
// continuous BPM number -- turning pulses into a stable tempo estimate is
// meaningfully harder (the classic failure mode is locking onto double or
// half the real tempo) and isn't needed to drive reactive visuals.
struct AudioFeatures {
    float loudness = 0.0f;      // roughly normalized to [0, 1], adaptive to recent volume
    bool beatDetected = false;  // true for exactly one update() call per detected pulse
};

class BeatDetector {
public:
    BeatDetector();

    // Call once per frame with the same raw sample window and FFT
    // magnitude buckets used for rendering.
    AudioFeatures update(const std::vector<float>& samples, const std::vector<float>& buckets);

private:
    float computeLoudness(const std::vector<float>& samples);
    float computeFlux(const std::vector<float>& buckets);
    bool detectBeat(float flux);

    float runningMaxRms_ = 1e-6f; // avoids divide-by-zero before any real audio arrives
    float runningMaxDecayPerFrame_; // derived from config::kFrameDeltaSeconds in the constructor

    std::vector<float> previousBuckets_;

    // Spectral-flux history feeding an adaptive (mean + k*stddev)
    // threshold, stored as a ring buffer.
    std::vector<float> fluxHistory_;
    size_t fluxHistoryPos_ = 0;
    bool fluxHistoryFull_ = false;

    int framesSinceLastBeat_ = 0;
};

} // namespace mitm
