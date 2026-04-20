#include <QApplication>
#include <QFile>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    // In WSLg, GUI apps may be launched without PULSE_SERVER exported in the current shell.
    // Auto-wire PulseAudio socket to keep audio output reliable across launches.
    if (qEnvironmentVariableIsEmpty("PULSE_SERVER") && QFile::exists("/mnt/wslg/PulseServer")) {
        qputenv("PULSE_SERVER", QByteArray("unix:/mnt/wslg/PulseServer"));
    }

    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
