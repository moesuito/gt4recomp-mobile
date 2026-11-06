# GT4Recomp Android build (PCSX2-tainted, GPL-3.0-or-later). libgt4gs.so: the ARMSX2 GS inside the game process,
# driven by the game's GS stream through a flat C ABI (gt4gs_api.h). Built by the imported ARMSX2 CMake entry point
# (third_party/armsx2/platforms/android/app/src/main/cpp/CMakeLists.txt) with -DGT4GS_FRONTEND_DIR=<this folder>.
cmake_minimum_required(VERSION 3.22)

set(GT4GS_SOURCES
    gt4gs_host.cpp
    gt4gs_queue.cpp
    gt4gs_replayer.cpp
    gt4gs_hud.cpp
    android_stubs.cpp
    gt4gs_api.h
    gt4gs_host.h
    gt4gs_queue.h
    gt4gs_replayer.h
    gt4gs_hud.h)

add_library(gt4gs SHARED ${GT4GS_SOURCES})

set_target_properties(gt4gs PROPERTIES OUTPUT_NAME gt4gs CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

target_compile_definitions(gt4gs PRIVATE GT4GS_INPROCESS=1)
target_include_directories(gt4gs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native"
    "${ARMSX2_ROOT}"
    "${ARMSX2_ROOT}/pcsx2"
    "${CMAKE_BINARY_DIR}/common/include")

# The core's own interface flags (defines such as ENABLE_OPENGL and the page-size override, the warning set, the
# 3rdparty include paths) and the static core itself.
# A linker group: the core is one static archive whose objects reference each other (the Android stubs of
# FileSystem live in one object that the CDVD objects need), and a plain archive is scanned once.
# EGL and GLESv2: the Auto API rule probes the device's GL identity with a 1x1 pbuffer before the GS starts (TASK-186
# round 7), so ARMSX2's own per-GPU choice is made from the same strings its app would read.
target_link_libraries(gt4gs PRIVATE PCSX2_FLAGS -Wl,--start-group PCSX2 -Wl,--end-group android log EGL GLESv2)

# Vulkan (owner, 2026-11-06): the GS device compiles shaders at run time through shaderc, which the Android tree builds
# from 3rdparty/shaderc (shaderc + glslang + SPIRV-Tools + SPIRV-Headers, pinned by its DEPS). The loader itself is
# Android's libvulkan.so, opened at run time.
if(USE_VULKAN)
    target_link_libraries(gt4gs PRIVATE -Wl,--start-group shaderc -Wl,--end-group)
endif()

# 16 KB page alignment for every LOAD segment (Android 15+ devices, and the same rule the game library follows).
target_link_options(gt4gs PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)

# TASK-186 round 7: the Auto API unit test (six GPU strings through ARMSX2's own decision). Built on request and run on
# the device; never packaged in the APK.
option(GT4GS_BUILD_TESTS "Build the Auto API unit test" OFF)
if(GT4GS_BUILD_TESTS)
    add_executable(gt4gs_auto_test tests/gt4gs_auto_test.cpp)
    target_link_libraries(gt4gs_auto_test PRIVATE PCSX2_FLAGS -Wl,--start-group PCSX2 -Wl,--end-group android log)
    target_link_options(gt4gs_auto_test PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
endif()
