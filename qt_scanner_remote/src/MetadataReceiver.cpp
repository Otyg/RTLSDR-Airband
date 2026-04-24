#include "MetadataReceiver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

MetadataReceiver::MetadataReceiver(QObject* parent) : QObject(parent) {
    connect(&socket_, &QTcpSocket::readyRead, this, &MetadataReceiver::onReadyRead);
    connect(&socket_, &QAbstractSocket::errorOccurred, this, &MetadataReceiver::onSocketError);
}

bool MetadataReceiver::connectToHost(QString const& host, quint16 port, int timeoutMs) {
    close();
    socket_.connectToHost(host, port);
    if (!socket_.waitForConnected(timeoutMs)) {
        emit errorMessage(QString("Metadata connect failed to %1:%2: %3").arg(host).arg(port).arg(socket_.errorString()));
        return false;
    }
    return true;
}

void MetadataReceiver::close() {
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.disconnectFromHost();
        if (socket_.state() != QAbstractSocket::UnconnectedState) {
            socket_.waitForDisconnected(200);
        }
    }
    pending_.clear();
}

void MetadataReceiver::onReadyRead() {
    pending_.append(socket_.readAll());
    int newlinePos = -1;
    while ((newlinePos = pending_.indexOf('\n')) != -1) {
        QByteArray line = pending_.left(newlinePos).trimmed();
        pending_.remove(0, newlinePos + 1);
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError parseError;
        QJsonDocument json = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
            emit errorMessage(QString("Metadata parse error: %1").arg(parseError.errorString()));
            continue;
        }

        QJsonObject obj = json.object();
        QString msgType = obj.value("type").toString();
        if (msgType == "channels") {
            QList<qint64> freqsHz;
            QStringList labels;
            QJsonArray channels = obj.value("channels").toArray();
            for (QJsonValue const& chan : channels) {
                if (!chan.isObject()) {
                    continue;
                }
                QJsonObject c = chan.toObject();
                freqsHz.append(static_cast<qint64>(c.value("freq_hz").toDouble(0)));
                labels.append(c.value("label").toString());
            }
            emit channelsReceived(freqsHz, labels);
            continue;
        }

        QString eventType = obj.value("event").toString();
        if (eventType == "decoded") {
            int device = obj.value("device").toInt(-1);
            qint64 freqHz = static_cast<qint64>(obj.value("freq_hz").toDouble(0));
            QString label = obj.value("label").toString();
            QString modulation = obj.value("modulation").toString();
            QString decodedType = obj.value("msg_type").toString();
            bool crcOk = obj.value("crc_ok").toBool(false);
            int mmsi = obj.value("mmsi").toInt(-1);
            QString payload = obj.value("payload").toString();
            quint32 seq = static_cast<quint32>(obj.value("seq").toInt(0));
            emit decodedMessageReceived(device, freqHz, label, modulation, decodedType, crcOk, mmsi, payload, seq);
            continue;
        }

        int device = obj.value("device").toInt(-1);
        qint64 freqHz = static_cast<qint64>(obj.value("freq_hz").toDouble(0));
        bool squelchOpen = obj.value("squelch_open").toBool(false);
        QString label = obj.value("label").toString();
        quint32 seq = static_cast<quint32>(obj.value("seq").toInt(0));
        emit metadataReceived(device, freqHz, squelchOpen, label, seq);
    }
}

void MetadataReceiver::onSocketError(QAbstractSocket::SocketError socketError) {
    Q_UNUSED(socketError);
    emit errorMessage(QString("Metadata socket error: %1").arg(socket_.errorString()));
}
