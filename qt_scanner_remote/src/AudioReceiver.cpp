#include "AudioReceiver.h"

AudioReceiver::AudioReceiver(QObject* parent) : QObject(parent) {
    connect(&socket_, &QTcpSocket::readyRead, this, &AudioReceiver::onReadyRead);
    connect(&socket_, &QAbstractSocket::errorOccurred, this, &AudioReceiver::onSocketError);
}

bool AudioReceiver::connectToHost(QString const& host, quint16 port, int timeoutMs) {
    close();
    socket_.connectToHost(host, port);
    if (!socket_.waitForConnected(timeoutMs)) {
        emit errorMessage(QString("Audio connect failed to %1:%2: %3").arg(host).arg(port).arg(socket_.errorString()));
        return false;
    }
    return true;
}

void AudioReceiver::close() {
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.disconnectFromHost();
        if (socket_.state() != QAbstractSocket::UnconnectedState) {
            socket_.waitForDisconnected(200);
        }
    }
    pending_.clear();
}

void AudioReceiver::onReadyRead() {
    pending_.append(socket_.readAll());
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
