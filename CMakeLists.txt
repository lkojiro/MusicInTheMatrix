cmake_minimum_required(VERSION 3.16)
project(MusicInTheMatrix LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Curses REQUIRED)
find_package(PortAudio REQUIRED)
find_package(Threads REQUIRED)

# KissFFT (vendored, BSD-licensed, pinned to release v131 -- see
# third_party/kissfft/COPYING). Only the real-input variant is built since
# audio samples are real-valued.
add_library(kissfft STATIC
    third_party/kissfft/kiss_fft.c
    third_party/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC third_party/kissfft)

add_executable(mitm
    src/main.cpp
    src/args.cpp
    src/host.cpp
    src/subordinate.cpp
    src/controller.cpp
    src/host_discovery.cpp
    src/audio_capture.cpp
    src/fft_processor.cpp
    src/terminal_renderer.cpp
    src/matrix_rain_renderer.cpp
    src/checkerboard_renderer.cpp
    src/beat_detector.cpp
    src/curses_util.cpp
    src/color_scheme.cpp
    src/global_config.cpp
    src/unix_socket.cpp
    src/window_launcher.cpp
)
target_include_directories(mitm PRIVATE
    include
    third_party/json # provides <nlohmann/json.hpp>
    ${CURSES_INCLUDE_DIR}
    ${PortAudio_INCLUDE_DIRS}
)
target_link_libraries(mitm PRIVATE
    kissfft
    ${CURSES_LIBRARIES}
    ${PortAudio_LIBRARIES}
    Threads::Threads
)
