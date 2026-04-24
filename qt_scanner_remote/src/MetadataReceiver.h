#ifndef METADATA_RECEIVER_H
#define METADATA_RECEIVER_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>

class MetadataReceiver : public QObject {
    Q_OBJECT

   public:
    explicit MetadataReceiver(QObject* parent = nullptr);
    bool connectToHost(QString const& host, quint16 port, int timeoutMs = 3000);
    void close();

   signals:
    void channelsReceived(QList<qint64> freqsHz, QStringList labels);
    void metadataReceived(int device, qint64 freqHz, bool squelchOpen, QString label, quint32 seq);
    void decodedMessageReceived(int device, qint64 freqHz, QString label, QString modulation, QString msgType, bool crcOk, int mmsi, QString payload, quint32 seq);
    void errorMessage(QString message);

   private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);

   private:
    QTcpSocket socket_;
    QByteArray pending_;
};

#endif
