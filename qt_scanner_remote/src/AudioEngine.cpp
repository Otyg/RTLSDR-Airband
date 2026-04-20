#include "AudioEngine.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>

class AudioEngine::AudioBufferDevice : public QIODevice {
   public:
    explicit AudioBufferDevice(QObject* parent = nullptr) : QIODevice(parent) {}

    void append(QByteArray const& chunk) {
        QMutexLocker lock(&mutex_);
        buffer_.append(chunk);

        static const int maxBufferedBytes = 16000 * 2 * 2;
        if (buffer_.size() > maxBufferedBytes) {
            buffer_.remove(0, buffer_.size() - maxBufferedBytes);
        }
    }

   protected:
    qint64 readData(char* data, qint64 maxSize) override {
        QMutexLocker lock(&mutex_);
        qint64 toCopy = std::min<qint64>(maxSize, buffer_.size());
        if (toCopy > 0) {
            memcpy(data, buffer_.constData(), static_cast<size_t>(toCopy));
            buffer_.remove(0, static_cast<int>(toCopy));
            return toCopy;
        }
        memset(data, 0, static_cast<size_t>(maxSize));
        return maxSize;
    }

    qint64 writeData(char const*, qint64) override {
        return -1;
    }

   private:
    QMutex mutex_;
    QByteArray buffer_;
};

AudioEngine::AudioEngine(QObject* parent) : QObject(parent), sink_(nullptr), device_(nullptr) {}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start() {
    stop();

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

    device_ = new AudioBufferDevice(this);
    device_->open(QIODevice::ReadOnly);

    sink_ = new QAudioSink(output, format, this);
    sink_->setVolume(1.0f);
    sink_->start(device_);
    return true;
}

void AudioEngine::stop() {
    if (sink_) {
        sink_->stop();
        sink_->deleteLater();
        sink_ = nullptr;
    }
    if (device_) {
        device_->close();
        device_->deleteLater();
        device_ = nullptr;
    }
}

void AudioEngine::setVolume(float volume) {
    if (sink_) {
        sink_->setVolume(std::clamp(volume, 0.0f, 1.0f));
    }
}

void AudioEngine::pushFloat32Mono(QByteArray data) {
    if (!device_ || data.isEmpty()) {
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

    device_->append(pcm);
}
