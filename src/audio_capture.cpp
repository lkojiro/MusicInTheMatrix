#include "mitm/audio_capture.hpp"

#include <algorithm>
#include <cctype>

namespace mitm {

namespace {

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

AudioCapture::AudioCapture(int sampleRate, size_t bufferCapacity, std::string preferredDeviceName)
    : sampleRate_(sampleRate),
      preferredDeviceName_(std::move(preferredDeviceName)),
      ringBuffer_(bufferCapacity, 0.0f) {}

AudioCapture::~AudioCapture() {
    stop();
}

int AudioCapture::findDeviceByNameSubstring(const std::string& substr) {
    std::string needle = toLower(substr);
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0 && toLower(info->name).find(needle) != std::string::npos) {
            return i;
        }
    }
    return paNoDevice;
}

std::vector<std::string> AudioCapture::listInputDeviceNames() {
    std::vector<std::string> names;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0) {
            names.push_back(info->name);
        }
    }
    return names;
}

bool AudioCapture::start() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        lastError_ = Pa_GetErrorText(err);
        return false;
    }
    portAudioInitialized_ = true;

    PaDeviceIndex device = paNoDevice;
    if (!preferredDeviceName_.empty()) {
        device = findDeviceByNameSubstring(preferredDeviceName_);
        if (device == paNoDevice) {
            std::string available;
            for (const auto& name : listInputDeviceNames()) {
                if (!available.empty()) available += ", ";
                available += name;
            }
            lastError_ = "No input device matching \"" + preferredDeviceName_ + "\" found. "
                         "If you're trying to visualize system audio, install a loopback driver "
                         "(e.g. `brew install blackhole-2ch`) and route output to it via a "
                         "Multi-Output Device in Audio MIDI Setup -- see the README. "
                         "Available input devices: " + (available.empty() ? "(none)" : available);
            return false;
        }
    } else {
        device = Pa_GetDefaultInputDevice();
        if (device == paNoDevice) {
            lastError_ = "No default input device found";
            return false;
        }
    }

    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(device);
    channelCount_ = std::max(1, std::min(2, deviceInfo->maxInputChannels));

    PaStreamParameters inputParams{};
    inputParams.device = device;
    inputParams.channelCount = channelCount_;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = deviceInfo->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream(&stream_, &inputParams, nullptr /* no output stream */,
                         sampleRate_, paFramesPerBufferUnspecified, paClipOff,
                         &AudioCapture::paCallback, this);
    if (err != paNoError) {
        lastError_ = Pa_GetErrorText(err);
        return false;
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        lastError_ = Pa_GetErrorText(err);
        return false;
    }

    return true;
}

void AudioCapture::stop() {
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    if (portAudioInitialized_) {
        Pa_Terminate();
        portAudioInitialized_ = false;
    }
}

void AudioCapture::readLatest(std::vector<float>& out, size_t count) {
    out.assign(count, 0.0f);

    std::lock_guard<std::mutex> lock(bufferMutex_);
    size_t available = std::min(count, samplesWritten_);
    size_t capacity = ringBuffer_.size();

    // Samples are laid out with the most recent one at index
    // (writePos_ - 1), wrapping backwards for older samples. Walk backwards
    // from writePos_ to pull the most recent `available` samples into the
    // tail of `out`, leaving any leading entries as the zero-fill above.
    for (size_t i = 0; i < available; ++i) {
        size_t idx = (writePos_ + capacity - 1 - i) % capacity;
        out[count - 1 - i] = ringBuffer_[idx];
    }
}

int AudioCapture::paCallback(const void* input, void* /*output*/,
                              unsigned long frameCount,
                              const PaStreamCallbackTimeInfo* /*timeInfo*/,
                              PaStreamCallbackFlags /*statusFlags*/,
                              void* userData) {
    auto* self = static_cast<AudioCapture*>(userData);
    self->onAudio(static_cast<const float*>(input), frameCount);
    return paContinue;
}

void AudioCapture::onAudio(const float* input, unsigned long frameCount) {
    if (!input) return; // can happen on an input underflow

    std::lock_guard<std::mutex> lock(bufferMutex_);
    size_t capacity = ringBuffer_.size();
    for (unsigned long i = 0; i < frameCount; ++i) {
        float sample;
        if (channelCount_ == 1) {
            sample = input[i];
        } else {
            // Interleaved multi-channel frame -> mono by averaging
            // channels. Loopback devices are typically stereo; the FFT
            // pipeline only wants a single signal.
            sample = 0.0f;
            for (int c = 0; c < channelCount_; ++c) {
                sample += input[i * static_cast<unsigned long>(channelCount_) + static_cast<unsigned long>(c)];
            }
            sample /= static_cast<float>(channelCount_);
        }
        ringBuffer_[writePos_] = sample;
        writePos_ = (writePos_ + 1) % capacity;
    }
    samplesWritten_ = std::min(capacity, samplesWritten_ + frameCount);
}

} // namespace mitm
