cmake_minimum_required(VERSION 3.16)
project(OfxDatamosh LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(OFX_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/openfx/include" CACHE PATH "OpenFX headers")
option(BUILD_TOOLS "Build the offline test tools" ON)

# OpenFX bundle architecture directory name.
if(WIN32)
  set(OFX_ARCH "Win64")
elseif(APPLE)
  set(OFX_ARCH "MacOS")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(OFX_ARCH "Linux-x86-64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(OFX_ARCH "Linux-arm64")
else()
  set(OFX_ARCH "Linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(BUNDLE_NAME Datamosh.ofx.bundle)
set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/${BUNDLE_NAME})

# GPU kernels: OpenCL source and its PTX (tools/gen_ptx.sh), embedded in the plugin.
set(KERNELS_EMBED ${CMAKE_BINARY_DIR}/gen/mosh_kernels_embed.h)
add_custom_command(OUTPUT ${KERNELS_EMBED}
  COMMAND ${CMAKE_COMMAND} -DCL=${CMAKE_SOURCE_DIR}/src/mosh_kernels.cl -DPTX=${CMAKE_SOURCE_DIR}/src/mosh_kernels.ptx
          -DOUT=${KERNELS_EMBED} -P ${CMAKE_SOURCE_DIR}/cmake/embed_kernels.cmake
  DEPENDS src/mosh_kernels.cl src/mosh_kernels.ptx cmake/embed_kernels.cmake
  COMMENT "Embedding GPU kernels")

add_library(Datamosh SHARED src/datamosh_plugin.cpp src/mosh_gpu.cpp src/gpu_backend.cpp ${KERNELS_EMBED})
target_include_directories(Datamosh PRIVATE ${OFX_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/gen)
# CUDA and OpenCL drivers are loaded at run time.
target_link_libraries(Datamosh PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(Datamosh PROPERTIES
  PREFIX ""
  SUFFIX ".ofx"
  LIBRARY_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/${OFX_ARCH}
  RUNTIME_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/${OFX_ARCH})
# Multi-config generators (Visual Studio, Xcode) append the config name; keep the bundle flat.
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} CFG)
  set_target_properties(Datamosh PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${CFG} ${BUNDLE_DIR}/Contents/${OFX_ARCH}
    RUNTIME_OUTPUT_DIRECTORY_${CFG} ${BUNDLE_DIR}/Contents/${OFX_ARCH})
endforeach()

if(MSVC)
  target_compile_options(Datamosh PRIVATE /W3 /O2 /EHsc)
  target_compile_definitions(Datamosh PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(Datamosh PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE AND NOT WIN32)
  # Resolve ships its own runtime libraries; a static C++ runtime avoids
  # GLIBCXX version clashes when the host loads the plugin.
  target_link_options(Datamosh PRIVATE -static-libstdc++ -static-libgcc)
endif()
if(MINGW)
  target_link_options(Datamosh PRIVATE -static)
endif()

configure_file(${CMAKE_SOURCE_DIR}/packaging/Info.plist ${BUNDLE_DIR}/Contents/Info.plist COPYONLY)

# System-wide OpenFX plugin directory per platform.
if(WIN32)
  set(OFX_PLUGIN_DIR "C:/Program Files/Common Files/OFX/Plugins")
elseif(APPLE)
  set(OFX_PLUGIN_DIR "/Library/OFX/Plugins")
else()
  set(OFX_PLUGIN_DIR "/usr/OFX/Plugins")
endif()
install(DIRECTORY ${BUNDLE_DIR} DESTINATION ${OFX_PLUGIN_DIR} USE_SOURCE_PERMISSIONS)

if(BUILD_TOOLS)
  find_package(Threads REQUIRED)
  add_executable(gpu_selftest tools/gpu_selftest.cpp src/mosh_gpu.cpp src/gpu_backend.cpp ${KERNELS_EMBED})
  target_include_directories(gpu_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/gen)
  target_link_libraries(gpu_selftest PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if(MINGW)
    target_link_options(gpu_selftest PRIVATE -static)
  endif()
  add_executable(mosh_test tools/mosh_test.cpp)
  target_include_directories(mosh_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(mosh_test PRIVATE Threads::Threads)
  if(NOT WIN32)
    add_executable(mock_host tools/mock_host.cpp)
    target_include_directories(mock_host PRIVATE ${OFX_INCLUDE_DIR})
    target_link_libraries(mock_host PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()
