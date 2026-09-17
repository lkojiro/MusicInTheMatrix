#pragma once

#include <vector>

namespace mitm {

// One tick's worth of audio-reactive data, computed once by whichever
// process owns the central AudioCapture/FftProcessor/BeatDetector
// pipeline (the host) and handed to every registered AudioEventSink.
struct AudioFrame {
    std::vector<float> buckets; // FFT magnitudes, config::kBucketCount entries
    // Raw time-domain waveform, downsampled to config::kWaveformPointCount
    // entries, values roughly in [-1, 1], earliest sample first -- for
    // OscilloscopeRenderer. Everything else here is frequency-domain
    // (buckets) or a single scalar (loudness/beatDetected); this is the
    // one field a sink reads if it wants the actual wave shape.
    std::vector<float> waveform;
    float loudness = 0.0f;
    bool beatDetected = false;
};

// A subscriber to the audio pipeline's per-tick output. Deliberately one
// method wide: a sink reads whatever subset of AudioFrame it actually
// needs (a beat-only light ignores buckets; a spectrum renderer uses
// everything) rather than the interface growing a method per subscriber
// need. Adding a new kind of subscriber later -- a UDP broadcast to
// external hardware, say -- means writing a new class that implements
// this interface and owns its own transport/connection bookkeeping, then
// registering an instance alongside the existing ones. The loop that
// calls onFrame() on every registered sink never needs to change.
class AudioEventSink {
public:
    virtual ~AudioEventSink() = default;
    virtual void onFrame(const AudioFrame& frame) = 0;
};

} // namespace mitm
