#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QResizeEvent>
#include <QStringList>
#include <QVector>

#include "AudioEngine.h"
#include "AudioReceiver.h"
#include "ChannelWaterfallDialog.h"
#include "MetadataReceiver.h"
#include "Mp3FilePlayer.h"
#include "SpectrumWidget.h"
#include "WaterfallWidget.h"
#include "WaveformWidget.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
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
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    QString defaultChannelColor(qint64 freqHz) const;
    void resetSessionTrafficState();
    void recordSquelchDuration(qint64 freqHz, qint64 durationMs);
    void flushOpenSquelchIfAny();
    void appendSquelchLogEntry(QDateTime const& start, qint64 freqHz, QString const& label, qint64 durationMs);
    QVector<float> downsampleForWaveform(QByteArray const& data) const;
    QVector<float> computeWaterfallBins(QByteArray const& data) const;
    void appendChannelWaterfallFrame(qint64 freqHz, QVector<float> const& bins);
    void showChannelWaterfallDialog(qint64 freqHz);
    void refreshAudioOutputDevices();
    void applySelectedAudioOutput();

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
    SpectrumWidget* inputSpectrum_;
    WaterfallWidget* inputWaterfall_;
    QComboBox* audioOutputDeviceSelect_;
    QDoubleSpinBox* noiseSuppressionInput_;
    QDoubleSpinBox* presenceBoostInput_;
    QCheckBox* highPassFilterToggle_;
    QCheckBox* lowPassFilterToggle_;
    QSpinBox* sessionMarkingTimeInput_;
    QGroupBox* scannerGroup_;
    QGridLayout* scannerGrid_;
    QHash<qint64, QList<QLabel*>> textByFreq_;
    QHash<qint64, bool> sessionLongSignalByFreq_;
    QPlainTextEdit* squelchLog_;
    QPushButton* startButton_;
    QPushButton* loadMp3Button_;
    QPushButton* stopButton_;
    QList<qint64> channelFreqs_;
    QStringList channelLabels_;
    QHash<qint64, QString> channelLabelByFreq_;
    QHash<qint64, QList<QVector<float>>> channelWaterfallByFreq_;
    QHash<qint64, QPointer<ChannelWaterfallDialog>> channelWaterfallDialogs_;

    bool squelchOpen_;
    bool localFileMode_;
    qint64 sessionTrafficHighlightThresholdMs_;
    QDateTime squelchStart_;
    qint64 squelchFreqHz_;
    QString squelchLabel_;
};

#endif
