#include "AudioEngine.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>

namespace {
constexpr int kSampleRateHz = 16000;
constexpr int kBytesPerSample = static_cast<int>(sizeof(int16_t));
constexpr int kMinPlaybackDurationMs = 1000;
constexpr int kMinPlaybackBytes = (kSampleRateHz * kBytesPerSample * kMinPlaybackDurationMs) / 1000;
constexpr int kMaxPendingBytes = kSampleRateHz * kBytesPerSample * 3 / 10;
}

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent),
      sink_(nullptr),
      outputDevice_(nullptr),
      flushTimer_(new QTimer(this)),
      volume_(1.0f),
      running_(false),
      noiseReduction_(std::make_unique<NoiseReduction>(0.7f)),
      noiseReductionEnabled_(true),
      playbackActive_(false),
      playbackUnlocked_(false),
      lastWriteFailed_(false),
      restartPending_(false) {
    flushTimer_->setInterval(15);
    connect(flushTimer_, &QTimer::timeout, this, [this]() { flushPendingPcm(); });
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start() {
    stop();
    running_ = true;
    pendingPcm_.clear();
    gatedPcm_.clear();
    playbackActive_ = false;
    playbackUnlocked_ = false;
    lastWriteFailed_ = false;
    restartPending_ = false;
    if (!createSink()) {
        running_ = false;
        return false;
    }
    flushTimer_->start();
    emit statusMessage(QString("Audio: %1").arg(stateToText()));
    return true;
}

void AudioEngine::stop() {
    running_ = false;
    restartPending_ = false;
    if (sink_) {
        teardownSink();
    }
    if (flushTimer_->isActive()) {
        flushTimer_->stop();
    }
    outputDevice_ = nullptr;
    pendingPcm_.clear();
    gatedPcm_.clear();
    playbackActive_ = false;
    playbackUnlocked_ = false;
    lastWriteFailed_ = false;
    emit statusMessage("Audio: stopped");
}

void AudioEngine::setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (sink_) {
        sink_->setVolume(volume_);
    }
}

void AudioEngine::setNoiseReductionEnabled(bool enabled) {
    noiseReductionEnabled_ = enabled;
}

void AudioEngine::setNoiseReductionStrength(float strength) {
    if (noiseReduction_) {
        noiseReduction_->setSuppressionStrength(strength);
    }
}

void AudioEngine::setPlaybackActive(bool active) {
    if (playbackActive_ == active) {
        return;
    }

    playbackActive_ = active;
    if (!playbackActive_) {
        gatedPcm_.clear();
        pendingPcm_.clear();
        playbackUnlocked_ = false;
    } else {
        gatedPcm_.clear();
        playbackUnlocked_ = false;
    }
}

void AudioEngine::pushFloat32Mono(QByteArray data) {
    if (!running_ || data.isEmpty()) {
        return;
    }
    if (data.size() % static_cast<int>(sizeof(float)) != 0) {
        return;
    }

    int sampleCount = data.size() / static_cast<int>(sizeof(float));

    QByteArray pcm;
    pcm.resize(sampleCount * static_cast<int>(sizeof(int16_t)));

    float const* in = reinterpret_cast<float const*>(data.constData());
    int16_t* out = reinterpret_cast<int16_t*>(pcm.data());
    for (int i = 0; i < sampleCount; ++i) {
        float v = std::clamp(in[i], -1.0f, 1.0f);
        out[i] = static_cast<int16_t>(v * 32767.0f);
    }

    if (noiseReductionEnabled_ && noiseReduction_ && noiseReduction_->isInitialized()) {
        noiseReduction_->processFrame(out, sampleCount);
    }

    emit pcmChunk(pcm);
    if (!playbackActive_) {
        return;
    }

    if (!playbackUnlocked_) {
        gatedPcm_.append(pcm);
        if (gatedPcm_.size() < kMinPlaybackBytes) {
            return;
        }
        playbackUnlocked_ = true;
        appendPendingPcm(gatedPcm_);
        gatedPcm_.clear();
    } else {
        appendPendingPcm(pcm);
    }

    flushPendingPcm();
}

void AudioEngine::appendPendingPcm(QByteArray const& pcm) {
    pendingPcm_.append(pcm);
    if (pendingPcm_.size() > kMaxPendingBytes) {
        pendingPcm_.remove(0, pendingPcm_.size() - kMaxPendingBytes);
    }
}

void AudioEngine::flushPendingPcm() {
    if (!running_ || pendingPcm_.isEmpty()) {
        return;
    }
    if (!sink_ || !outputDevice_) {
        scheduleSinkRestart("sink missing");
        return;
    }

    if (sink_->state() == QAudio::SuspendedState) {
        sink_->resume();
    } else if (sink_->state() == QAudio::StoppedState && sink_->error() != QAudio::NoError) {
        scheduleSinkRestart(stateToText());
        return;
    }

    qint64 written = outputDevice_->write(pendingPcm_.constData(), pendingPcm_.size());
    if (written > 0) {
        pendingPcm_.remove(0, static_cast<int>(written));
        lastWriteFailed_ = false;
    } else if (written < 0) {
        if (!lastWriteFailed_ && running_) {
            emit errorMessage(QString("Audio write failed: %1").arg(stateToText()));
            lastWriteFailed_ = true;
        }
        scheduleSinkRestart("write error");
    }
}

bool AudioEngine::createSink() {
    QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull()) {
        emit errorMessage("No audio output device available");
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kSampleRateHz);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!output.isFormatSupported(format)) {
        emit errorMessage("Audio format 16kHz mono s16 is not supported");
        return false;
    }

    sink_ = new QAudioSink(output, format, this);
    sink_->setBufferSize(kSampleRateHz * kBytesPerSample / 10);
    sink_->setVolume(volume_);
    connect(sink_, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        emit statusMessage(QString("Audio: %1").arg(stateToText()));
        if (!running_) {
            return;
        }
        if (state == QAudio::StoppedState && sink_ && sink_->error() != QAudio::NoError) {
            scheduleSinkRestart(stateToText());
        }
    });

    outputDevice_ = sink_->start();
    if (!outputDevice_) {
        emit errorMessage("Failed to start audio output stream");
        sink_->deleteLater();
        sink_ = nullptr;
        return false;
    }
    return true;
}

void AudioEngine::teardownSink() {
    if (!sink_) {
        outputDevice_ = nullptr;
        return;
    }
    sink_->stop();
    sink_->deleteLater();
    sink_ = nullptr;
    outputDevice_ = nullptr;
}

void AudioEngine::scheduleSinkRestart(QString const& reason) {
    if (!running_ || restartPending_) {
        return;
    }
    restartPending_ = true;
    emit statusMessage(QString("Audio: restarting (%1)").arg(reason));
    QTimer::singleShot(50, this, [this]() {
        restartPending_ = false;
        if (!running_) {
            return;
        }
        teardownSink();
        if (!createSink()) {
            emit errorMessage("Audio restart failed");
            return;
        }
        flushPendingPcm();
    });
}

QString AudioEngine::stateToText() const {
    if (!sink_) {
        return "no sink";
    }

    QString state;
    switch (sink_->state()) {
        case QAudio::ActiveState:
            state = "active";
            break;
        case QAudio::IdleState:
            state = "idle";
            break;
        case QAudio::SuspendedState:
            state = "suspended";
            break;
        case QAudio::StoppedState:
            state = "stopped";
            break;
    }

    QString error;
    switch (sink_->error()) {
        case QAudio::NoError:
            error = "no-error";
            break;
        case QAudio::OpenError:
            error = "open-error";
            break;
        case QAudio::IOError:
            error = "io-error";
            break;
        case QAudio::UnderrunError:
            error = "underrun";
            break;
        case QAudio::FatalError:
            error = "fatal-error";
            break;
    }

    return QString("%1 (%2)").arg(state, error);
}
