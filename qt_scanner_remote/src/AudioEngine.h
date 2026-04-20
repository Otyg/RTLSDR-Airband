#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <QByteArray>
#include <QObject>
#include <QString>

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
    bool lastWriteFailed_;
    bool restartPending_;
    bool createSink();
    void teardownSink();
    void scheduleSinkRestart(QString const& reason);
    void flushPendingPcm();
    QString stateToText() const;
};

#endif
