#include "MainWindow.h"

#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdint>
#include <cmath>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);

    QGroupBox* networkGroup = new QGroupBox("Network", central);
    QFormLayout* networkLayout = new QFormLayout(networkGroup);
    backendHost_ = new QLineEdit(networkGroup);
    backendHost_->setText("127.0.0.1");
    audioPort_ = new QSpinBox(networkGroup);
    audioPort_->setRange(1, 65535);
    audioPort_->setValue(9000);
    metadataPort_ = new QSpinBox(networkGroup);
    metadataPort_->setRange(1, 65535);
    metadataPort_->setValue(9001);
    networkLayout->addRow("Backend host", backendHost_);
    networkLayout->addRow("Audio TCP port", audioPort_);
    networkLayout->addRow("Metadata TCP port", metadataPort_);

    QGroupBox* audioGroup = new QGroupBox("Audio", central);
    QFormLayout* audioLayout = new QFormLayout(audioGroup);
    volume_ = new QSlider(Qt::Horizontal, audioGroup);
    volume_->setRange(0, 100);
    volume_->setValue(100);
    inputLevelBar_ = new QProgressBar(audioGroup);
    inputLevelBar_->setRange(0, 100);
    inputLevelBar_->setValue(0);
    inputWaveform_ = new WaveformWidget(audioGroup);
    waterfall_ = new WaterfallWidget(audioGroup);
    audioLayout->addRow("Volume", volume_);
    audioLayout->addRow("Level", inputLevelBar_);
    audioLayout->addRow("Waveform", inputWaveform_);
    audioLayout->addRow("Waterfall", waterfall_);

    QGroupBox* metadataGroup = new QGroupBox("Scanner", central);
    QGridLayout* metadataLayout = new QGridLayout(metadataGroup);
    statusValue_ = new QLabel("Idle", metadataGroup);
    freqValue_ = new QLabel("-", metadataGroup);
    labelValue_ = new QLabel("-", metadataGroup);
    squelchValue_ = new QLabel("-", metadataGroup);

    metadataLayout->addWidget(new QLabel("Status", metadataGroup), 0, 0);
    metadataLayout->addWidget(statusValue_, 0, 1);
    metadataLayout->addWidget(new QLabel("Frequency", metadataGroup), 1, 0);
    metadataLayout->addWidget(freqValue_, 1, 1);
    metadataLayout->addWidget(new QLabel("Label", metadataGroup), 2, 0);
    metadataLayout->addWidget(labelValue_, 2, 1);
    metadataLayout->addWidget(new QLabel("Squelch", metadataGroup), 3, 0);
    metadataLayout->addWidget(squelchValue_, 3, 1);

    QGroupBox* logGroup = new QGroupBox("Squelch Log", central);
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    squelchLog_ = new QPlainTextEdit(logGroup);
    squelchLog_->setReadOnly(true);
    squelchLog_->setMaximumBlockCount(4000);
    squelchLog_->setPlaceholderText("timestamp frequency label duration");
    logLayout->addWidget(squelchLog_);

    QHBoxLayout* controls = new QHBoxLayout();
    startButton_ = new QPushButton("Start", central);
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setEnabled(false);
    controls->addWidget(startButton_);
    controls->addWidget(stopButton_);

    root->addWidget(networkGroup);
    root->addWidget(audioGroup);
    root->addWidget(metadataGroup);
    root->addWidget(logGroup);
    root->addLayout(controls);

    setCentralWidget(central);
    setWindowTitle("RTLSDR-Airband Scanner Remote");
    statusBar()->showMessage("Ready");

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startListening);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopListening);
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        audioEngine_.setVolume(static_cast<float>(v) / 100.0f);
    });

    connect(&audioReceiver_, &AudioReceiver::audioChunk, &audioEngine_, &AudioEngine::pushFloat32Mono);
    connect(&audioEngine_, &AudioEngine::pcmChunk, this, &MainWindow::onAudioChunk);
    connect(&metadataReceiver_, &MetadataReceiver::metadataReceived, this, &MainWindow::onMetadata);

    connect(&audioReceiver_, &AudioReceiver::errorMessage, this, &MainWindow::onError);
    connect(&metadataReceiver_, &MetadataReceiver::errorMessage, this, &MainWindow::onError);
    connect(&audioEngine_, &AudioEngine::errorMessage, this, &MainWindow::onError);

    squelchOpen_ = false;
    squelchFreqHz_ = 0;
}

void MainWindow::startListening() {
    if (!audioEngine_.start()) {
        return;
    }

    QString const host = backendHost_->text().trimmed();
    if (host.isEmpty()) {
        statusBar()->showMessage("Backend host must not be empty");
        audioEngine_.stop();
        return;
    }

    if (!audioReceiver_.connectToHost(host, static_cast<quint16>(audioPort_->value()))) {
        audioEngine_.stop();
        return;
    }
    if (!metadataReceiver_.connectToHost(host, static_cast<quint16>(metadataPort_->value()))) {
        audioReceiver_.close();
        audioEngine_.stop();
        return;
    }

    startButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    setStatus("Listening");
    statusBar()->showMessage(QString("Connected to %1").arg(host));
}

void MainWindow::stopListening() {
    flushOpenSquelchIfAny();
    metadataReceiver_.close();
    audioReceiver_.close();
    audioEngine_.stop();

    startButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    setStatus("Stopped");
    statusBar()->showMessage("Stopped");
}

void MainWindow::onMetadata(int device, qint64 freqHz, bool squelchOpen, QString label, quint32 seq) {
    Q_UNUSED(device);

    double mhz = static_cast<double>(freqHz) / 1000000.0;
    freqValue_->setText(QString::number(mhz, 'f', 3) + " MHz");
    labelValue_->setText(label.isEmpty() ? "-" : label);
    squelchValue_->setText(squelchOpen ? "Open" : "Closed");
    Q_UNUSED(seq);

    QDateTime now = QDateTime::currentDateTime();
    bool sameSignal = squelchOpen_ && squelchFreqHz_ == freqHz && squelchLabel_ == label;

    if (squelchOpen) {
        if (!squelchOpen_) {
            squelchOpen_ = true;
            squelchStart_ = now;
            squelchFreqHz_ = freqHz;
            squelchLabel_ = label;
        } else if (!sameSignal) {
            qint64 durationMs = squelchStart_.msecsTo(now);
            appendSquelchLogEntry(squelchStart_, squelchFreqHz_, squelchLabel_, durationMs);
            squelchStart_ = now;
            squelchFreqHz_ = freqHz;
            squelchLabel_ = label;
        }
    } else if (squelchOpen_) {
        qint64 durationMs = squelchStart_.msecsTo(now);
        appendSquelchLogEntry(squelchStart_, squelchFreqHz_, squelchLabel_, durationMs);
        squelchOpen_ = false;
    }
}

void MainWindow::onError(QString message) {
    statusBar()->showMessage(message);
}

void MainWindow::onAudioChunk(QByteArray pcmData) {
    if (pcmData.isEmpty() || pcmData.size() % static_cast<int>(sizeof(int16_t)) != 0) {
        return;
    }

    int const sampleCount = pcmData.size() / static_cast<int>(sizeof(int16_t));
    int16_t const* in = reinterpret_cast<int16_t const*>(pcmData.constData());

    double sumSq = 0.0;
    QByteArray floatData;
    floatData.resize(sampleCount * static_cast<int>(sizeof(float)));
    float* out = reinterpret_cast<float*>(floatData.data());
    for (int i = 0; i < sampleCount; ++i) {
        float v = static_cast<float>(in[i]) / 32767.0f;
        out[i] = v;
        sumSq += static_cast<double>(v) * static_cast<double>(v);
    }

    float const rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(sampleCount)));
    int const bar = static_cast<int>(std::round(std::fmin(1.0f, rms) * 100.0f));
    inputLevelBar_->setValue(bar);

    inputWaveform_->setSamples(downsampleForWaveform(floatData));
    waterfall_->appendFrame(downsampleForWaterfall(floatData));
}

QVector<float> MainWindow::downsampleForWaveform(QByteArray const& data) const {
    int const sampleCount = data.size() / static_cast<int>(sizeof(float));
    float const* in = reinterpret_cast<float const*>(data.constData());

    int const target = 512;
    QVector<float> out;
    out.reserve(target);

    if (sampleCount <= target) {
        for (int i = 0; i < sampleCount; ++i) {
            out.push_back(in[i]);
        }
        return out;
    }

    for (int x = 0; x < target; ++x) {
        int start = (x * sampleCount) / target;
        int end = ((x + 1) * sampleCount) / target;
        if (end <= start) {
            end = start + 1;
        }
        if (end > sampleCount) {
            end = sampleCount;
        }

        float peak = 0.0f;
        for (int i = start; i < end; ++i) {
            float a = std::fabs(in[i]);
            if (a > peak) {
                peak = a;
            }
        }
        float signedSample = in[start];
        out.push_back((signedSample < 0.0f) ? -peak : peak);
    }

    return out;
}

QVector<float> MainWindow::downsampleForWaterfall(QByteArray const& data) const {
    int const sampleCount = data.size() / static_cast<int>(sizeof(float));
    float const* in = reinterpret_cast<float const*>(data.constData());

    int const bins = 192;
    QVector<float> out;
    out.reserve(bins);

    if (sampleCount <= 0) {
        return out;
    }

    for (int x = 0; x < bins; ++x) {
        int start = (x * sampleCount) / bins;
        int end = ((x + 1) * sampleCount) / bins;
        if (end <= start) {
            end = start + 1;
        }
        if (end > sampleCount) {
            end = sampleCount;
        }

        float peak = 0.0f;
        for (int i = start; i < end; ++i) {
            float a = std::fabs(in[i]);
            if (a > peak) {
                peak = a;
            }
        }
        out.push_back(peak);
    }

    return out;
}

void MainWindow::setStatus(QString status) {
    statusValue_->setText(status);
}

void MainWindow::flushOpenSquelchIfAny() {
    if (!squelchOpen_) {
        return;
    }
    QDateTime now = QDateTime::currentDateTime();
    qint64 durationMs = squelchStart_.msecsTo(now);
    appendSquelchLogEntry(squelchStart_, squelchFreqHz_, squelchLabel_, durationMs);
    squelchOpen_ = false;
}

void MainWindow::appendSquelchLogEntry(QDateTime const& start, qint64 freqHz, QString const& label, qint64 durationMs) {
    double mhz = static_cast<double>(freqHz) / 1000000.0;
    QString stamp = start.toString("yyyy-MM-dd HH:mm:ss");
    QString shownLabel = label.isEmpty() ? "-" : label;
    QString duration = QString::number(static_cast<double>(durationMs) / 1000.0, 'f', 2) + "s";
    squelchLog_->appendPlainText(QString("%1 %2 %3 %4").arg(stamp).arg(mhz, 0, 'f', 3).arg(shownLabel).arg(duration));
}
