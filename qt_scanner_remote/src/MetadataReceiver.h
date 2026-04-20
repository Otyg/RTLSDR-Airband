#ifndef METADATA_RECEIVER_H
#define METADATA_RECEIVER_H

#include <QObject>
#include <QString>
#include <QUdpSocket>

class MetadataReceiver : public QObject {
    Q_OBJECT

   public:
    explicit MetadataReceiver(QObject* parent = nullptr);
    bool bind(quint16 port);
    void close();

   signals:
    void metadataReceived(int device, qint64 freqHz, bool squelchOpen, QString label, quint32 seq);
    void errorMessage(QString message);

   private slots:
    void onReadyRead();

   private:
    QUdpSocket socket_;
};

#endif
