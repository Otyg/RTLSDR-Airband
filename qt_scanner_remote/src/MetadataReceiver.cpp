#include "MetadataReceiver.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

MetadataReceiver::MetadataReceiver(QObject* parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &MetadataReceiver::onReadyRead);
}

bool MetadataReceiver::bind(quint16 port) {
    close();
    bool ok = socket_.bind(QHostAddress::AnyIPv4, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!ok) {
        emit errorMessage(QString("Metadata bind failed on port %1: %2").arg(port).arg(socket_.errorString()));
    }
    return ok;
}

void MetadataReceiver::close() {
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.close();
    }
}

void MetadataReceiver::onReadyRead() {
    while (socket_.hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(socket_.pendingDatagramSize()));
        if (socket_.readDatagram(datagram.data(), datagram.size()) <= 0) {
            continue;
        }

        QJsonParseError parseError;
        QJsonDocument json = QJsonDocument::fromJson(datagram.trimmed(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
            emit errorMessage(QString("Metadata parse error: %1").arg(parseError.errorString()));
            continue;
        }

        QJsonObject obj = json.object();
        int device = obj.value("device").toInt(-1);
        qint64 freqHz = static_cast<qint64>(obj.value("freq_hz").toDouble(0));
        bool squelchOpen = obj.value("squelch_open").toBool(false);
        QString label = obj.value("label").toString();
        quint32 seq = static_cast<quint32>(obj.value("seq").toInt(0));
        emit metadataReceived(device, freqHz, squelchOpen, label, seq);
    }
}
