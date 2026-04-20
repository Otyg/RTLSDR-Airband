cmake_minimum_required(VERSION 3.16)
project(qt_scanner_remote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia)

add_executable(qt_scanner_remote
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/AudioReceiver.h
    src/AudioReceiver.cpp
    src/MetadataReceiver.h
    src/MetadataReceiver.cpp
    src/AudioEngine.h
    src/AudioEngine.cpp
    src/WaveformWidget.h
    src/WaveformWidget.cpp
)

target_link_libraries(qt_scanner_remote PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)
