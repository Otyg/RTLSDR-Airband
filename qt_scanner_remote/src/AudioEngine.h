#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <array>
#include <memory>

#include "NoiseReduction.h"

QT_BEGIN_NAMESPACE
class QAudioDevice;
class QAudioSink;
class QIODevice;
class QTimer;
QT_END_NAMESPACE

class AudioEngine : public QObject {
    Q_OBJECT

   public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    bool start();
    void stop();
    void setVolume(float volume);
    void setNoiseReductionEnabled(bool enabled);
    void setNoiseReductionStrength(float strength);
    void setHighPassFilterEnabled(bool enabled);
    void setLowPassFilterEnabled(bool enabled);
    void setPresenceBoostDb(float gainDb);
    void setOutputDeviceId(QByteArray const& deviceId);
    void setPlaybackActive(bool active);

   public slots:
    void pushFloat32Mono(QByteArray data);

   signals:
    void errorMessage(QString message);
    void pcmChunk(QByteArray data);
    void statusMessage(QString message);

   private:
    QAudioSink* sink_;
    QIODevice* outputDevice_;
    QByteArray pendingPcm_;
    QTimer* flushTimer_;
    float volume_;
    bool running_;
    std::unique_ptr<NoiseReduction> noiseReduction_;
    bool noiseReductionEnabled_;
    QByteArray gatedPcm_;
    bool playbackActive_;
    bool playbackUnlocked_;
    bool lastWriteFailed_;
    bool restartPending_;
    QByteArray outputDeviceId_;
    bool highPassFilterEnabled_;
    bool lowPassFilterEnabled_;
    float presenceBoostDb_;
    float highPassAlpha_;
    float lowPassAlpha_;
    std::array<float, 3> presenceB_;
    std::array<float, 3> presenceA_;
    std::array<float, 4> highPassPrevInput_;
    std::array<float, 4> highPassPrevOutput_;
    std::array<float, 4> lowPassPrevOutput_;
    float presencePrevInput1_;
    float presencePrevInput2_;
    float presencePrevOutput1_;
    float presencePrevOutput2_;
    bool createSink();
    void teardownSink();
    void scheduleSinkRestart(QString const& reason);
    void appendPendingPcm(QByteArray const& pcm);
    void flushPendingPcm();
    QString stateToText() const;
    QAudioDevice resolveOutputDevice() const;
    void updatePresenceBoostCoefficients();
    void resetFilterState();
    void applyFilters(int16_t* samples, int sampleCount);
};

#endif
