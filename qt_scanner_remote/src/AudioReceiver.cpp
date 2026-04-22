#include "AudioReceiver.h"

#include <QHostAddress>

AudioReceiver::AudioReceiver(QObject* parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &AudioReceiver::onReadyRead);
    connect(&socket_, &QAbstractSocket::errorOccurred, this, &AudioReceiver::onSocketError);
}

bool AudioReceiver::connectToHost(QString const& host, quint16 port, int timeoutMs) {
    close();

    if (!socket_.bind(QHostAddress::AnyIPv4, 0)) {
        emit errorMessage(QString("Audio UDP bind failed for %1:%2: %3").arg(host).arg(port).arg(socket_.errorString()));
        return false;
    }

    socket_.connectToHost(host, port, QIODeviceBase::ReadWrite);
    if (!socket_.waitForConnected(timeoutMs)) {
        emit errorMessage(QString("Audio UDP setup failed for %1:%2: %3").arg(host).arg(port).arg(socket_.errorString()));
        return false;
    }

    static QByteArray const kInitialPacket("start");
    qint64 const bytesWritten = socket_.write(kInitialPacket);
    if (bytesWritten != kInitialPacket.size()) {
        emit errorMessage(QString("Audio UDP init packet failed for %1:%2: %3").arg(host).arg(port).arg(socket_.errorString()));
        return false;
    }

    socket_.flush();
    return true;
}

void AudioReceiver::close() {
    socket_.disconnectFromHost();
    socket_.close();
    pending_.clear();
}

void AudioReceiver::onReadyRead() {
    while (socket_.hasPendingDatagrams()) {
        qint64 const datagramSize = socket_.pendingDatagramSize();
        if (datagramSize <= 0) {
            socket_.readDatagram(nullptr, 0);
            continue;
        }

        QByteArray datagram;
        datagram.resize(static_cast<int>(datagramSize));
        qint64 const received = socket_.readDatagram(datagram.data(), datagram.size());
        if (received <= 0) {
            continue;
        }
        datagram.resize(static_cast<int>(received));
        pending_.append(datagram);
    }

    int const frameBytes = static_cast<int>(sizeof(float));
    int const usable = pending_.size() - (pending_.size() % frameBytes);
    if (usable > 0) {
        emit audioChunk(pending_.left(usable));
        pending_.remove(0, usable);
    }
}

void AudioReceiver::onSocketError(QAbstractSocket::SocketError socketError) {
    Q_UNUSED(socketError);
    emit errorMessage(QString("Audio socket error: %1").arg(socket_.errorString()));
}
