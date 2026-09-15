#include "mitm/beat_detector.hpp"

#include "mitm/visualizer_config.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mitm {

namespace {

// How much real time the adaptive flux threshold looks back over --
// long enough to average across several beats at typical tempos, short
// enough to adapt as a track's energy changes. Converted to a frame
// count below via config::kFrameDeltaSeconds so this stays a ~1-second
// window regardless of the configured frame rate.
constexpr float kFluxHistorySeconds = 1.0f;
constexpr size_t kFluxHistorySize =
    static_cast<size_t>(kFluxHistorySeconds / config::kFrameDeltaSeconds + 0.5f);

// Threshold = mean + kSensitivity * stddev of recent flux. Higher is
// stricter (fewer, more confident beats); lower is more sensitive.
constexpr float kSensitivity = 1.5f;

// Minimum flux magnitude to ever count as a beat, regardless of how low
// the adaptive threshold drops during near-silence -- otherwise ambient
// noise-floor jitter can trigger spurious "beats".
constexpr float kMinFlux = 0.05f;

// Minimum real time between beats (a refractory period), capping the
// fastest detectable beat rate. 150ms is a ceiling around 400 BPM --
// comfortably above any real track's tempo, so this only ever suppresses
// double-triggering on one transient's ringing. Converted to a frame
// count so it stays 150ms regardless of the configured frame rate.
constexpr float kRefractorySeconds = 0.15f;
constexpr int kRefractoryFrames =
    static_cast<int>(kRefractorySeconds / config::kFrameDeltaSeconds + 0.5f);

// The loudness running-max normalizer decays toward this fraction of
// itself per second of quiet, so the visualizer re-adapts if a track
// just gets quieter, rather than permanently pinning "loud" to whatever
// the single loudest moment was. Converted to a per-frame factor in the
// constructor below (via std::pow, so it isn't a compile-time constant).
constexpr float kRunningMaxDecayPerSecond = 0.94f;

} // namespace

BeatDetector::BeatDetector()
    : runningMaxDecayPerFrame_(std::pow(kRunningMaxDecayPerSecond, config::kFrameDeltaSeconds)),
      fluxHistory_(kFluxHistorySize, 0.0f) {}

float BeatDetector::computeLoudness(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;

    double sumSquares = 0.0;
    for (float s : samples) sumSquares += static_cast<double>(s) * s;
    float rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(samples.size())));

    runningMaxRms_ = std::max(rms, runningMaxRms_ * runningMaxDecayPerFrame_);
    return std::clamp(rms / runningMaxRms_, 0.0f, 1.0f);
}

float BeatDetector::computeFlux(const std::vector<float>& buckets) {
    if (previousBuckets_.size() != buckets.size()) {
        // First call, or bucket count changed -- nothing to diff against
        // yet, so treat this frame as flat (flux 0) rather than spiking
        // off comparing against mismatched or default-zero data.
        previousBuckets_ = buckets;
        return 0.0f;
    }

    float flux = 0.0f;
    for (size_t i = 0; i < buckets.size(); ++i) {
        float diff = buckets[i] - previousBuckets_[i];
        if (diff > 0.0f) flux += diff; // only energy increases count toward onsets
    }
    previousBuckets_ = buckets;
    return flux;
}

bool BeatDetector::detectBeat(float flux) {
    float mean = std::accumulate(fluxHistory_.begin(), fluxHistory_.end(), 0.0f) /
                 static_cast<float>(fluxHistory_.size());

    float variance = 0.0f;
    for (float f : fluxHistory_) variance += (f - mean) * (f - mean);
    variance /= static_cast<float>(fluxHistory_.size());
    float stddev = std::sqrt(variance);

    fluxHistory_[fluxHistoryPos_] = flux;
    fluxHistoryPos_ = (fluxHistoryPos_ + 1) % fluxHistory_.size();
    if (fluxHistoryPos_ == 0) fluxHistoryFull_ = true; // wrapped around once

    ++framesSinceLastBeat_;

    if (!fluxHistoryFull_) return false; // still warming up the adaptive threshold

    float threshold = mean + kSensitivity * stddev;
    bool isBeat = flux > threshold && flux > kMinFlux && framesSinceLastBeat_ >= kRefractoryFrames;
    if (isBeat) framesSinceLastBeat_ = 0;
    return isBeat;
}

AudioFeatures BeatDetector::update(const std::vector<float>& samples, const std::vector<float>& buckets) {
    AudioFeatures features;
    features.loudness = computeLoudness(samples);
    float flux = computeFlux(buckets);
    features.beatDetected = detectBeat(flux);
    return features;
}

} // namespace mitm
