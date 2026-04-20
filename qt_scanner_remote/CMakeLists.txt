cmake_minimum_required(VERSION 3.16)
project(qt_scanner_remote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Define release directory for packaging
set(RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RELEASE_DIR}")

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
    src/WaterfallWidget.h
    src/WaterfallWidget.cpp
)

target_link_libraries(qt_scanner_remote PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)

# Post-build: Deploy Qt dependencies to release directory
add_custom_command(TARGET qt_scanner_remote POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Deploying Qt libraries to ${RELEASE_DIR}..."
    COMMAND "C:/qt/6.11.0/mingw_64/bin/windeployqt.exe" --release "$<TARGET_FILE:qt_scanner_remote>"
    COMMENT "Running windeployqt to bundle Qt libraries"
)
