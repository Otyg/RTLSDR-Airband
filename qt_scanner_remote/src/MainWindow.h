#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QResizeEvent>
#include <QStringList>
#include <QVector>

#include "AudioEngine.h"
#include "AudioReceiver.h"
#include "MetadataReceiver.h"
#include "Mp3FilePlayer.h"
#include "WaterfallWidget.h"
#include "WaveformWidget.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QProgressBar;
class QPlainTextEdit;
class QGridLayout;
class QGroupBox;
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);
    void resizeEvent(QResizeEvent* event) override;

   private slots:
    void startListening();
    void loadMp3File();
    void stopListening();
    void onChannelsReceived(QList<qint64> freqsHz, QStringList labels);
    void onMetadata(int device, qint64 freqHz, bool squelchOpen, QString label, quint32 seq);
    void onError(QString message);
    void onAudioChunk(QByteArray pcmData);
    void onMp3PlaybackStarted();
    void onMp3PlaybackFinished();

   private:
    void setStatus(QString status);
    void rebuildChannelGrid(QList<qint64> const& freqsHz, QStringList const& labels);
    void clearChannelGrid();
    void setChannelTextColor(qint64 freqHz, QString const& color);
    void flushOpenSquelchIfAny();
    void appendSquelchLogEntry(QDateTime const& start, qint64 freqHz, QString const& label, qint64 durationMs);
    QVector<float> downsampleForWaveform(QByteArray const& data) const;
    QVector<float> computeWaterfallBins(QByteArray const& data) const;

    AudioReceiver audioReceiver_;
    MetadataReceiver metadataReceiver_;
    AudioEngine audioEngine_;
    Mp3FilePlayer mp3FilePlayer_;

    QLabel* statusValue_;
    QLineEdit* backendHost_;
    QSpinBox* audioPort_;
    QSpinBox* metadataPort_;
    QProgressBar* inputLevelBar_;
    WaveformWidget* inputWaveform_;
    WaterfallWidget* inputWaterfall_;
    QDoubleSpinBox* noiseSuppressionInput_;
    QGroupBox* scannerGroup_;
    QGridLayout* scannerGrid_;
    QHash<qint64, QList<QLabel*>> textByFreq_;
    QPlainTextEdit* squelchLog_;
    QPushButton* startButton_;
    QPushButton* loadMp3Button_;
    QPushButton* stopButton_;
    QList<qint64> channelFreqs_;
    QStringList channelLabels_;

    bool squelchOpen_;
    bool localFileMode_;
    QDateTime squelchStart_;
    qint64 squelchFreqHz_;
    QString squelchLabel_;
};

#endif
