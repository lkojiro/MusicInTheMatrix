#pragma once

#include <chrono>
#include <cstddef>

namespace mitm::config {

constexpr int kSampleRate = 44100;
constexpr size_t kWindowSize = 2048;  // ~46ms of audio at 44.1kHz
constexpr size_t kBucketCount = 64;   // MVP: fixed bar count, not yet tied to terminal width
// How many points of the raw time-domain waveform get sent per tick, for
// OscilloscopeRenderer. Downsampled from kWindowSize on the host (see
// downsampleWaveform() in host.cpp) before broadcasting, same idea as
// kBucketCount already being far fewer points than the FFT's own
// resolution -- keeps the per-tick packet small and lets the renderer
// just spread these across the terminal width like it does with buckets,
// rather than every subordinate downsampling 2048 raw samples itself.
constexpr size_t kWaveformPointCount = 128;
// ~125fps -- bumped up from an original 60fps target for smoother motion
// in the animated visualizers. Every "per-frame" rate elsewhere in this
// project (peak-hold decay, Matrix rain fall speed/spawn density/flicker,
// BeatDetector's timing windows) is expressed in per-second terms and
// scaled by kFrameDeltaSeconds specifically so this can be retuned here
// without silently speeding up or slowing down everything that depends
// on frame cadence.
constexpr auto kFrameInterval = std::chrono::milliseconds(8);
constexpr float kFrameDeltaSeconds = std::chrono::duration<float>(kFrameInterval).count();

} // namespace mitm::config
