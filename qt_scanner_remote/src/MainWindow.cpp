#include "MainWindow.h"

#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);

    QGroupBox* networkGroup = new QGroupBox("Network", central);
    QFormLayout* networkLayout = new QFormLayout(networkGroup);
    audioPort_ = new QSpinBox(networkGroup);
    audioPort_->setRange(1, 65535);
    audioPort_->setValue(9000);
    metadataPort_ = new QSpinBox(networkGroup);
    metadataPort_->setRange(1, 65535);
    metadataPort_->setValue(9001);
    networkLayout->addRow("Audio UDP port", audioPort_);
    networkLayout->addRow("Metadata UDP port", metadataPort_);

    QGroupBox* audioGroup = new QGroupBox("Audio", central);
    QFormLayout* audioLayout = new QFormLayout(audioGroup);
    volume_ = new QSlider(Qt::Horizontal, audioGroup);
    volume_->setRange(0, 100);
    volume_->setValue(100);
    audioLayout->addRow("Volume", volume_);

    QGroupBox* metadataGroup = new QGroupBox("Scanner", central);
    QGridLayout* metadataLayout = new QGridLayout(metadataGroup);
    statusValue_ = new QLabel("Idle", metadataGroup);
    freqValue_ = new QLabel("-", metadataGroup);
    labelValue_ = new QLabel("-", metadataGroup);
    squelchValue_ = new QLabel("-", metadataGroup);
    seqValue_ = new QLabel("-", metadataGroup);

    metadataLayout->addWidget(new QLabel("Status", metadataGroup), 0, 0);
    metadataLayout->addWidget(statusValue_, 0, 1);
    metadataLayout->addWidget(new QLabel("Frequency", metadataGroup), 1, 0);
    metadataLayout->addWidget(freqValue_, 1, 1);
    metadataLayout->addWidget(new QLabel("Label", metadataGroup), 2, 0);
    metadataLayout->addWidget(labelValue_, 2, 1);
    metadataLayout->addWidget(new QLabel("Squelch", metadataGroup), 3, 0);
    metadataLayout->addWidget(squelchValue_, 3, 1);
    metadataLayout->addWidget(new QLabel("Seq", metadataGroup), 4, 0);
    metadataLayout->addWidget(seqValue_, 4, 1);

    QHBoxLayout* controls = new QHBoxLayout();
    startButton_ = new QPushButton("Start", central);
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setEnabled(false);
    controls->addWidget(startButton_);
    controls->addWidget(stopButton_);

    root->addWidget(networkGroup);
    root->addWidget(audioGroup);
    root->addWidget(metadataGroup);
    root->addLayout(controls);

    setCentralWidget(central);
    setWindowTitle("RTLSDR-Airband Scanner Remote");
    statusBar()->showMessage("Ready");

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startListening);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopListening);
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        audioEngine_.setVolume(static_cast<float>(v) / 100.0f);
    });

    connect(&audioReceiver_, &AudioReceiver::audioDatagram, &audioEngine_, &AudioEngine::pushFloat32Mono);
    connect(&metadataReceiver_, &MetadataReceiver::metadataReceived, this, &MainWindow::onMetadata);

    connect(&audioReceiver_, &AudioReceiver::errorMessage, this, &MainWindow::onError);
    connect(&metadataReceiver_, &MetadataReceiver::errorMessage, this, &MainWindow::onError);
    connect(&audioEngine_, &AudioEngine::errorMessage, this, &MainWindow::onError);
}

void MainWindow::startListening() {
    if (!audioEngine_.start()) {
        return;
    }

    if (!audioReceiver_.bind(static_cast<quint16>(audioPort_->value()))) {
        audioEngine_.stop();
        return;
    }
    if (!metadataReceiver_.bind(static_cast<quint16>(metadataPort_->value()))) {
        audioReceiver_.close();
        audioEngine_.stop();
        return;
    }

    startButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    setStatus("Listening");
    statusBar()->showMessage("Listening for audio and metadata streams");
}

void MainWindow::stopListening() {
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
    seqValue_->setText(QString::number(seq));
}

void MainWindow::onError(QString message) {
    statusBar()->showMessage(message);
}

void MainWindow::setStatus(QString status) {
    statusValue_->setText(status);
}
