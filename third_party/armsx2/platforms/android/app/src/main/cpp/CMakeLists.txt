# GT4Recomp private test build (PCSX2-tainted, GPL): the ARMSX2 core as a plain shared library for the GT4Recomp
# Android app, instead of the emulator app's `emucore` JNI library. The original entry point is kept next to this file
# as CMakeLists-app-emucore.txt and is NOT used here.
#
# This file lives in the vendored ARMSX2 tree (third_party/armsx2) so that the relative `3rdparty/...` paths of the
# Android CMake modules resolve exactly as they do for the app. The upstream emulator-app entry point is kept next to
# it as CMakeLists-app-emucore.txt and is NOT used here.
cmake_minimum_required(VERSION 3.16...3.25)

cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

project(GT4GS C CXX)

get_filename_component(ARMSX2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# No JIT (owner, 2026-11-06; the project rule): the shipped library never generates machine code at run time. The
# software rasterizer takes its C++ path, no executable code memory is mapped, and the emulator's recompiler providers
# are never initialized (VMManager::InitializeCPUProviders returns early without code memory). The GS SW code generators
# (xbyak/vixl) are not compiled into this build.
set(GT4GS_NO_JIT ON CACHE BOOL "" FORCE)
add_compile_definitions(GT4GS_NO_JIT)
# NDK 26.1 is clang 17, whose AArch64 backend refuses __attribute__((preserve_all)) ("Unsupported calling convention")
# instead of falling back. The attribute exists for the recompilers, which this library never runs, so the documented
# wide-spill form is used. Defined here, with the no-JIT switch, so a clean build needs no extra compiler flags.
add_compile_definitions(GT4GS_NO_PRESERVE_ALL)

# No Qt UI, no emulator app: one shared library with the GS core and the GT4Recomp stream frontend.
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(TOP_CMAKE_WAS_SOURCED TRUE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()

include(BuildParameters)
include(SearchForStuff)

get_git_version_info()
write_svnrev_h()

add_subdirectory("${ARMSX2_ROOT}/common" "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${ARMSX2_ROOT}/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${ARMSX2_ROOT}/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")

# 7-Zip's AES code (3rdparty/lzma) takes its hardware path on arm64 and asks clang for `target("crypto")`; NDK 26's
# clang 17 refuses the always_inline NEON crypto intrinsics that way unless the translation unit itself is built with
# the crypto extension. The instructions are only reached when the CPU reports them (AesCbc_Init), and GT4 never reads
# a 7z archive, so building this one static library with +crypto is the smallest fix. Private build only.
if(ANDROID AND TARGET pcsx2-lzma)
  target_compile_options(pcsx2-lzma PRIVATE -march=armv8-a+crypto)
endif()

# The GT4Recomp in-process GS sink (our own source tree, GPL, kept with the private clone). Optional while the core
# alone is being built; a release build always passes it.
if(GT4GS_FRONTEND_DIR)
  add_subdirectory("${GT4GS_FRONTEND_DIR}" "${CMAKE_BINARY_DIR}/gt4gs")
endif()
