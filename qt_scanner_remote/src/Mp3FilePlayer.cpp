#include "Mp3FilePlayer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kTargetSampleRate = 16000;
constexpr int kPlaybackChunkMs = 30;
constexpr int kPlaybackChunkSamples = (kTargetSampleRate * kPlaybackChunkMs) / 1000;
constexpr int kPlaybackChunkBytes = kPlaybackChunkSamples * static_cast<int>(sizeof(float));

float normalizeSample(QAudioFormat::SampleFormat sampleFormat, char const* sampleData) {
    switch (sampleFormat) {
        case QAudioFormat::UInt8:
            return (static_cast<float>(static_cast<unsigned char>(*sampleData)) - 128.0f) / 128.0f;
        case QAudioFormat::Int16:
            return static_cast<float>(*reinterpret_cast<int16_t const*>(sampleData)) / 32768.0f;
        case QAudioFormat::Int32:
            return static_cast<float>(*reinterpret_cast<int32_t const*>(sampleData)) / 2147483648.0f;
        case QAudioFormat::Float:
            return std::clamp(*reinterpret_cast<float const*>(sampleData), -1.0f, 1.0f);
        case QAudioFormat::Unknown:
            break;
    }
    return 0.0f;
}
}

Mp3FilePlayer::Mp3FilePlayer(QObject* parent)
    : QObject(parent), decoder_(new QAudioDecoder(this)), playbackTimer_(new QTimer(this)), decodedSampleRate_(0), playbackOffsetBytes_(0) {
    playbackTimer_->setInterval(kPlaybackChunkMs);

    connect(decoder_, &QAudioDecoder::bufferReady, this, &Mp3FilePlayer::onBufferReady);
    connect(decoder_, &QAudioDecoder::finished, this, &Mp3FilePlayer::onDecodeFinished);
    connect(decoder_, static_cast<void (QAudioDecoder::*)(QAudioDecoder::Error)>(&QAudioDecoder::error), this,
            [this](QAudioDecoder::Error error) {
        if (error == QAudioDecoder::NoError) {
            return;
        }
        QString message = decoder_->errorString();
        if (message.isEmpty()) {
            message = "MP3 decoding failed";
        }
        stop();
        emit errorMessage(message);
    });
    connect(playbackTimer_, &QTimer::timeout, this, &Mp3FilePlayer::onPlaybackTick);
}

bool Mp3FilePlayer::start(QString const& filePath) {
    if (filePath.isEmpty()) {
        emit errorMessage("No MP3 file selected");
        return false;
    }

    stop();
    resetDecodeState();
    resetPlaybackState();

    QAudioFormat preferredFormat;
    preferredFormat.setSampleRate(kTargetSampleRate);
    preferredFormat.setChannelCount(1);
    preferredFormat.setSampleFormat(QAudioFormat::Float);
    decoder_->setAudioFormat(preferredFormat);
    decoder_->setSource(QUrl::fromLocalFile(filePath));
    decoder_->start();
    return true;
}

void Mp3FilePlayer::stop() {
    if (decoder_->isDecoding()) {
        decoder_->stop();
    }
    resetDecodeState();
    resetPlaybackState();
}

bool Mp3FilePlayer::isActive() const {
    return decoder_->isDecoding() || playbackTimer_->isActive() || !playbackData_.isEmpty();
}

void Mp3FilePlayer::onBufferReady() {
    QAudioBuffer const buffer = decoder_->read();
    if (!buffer.isValid()) {
        return;
    }

    if (decodedSampleRate_ <= 0) {
        decodedSampleRate_ = buffer.format().sampleRate();
    }

    QVector<float> const mono = bufferToMonoFloat(buffer);
    decodedSamples_.reserve(decodedSamples_.size() + mono.size());
    for (float sample : mono) {
        decodedSamples_.push_back(sample);
    }
}

void Mp3FilePlayer::onDecodeFinished() {
    if (decodedSamples_.isEmpty()) {
        emit errorMessage("The selected MP3 file did not contain playable audio");
        stop();
        return;
    }

    startBufferedPlayback();
}

void Mp3FilePlayer::onPlaybackTick() {
    if (playbackOffsetBytes_ >= playbackData_.size()) {
        playbackTimer_->stop();
        playbackData_.clear();
        playbackOffsetBytes_ = 0;
        emit playbackFinished();
        return;
    }

    int const remainingBytes = static_cast<int>(playbackData_.size()) - playbackOffsetBytes_;
    int const chunkBytes = std::min(kPlaybackChunkBytes, remainingBytes);
    emit audioChunk(playbackData_.mid(playbackOffsetBytes_, chunkBytes));
    playbackOffsetBytes_ += chunkBytes;
}

QVector<float> Mp3FilePlayer::bufferToMonoFloat(QAudioBuffer const& buffer) const {
    QVector<float> out;
    QAudioFormat const format = buffer.format();
    if (!buffer.isValid() || format.sampleFormat() == QAudioFormat::Unknown || format.bytesPerFrame() <= 0 || format.channelCount() <= 0) {
        return out;
    }

    int const frameCount = buffer.frameCount();
    int const channelCount = format.channelCount();
    int const bytesPerSample = format.bytesPerFrame() / channelCount;
    char const* raw = buffer.constData<char>();
    out.reserve(frameCount);

    for (int frame = 0; frame < frameCount; ++frame) {
        float sum = 0.0f;
        char const* framePtr = raw + frame * format.bytesPerFrame();
        for (int channel = 0; channel < channelCount; ++channel) {
            sum += normalizeSample(format.sampleFormat(), framePtr + channel * bytesPerSample);
        }
        out.push_back(std::clamp(sum / static_cast<float>(channelCount), -1.0f, 1.0f));
    }

    return out;
}

QVector<float> Mp3FilePlayer::resampleToTargetRate(QVector<float> const& input, int sourceRate, int targetRate) const {
    if (input.isEmpty() || sourceRate <= 0 || targetRate <= 0 || sourceRate == targetRate) {
        return input;
    }

    int const inputSize = static_cast<int>(input.size());
    int const outputSize = std::max(1, static_cast<int>((static_cast<long long>(inputSize) * targetRate) / sourceRate));
    QVector<float> out;
    out.resize(outputSize);

    for (int i = 0; i < outputSize; ++i) {
        double const sourceIndex = (static_cast<double>(i) * sourceRate) / targetRate;
        int const index0 = std::clamp(static_cast<int>(sourceIndex), 0, inputSize - 1);
        int const index1 = std::min(index0 + 1, inputSize - 1);
        float const fraction = static_cast<float>(sourceIndex - index0);
        out[i] = input[index0] + (input[index1] - input[index0]) * fraction;
    }

    return out;
}

void Mp3FilePlayer::resetDecodeState() {
    decodedSamples_.clear();
    decodedSampleRate_ = 0;
}

void Mp3FilePlayer::resetPlaybackState() {
    playbackTimer_->stop();
    playbackData_.clear();
    playbackOffsetBytes_ = 0;
}

void Mp3FilePlayer::startBufferedPlayback() {
    QVector<float> const monoSamples = resampleToTargetRate(decodedSamples_, decodedSampleRate_, kTargetSampleRate);
    playbackData_.resize(monoSamples.size() * static_cast<int>(sizeof(float)));
    if (!monoSamples.isEmpty()) {
        std::memcpy(playbackData_.data(), monoSamples.constData(), playbackData_.size());
    }
    playbackOffsetBytes_ = 0;
    resetDecodeState();
    emit playbackStarted();
    playbackTimer_->start();
}
