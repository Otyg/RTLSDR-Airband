#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>

#include "AudioEngine.h"
#include "AudioReceiver.h"
#include "MetadataReceiver.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QSpinBox;
class QSlider;
class QLineEdit;
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

   private:
    void setStatus(QString status);

    AudioReceiver audioReceiver_;
    MetadataReceiver metadataReceiver_;
    AudioEngine audioEngine_;

    QLabel* statusValue_;
    QLabel* freqValue_;
    QLabel* labelValue_;
    QLabel* squelchValue_;
    QLabel* seqValue_;
    QLineEdit* backendHost_;
    QSpinBox* audioPort_;
    QSpinBox* metadataPort_;
    QSlider* volume_;
    QPushButton* startButton_;
    QPushButton* stopButton_;
};

#endif
