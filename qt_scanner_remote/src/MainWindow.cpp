#include "MainWindow.h"
#include "Theme.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QMediaDevices>
#include <QSignalBlocker>
#include <algorithm>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionMatch>
#include <QSet>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTextStream>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QMouseEvent>
#include <cstdint>
#include <cmath>

namespace {
constexpr int kMaxChannelWaterfallFrames = 4000;

QComboBox* createModulationComboBox(QWidget* parent, QString const& value) {
    QComboBox* combo = new QComboBox(parent);
    combo->addItem("am");
    combo->addItem("nfm");
    int const index = combo->findText(value.trimmed().toLower());
    combo->setCurrentIndex(index >= 0 ? index : 0);
    return combo;
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), localFileMode_(false) {
    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    configEditorWindow_ = nullptr;

    QGroupBox* networkGroup = new QGroupBox("Network", central);
    QGridLayout* networkLayout = new QGridLayout(networkGroup);
    networkLayout->setHorizontalSpacing(10);
    networkLayout->setVerticalSpacing(4);
    backendHost_ = new QLineEdit(networkGroup);
    backendHost_->setText("127.0.0.1");
    audioPort_ = new QSpinBox(networkGroup);
    audioPort_->setRange(1, 65535);
    audioPort_->setValue(9000);
    metadataPort_ = new QSpinBox(networkGroup);
    metadataPort_->setRange(1, 65535);
    metadataPort_->setValue(9001);
    networkLayout->addWidget(new QLabel("Backend host", networkGroup), 0, 0);
    networkLayout->addWidget(new QLabel("Audio UDP port", networkGroup), 0, 1);
    networkLayout->addWidget(new QLabel("Metadata TCP port", networkGroup), 0, 2);
    networkLayout->addWidget(backendHost_, 1, 0);
    networkLayout->addWidget(audioPort_, 1, 1);
    networkLayout->addWidget(metadataPort_, 1, 2);
    networkLayout->setColumnStretch(0, 2);
    networkLayout->setColumnStretch(1, 1);
    networkLayout->setColumnStretch(2, 1);

    QGroupBox* audioGroup = new QGroupBox("Audio", central);
    QFormLayout* audioLayout = new QFormLayout(audioGroup);
    inputLevelBar_ = new QProgressBar(audioGroup);
    inputLevelBar_->setRange(0, 100);
    inputLevelBar_->setValue(0);
    inputWaveform_ = new WaveformWidget(audioGroup);
    inputSpectrum_ = new SpectrumWidget(audioGroup);
    inputWaveform_->setMinimumHeight(70);
    inputWaveform_->setMaximumHeight(70);
    inputSpectrum_->setMinimumHeight(70);
    inputSpectrum_->setMaximumHeight(70);
    QHBoxLayout* scopeLayout = new QHBoxLayout();
    scopeLayout->setSpacing(8);
    scopeLayout->addWidget(inputWaveform_, 1);
    QVBoxLayout* spectrumLayout = new QVBoxLayout();
    spectrumLayout->setSpacing(8);
    spectrumLayout->setContentsMargins(0, 0, 0, 0);
    spectrumLayout->addWidget(inputSpectrum_);
    scopeLayout->addLayout(spectrumLayout, 1);
    audioLayout->addRow("Level", inputLevelBar_);
    audioLayout->addRow("Signal", scopeLayout);
    audioOutputDeviceSelect_ = new QComboBox(audioGroup);
    audioLayout->addRow("Output Device", audioOutputDeviceSelect_);

    noiseSuppressionInput_ = new QDoubleSpinBox(audioGroup);
    noiseSuppressionInput_->setRange(0.0, 100.0);
    noiseSuppressionInput_->setDecimals(0);
    noiseSuppressionInput_->setSingleStep(5.0);
    noiseSuppressionInput_->setSuffix("%");
    noiseSuppressionInput_->setValue(70.0);

    presenceBoostInput_ = new QDoubleSpinBox(audioGroup);
    presenceBoostInput_->setRange(0.0, 4.0);
    presenceBoostInput_->setDecimals(1);
    presenceBoostInput_->setSingleStep(0.5);
    presenceBoostInput_->setSuffix(" dB");
    presenceBoostInput_->setValue(0.0);

    highPassFilterToggle_ = new QCheckBox("High-pass 250 Hz", audioGroup);
    highPassFilterToggle_->setChecked(false);
    lowPassFilterToggle_ = new QCheckBox("Low-pass 2.9 kHz", audioGroup);
    lowPassFilterToggle_->setChecked(false);

    sessionMarkingTimeInput_ = new QSpinBox(audioGroup);
    sessionMarkingTimeInput_->setRange(0, 60000);
    sessionMarkingTimeInput_->setSingleStep(250);
    sessionMarkingTimeInput_->setSuffix(" ms");
    sessionMarkingTimeInput_->setValue(static_cast<int>(Theme::kDefaultSessionTrafficHighlightThresholdMs));

    QHBoxLayout* audioSettingsLayout = new QHBoxLayout();
    audioSettingsLayout->setSpacing(8);
    audioSettingsLayout->addWidget(noiseSuppressionInput_);
    audioSettingsLayout->addWidget(new QLabel("1.5-2.5 kHz Boost", audioGroup));
    audioSettingsLayout->addWidget(presenceBoostInput_);
    audioSettingsLayout->addWidget(highPassFilterToggle_);
    audioSettingsLayout->addWidget(lowPassFilterToggle_);
    audioSettingsLayout->addWidget(new QLabel("Session Marking", audioGroup));
    audioSettingsLayout->addWidget(sessionMarkingTimeInput_);
    audioSettingsLayout->addStretch(1);
    audioLayout->addRow("Noise Suppression", audioSettingsLayout);

    statusValue_ = new QLabel("Idle", central);
    scannerGroup_ = new QGroupBox("Scanner Channels", central);
    scannerGrid_ = new QGridLayout(scannerGroup_);
    scannerGrid_->addWidget(new QLabel("Waiting for channel list from backend..."), 0, 0);

    QGroupBox* logGroup = new QGroupBox("Squelch Log", central);
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    squelchLog_ = new QPlainTextEdit(logGroup);
    squelchLog_->setReadOnly(true);
    squelchLog_->setMaximumBlockCount(4000);
    squelchLog_->setPlaceholderText("timestamp frequency label duration");
    logLayout->addWidget(squelchLog_);

    configEditorWindow_ = new QWidget(this, Qt::Window);
    configEditorWindow_->setWindowTitle("Scanner Config Editor");
    configEditorWindow_->resize(760, 520);
    QVBoxLayout* configWindowLayout = new QVBoxLayout(configEditorWindow_);

    QGroupBox* configGroup = new QGroupBox("Scanner Config", configEditorWindow_);
    QVBoxLayout* configLayout = new QVBoxLayout(configGroup);
    QHBoxLayout* configPathLayout = new QHBoxLayout();
    configFilePath_ = new QLineEdit(configGroup);
    configFilePath_->setPlaceholderText("Select an rtl_airband .conf file");
    browseConfigButton_ = new QPushButton("Browse", configGroup);
    importConfigButton_ = new QPushButton("Import", configGroup);
    saveConfigButton_ = new QPushButton("Save", configGroup);
    configPathLayout->addWidget(configFilePath_, 1);
    configPathLayout->addWidget(browseConfigButton_);
    configPathLayout->addWidget(importConfigButton_);
    configPathLayout->addWidget(saveConfigButton_);

    configChannelTable_ = new QTableWidget(0, 3, configGroup);
    configChannelTable_->setHorizontalHeaderLabels(QStringList() << "Frequency (MHz)" << "Modulation" << "Label");
    configChannelTable_->horizontalHeader()->setStretchLastSection(true);
    configChannelTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    configChannelTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    configChannelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    configChannelTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    QHBoxLayout* configButtonsLayout = new QHBoxLayout();
    addChannelButton_ = new QPushButton("Add Channel", configGroup);
    removeChannelButton_ = new QPushButton("Remove Selected", configGroup);
    configButtonsLayout->addWidget(addChannelButton_);
    configButtonsLayout->addWidget(removeChannelButton_);
    configButtonsLayout->addStretch(1);

    configLayout->addLayout(configPathLayout);
    configLayout->addWidget(configChannelTable_);
    configLayout->addLayout(configButtonsLayout);
    configWindowLayout->addWidget(configGroup);

    QHBoxLayout* controls = new QHBoxLayout();
    startButton_ = new QPushButton("Start", central);
    loadMp3Button_ = new QPushButton("Load MP3", central);
    stopButton_ = new QPushButton("Stop", central);
    openConfigEditorButton_ = new QPushButton("Edit Config", central);
    stopButton_->setEnabled(false);
    controls->addWidget(startButton_);
    controls->addWidget(loadMp3Button_);
    controls->addWidget(stopButton_);
    controls->addWidget(openConfigEditorButton_);

    root->addWidget(networkGroup);
    root->addWidget(audioGroup);
    root->addWidget(statusValue_);
    root->addWidget(scannerGroup_);
    root->addWidget(logGroup);
    root->addLayout(controls);

    setCentralWidget(central);
    setWindowTitle("RTLSDR-Airband Scanner Remote");
    statusBar()->showMessage("Ready");

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startListening);
    connect(loadMp3Button_, &QPushButton::clicked, this, &MainWindow::loadMp3File);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopListening);
    connect(openConfigEditorButton_, &QPushButton::clicked, this, &MainWindow::openConfigEditorWindow);
    connect(browseConfigButton_, &QPushButton::clicked, this, &MainWindow::browseConfigFile);
    connect(importConfigButton_, &QPushButton::clicked, this, &MainWindow::importChannelListFile);
    connect(saveConfigButton_, &QPushButton::clicked, this, &MainWindow::saveConfigFile);
    connect(addChannelButton_, &QPushButton::clicked, this, &MainWindow::addConfigChannel);
    connect(removeChannelButton_, &QPushButton::clicked, this, &MainWindow::removeSelectedConfigChannels);
    connect(audioOutputDeviceSelect_, &QComboBox::currentIndexChanged, this, [this](int) {
        applySelectedAudioOutput();
    });
    connect(noiseSuppressionInput_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        float const strength = static_cast<float>(value / 100.0);
        audioEngine_.setNoiseReductionEnabled(strength > 0.0f);
        audioEngine_.setNoiseReductionStrength(strength);
    });
    connect(presenceBoostInput_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        audioEngine_.setPresenceBoostDb(static_cast<float>(value));
    });
    connect(highPassFilterToggle_, &QCheckBox::toggled, this, [this](bool checked) {
        audioEngine_.setHighPassFilterEnabled(checked);
    });
    connect(lowPassFilterToggle_, &QCheckBox::toggled, this, [this](bool checked) {
        audioEngine_.setLowPassFilterEnabled(checked);
    });
    connect(sessionMarkingTimeInput_, &QSpinBox::valueChanged, this, [this](int value) {
        sessionTrafficHighlightThresholdMs_ = static_cast<qint64>(value);
    });

    connect(&audioReceiver_, &AudioReceiver::audioChunk, &audioEngine_, &AudioEngine::pushFloat32Mono);
    connect(&audioEngine_, &AudioEngine::pcmChunk, this, &MainWindow::onAudioChunk);
    connect(&metadataReceiver_, &MetadataReceiver::channelsReceived, this, &MainWindow::onChannelsReceived);
    connect(&metadataReceiver_, &MetadataReceiver::metadataReceived, this, &MainWindow::onMetadata);

    connect(&audioReceiver_, &AudioReceiver::errorMessage, this, &MainWindow::onError);
    connect(&metadataReceiver_, &MetadataReceiver::errorMessage, this, &MainWindow::onError);
    connect(&audioEngine_, &AudioEngine::errorMessage, this, &MainWindow::onError);
    connect(&mp3FilePlayer_, &Mp3FilePlayer::audioChunk, &audioEngine_, &AudioEngine::pushFloat32Mono);
    connect(&mp3FilePlayer_, &Mp3FilePlayer::errorMessage, this, &MainWindow::onError);
    connect(&mp3FilePlayer_, &Mp3FilePlayer::playbackStarted, this, &MainWindow::onMp3PlaybackStarted);
    connect(&mp3FilePlayer_, &Mp3FilePlayer::playbackFinished, this, &MainWindow::onMp3PlaybackFinished);

    squelchOpen_ = false;
    squelchFreqHz_ = 0;
    sessionTrafficHighlightThresholdMs_ = Theme::kDefaultSessionTrafficHighlightThresholdMs;
    refreshAudioOutputDevices();
    audioEngine_.setHighPassFilterEnabled(highPassFilterToggle_->isChecked());
    audioEngine_.setLowPassFilterEnabled(lowPassFilterToggle_->isChecked());
    audioEngine_.setPresenceBoostDb(static_cast<float>(presenceBoostInput_->value()));

    QMediaDevices* mediaDevices = new QMediaDevices(this);
    connect(mediaDevices, &QMediaDevices::audioOutputsChanged, this, [this]() { refreshAudioOutputDevices(); });
}

void MainWindow::startListening() {
    if (localFileMode_ || mp3FilePlayer_.isActive()) {
        stopListening();
    }
    resetSessionTrafficState();
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

    audioEngine_.setPlaybackActive(false);

    startButton_->setEnabled(false);
    loadMp3Button_->setEnabled(false);
    stopButton_->setEnabled(true);
    setStatus("Listening");
    statusBar()->showMessage(QString("Connected to %1").arg(host));
}

void MainWindow::loadMp3File() {
    QString const filePath = QFileDialog::getOpenFileName(this, "Open MP3 File", QString(), "MP3 files (*.mp3)");
    if (filePath.isEmpty()) {
        return;
    }

    if (!startButton_->isEnabled() || localFileMode_ || mp3FilePlayer_.isActive()) {
        stopListening();
    }

    if (!audioEngine_.start()) {
        return;
    }

    localFileMode_ = true;
    audioEngine_.setPlaybackActive(true);
    if (!mp3FilePlayer_.start(filePath)) {
        localFileMode_ = false;
        audioEngine_.setPlaybackActive(false);
        audioEngine_.stop();
        return;
    }

    startButton_->setEnabled(false);
    loadMp3Button_->setEnabled(false);
    stopButton_->setEnabled(true);
    setStatus("Loading MP3");
    statusBar()->showMessage(QString("Loading %1").arg(filePath));
}

void MainWindow::stopListening() {
    flushOpenSquelchIfAny();
    metadataReceiver_.close();
    audioReceiver_.close();
    mp3FilePlayer_.stop();
    audioEngine_.setPlaybackActive(false);
    audioEngine_.stop();
    localFileMode_ = false;

    startButton_->setEnabled(true);
    loadMp3Button_->setEnabled(true);
    stopButton_->setEnabled(false);
    resetSessionTrafficState();
    setStatus("Stopped");
    statusBar()->showMessage("Stopped");
}

void MainWindow::onChannelsReceived(QList<qint64> freqsHz, QStringList labels) {
    channelFreqs_ = freqsHz;
    channelLabels_ = labels;
    channelLabelByFreq_.clear();
    int const count = std::min(freqsHz.size(), labels.size());
    for (int i = 0; i < count; ++i) {
        channelLabelByFreq_[freqsHz[i]] = labels[i].isEmpty() ? "-" : labels[i];
    }
    rebuildChannelGrid(freqsHz, labels);
}

void MainWindow::onMetadata(int device, qint64 freqHz, bool squelchOpen, QString label, quint32 seq) {
    Q_UNUSED(device);
    Q_UNUSED(seq);

    for (qint64 f : textByFreq_.keys()) {
        setChannelTextColor(f, defaultChannelColor(f));
    }
    if (textByFreq_.contains(freqHz)) {
        setChannelTextColor(freqHz, squelchOpen ? Theme::kActiveChannelColor : Theme::kScannedChannelColor);
    }
    audioEngine_.setPlaybackActive(squelchOpen);

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
            recordSquelchDuration(squelchFreqHz_, durationMs);
            appendSquelchLogEntry(squelchStart_, squelchFreqHz_, squelchLabel_, durationMs);
            squelchStart_ = now;
            squelchFreqHz_ = freqHz;
            squelchLabel_ = label;
        }
    } else if (squelchOpen_) {
        qint64 durationMs = squelchStart_.msecsTo(now);
        recordSquelchDuration(squelchFreqHz_, durationMs);
        appendSquelchLogEntry(squelchStart_, squelchFreqHz_, squelchLabel_, durationMs);
        squelchOpen_ = false;
    }
}

void MainWindow::onError(QString message) {
    statusBar()->showMessage(message);
}

void MainWindow::onMp3PlaybackStarted() {
    setStatus("Playing file");
    statusBar()->showMessage("Playing MP3 through audio chain");
}

void MainWindow::onMp3PlaybackFinished() {
    if (!localFileMode_) {
        return;
    }
    localFileMode_ = false;
    audioEngine_.setPlaybackActive(false);
    startButton_->setEnabled(true);
    loadMp3Button_->setEnabled(true);
    stopButton_->setEnabled(false);
    setStatus("Stopped");
    statusBar()->showMessage("MP3 playback finished");
    QTimer::singleShot(400, this, [this]() {
        if (!localFileMode_ && !mp3FilePlayer_.isActive()) {
            audioEngine_.stop();
        }
    });
}

void MainWindow::openConfigEditorWindow() {
    if (!configEditorWindow_) {
        return;
    }

    configEditorWindow_->show();
    configEditorWindow_->raise();
    configEditorWindow_->activateWindow();
}

void MainWindow::browseConfigFile() {
    QString const filePath = QFileDialog::getOpenFileName(
        this, "Open rtl_airband Config", configFilePath_->text().trimmed(), "Config files (*.conf *.cfg *.txt);;All files (*)");
    if (filePath.isEmpty()) {
        return;
    }

    configFilePath_->setText(filePath);
    loadConfigFile();
}

void MainWindow::importChannelListFile() {
    QString const filePath = QFileDialog::getOpenFileName(
        this,
        "Import Channel List",
        QString(),
        "Channel lists (*.txt *.csv);;All files (*)");
    if (filePath.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!loadChannelsFromSemicolonFile(filePath, &errorMessage)) {
        QMessageBox::critical(this, "Import Channel List", errorMessage);
        return;
    }

    statusBar()->showMessage(QString("Imported channels from %1").arg(filePath), 4000);
}

void MainWindow::loadConfigFile() {
    QString const path = configFilePath_->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, "Load Config", "Choose a config file first.");
        return;
    }

    QString errorMessage;
    if (!loadChannelsFromConfig(path, &errorMessage)) {
        QMessageBox::critical(this, "Load Config", errorMessage);
        return;
    }

    statusBar()->showMessage(QString("Loaded channels from %1").arg(path), 4000);
}

void MainWindow::saveConfigFile() {
    QString path = configFilePath_->text().trimmed();
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, "Save rtl_airband Config", QString(), "Config files (*.conf *.cfg *.txt);;All files (*)");
        if (path.isEmpty()) {
            return;
        }
        configFilePath_->setText(path);
    }

    QString errorMessage;
    if (!saveChannelsToConfig(path, &errorMessage)) {
        QMessageBox::critical(this, "Save Config", errorMessage);
        return;
    }

    statusBar()->showMessage(QString("Saved channels to %1").arg(path), 4000);
}

void MainWindow::addConfigChannel() {
    int const row = configChannelTable_->rowCount();
    configChannelTable_->insertRow(row);
    configChannelTable_->setItem(row, 0, new QTableWidgetItem("118.150"));
    configChannelTable_->setCellWidget(row, 1, createModulationComboBox(configChannelTable_, "am"));
    configChannelTable_->setItem(row, 2, new QTableWidgetItem(QString("Channel %1").arg(row + 1)));
    configChannelTable_->setCurrentCell(row, 0);
}

void MainWindow::removeSelectedConfigChannels() {
    QModelIndexList const selectedRows = configChannelTable_->selectionModel()
                                             ? configChannelTable_->selectionModel()->selectedRows()
                                             : QModelIndexList();
    if (selectedRows.isEmpty()) {
        return;
    }

    QList<int> rows;
    rows.reserve(selectedRows.size());
    for (QModelIndex const& index : selectedRows) {
        rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        configChannelTable_->removeRow(row);
    }
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

    QVector<float> const waterfallBins = computeWaterfallBins(floatData);
    inputWaveform_->setSamples(downsampleForWaveform(floatData));
    inputSpectrum_->setBins(waterfallBins);

    if (squelchOpen_ && squelchFreqHz_ != 0) {
        appendChannelWaterfallFrame(squelchFreqHz_, waterfallBins);
    }
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

QVector<float> MainWindow::computeWaterfallBins(QByteArray const& data) const {
    constexpr float kPi = 3.14159265358979323846f;
    int const sampleCount = data.size() / static_cast<int>(sizeof(float));
    float const* in = reinterpret_cast<float const*>(data.constData());

    int const fftSize = std::min(sampleCount, 512);
    int const binCount = 192;
    QVector<float> bins;
    bins.resize(binCount);
    if (fftSize < 32) {
        std::fill(bins.begin(), bins.end(), 0.0f);
        return bins;
    }

    int const startOffset = sampleCount - fftSize;
    float maxMagnitude = 1e-6f;
    for (int bin = 0; bin < binCount; ++bin) {
        double const center = static_cast<double>(bin) * (fftSize / 2.0) / binCount;
        float real = 0.0f;
        float imag = 0.0f;
        for (int n = 0; n < fftSize; ++n) {
            float const sample = in[startOffset + n];
            float const window = 0.5f - 0.5f * std::cos((2.0f * kPi * n) / (fftSize - 1));
            float const phase = static_cast<float>((2.0 * kPi * center * n) / fftSize);
            real += sample * window * std::cos(phase);
            imag -= sample * window * std::sin(phase);
        }
        float const magnitude = std::sqrt(real * real + imag * imag);
        bins[bin] = magnitude;
        if (magnitude > maxMagnitude) {
            maxMagnitude = magnitude;
        }
    }

    for (float& value : bins) {
        value = std::clamp(std::log10(1.0f + 9.0f * (value / maxMagnitude)), 0.0f, 1.0f);
    }
    return bins;
}

void MainWindow::clearChannelGrid() {
    if (!scannerGrid_) {
        return;
    }
    QLayoutItem* item = nullptr;
    while ((item = scannerGrid_->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    textByFreq_.clear();
}

void MainWindow::setChannelTextColor(qint64 freqHz, QString const& color) {
    if (!textByFreq_.contains(freqHz)) {
        return;
    }
    for (QLabel* label : textByFreq_[freqHz]) {
        if (label) {
            label->setStyleSheet(QString(Theme::kChannelFrequencyStyle).arg(color));
        }
    }
}

QString MainWindow::defaultChannelColor(qint64 freqHz) const {
    return sessionLongSignalByFreq_.value(freqHz, false)
               ? Theme::kSessionTrafficChannelColor
               : Theme::kDefaultChannelColor;
}

void MainWindow::resetSessionTrafficState() {
    sessionLongSignalByFreq_.clear();
    for (qint64 freqHz : textByFreq_.keys()) {
        setChannelTextColor(freqHz, defaultChannelColor(freqHz));
    }
}

void MainWindow::recordSquelchDuration(qint64 freqHz, qint64 durationMs) {
    if (freqHz == 0 || durationMs < sessionTrafficHighlightThresholdMs_) {
        return;
    }

    sessionLongSignalByFreq_[freqHz] = true;
    setChannelTextColor(freqHz, defaultChannelColor(freqHz));
}

void MainWindow::rebuildChannelGrid(QList<qint64> const& freqsHz, QStringList const& labels) {
    clearChannelGrid();

    int count = std::min(freqsHz.size(), labels.size());
    if (count <= 0) {
        scannerGrid_->addWidget(new QLabel("No channels received from backend."), 0, 0);
        return;
    }

    int availableWidth = scannerGroup_->width();
    if (availableWidth <= 0) {
        availableWidth = width();
    }

    QFont labelFont;
    labelFont.setBold(true);
    QFontMetrics labelMetrics(labelFont);
    QFontMetrics freqMetrics(font());

    int maxTextWidth = 0;
    for (int i = 0; i < count; ++i) {
        qint64 const freqHz = freqsHz[i];
        QString const label = labels[i].isEmpty() ? "-" : labels[i];
        QString const freqText = QString::number(static_cast<double>(freqHz) / 1000000.0, 'f', 3) + " MHz";
        maxTextWidth = std::max(maxTextWidth, labelMetrics.horizontalAdvance(label));
        maxTextWidth = std::max(maxTextWidth, freqMetrics.horizontalAdvance(freqText));
    }

    QMargins const gridMargins = scannerGrid_->contentsMargins();
    int const gridSpacing = scannerGrid_->horizontalSpacing() >= 0 ? scannerGrid_->horizontalSpacing() : scannerGrid_->spacing();
    int const boxHorizontalPadding = 18;
    int const minBoxWidth = std::max(150, maxTextWidth + boxHorizontalPadding * 2);
    int const contentWidth = std::max(1, availableWidth - gridMargins.left() - gridMargins.right());

    int cols = 1;
    while ((cols + 1) * minBoxWidth + cols * gridSpacing <= contentWidth) {
        ++cols;
    }

    for (int i = 0; i < count; ++i) {
        qint64 freqHz = freqsHz[i];
        QString label = labels[i].isEmpty() ? "-" : labels[i];
        QString freqText = QString::number(static_cast<double>(freqHz) / 1000000.0, 'f', 3) + " MHz";

        QFrame* box = new QFrame(scannerGroup_);
        box->setFrameShape(QFrame::StyledPanel);
        box->setCursor(Qt::PointingHandCursor);
        box->setProperty("channelFreqHz", QVariant::fromValue(freqHz));
        box->installEventFilter(this);
        box->setStyleSheet(
            QString(Theme::kChannelBoxStyle).arg(Theme::kChannelBoxBackgroundColor, Theme::kChannelBoxBorderColor));
        QVBoxLayout* boxLayout = new QVBoxLayout(box);
        box->setMinimumWidth(minBoxWidth);

        QLabel* labelText = new QLabel(label, box);
        labelText->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        labelText->setFont(labelFont);
        labelText->setStyleSheet(QString(Theme::kChannelLabelStyle).arg(defaultChannelColor(freqHz)));
        QLabel* freqLabel = new QLabel(freqText, box);
        freqLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        freqLabel->setStyleSheet(QString(Theme::kChannelFrequencyStyle).arg(defaultChannelColor(freqHz)));

        boxLayout->addWidget(labelText);
        boxLayout->addWidget(freqLabel);

        int row = i / cols;
        int col = i % cols;
        scannerGrid_->addWidget(box, row, col);
        textByFreq_.insert(freqHz, QList<QLabel*>{labelText, freqLabel});
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (!channelFreqs_.isEmpty() && channelFreqs_.size() == channelLabels_.size()) {
        rebuildChannelGrid(channelFreqs_, channelLabels_);
    }
}

void MainWindow::setStatus(QString status) {
    statusValue_->setText(status);
}

void MainWindow::refreshAudioOutputDevices() {
    QByteArray selectedDeviceId;
    QVariant const currentData = audioOutputDeviceSelect_->currentData();
    if (currentData.isValid()) {
        selectedDeviceId = currentData.toByteArray();
    }

    QAudioDevice const defaultDevice = QMediaDevices::defaultAudioOutput();
    QList<QAudioDevice> const outputs = QMediaDevices::audioOutputs();

    {
        QSignalBlocker blocker(audioOutputDeviceSelect_);
        audioOutputDeviceSelect_->clear();

        int selectedIndex = -1;
        for (QAudioDevice const& device : outputs) {
            QString name = device.description();
            if (device.id() == defaultDevice.id()) {
                name += " (Default)";
            }

            audioOutputDeviceSelect_->addItem(name, device.id());
            int const index = audioOutputDeviceSelect_->count() - 1;
            if (!selectedDeviceId.isEmpty() && device.id() == selectedDeviceId) {
                selectedIndex = index;
            } else if (selectedDeviceId.isEmpty() && device.id() == defaultDevice.id()) {
                selectedIndex = index;
            }
        }

        if (selectedIndex >= 0) {
            audioOutputDeviceSelect_->setCurrentIndex(selectedIndex);
            audioOutputDeviceSelect_->setEnabled(true);
        } else if (audioOutputDeviceSelect_->count() > 0) {
            audioOutputDeviceSelect_->setCurrentIndex(0);
            audioOutputDeviceSelect_->setEnabled(true);
        } else {
            audioOutputDeviceSelect_->addItem("No output devices available", QByteArray());
            audioOutputDeviceSelect_->setEnabled(false);
        }
    }
    applySelectedAudioOutput();
}

void MainWindow::applySelectedAudioOutput() {
    if (!audioOutputDeviceSelect_->isEnabled()) {
        audioEngine_.setOutputDeviceId(QByteArray());
        return;
    }

    audioEngine_.setOutputDeviceId(audioOutputDeviceSelect_->currentData().toByteArray());
}

void MainWindow::flushOpenSquelchIfAny() {
    if (!squelchOpen_) {
        return;
    }
    QDateTime now = QDateTime::currentDateTime();
    qint64 durationMs = squelchStart_.msecsTo(now);
    recordSquelchDuration(squelchFreqHz_, durationMs);
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

void MainWindow::appendChannelWaterfallFrame(qint64 freqHz, QVector<float> const& bins) {
    QList<QVector<float>>& frames = channelWaterfallByFreq_[freqHz];
    frames.append(bins);
    while (frames.size() > kMaxChannelWaterfallFrames) {
        frames.removeFirst();
    }

    ChannelWaterfallDialog* dialog = channelWaterfallDialogs_.value(freqHz, nullptr);
    if (dialog) {
        dialog->setFrames(frames);
    }
}

void MainWindow::showChannelWaterfallDialog(qint64 freqHz) {
    if (freqHz == 0) {
        return;
    }

    ChannelWaterfallDialog* dialog = channelWaterfallDialogs_.value(freqHz, nullptr);
    if (!dialog) {
        dialog = new ChannelWaterfallDialog(this);
        channelWaterfallDialogs_[freqHz] = dialog;
        connect(dialog, &QObject::destroyed, this, [this, freqHz]() {
            channelWaterfallDialogs_.remove(freqHz);
        });
    }

    dialog->setChannelInfo(freqHz, channelLabelByFreq_.value(freqHz, "-"));
    dialog->setFrames(channelWaterfallByFreq_.value(freqHz));
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

bool MainWindow::loadChannelsFromConfig(QString const& path, QString* errorMessage) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("Unable to open %1: %2").arg(path).arg(file.errorString());
        }
        return false;
    }

    QTextStream stream(&file);
    QString const content = stream.readAll();
    QList<ConfigChannelEntry> entries;
    if (!parseConfigChannels(content, &entries, errorMessage)) {
        return false;
    }

    populateConfigTable(entries);
    return true;
}

bool MainWindow::loadChannelsFromSemicolonFile(QString const& path, QString* errorMessage) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QString("Unable to open %1: %2").arg(path).arg(file.errorString());
        }
        return false;
    }

    QTextStream stream(&file);
    QString const content = stream.readAll();
    QList<ConfigChannelEntry> entries;
    if (!parseSemicolonChannels(content, &entries, errorMessage)) {
        return false;
    }

    auto normalizeFrequency = [](QString const& frequency) {
        bool ok = false;
        double const value = frequency.trimmed().toDouble(&ok);
        return ok ? QString::number(value, 'g', 12) : frequency.trimmed();
    };

    QList<ConfigChannelEntry> mergedEntries = configChannelsFromTable();
    QSet<QString> existingFrequencies;
    for (ConfigChannelEntry const& entry : mergedEntries) {
        existingFrequencies.insert(normalizeFrequency(entry.frequency));
    }

    for (ConfigChannelEntry const& entry : entries) {
        QString const normalizedFrequency = normalizeFrequency(entry.frequency);
        if (existingFrequencies.contains(normalizedFrequency)) {
            continue;
        }
        mergedEntries.append(entry);
        existingFrequencies.insert(normalizedFrequency);
    }

    populateConfigTable(mergedEntries);
    return true;
}

bool MainWindow::saveChannelsToConfig(QString const& path, QString* errorMessage) {
    QList<ConfigChannelEntry> const entries = configChannelsFromTable();
    if (entries.isEmpty()) {
        if (errorMessage) {
            *errorMessage = "Add at least one channel before saving.";
        }
        return false;
    }

    QString content;
    QFile inputFile(path);
    if (inputFile.exists()) {
        if (!inputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (errorMessage) {
                *errorMessage = QString("Unable to read %1: %2").arg(path).arg(inputFile.errorString());
            }
            return false;
        }
        QTextStream inputStream(&inputFile);
        content = inputStream.readAll();
        inputFile.close();
    } else {
        content = "devices:\n({\n  channels:\n  (\n    {\n    }\n  );\n});\n";
    }

    content = replaceConfigList(content, "freqs", formatFrequencyList(entries));
    content = replaceConfigList(content, "modulation", formatQuotedList(entries, &ConfigChannelEntry::modulation));
    content = replaceConfigList(content, "labels", formatQuotedList(entries, &ConfigChannelEntry::label));

    QFile outputFile(path);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QString("Unable to write %1: %2").arg(path).arg(outputFile.errorString());
        }
        return false;
    }

    QTextStream outputStream(&outputFile);
    outputStream << content;
    return true;
}

bool MainWindow::parseConfigChannels(QString const& content,
                                     QList<ConfigChannelEntry>* entries,
                                     QString* errorMessage) const {
    if (!entries) {
        if (errorMessage) {
            *errorMessage = "Internal error: no config output container provided.";
        }
        return false;
    }

    auto findListBody = [&](QString const& key) -> QString {
        QRegularExpression const regex(
            QString("%1\\s*=\\s*\\((.*?)\\)\\s*;").arg(QRegularExpression::escape(key)),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch const match = regex.match(content);
        return match.hasMatch() ? match.captured(1) : QString();
    };

    QString const freqsBody = findListBody("freqs");
    if (freqsBody.isEmpty()) {
        if (errorMessage) {
            *errorMessage = "Could not find a `freqs = ( ... );` list in the config.";
        }
        return false;
    }

    auto splitBareList = [](QString const& body) {
        QString cleaned = body;
        cleaned.replace('\n', ' ');
        cleaned.replace('\r', ' ');
        QStringList parts = cleaned.split(',', Qt::SkipEmptyParts);
        for (QString& part : parts) {
            part = part.trimmed();
        }
        return parts;
    };

    auto splitQuotedList = [](QString const& body) {
        QStringList parts;
        QRegularExpression const regex("\"((?:[^\"\\\\]|\\\\.)*)\"");
        QRegularExpressionMatchIterator it = regex.globalMatch(body);
        while (it.hasNext()) {
            QRegularExpressionMatch const match = it.next();
            QString value = match.captured(1);
            value.replace("\\\"", "\"");
            value.replace("\\\\", "\\");
            parts.append(value);
        }
        return parts;
    };

    QStringList const freqs = splitBareList(freqsBody);
    QStringList modulations = splitQuotedList(findListBody("modulation"));
    QStringList labels = splitQuotedList(findListBody("labels"));

    if (freqs.isEmpty()) {
        if (errorMessage) {
            *errorMessage = "The config file contains an empty frequency list.";
        }
        return false;
    }

    if (modulations.size() < freqs.size()) {
        modulations.reserve(freqs.size());
        while (modulations.size() < freqs.size()) {
            modulations.append("am");
        }
    }
    if (labels.size() < freqs.size()) {
        labels.reserve(freqs.size());
        while (labels.size() < freqs.size()) {
            labels.append(QString());
        }
    }

    entries->clear();
    entries->reserve(freqs.size());
    for (int i = 0; i < freqs.size(); ++i) {
        ConfigChannelEntry entry;
        entry.frequency = freqs[i];
        entry.modulation = modulations.value(i, "am");
        entry.label = labels.value(i);
        entries->append(entry);
    }

    return true;
}

bool MainWindow::parseSemicolonChannels(QString const& content,
                                        QList<ConfigChannelEntry>* entries,
                                        QString* errorMessage) const {
    if (!entries) {
        if (errorMessage) {
            *errorMessage = "Internal error: no channel output container provided.";
        }
        return false;
    }

    QList<ConfigChannelEntry> parsedEntries;
    QStringList const lines = content.split(QRegularExpression("\\r?\\n"));
    for (int i = 0; i < lines.size(); ++i) {
        QString const trimmedLine = lines[i].trimmed();
        if (trimmedLine.isEmpty() || trimmedLine.startsWith('#')) {
            continue;
        }

        QStringList const parts = trimmedLine.split(';');
        if (parts.size() < 3) {
            if (errorMessage) {
                *errorMessage = QString("Line %1 must use the format <MHz>;<modulation>;<name>.").arg(i + 1);
            }
            return false;
        }

        QString const frequency = parts[0].trimmed();
        QString const modulation = parts[1].trimmed().toLower();
        QString const label = parts.mid(2).join(";").trimmed();

        bool frequencyOk = false;
        frequency.toDouble(&frequencyOk);
        if (!frequencyOk) {
            if (errorMessage) {
                *errorMessage = QString("Line %1 has an invalid MHz value: %2").arg(i + 1).arg(frequency);
            }
            return false;
        }

        if (modulation != "am" && modulation != "nfm") {
            if (errorMessage) {
                *errorMessage = QString("Line %1 has an unsupported modulation: %2").arg(i + 1).arg(modulation);
            }
            return false;
        }

        if (label.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QString("Line %1 is missing a channel name.").arg(i + 1);
            }
            return false;
        }

        ConfigChannelEntry entry;
        entry.frequency = frequency;
        entry.modulation = modulation;
        entry.label = label;
        parsedEntries.append(entry);
    }

    if (parsedEntries.isEmpty()) {
        if (errorMessage) {
            *errorMessage = "The selected file does not contain any importable channel rows.";
        }
        return false;
    }

    *entries = parsedEntries;
    return true;
}

QString MainWindow::replaceConfigList(QString const& content, QString const& key, QString const& replacementBody) const {
    QRegularExpression const regex(
        QString("(%1\\s*=\\s*\\()(.*?)(\\)\\s*;)").arg(QRegularExpression::escape(key)),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch const match = regex.match(content);
    if (match.hasMatch()) {
        QString updated = content;
        updated.replace(match.capturedStart(2), match.capturedLength(2), replacementBody);
        return updated;
    }

    QRegularExpression const channelBlockRegex("(channels\\s*:\\s*\\(\\s*\\{\\s*)",
                                               QRegularExpression::DotMatchesEverythingOption |
                                                   QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch const channelBlockMatch = channelBlockRegex.match(content);
    if (channelBlockMatch.hasMatch()) {
        QString insertion = QString("%1 = (%2);\n      ").arg(key, replacementBody);
        QString updated = content;
        updated.insert(channelBlockMatch.capturedEnd(1), insertion);
        return updated;
    }

    QString appended = content;
    if (!appended.endsWith('\n')) {
        appended += '\n';
    }
    appended += QString("%1 = (%2);\n").arg(key, replacementBody);
    return appended;
}

QString MainWindow::formatFrequencyList(QList<ConfigChannelEntry> const& entries) const {
    QStringList parts;
    parts.reserve(entries.size());
    for (ConfigChannelEntry const& entry : entries) {
        parts.append(entry.frequency.trimmed());
    }
    return QString(" %1 ").arg(parts.join(", "));
}

QString MainWindow::formatQuotedList(QList<ConfigChannelEntry> const& entries,
                                     QString ConfigChannelEntry::*field) const {
    QStringList parts;
    parts.reserve(entries.size());
    for (ConfigChannelEntry const& entry : entries) {
        QString value = entry.*field;
        value.replace("\\", "\\\\");
        value.replace("\"", "\\\"");
        parts.append(QString("\"%1\"").arg(value));
    }
    return QString(" %1 ").arg(parts.join(", "));
}

QList<MainWindow::ConfigChannelEntry> MainWindow::configChannelsFromTable() const {
    QList<ConfigChannelEntry> entries;
    for (int row = 0; row < configChannelTable_->rowCount(); ++row) {
        auto textAt = [&](int column) {
            QTableWidgetItem const* item = configChannelTable_->item(row, column);
            return item ? item->text().trimmed() : QString();
        };
        auto modulationAt = [&]() {
            if (QComboBox* combo = qobject_cast<QComboBox*>(configChannelTable_->cellWidget(row, 1))) {
                return combo->currentText().trimmed();
            }
            return textAt(1);
        };

        QString const frequency = textAt(0);
        if (frequency.isEmpty()) {
            continue;
        }

        ConfigChannelEntry entry;
        entry.frequency = frequency;
        QString const modulation = modulationAt();
        entry.modulation = modulation.isEmpty() ? "am" : modulation;
        entry.label = textAt(2);
        entries.append(entry);
    }
    return entries;
}

void MainWindow::populateConfigTable(QList<ConfigChannelEntry> const& entries) {
    configChannelTable_->setRowCount(0);
    for (ConfigChannelEntry const& entry : entries) {
        int const row = configChannelTable_->rowCount();
        configChannelTable_->insertRow(row);
        configChannelTable_->setItem(row, 0, new QTableWidgetItem(entry.frequency));
        configChannelTable_->setCellWidget(row, 1, createModulationComboBox(configChannelTable_, entry.modulation));
        configChannelTable_->setItem(row, 2, new QTableWidgetItem(entry.label));
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        QVariant const freqValue = watched->property("channelFreqHz");
        if (freqValue.isValid()) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                showChannelWaterfallDialog(freqValue.toLongLong());
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}
