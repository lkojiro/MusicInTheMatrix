# Locates PortAudio (v19), typically installed via Homebrew.
#
# CMake doesn't ship a FindPortAudio module and pkg-config isn't guaranteed
# to be present, so this does a plain find_path/find_library search across
# the usual Homebrew prefixes (Apple Silicon and Intel) plus system paths.
#
# Defines:
#   PortAudio_FOUND
#   PortAudio_INCLUDE_DIRS
#   PortAudio_LIBRARIES

find_path(PortAudio_INCLUDE_DIR
    NAMES portaudio.h
    PATHS /opt/homebrew/include /usr/local/include /usr/include
)

find_library(PortAudio_LIBRARY
    NAMES portaudio
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PortAudio
    REQUIRED_VARS PortAudio_LIBRARY PortAudio_INCLUDE_DIR
)

if(PortAudio_FOUND)
    set(PortAudio_INCLUDE_DIRS ${PortAudio_INCLUDE_DIR})
    set(PortAudio_LIBRARIES ${PortAudio_LIBRARY})
endif()

mark_as_advanced(PortAudio_INCLUDE_DIR PortAudio_LIBRARY)
