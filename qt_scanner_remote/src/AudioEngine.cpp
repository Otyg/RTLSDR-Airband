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

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent), sink_(nullptr), outputDevice_(nullptr), flushTimer_(new QTimer(this)), volume_(1.0f), running_(false), lastWriteFailed_(false), restartPending_(false) {
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
    lastWriteFailed_ = false;
    emit statusMessage("Audio: stopped");
}

void AudioEngine::setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (sink_) {
        sink_->setVolume(volume_);
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

    emit pcmChunk(pcm);
    pendingPcm_.append(pcm);
    // Cap queued audio to keep latency bounded (~300 ms).
    static const int maxPendingBytes = 16000 * 2 * 3 / 10;
    if (pendingPcm_.size() > maxPendingBytes) {
        pendingPcm_.remove(0, pendingPcm_.size() - maxPendingBytes);
    }
    flushPendingPcm();
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
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!output.isFormatSupported(format)) {
        emit errorMessage("Audio format 16kHz mono s16 is not supported");
        return false;
    }

    sink_ = new QAudioSink(output, format, this);
    sink_->setBufferSize(16000 * 2 / 10);
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
