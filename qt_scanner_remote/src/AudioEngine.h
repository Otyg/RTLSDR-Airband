#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <QByteArray>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QAudioSink;
class QIODevice;
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

   private:
    class AudioBufferDevice;
    QAudioSink* sink_;
    AudioBufferDevice* device_;
};

#endif
