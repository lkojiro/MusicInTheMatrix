#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <portaudio.h>

namespace mitm {

// Captures audio from a PortAudio input device and makes the most recent
// samples available to consumers (e.g. the FFT processor).
//
// This targets system-audio loopback rather than the microphone: macOS has
// no built-in "listen to what's playing" device, so this looks for an
// input device whose name contains `preferredDeviceName` (a case-insensitive
// substring match), which is expected to be a virtual loopback driver such
// as BlackHole (https://github.com/ExistentialAudio/BlackHole) that the
// user has installed and routed system output to via a Multi-Output Device.
// See the README for setup. Pass a substring matching a real microphone
// name instead if mic capture is what you actually want.
//
// If no matching device is found, start() fails loudly rather than quietly
// falling back to the default mic -- silently visualizing the wrong audio
// source would be more confusing than an explicit error.
//
// Internally this keeps a fixed-size circular buffer of mono float samples
// (stereo input is downmixed by averaging channels), written to by
// PortAudio's realtime callback and read from the main thread. A mutex is
// fine here: the copy is a few KB at most and this isn't a low-latency
// audio pipeline, just a visualizer.
class AudioCapture {
public:
    explicit AudioCapture(int sampleRate = 44100, size_t bufferCapacity = 1 << 15,
                           std::string preferredDeviceName = "BlackHole");
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Finds the input device matching preferredDeviceName and starts the
    // PortAudio stream. Returns false (with a message retrievable via
    // lastError(), including the list of available input devices) on
    // failure.
    bool start();

    // Stops and closes the stream. Safe to call even if start() was never
    // called or failed.
    void stop();

    // Copies the most recent `count` samples into `out`, resizing it as
    // needed. If fewer than `count` samples have been captured yet, the
    // remainder is zero-filled (silence) so callers can always assume a
    // full window.
    void readLatest(std::vector<float>& out, size_t count);

    int sampleRate() const { return sampleRate_; }
    const std::string& lastError() const { return lastError_; }

private:
    static int paCallback(const void* input, void* output,
                           unsigned long frameCount,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void* userData);

    void onAudio(const float* input, unsigned long frameCount);

    // Case-insensitive substring search over input-capable devices.
    // Returns paNoDevice if nothing matches. Requires Pa_Initialize() to
    // have already been called.
    static int findDeviceByNameSubstring(const std::string& substr);
    static std::vector<std::string> listInputDeviceNames();

    int sampleRate_;
    std::string preferredDeviceName_;
    int channelCount_ = 1;
    PaStream* stream_ = nullptr;
    bool portAudioInitialized_ = false;
    std::string lastError_;

    std::mutex bufferMutex_;
    std::vector<float> ringBuffer_;
    size_t writePos_ = 0;
    size_t samplesWritten_ = 0; // total ever written, capped at ringBuffer_.size()
};

} // namespace mitm
