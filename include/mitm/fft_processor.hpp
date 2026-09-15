#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include <kiss_fftr.h>

namespace mitm {

// Turns a raw audio window into a fixed number of log-spaced magnitude
// buckets suitable for driving a bar-chart style visualizer. Log spacing
// matters here: on a linear frequency axis, a handful of low buckets would
// hold almost all the visible energy in most music and the rest would sit
// flat, since human hearing (and most musical content) is logarithmic in
// pitch.
class FftProcessor {
public:
    // windowSize must be even (kiss_fftr's requirement); powers of two are
    // fastest and simplest to reason about. bucketCount is typically driven
    // by terminal width.
    FftProcessor(int sampleRate, size_t windowSize, size_t bucketCount);
    ~FftProcessor();

    FftProcessor(const FftProcessor&) = delete;
    FftProcessor& operator=(const FftProcessor&) = delete;

    size_t windowSize() const { return windowSize_; }
    size_t bucketCount() const { return bucketCount_; }

    // samples.size() must equal windowSize(). Fills `buckets` with one
    // magnitude value per bucket, roughly normalized to [0, 1] for typical
    // mic/music input levels -- not a hard guarantee, loud input can exceed
    // 1 and callers should clamp when rendering.
    void process(const std::vector<float>& samples, std::vector<float>& buckets);

private:
    int sampleRate_;
    size_t windowSize_;
    size_t bucketCount_;

    kiss_fftr_cfg cfg_ = nullptr;
    std::vector<float> hannWindow_;
    std::vector<float> windowed_;
    std::vector<kiss_fft_cpx> spectrum_; // size windowSize_/2 + 1

    // Precomputed [start, end) FFT-bin ranges for each log-spaced bucket.
    std::vector<std::pair<size_t, size_t>> bucketBinRanges_;
};

} // namespace mitm
