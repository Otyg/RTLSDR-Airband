#include "AudioEngine.h"

#include <algorithm>
#include <cmath>
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
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHighPassCutoffHz = 250.0f;
constexpr float kLowPassCutoffHz = 2900.0f;
constexpr float kPresenceCenterHz = 2000.0f;
constexpr float kPresenceQ = 2.0f;
constexpr int kFilterStages = 4;
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
      restartPending_(false),
      highPassFilterEnabled_(false),
      lowPassFilterEnabled_(false),
      presenceBoostDb_(0.0f),
      highPassAlpha_(0.0f),
      lowPassAlpha_(0.0f),
      presenceB_{1.0f, 0.0f, 0.0f},
      presenceA_{1.0f, 0.0f, 0.0f},
      highPassPrevInput_{},
      highPassPrevOutput_{},
      lowPassPrevOutput_{},
      presencePrevInput1_(0.0f),
      presencePrevInput2_(0.0f),
      presencePrevOutput1_(0.0f),
      presencePrevOutput2_(0.0f) {
    float const sampleInterval = 1.0f / static_cast<float>(kSampleRateHz);
    float const highPassRc = 1.0f / (2.0f * kPi * kHighPassCutoffHz);
    float const lowPassRc = 1.0f / (2.0f * kPi * kLowPassCutoffHz);
    highPassAlpha_ = highPassRc / (highPassRc + sampleInterval);
    lowPassAlpha_ = sampleInterval / (lowPassRc + sampleInterval);
    updatePresenceBoostCoefficients();
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
    resetFilterState();
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
    resetFilterState();
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

void AudioEngine::setHighPassFilterEnabled(bool enabled) {
    highPassFilterEnabled_ = enabled;
    resetFilterState();
}

void AudioEngine::setLowPassFilterEnabled(bool enabled) {
    lowPassFilterEnabled_ = enabled;
    resetFilterState();
}

void AudioEngine::setPresenceBoostDb(float gainDb) {
    float const clampedGain = std::clamp(gainDb, 0.0f, 4.0f);
    if (std::fabs(presenceBoostDb_ - clampedGain) < 0.001f) {
        return;
    }

    presenceBoostDb_ = clampedGain;
    updatePresenceBoostCoefficients();
    resetFilterState();
}

void AudioEngine::setOutputDeviceId(QByteArray const& deviceId) {
    if (outputDeviceId_ == deviceId) {
        return;
    }

    outputDeviceId_ = deviceId;
    if (!running_) {
        return;
    }

    teardownSink();
    if (!createSink()) {
        emit errorMessage("Failed to switch audio output device");
        return;
    }
    flushPendingPcm();
    emit statusMessage(QString("Audio: %1").arg(stateToText()));
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

    applyFilters(out, sampleCount);

    emit pcmChunk(pcm);
    if (!playbackActive_) {
        // Replay audio can arrive without matching metadata squelch-open events.
        // In that case, play received audio instead of hard-muting it.
        appendPendingPcm(pcm);
        flushPendingPcm();
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
    QAudioDevice output = resolveOutputDevice();
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

QAudioDevice AudioEngine::resolveOutputDevice() const {
    QList<QAudioDevice> const outputs = QMediaDevices::audioOutputs();
    if (!outputDeviceId_.isEmpty()) {
        for (QAudioDevice const& device : outputs) {
            if (device.id() == outputDeviceId_) {
                return device;
            }
        }
    }

    return QMediaDevices::defaultAudioOutput();
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

void AudioEngine::updatePresenceBoostCoefficients() {
    if (presenceBoostDb_ <= 0.0f) {
        presenceB_ = {1.0f, 0.0f, 0.0f};
        presenceA_ = {1.0f, 0.0f, 0.0f};
        return;
    }

    float const a = std::pow(10.0f, presenceBoostDb_ / 40.0f);
    float const omega = 2.0f * kPi * kPresenceCenterHz / static_cast<float>(kSampleRateHz);
    float const alpha = std::sin(omega) / (2.0f * kPresenceQ);
    float const cosOmega = std::cos(omega);

    float const b0 = 1.0f + alpha * a;
    float const b1 = -2.0f * cosOmega;
    float const b2 = 1.0f - alpha * a;
    float const a0 = 1.0f + alpha / a;
    float const a1 = -2.0f * cosOmega;
    float const a2 = 1.0f - alpha / a;

    presenceB_ = {b0 / a0, b1 / a0, b2 / a0};
    presenceA_ = {1.0f, a1 / a0, a2 / a0};
}

void AudioEngine::resetFilterState() {
    highPassPrevInput_.fill(0.0f);
    highPassPrevOutput_.fill(0.0f);
    lowPassPrevOutput_.fill(0.0f);
    presencePrevInput1_ = 0.0f;
    presencePrevInput2_ = 0.0f;
    presencePrevOutput1_ = 0.0f;
    presencePrevOutput2_ = 0.0f;
}

void AudioEngine::applyFilters(int16_t* samples, int sampleCount) {
    bool const presenceBoostEnabled = presenceBoostDb_ > 0.0f;
    if ((!highPassFilterEnabled_ && !lowPassFilterEnabled_ && !presenceBoostEnabled) || !samples || sampleCount <= 0) {
        return;
    }

    for (int i = 0; i < sampleCount; ++i) {
        float sample = static_cast<float>(samples[i]) / 32767.0f;

        if (highPassFilterEnabled_) {
            for (int stage = 0; stage < kFilterStages; ++stage) {
                float const filtered =
                    highPassAlpha_ * (highPassPrevOutput_[stage] + sample - highPassPrevInput_[stage]);
                highPassPrevInput_[stage] = sample;
                highPassPrevOutput_[stage] = filtered;
                sample = filtered;
            }
        }

        if (lowPassFilterEnabled_) {
            for (int stage = 0; stage < kFilterStages; ++stage) {
                lowPassPrevOutput_[stage] += lowPassAlpha_ * (sample - lowPassPrevOutput_[stage]);
                sample = lowPassPrevOutput_[stage];
            }
        }

        if (presenceBoostEnabled) {
            float const filtered = presenceB_[0] * sample + presenceB_[1] * presencePrevInput1_ +
                                   presenceB_[2] * presencePrevInput2_ - presenceA_[1] * presencePrevOutput1_ -
                                   presenceA_[2] * presencePrevOutput2_;
            presencePrevInput2_ = presencePrevInput1_;
            presencePrevInput1_ = sample;
            presencePrevOutput2_ = presencePrevOutput1_;
            presencePrevOutput1_ = filtered;
            sample = filtered;
        }

        sample = std::clamp(sample, -1.0f, 1.0f);
        samples[i] = static_cast<int16_t>(sample * 32767.0f);
    }
}
