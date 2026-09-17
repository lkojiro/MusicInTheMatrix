#include "mitm/fft_processor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mitm {

namespace {

// Precompute log-spaced bin ranges so low buckets (bass) cover a handful of
// bins and high buckets (treble) cover many. Bin 0 (DC offset) is skipped
// since it carries no frequency information.
std::vector<std::pair<size_t, size_t>> computeLogBuckets(size_t numBins, size_t bucketCount) {
    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.reserve(bucketCount);

    double logMin = std::log(1.0);
    double logMax = std::log(static_cast<double>(numBins - 1));

    size_t prevEnd = 1;
    for (size_t b = 0; b < bucketCount; ++b) {
        double frac = static_cast<double>(b + 1) / static_cast<double>(bucketCount);
        double logEdge = logMin + frac * (logMax - logMin);
        size_t edge = static_cast<size_t>(std::round(std::exp(logEdge)));
        size_t end = std::clamp(edge, prevEnd + 1, numBins);
        ranges.emplace_back(prevEnd, end);
        prevEnd = end;
    }
    return ranges;
}

// Most real audio (music especially) carries far more raw energy at low
// frequencies than high -- both because that's how natural/musical
// spectra actually tilt, and because equal-loudness perception falls off
// at the high end -- so raw FFT magnitude alone makes the bass buckets
// read as almost always maxed out while the treble buckets barely
// register anything, regardless of how "loud" that treble content
// actually sounds. This multiplies each bucket by a gain that grows
// exponentially with frequency (1x at the lowest bucket, up to
// kHighFrequencyBoost at the highest) to counteract that tilt, so a
// bucket's displayed level reflects how much it stands out *within its
// own part of the spectrum* rather than being dominated by the bass end
// no matter what.
constexpr float kHighFrequencyBoost = 20.0f;

float highFrequencyGain(size_t bucket, size_t bucketCount) {
    if (bucketCount <= 1) return 1.0f;
    float t = static_cast<float>(bucket) / static_cast<float>(bucketCount - 1); // 0 at lowest, 1 at highest
    return std::pow(kHighFrequencyBoost, t);
}

} // namespace

FftProcessor::FftProcessor(int sampleRate, size_t windowSize, size_t bucketCount)
    : sampleRate_(sampleRate), windowSize_(windowSize), bucketCount_(bucketCount) {
    if (windowSize_ % 2 != 0) {
        throw std::invalid_argument("FftProcessor: windowSize must be even");
    }

    cfg_ = kiss_fftr_alloc(static_cast<int>(windowSize_), 0 /* forward */, nullptr, nullptr);
    if (!cfg_) {
        throw std::runtime_error("FftProcessor: kiss_fftr_alloc failed");
    }

    // Hann window: tapers the edges of each sample block to near zero,
    // which keeps the FFT from treating the sharp cut at each window
    // boundary as a burst of spurious high-frequency content ("spectral
    // leakage").
    hannWindow_.resize(windowSize_);
    for (size_t i = 0; i < windowSize_; ++i) {
        hannWindow_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (windowSize_ - 1)));
    }

    windowed_.resize(windowSize_);
    spectrum_.resize(windowSize_ / 2 + 1);

    bucketBinRanges_ = computeLogBuckets(spectrum_.size(), bucketCount_);
}

FftProcessor::~FftProcessor() {
    if (cfg_) kiss_fftr_free(cfg_);
}

void FftProcessor::process(const std::vector<float>& samples, std::vector<float>& buckets) {
    if (samples.size() != windowSize_) {
        throw std::invalid_argument("FftProcessor::process: samples.size() must equal windowSize()");
    }

    for (size_t i = 0; i < windowSize_; ++i) {
        windowed_[i] = samples[i] * hannWindow_[i];
    }

    kiss_fftr(cfg_, windowed_.data(), spectrum_.data());

    buckets.assign(bucketCount_, 0.0f);
    for (size_t b = 0; b < bucketCount_; ++b) {
        auto [start, end] = bucketBinRanges_[b];
        float sum = 0.0f;
        for (size_t bin = start; bin < end; ++bin) {
            float re = spectrum_[bin].r;
            float im = spectrum_[bin].i;
            sum += std::sqrt(re * re + im * im);
        }
        size_t count = end - start;
        float avgMag = count > 0 ? sum / static_cast<float>(count) : 0.0f;

        // Rough normalization: raw magnitudes scale with windowSize_ and
        // input amplitude. This divisor was picked empirically so typical
        // mic/music input lands roughly in [0, 1] -- tune to taste, or make
        // this adaptive (e.g. running max) once the MVP is working.
        float normalized = avgMag / (static_cast<float>(windowSize_) * 0.05f);

        // See highFrequencyGain()'s own comment -- counteracts low
        // buckets otherwise dominating purely because bass carries more
        // raw energy, not because it's actually "louder" in context.
        buckets[b] = normalized * highFrequencyGain(b, bucketCount_);
    }
}

std::pair<float, float> bucketFrequencyRange(int sampleRate, size_t windowSize, size_t bucketCount,
                                              size_t bucket) {
    size_t numBins = windowSize / 2 + 1;
    std::vector<std::pair<size_t, size_t>> ranges = computeLogBuckets(numBins, bucketCount);
    auto [startBin, endBin] = ranges.at(bucket);
    float hzPerBin = static_cast<float>(sampleRate) / static_cast<float>(windowSize);
    return {static_cast<float>(startBin) * hzPerBin, static_cast<float>(endBin) * hzPerBin};
}

} // namespace mitm
