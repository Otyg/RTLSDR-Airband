#ifndef AUDIO_RECEIVER_H
#define AUDIO_RECEIVER_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>

class AudioReceiver : public QObject {
    Q_OBJECT

   public:
    explicit AudioReceiver(QObject* parent = nullptr);
    bool connectToHost(QString const& host, quint16 port, int timeoutMs = 3000);
    void close();

   signals:
    void audioChunk(QByteArray data);
    void errorMessage(QString message);

   private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);

   private:
    QTcpSocket socket_;
    QByteArray pending_;
};

#endif
