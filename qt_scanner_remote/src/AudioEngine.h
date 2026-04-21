#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

#include "NoiseReduction.h"

QT_BEGIN_NAMESPACE
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
    bool createSink();
    void teardownSink();
    void scheduleSinkRestart(QString const& reason);
    void appendPendingPcm(QByteArray const& pcm);
    void flushPendingPcm();
    QString stateToText() const;
};

#endif
