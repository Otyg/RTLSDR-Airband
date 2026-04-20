#include "AudioReceiver.h"

#include <QHostAddress>

AudioReceiver::AudioReceiver(QObject* parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &AudioReceiver::onReadyRead);
}

bool AudioReceiver::bind(quint16 port) {
    close();
    bool ok = socket_.bind(QHostAddress::AnyIPv4, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!ok) {
        emit errorMessage(QString("Audio bind failed on port %1: %2").arg(port).arg(socket_.errorString()));
    }
    return ok;
}

void AudioReceiver::close() {
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.close();
    }
}

void AudioReceiver::onReadyRead() {
    while (socket_.hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(socket_.pendingDatagramSize()));
        if (socket_.readDatagram(datagram.data(), datagram.size()) > 0) {
            emit audioDatagram(datagram);
        }
    }
}
