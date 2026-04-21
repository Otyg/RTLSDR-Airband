#ifndef MP3_FILE_PLAYER_H
#define MP3_FILE_PLAYER_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAudioBuffer;
class QAudioDecoder;
class QTimer;
QT_END_NAMESPACE

class Mp3FilePlayer : public QObject {
    Q_OBJECT

   public:
    explicit Mp3FilePlayer(QObject* parent = nullptr);

    bool start(QString const& filePath);
    void stop();
    bool isActive() const;

   signals:
    void audioChunk(QByteArray data);
    void errorMessage(QString message);
    void playbackStarted();
    void playbackFinished();

   private slots:
    void onBufferReady();
    void onDecodeFinished();
    void onPlaybackTick();

   private:
    QVector<float> bufferToMonoFloat(QAudioBuffer const& buffer) const;
    QVector<float> resampleToTargetRate(QVector<float> const& input, int sourceRate, int targetRate) const;
    void resetDecodeState();
    void resetPlaybackState();
    void startBufferedPlayback();

    QAudioDecoder* decoder_;
    QTimer* playbackTimer_;
    QVector<float> decodedSamples_;
    QByteArray playbackData_;
    int decodedSampleRate_;
    int playbackOffsetBytes_;
};

#endif
