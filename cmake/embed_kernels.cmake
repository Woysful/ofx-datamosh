# Writes OUT: the OpenCL source CL and the PTX of the same kernels PTX as
# NUL-terminated byte arrays kMoshKernelsCl and kMoshKernelsPtx.
# Run as: cmake -DCL=... -DPTX=... -DOUT=... -P embed_kernels.cmake
file(SHA256 "${CL}" cl_hash)
file(STRINGS "${PTX}" ptx_tag LIMIT_COUNT 1 REGEX "mosh_kernels.cl sha256")
if(NOT ptx_tag MATCHES "${cl_hash}")
  message(WARNING "src/mosh_kernels.ptx was not generated from this src/mosh_kernels.cl; "
                  "the CUDA path runs the old kernels. Regenerate it with tools/gen_ptx.sh.")
endif()

function(byte_array file var out)
  file(READ "${file}" hex HEX)
  set(row "")
  foreach(i RANGE 15)
    string(APPEND row "[0-9a-f][0-9a-f]")
  endforeach()
  string(REGEX REPLACE "(${row})" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  set(${out} "static const unsigned char ${var}[] = {\n${hex}0x00};\nstatic const size_t ${var}Size = sizeof(${var}) - 1;\n" PARENT_SCOPE)
endfunction()

byte_array("${CL}" kMoshKernelsCl cl_array)
byte_array("${PTX}" kMoshKernelsPtx ptx_array)
file(WRITE "${OUT}" "// Generated from src/mosh_kernels.cl and src/mosh_kernels.ptx by cmake/embed_kernels.cmake.\n#pragma once\n#include <cstddef>\n${cl_array}${ptx_array}")
