#ifndef AUDIO_RECEIVER_H
#define AUDIO_RECEIVER_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUdpSocket>

class AudioReceiver : public QObject {
    Q_OBJECT

   public:
    explicit AudioReceiver(QObject* parent = nullptr);
    bool bind(quint16 port);
    void close();

   signals:
    void audioDatagram(QByteArray data);
    void errorMessage(QString message);

   private slots:
    void onReadyRead();

   private:
    QUdpSocket socket_;
};

#endif
