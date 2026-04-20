#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QByteArray>
#include <QDateTime>
#include <QMainWindow>
#include <QVector>

#include "AudioEngine.h"
#include "AudioReceiver.h"
#include "MetadataReceiver.h"
#include "WaterfallWidget.h"
#include "WaveformWidget.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QSpinBox;
class QSlider;
class QLineEdit;
class QProgressBar;
class QPlainTextEdit;
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);

   private slots:
    void startListening();
    void stopListening();
    void onMetadata(int device, qint64 freqHz, bool squelchOpen, QString label, quint32 seq);
    void onError(QString message);
    void onAudioChunk(QByteArray pcmData);

   private:
    void setStatus(QString status);
    void flushOpenSquelchIfAny();
    void appendSquelchLogEntry(QDateTime const& start, qint64 freqHz, QString const& label, qint64 durationMs);
    QVector<float> downsampleForWaveform(QByteArray const& data) const;
    QVector<float> downsampleForWaterfall(QByteArray const& data) const;

    AudioReceiver audioReceiver_;
    MetadataReceiver metadataReceiver_;
    AudioEngine audioEngine_;

    QLabel* statusValue_;
    QLabel* freqValue_;
    QLabel* labelValue_;
    QLabel* squelchValue_;
    QLineEdit* backendHost_;
    QSpinBox* audioPort_;
    QSpinBox* metadataPort_;
    QSlider* volume_;
    QProgressBar* inputLevelBar_;
    WaveformWidget* inputWaveform_;
    WaterfallWidget* waterfall_;
    QPlainTextEdit* squelchLog_;
    QPushButton* startButton_;
    QPushButton* stopButton_;

    bool squelchOpen_;
    QDateTime squelchStart_;
    qint64 squelchFreqHz_;
    QString squelchLabel_;
};

#endif
