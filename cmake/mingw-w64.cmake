# Cross-compile for Windows x64 with MinGW-w64 (Debian/Ubuntu: g++-mingw-w64-x86-64-posix,
# Fedora: mingw64-gcc-c++, Arch: mingw-w64-gcc, macOS Homebrew: mingw-w64).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 tool prefix")
find_program(CMAKE_CXX_COMPILER NAMES ${MINGW_PREFIX}-g++-posix ${MINGW_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${MINGW_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
