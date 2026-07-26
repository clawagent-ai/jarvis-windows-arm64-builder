# Builds the two JARVIS-owned native tools for Windows ARM64.
#
# whisper-cli.exe is not built here: it comes from the whisper.cpp tree itself, at the exact
# commit recorded in whisper-manifest.json. This file only covers the sources JARVIS ships.
#
# Configure with the ARM64 platform of a Visual Studio generator, for example:
#   cmake -S build/windows-arm64 -B out -A ARM64 -DWHISPER_INSTALL_DIR=<whisper install prefix>
#   cmake --build out --config Release
#
# clap-vad needs the installed whisper.cpp package; jarvis-windows-helper needs only the
# Windows SDK, so it still configures when WHISPER_INSTALL_DIR is absent.

cmake_minimum_required(VERSION 3.21)
project(jarvis_windows_arm64_tools CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT keeps the executables self-contained, matching the shipped win32-x64 build
# (whisper-manifest.json records runtimeLibraries: static). Without this the tools would
# need the Visual C++ redistributable on the user's machine.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  message(FATAL_ERROR
    "This project must be configured with -A ARM64. Current platform: '${CMAKE_GENERATOR_PLATFORM}'. "
    "An x64 build must never be shipped as the Windows ARM64 native tool.")
endif()

set(JARVIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ---------------------------------------------------------------------------
# clap-vad.exe
# ---------------------------------------------------------------------------
if(DEFINED WHISPER_INSTALL_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${WHISPER_INSTALL_DIR}")
endif()

find_package(whisper CONFIG REQUIRED)

add_executable(clap-vad "${JARVIS_ROOT}/native/clap-vad.cpp")
# The whisper package pulls in the ggml targets transitively, which is where
# ggml-backend.h and ggml_backend_load_all come from.
target_link_libraries(clap-vad PRIVATE whisper)
set_target_properties(clap-vad PROPERTIES OUTPUT_NAME "clap-vad")

# ---------------------------------------------------------------------------
# jarvis-windows-helper.exe
# ---------------------------------------------------------------------------
add_executable(jarvis-windows-helper
  "${JARVIS_ROOT}/platform/windows/native/jarvis-windows-helper.cpp")

# The source declares no #pragma comment(lib, ...), so every Windows library it uses is
# listed explicitly here:
#   gdiplus  - GdiplusStartup, Bitmap, GetImageEncoders (screenshot)
#   ole32    - CoInitializeEx, CoCreateInstance (audio endpoint volume)
#   oleaut32 - COM automation types used by the audio interfaces
#   user32   - SetForegroundWindow and the window actions
#   advapi32 - process access helpers
target_link_libraries(jarvis-windows-helper PRIVATE
  gdiplus
  ole32
  oleaut32
  user32
  advapi32)

# <windows.h> defines min and max as macros, which rewrites the std::max/std::min calls in
# volume_action() into a parse error under clang-cl. NOMINMAX suppresses those macros and
# keeps the source itself byte-identical to the copy in the JARVIS distribution.
target_compile_definitions(jarvis-windows-helper PRIVATE NOMINMAX)

# The entry point is wmain, so this must stay a console subsystem application.
set_target_properties(jarvis-windows-helper PROPERTIES
  OUTPUT_NAME "jarvis-windows-helper"
  WIN32_EXECUTABLE FALSE)
