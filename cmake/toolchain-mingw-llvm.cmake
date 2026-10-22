# Cross-compiling Patina for Windows x86_64 from Linux with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw - prebuilt releases, or built from
# its scripts with a system clang as scripts/build-windows.sh does).
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-llvm.cmake -DLLVM_MINGW=/opt/llvm-mingw
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW)
  set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
if(NOT LLVM_MINGW)
  set(LLVM_MINGW /opt/llvm-mingw)
endif()
set(LLVM_MINGW ${LLVM_MINGW} CACHE PATH "llvm-mingw toolchain root")
set(_t x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/${_t}-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/${_t}-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/${_t}-windres)
set(CMAKE_AR ${LLVM_MINGW}/bin/llvm-ar)
set(CMAKE_RANLIB ${LLVM_MINGW}/bin/llvm-ranlib)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/${_t})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
