cmake_minimum_required(VERSION 3.16)
project(qt_scanner_remote LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Define release directory for packaging
set(RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RELEASE_DIR}")

include(FetchContent)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia)

option(QT_SCANNER_REMOTE_RNNOISE_MARCH_NATIVE "Build RNNoise with -march=native for better local CPU optimizations" ON)
set(QT_SCANNER_REMOTE_RNNOISE_URL "https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz" CACHE STRING "Official RNNoise release tarball URL")

add_executable(qt_scanner_remote
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/Theme.h
    src/AudioReceiver.h
    src/AudioReceiver.cpp
    src/MetadataReceiver.h
    src/MetadataReceiver.cpp
    src/Mp3FilePlayer.h
    src/Mp3FilePlayer.cpp
    src/AudioEngine.h
    src/AudioEngine.cpp
    src/NoiseReduction.h
    src/NoiseReduction.cpp
    src/WaveformWidget.h
    src/WaveformWidget.cpp
    src/WaterfallWidget.h
    src/WaterfallWidget.cpp
    src/rnnoise/rnnoise_wrapper.h
)

FetchContent_Declare(rnnoise_external
    URL ${QT_SCANNER_REMOTE_RNNOISE_URL}
)
FetchContent_GetProperties(rnnoise_external)
if(NOT rnnoise_external_POPULATED)
    FetchContent_Populate(rnnoise_external)
endif()

set(RNNOISE_EXTERNAL_SOURCES
    "${rnnoise_external_SOURCE_DIR}/src/celt_lpc.c"
    "${rnnoise_external_SOURCE_DIR}/src/denoise.c"
    "${rnnoise_external_SOURCE_DIR}/src/kiss_fft.c"
    "${rnnoise_external_SOURCE_DIR}/src/nnet.c"
    "${rnnoise_external_SOURCE_DIR}/src/nnet_default.c"
    "${rnnoise_external_SOURCE_DIR}/src/parse_lpcnet_weights.c"
    "${rnnoise_external_SOURCE_DIR}/src/pitch.c"
    "${rnnoise_external_SOURCE_DIR}/src/rnn.c"
    "${rnnoise_external_SOURCE_DIR}/src/rnnoise_data.c"
    "${rnnoise_external_SOURCE_DIR}/src/rnnoise_tables.c"
)

add_library(rnnoise STATIC ${RNNOISE_EXTERNAL_SOURCES})
target_compile_definitions(rnnoise PRIVATE COMPILE_OPUS RNNOISE_BUILD NO_OPTIMIZATIONS)
target_include_directories(rnnoise
    PUBLIC "${rnnoise_external_SOURCE_DIR}/include"
    PRIVATE "${rnnoise_external_SOURCE_DIR}/src"
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rnnoise PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-cpp>)
endif()
target_link_libraries(qt_scanner_remote PRIVATE rnnoise)

target_link_libraries(qt_scanner_remote PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)

target_include_directories(qt_scanner_remote PRIVATE src/rnnoise)

# Post-build: Deploy Qt dependencies to release directory
add_custom_command(TARGET qt_scanner_remote POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Deploying Qt libraries to ${RELEASE_DIR}..."
    COMMAND "C:/qt/6.11.0/mingw_64/bin/windeployqt.exe" --release "$<TARGET_FILE:qt_scanner_remote>"
    COMMENT "Running windeployqt to bundle Qt libraries"
)
