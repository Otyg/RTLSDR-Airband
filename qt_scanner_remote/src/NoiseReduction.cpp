#include "NoiseReduction.h"
#include "rnnoise/rnnoise_wrapper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr int kAppSampleRate = 16000;
constexpr int kRnnoiseSampleRate = 48000;
constexpr int kResampleRatio = kRnnoiseSampleRate / kAppSampleRate;

float clampSample(float value) {
    return std::clamp(value, -32768.0f, 32767.0f);
}

void upsample16kTo48k(int16_t const* input, int inputSize, float* output, int outputSize) {
    if (inputSize <= 0 || outputSize <= 0) {
        return;
    }

    for (int i = 0; i < outputSize; ++i) {
        float const sourceIndex = static_cast<float>(i) / static_cast<float>(kResampleRatio);
        int const index0 = std::clamp(static_cast<int>(sourceIndex), 0, inputSize - 1);
        int const index1 = std::min(index0 + 1, inputSize - 1);
        float const t = sourceIndex - static_cast<float>(index0);
        float const sample0 = static_cast<float>(input[index0]);
        float const sample1 = static_cast<float>(input[index1]);
        output[i] = sample0 + (sample1 - sample0) * t;
    }
}

void downsample48kTo16k(float const* input, int16_t const* original, int outputSize, float strength, int16_t* output) {
    for (int i = 0; i < outputSize; ++i) {
        int const base = i * kResampleRatio;
        float processed = 0.0f;
        for (int j = 0; j < kResampleRatio; ++j) {
            processed += input[base + j];
        }
        processed /= static_cast<float>(kResampleRatio);

        float const dry = static_cast<float>(original[i]);
        float const blended = dry + (processed - dry) * strength;
        output[i] = static_cast<int16_t>(clampSample(blended));
    }
}
}

NoiseReduction::NoiseReduction(float suppressionStrength)
    : state_(nullptr, &rnnoise_destroy), suppressionStrength_(std::clamp(suppressionStrength, 0.0f, 1.0f)) {
    initializeModel();
}

NoiseReduction::~NoiseReduction() = default;

bool NoiseReduction::initializeModel() {
    if (state_) {
        return true;  // Already initialized
    }

    DenoiseState* st = rnnoise_create(nullptr);
    if (!st) {
        return false;
    }

    state_.reset(st);
    return true;
}

void NoiseReduction::setSuppressionStrength(float strength) {
    suppressionStrength_ = std::clamp(strength, 0.0f, 1.0f);
}

void NoiseReduction::processFrame(int16_t* audioFrame, int frameSize) {
    if (!isInitialized() || frameSize <= 0) {
        return;
    }

    int const rnnoiseFrameSize = rnnoise_get_frame_size();
    if (rnnoiseFrameSize <= 0 || rnnoiseFrameSize % kResampleRatio != 0) {
        return;
    }

    int const appFrameSize = rnnoiseFrameSize / kResampleRatio;
    std::array<int16_t, 160> inputChunk{};
    std::array<float, 480> upsampled{};
    std::array<float, 480> denoised{};

    int offset = 0;
    while (offset < frameSize) {
        int const samplesThisChunk = std::min(frameSize - offset, appFrameSize);
        std::fill(inputChunk.begin(), inputChunk.end(), 0);
        std::memcpy(inputChunk.data(), audioFrame + offset, samplesThisChunk * static_cast<int>(sizeof(int16_t)));

        upsample16kTo48k(inputChunk.data(), appFrameSize, upsampled.data(), rnnoiseFrameSize);
        rnnoise_process_frame(state_.get(), denoised.data(), upsampled.data());
        downsample48kTo16k(denoised.data(), inputChunk.data(), samplesThisChunk, suppressionStrength_, audioFrame + offset);

        offset += samplesThisChunk;
    }
}
