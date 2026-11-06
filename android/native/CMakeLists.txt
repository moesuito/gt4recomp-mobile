# TASK-180 (docs/ANDROID-PORT-PLAN.md stage A2): libgt4.so, the whole game for Android in one shared library.
#
# The same sources and pinned capsules as the Windows executable (native/CMakeLists.txt, the v2.00 rc14 recipe), with
# the Android forms of the platform seam (native/platform/platform_posix.cpp, window_sdl.cpp, audio_sdl.cpp,
# frame_clock_posix.cpp, android_main.cpp) and CoreGS 2.0 as the only renderer. The SDL3 3.4.18 Android package (the
# official AAR, unpacked) gives libSDL3.so and its headers; the Java side is the same AAR in android/app.
#
# Configure with android/native/configure-android.cmd (it sets the NDK toolchain, the ABI and the inputs). Nothing
# game-derived is committed: the units, capsules and generated VU code are read from private/ folders or a build folder.
cmake_minimum_required(VERSION 3.22)
project(GT4RecompAndroid LANGUAGES C CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "android/native is the Android build; configure it with the NDK toolchain (configure-android.cmd)")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit ABIs (x86_64 for the emulator, arm64-v8a for phones)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(GT4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NATIVE "${GT4_ROOT}/native")

# ---- Inputs (all outside git) --------------------------------------------------------------------------------------
set(GT4_ASSETS_ROOT "" CACHE PATH "The main checkout with the verified private/ folders")
set(GT4_TARGET "v200" CACHE STRING "Game target: v200 (Spec II later)")
set(GT4_EE_UNITS_DIR "" CACHE PATH "Translated EE units of the target (unit_XXXX.cpp, table.cpp, units.hpp)")
set(GT4_EE_UNITS_MANIFEST "" CACHE FILEPATH "Hook manifest of the units (camera360-units JSON): changed units are checked")
set(GT4_PORT_DIR "" CACHE PATH "Pinned donor adapter (private/slice200-port)")
set(GT4_PERF_RUNTIME_SOURCE "" CACHE PATH "Runtime capsule (private/slice262-frame-cost-runtime)")
set(GT4_LICENSE_GUARD_SOURCE "" CACHE PATH "License guard capsule (private/slice376-...-license-guard-runtime)")
set(GT4_MEMCARD_RUNTIME_SOURCE "" CACHE PATH "Memory card capsule (private/slice200-memcard-runtime)")
set(GT4_IPU_RUNTIME_SOURCE "" CACHE PATH "IPU capsule (private/slice365-ipu-fast-runtime)")
set(GT4_SPU2_RUNTIME_SOURCE "" CACHE PATH "SPU2 capsule (private/slice396-spu2-runtime)")
set(GT4_VU1_GENERATED_DIR "" CACHE PATH "VU1 AOT emitter output (private/slice397-vu1-gen)")
set(GT4_VU0_GENERATED_DIR "" CACHE PATH "VU0 AOT emitter output (private/slice394-v200-vu0-gen)")
set(GT4_HW_SHADER_CPP_DIR "" CACHE PATH "CoreGS 2.0 shaders embedded as C++ (<Windows build>/native/hw_gs/shaders/*_spirv.cpp)")
set(GT4_VULKAN_INCLUDE "" CACHE PATH "Khronos Vulkan headers (the Windows build's Vulkan SDK Include folder)")
set(GT4_SDL3_ANDROID_AAR_DIR "" CACHE PATH "Unpacked SDL3 3.4.18 AAR (private/tooling/sdl3-3.4.18-android/aar)")
set(GT4_PYTHON "" CACHE FILEPATH "Host Python for scripts/prepare_fast_headers.py")
foreach(required IN ITEMS GT4_ASSETS_ROOT GT4_EE_UNITS_DIR GT4_PORT_DIR GT4_PERF_RUNTIME_SOURCE GT4_LICENSE_GUARD_SOURCE
                          GT4_MEMCARD_RUNTIME_SOURCE GT4_IPU_RUNTIME_SOURCE GT4_SPU2_RUNTIME_SOURCE GT4_VU1_GENERATED_DIR
                          GT4_VU0_GENERATED_DIR GT4_HW_SHADER_CPP_DIR GT4_VULKAN_INCLUDE GT4_SDL3_ANDROID_AAR_DIR GT4_PYTHON)
    if(NOT ${required})
        message(FATAL_ERROR "android/native needs -D${required}=...; see configure-android.cmd")
    endif()
endforeach()
if(NOT GT4_TARGET STREQUAL "v200" AND NOT GT4_TARGET STREQUAL "spec2")
    message(FATAL_ERROR "android/native builds the v2.00 (GT4_TARGET=v200) and Spec II (GT4_TARGET=spec2) game libraries")
endif()
set(DONOR "${GT4_ASSETS_ROOT}/private/references/ps2-web-recomp")
set(PORT "${GT4_PORT_DIR}")

# ---- Determinism and ABI flags (Android plan section 3, Linux plan "determinism foundation") ------------------------
# -ffp-contract=off: arm64 always has FMA, and a fused x*x+y*y+z*z changes VU results; pinned everywhere.
# -fno-strict-aliasing: clang-cl, which builds the Windows executable, never uses type-based alias analysis; the
#   translated code and the donor runtime are only qualified under that rule.
# -fsigned-char: char is signed on Windows and x86-64; arm64 Android makes it unsigned by default.
add_compile_options(-ffp-contract=off -fno-strict-aliasing -fsigned-char)
# Line tables only, as the Windows build (/Z7 -gline-tables-only): stack traces keep file and line, and the build folders
# stay about a third of the size with the NDK's default full -g (which the toolchain file adds before these options).
add_compile_options(-gline-tables-only)
add_compile_definitions(GT4_MTVU_RUNTIME _CRT_SECURE_NO_WARNINGS)
if(ANDROID_ABI STREQUAL "x86_64")
    add_compile_options(-msse4.1)  # the Windows floor (SSE4.1); the x86-64 Android ABI guarantees SSE4.2
endif()
# Android 15 and later phones may use 16 KB pages: every native library is aligned for them.
set(GT4_ANDROID_LINK_FLAGS -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,--no-undefined)

# ---- Pin checks (the same manifests as the Windows build; verification never updates a pin) -------------------------
function(gt4_require_sha256 file expected what)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${what}: missing ${file}")
    endif()
    file(SHA256 "${file}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${what}: ${file} differs from its pin")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
endfunction()

execute_process(COMMAND git -C "${DONOR}" rev-parse HEAD OUTPUT_VARIABLE donor_head OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git -C "${DONOR}" status --porcelain --untracked-files=all OUTPUT_VARIABLE donor_dirty
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT donor_head STREQUAL "1b4808793a12c45125262fd2d5f50a9ec9f7c020" OR donor_dirty)
    message(FATAL_ERROR "The donor runtime must be the pinned clean checkout")
endif()

get_filename_component(perf_capsule_name "${GT4_PERF_RUNTIME_SOURCE}" NAME)
file(READ "${GT4_ROOT}/docs/inputs/${perf_capsule_name}.json" perf_manifest)
string(JSON perf_receipt GET "${perf_manifest}" receipt_sha256)
gt4_require_sha256("${GT4_PERF_RUNTIME_SOURCE}/runtime.json" "${perf_receipt}" "runtime capsule receipt")
set(perf_sources system.cpp vif.cpp vu_micro.cpp gif.cpp memory.cpp hw.cpp dmac.cpp)
foreach(source IN LISTS perf_sources)
    string(JSON expected GET "${perf_manifest}" source_sha256 "${source}")
    gt4_require_sha256("${GT4_PERF_RUNTIME_SOURCE}/${source}" "${expected}" "runtime capsule")
endforeach()
string(JSON port_count LENGTH "${perf_manifest}" inputs PORT)
math(EXPR port_last "${port_count} - 1")
foreach(index RANGE 0 ${port_last})
    string(JSON port_name MEMBER "${perf_manifest}" inputs PORT ${index})
    string(JSON expected GET "${perf_manifest}" inputs PORT "${port_name}")
    gt4_require_sha256("${PORT}/${port_name}" "${expected}" "donor adapter (port)")
endforeach()

get_filename_component(guard_name "${GT4_LICENSE_GUARD_SOURCE}" NAME)
file(READ "${GT4_ROOT}/docs/inputs/${guard_name}.json" guard_manifest)
foreach(source IN ITEMS cdvdfsv.cpp dispatch.cpp kernel.cpp)
    string(JSON expected GET "${guard_manifest}" source_sha256 "${source}")
    gt4_require_sha256("${GT4_LICENSE_GUARD_SOURCE}/${source}" "${expected}" "license guard capsule")
endforeach()

foreach(capsule IN ITEMS "MEMCARD;sio2.cpp;sio2_sha256" "IPU;ipu.cpp;ipu_sha256" "SPU2;spu2.cpp;spu2_sha256")
    list(GET capsule 0 kind)
    list(GET capsule 1 source)
    list(GET capsule 2 key)
    file(READ "${GT4_${kind}_RUNTIME_SOURCE}/runtime.json" capsule_receipt)
    string(JSON expected GET "${capsule_receipt}" "${key}")
    gt4_require_sha256("${GT4_${kind}_RUNTIME_SOURCE}/${source}" "${expected}" "${kind} capsule")
endforeach()

# The translated units: the changed units of the hook manifest must match it; the rest are reported by a combined hash.
file(GLOB ee_unit_sources "${GT4_EE_UNITS_DIR}/*.cpp")
list(SORT ee_unit_sources)
list(LENGTH ee_unit_sources ee_unit_count)
if(ee_unit_count LESS 300 OR NOT EXISTS "${GT4_EE_UNITS_DIR}/table.cpp" OR NOT EXISTS "${GT4_EE_UNITS_DIR}/units.hpp")
    message(FATAL_ERROR "GT4_EE_UNITS_DIR does not hold a translated unit set")
endif()
if(GT4_EE_UNITS_MANIFEST)
    file(READ "${GT4_EE_UNITS_MANIFEST}" units_manifest)
    string(JSON changed_count LENGTH "${units_manifest}" units)
    math(EXPR changed_last "${changed_count} - 1")
    foreach(index RANGE 0 ${changed_last})
        string(JSON unit_name GET "${units_manifest}" units ${index} unit)
        string(JSON unit_hash GET "${units_manifest}" units ${index} sha256_after)
        gt4_require_sha256("${GT4_EE_UNITS_DIR}/${unit_name}" "${unit_hash}" "hooked EE unit")
    endforeach()
endif()
set(units_digest_text "")
foreach(unit IN LISTS ee_unit_sources)
    file(SHA256 "${unit}" unit_hash)
    get_filename_component(unit_name "${unit}" NAME)
    string(APPEND units_digest_text "${unit_hash}  ${unit_name}\n")
endforeach()
string(SHA256 units_digest "${units_digest_text}")
file(WRITE "${CMAKE_BINARY_DIR}/ee-units.sha256" "${units_digest_text}")
message(STATUS "EE units: ${ee_unit_count} files, combined SHA-256 ${units_digest} (list in ee-units.sha256)")

# ---- SDL3 3.4.18 (official Android package, prefab layout) -----------------------------------------------------------
set(SDL3_ANDROID_LIB "${GT4_SDL3_ANDROID_AAR_DIR}/prefab/modules/SDL3-shared/libs/android.${ANDROID_ABI}/libSDL3.so")
set(SDL3_ANDROID_INCLUDE "${GT4_SDL3_ANDROID_AAR_DIR}/prefab/modules/SDL3-Headers/include")
if(NOT EXISTS "${SDL3_ANDROID_LIB}" OR NOT EXISTS "${SDL3_ANDROID_INCLUDE}/SDL3/SDL.h")
    message(FATAL_ERROR "SDL3 Android package missing for ${ANDROID_ABI}: ${SDL3_ANDROID_LIB}")
endif()
file(READ "${GT4_SDL3_ANDROID_AAR_DIR}/.git-hash" sdl3_git_hash)
string(STRIP "${sdl3_git_hash}" sdl3_git_hash)
if(NOT sdl3_git_hash STREQUAL "829a65d769d935c4852f8159e964312c0957260a")
    message(FATAL_ERROR "SDL3 Android package is not release 3.4.18 (git ${sdl3_git_hash})")
endif()
add_library(gt4_sdl3 SHARED IMPORTED)
set_target_properties(gt4_sdl3 PROPERTIES IMPORTED_LOCATION "${SDL3_ANDROID_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL3_ANDROID_INCLUDE}")

# ---- The donor's fast headers (TASK-134), shared by the units and the runtime ----------------------------------------
set(GT4_FAST_HEADERS_DIR "${CMAKE_BINARY_DIR}/fast_headers")
execute_process(COMMAND "${GT4_PYTHON}" -I "${GT4_ROOT}/scripts/prepare_fast_headers.py" "${DONOR}" "${GT4_FAST_HEADERS_DIR}"
                RESULT_VARIABLE fast_headers_result)
if(NOT fast_headers_result EQUAL 0)
    message(FATAL_ERROR "scripts/prepare_fast_headers.py failed")
endif()

# ---- The game: translated EE code ----------------------------------------------------------------------------------
add_library(ps2game STATIC ${ee_unit_sources})
target_include_directories(ps2game PRIVATE "${GT4_FAST_HEADERS_DIR}" "${GT4_EE_UNITS_DIR}" "${DONOR}/runtime/include")
target_compile_options(ps2game PRIVATE -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)

# ---- Native libraries (same split and definitions as native/CMakeLists.txt) ----------------------------------------
add_library(gt4_platform STATIC "${NATIVE}/platform/platform_posix.cpp")
target_include_directories(gt4_platform PUBLIC "${NATIVE}")
target_compile_definitions(gt4_platform PRIVATE GT4_PLATFORM_SDL3)
target_link_libraries(gt4_platform PUBLIC gt4_sdl3 log)

add_library(gt4_pacing STATIC "${NATIVE}/pacing.cpp")
add_library(gt4_quickfix STATIC "${NATIVE}/frame_skip.cpp" "${NATIVE}/window_mode.cpp" "${NATIVE}/race_display.cpp"
    "${NATIVE}/race_widescreen.cpp" "${NATIVE}/cursor_autohide.cpp" "${NATIVE}/display_aspect.cpp"
    "${NATIVE}/flare_limits.cpp" "${NATIVE}/camera_360.cpp" "${NATIVE}/fov_adjust.cpp" "${NATIVE}/trigger_range.cpp")
target_link_libraries(gt4_quickfix PUBLIC gt4_pacing)
add_library(gt4_install_paths STATIC "${NATIVE}/install_paths.cpp")
target_include_directories(gt4_install_paths PUBLIC "${NATIVE}")
target_link_libraries(gt4_install_paths PUBLIC gt4_platform)
add_library(gt4_memcard STATIC "${NATIVE}/memcard_store.cpp")
add_library(gt4_license_guard STATIC "${NATIVE}/license_guard.cpp")
target_include_directories(gt4_license_guard PUBLIC "${NATIVE}")
add_library(gt4_perf_probe STATIC "${NATIVE}/perf_probe.cpp" "${NATIVE}/perf_clock.cpp")
target_compile_definitions(gt4_perf_probe PUBLIC GT4_PERF_VU_SPLIT GT4_VU1_AOT GT4_VU0_AOT)
add_library(gt4_mtvu_measure STATIC "${NATIVE}/mtvu_measure.cpp")
add_library(gt4_mtvu_journal STATIC "${NATIVE}/mtvu_journal.cpp")
add_library(gt4_mtvu_runtime STATIC "${NATIVE}/mtvu_runtime.cpp")
target_link_libraries(gt4_mtvu_runtime PUBLIC gt4_mtvu_journal gt4_mtvu_measure gt4_perf_probe gt4_quickfix)
add_library(gt4_pad_mapping STATIC "${NATIVE}/pad_mapping.cpp" "${NATIVE}/pad_choice.cpp" "${NATIVE}/input_recording.cpp")
add_library(native_host_io STATIC "${NATIVE}/host_io.cpp" "${NATIVE}/frame_clock_posix.cpp" "${NATIVE}/controls.cpp"
    "${NATIVE}/audio_sdl.cpp")
target_compile_definitions(native_host_io PRIVATE GT4_PERF_PROBE)
target_link_libraries(native_host_io PUBLIC gt4_pad_mapping gt4_perf_probe gt4_pacing gt4_platform gt4_sdl3)

# ---- CoreGS 2.0 (native/hw_gs; the Windows-only HUD glyphs and CPU sampler have POSIX forms) -------------------------
set(HW "${NATIVE}/hw_gs")
file(GLOB hw_shader_sources "${GT4_HW_SHADER_CPP_DIR}/*_spirv.cpp")
if(NOT hw_shader_sources)
    message(FATAL_ERROR "No *_spirv.cpp in GT4_HW_SHADER_CPP_DIR (build the Windows tree first)")
endif()
add_library(gt4_hw_shaders STATIC ${hw_shader_sources})
target_include_directories(gt4_hw_shaders PUBLIC "${HW}/shaders")
add_library(gt4_hw_options STATIC "${HW}/hw_gs_options.cpp")
add_library(gt4_hw_core STATIC "${HW}/cpuref/cpuref_memory.cpp" "${HW}/cpuref/cpuref_draw.cpp" "${HW}/memory/hw_memory.cpp"
    "${HW}/clut/hw_clut.cpp" "${HW}/frontend/hw_gs_registers.cpp" "${HW}/frontend/hw_gif_parser.cpp"
    "${HW}/frontend/hw_transfers.cpp" "${HW}/frontend/hw_batch.cpp" "${HW}/frontend/hw_front_end.cpp"
    "${HW}/pixel/hw_gs_pixel.cpp" "${HW}/display/hw_gs_display.cpp" "${HW}/core2/core2_upload.cpp"
    "${HW}/core2/core2_depth_alias.cpp")
target_include_directories(gt4_hw_core PUBLIC "${HW}/cpuref" "${HW}/memory" "${HW}/clut" "${HW}/frontend" "${HW}/pixel"
    "${HW}/display")
add_library(hw_gs_lod STATIC "${HW}/lod/hw_gs_lod.cpp")
target_include_directories(hw_gs_lod PUBLIC "${HW}/lod")
add_library(hw_gs_replace STATIC "${HW}/replace/src/gs_types.cpp" "${HW}/replace/src/xxh3.cpp"
    "${HW}/replace/src/xxh3_impl.cpp" "${HW}/replace/src/clut.cpp" "${HW}/replace/src/keys.cpp"
    "${HW}/replace/src/names.cpp" "${HW}/replace/src/packs.cpp" "${HW}/replace/src/images.cpp"
    "${HW}/replace/src/stb_image_impl.cpp" "${HW}/replace/tests/png_encode.cpp")
target_include_directories(hw_gs_replace PUBLIC "${HW}/replace/include" "${HW}/replace/tests"
    PRIVATE "${HW}/replace/third_party")
add_library(gt4_hw_gpu STATIC "${HW}/writeback/hw_gs_writeback_gpu.cpp" "${HW}/writeback/hw_gs_vulkan_device_table.cpp")
target_include_directories(gt4_hw_gpu PRIVATE "${GT4_VULKAN_INCLUDE}" "${HW}")
target_link_libraries(gt4_hw_gpu PUBLIC gt4_hw_shaders)
add_library(gt4_hw_writeback STATIC "${HW}/writeback/hw_gs_writeback.cpp" "${HW}/writeback/hw_gs_present_scheduler.cpp"
    "${HW}/core2/core2_renderer.cpp" "${HW}/core2/core2_gpu.cpp" "${HW}/core2/core2_replace.cpp")
target_include_directories(gt4_hw_writeback PRIVATE "${GT4_VULKAN_INCLUDE}" "${HW}")
target_link_libraries(gt4_hw_writeback PUBLIC gt4_hw_core gt4_hw_gpu hw_gs_lod hw_gs_replace)
add_library(gt4_hw_vulkan_loader STATIC "${HW}/hw_gs_vulkan_loader.cpp")
target_include_directories(gt4_hw_vulkan_loader PUBLIC "${HW}" "${GT4_VULKAN_INCLUDE}")
target_link_libraries(gt4_hw_vulkan_loader PUBLIC dl)
add_library(gt4_hw_hud STATIC "${HW}/hud/hw_hud.cpp" "${HW}/hud/hw_hud_system_posix.cpp")
# The HUD draws its glyphs and reads the refresh rate through the game activity (SDL3 gives the JNI environment).
target_link_libraries(gt4_hw_hud PRIVATE gt4_sdl3)
add_library(gt4_hw_backend STATIC "${HW}/hw_gs_backend.cpp" "${HW}/hw_gs_dispatch.cpp" "${HW}/hud/hw_hud_layer.cpp"
    "${HW}/hw_gs_prepare.cpp")
target_include_directories(gt4_hw_backend PRIVATE "${GT4_VULKAN_INCLUDE}")
target_link_libraries(gt4_hw_backend PUBLIC gt4_hw_hud gt4_hw_options gt4_pacing gt4_hw_writeback gt4_hw_vulkan_loader)
add_library(gt4_hw_stream_sink STATIC "${HW}/hw_gs_stream_sink.cpp" "${HW}/hw_cpu_sampler_posix.cpp")
target_include_directories(gt4_hw_stream_sink PRIVATE "${NATIVE}" "${NATIVE}/gs_stream")
target_link_libraries(gt4_hw_stream_sink PUBLIC gt4_hw_backend)

# The GS stream seam: on Android only its in-process CoreGS 2.0 sink (no GS process, no capture files).
add_library(gt4_gs_stream_client STATIC "${NATIVE}/gs_stream/gs_stream_client.cpp"
    "${NATIVE}/gs_stream/armsx2_gs_sink.cpp")
target_include_directories(gt4_gs_stream_client PUBLIC "${NATIVE}/gs_stream" "${NATIVE}")
target_link_libraries(gt4_gs_stream_client PUBLIC gt4_perf_probe gt4_install_paths gt4_quickfix gt4_hw_stream_sink
    gt4_platform)
# GT4_GS_STREAM_ARMSX2: the private test build's in-process ARMSX2 GS (libgt4gs.so, GPL, loaded at run time). It is an
# extra renderer beside CoreGS 2.0; a build without the library still starts and refuses the option with a message.
target_compile_definitions(gt4_gs_stream_client PRIVATE GT4_GS_STREAM_HW GT4_GS_STREAM_ARMSX2 INTERFACE
    GT4_GS_STREAM_CLIENT)

# ---- Runtime: the pinned donor plus the capsules of the rc14 v2.00 recipe -------------------------------------------
set(RUNTIME_FILES)
foreach(source IN ITEMS memory dispatch system kernel hw cop disc dmac vif vu_micro vu_jit vu_jit_web
                        ipu/ipu gs/gif gs/queue gs/raster iop/cpu iop/kernel iop/libs iop/hw iop/spu2 iop/sio2 iop/io
                        iop/cdvd iop/sif iop/cdvdfsv)
    list(APPEND RUNTIME_FILES "${DONOR}/runtime/src/${source}.cpp")
endforeach()
list(REMOVE_ITEM RUNTIME_FILES "${DONOR}/runtime/src/iop/sio2.cpp" "${DONOR}/runtime/src/ipu/ipu.cpp"
     "${DONOR}/runtime/src/iop/spu2.cpp" "${DONOR}/runtime/src/dispatch.cpp" "${DONOR}/runtime/src/kernel.cpp"
     "${DONOR}/runtime/src/iop/cdvdfsv.cpp" "${DONOR}/runtime/src/system.cpp" "${DONOR}/runtime/src/vif.cpp"
     "${DONOR}/runtime/src/vu_micro.cpp" "${DONOR}/runtime/src/gs/gif.cpp" "${DONOR}/runtime/src/memory.cpp"
     "${DONOR}/runtime/src/hw.cpp" "${DONOR}/runtime/src/dmac.cpp")
list(APPEND RUNTIME_FILES "${GT4_MEMCARD_RUNTIME_SOURCE}/sio2.cpp" "${GT4_IPU_RUNTIME_SOURCE}/ipu.cpp"
     "${GT4_SPU2_RUNTIME_SOURCE}/spu2.cpp" "${GT4_LICENSE_GUARD_SOURCE}/dispatch.cpp"
     "${GT4_LICENSE_GUARD_SOURCE}/kernel.cpp" "${GT4_LICENSE_GUARD_SOURCE}/cdvdfsv.cpp")
foreach(source IN LISTS perf_sources)
    list(APPEND RUNTIME_FILES "${GT4_PERF_RUNTIME_SOURCE}/${source}")
endforeach()
# LP64 portability of the pinned capsule (Windows is LLP64): in vu_micro.cpp the two report-period lambdas return
# std::strtoull (unsigned long long) on one path and std::uint64_t (unsigned long on LP64) on the other, which LP64
# refuses as two deduced return types. The capsule cannot change (it is pinned), so the Android build compiles a copy
# made at configure time from the verified file with exactly this cast added: the same value on every path. The
# generator (scripts/vu1_aot_overlay.inc and the VU0 overlay) should take the same cast in the next capsule.
file(READ "${GT4_PERF_RUNTIME_SOURCE}/vu_micro.cpp" vu_micro_text)
set(lp64_old "if (period) return std::strtoull(period, nullptr, 10);")
set(lp64_new "if (period) return static_cast<std::uint64_t>(std::strtoull(period, nullptr, 10));")
string(REPLACE "${lp64_old}" "" vu_micro_without "${vu_micro_text}")
string(LENGTH "${vu_micro_text}" length_with)
string(LENGTH "${vu_micro_without}" length_without)
string(LENGTH "${lp64_old}" length_old)
math(EXPR lp64_sites "(${length_with} - ${length_without}) / ${length_old}")
if(NOT lp64_sites EQUAL 2)
    message(FATAL_ERROR "vu_micro.cpp LP64 copy: expected 2 report-period lambdas, found ${lp64_sites}")
endif()
string(REPLACE "${lp64_old}" "${lp64_new}" vu_micro_text "${vu_micro_text}")
file(WRITE "${CMAKE_BINARY_DIR}/lp64/vu_micro.cpp" "${vu_micro_text}")
list(REMOVE_ITEM RUNTIME_FILES "${GT4_PERF_RUNTIME_SOURCE}/vu_micro.cpp")
list(APPEND RUNTIME_FILES "${CMAKE_BINARY_DIR}/lp64/vu_micro.cpp")
file(SHA256 "${CMAKE_BINARY_DIR}/lp64/vu_micro.cpp" lp64_hash)
message(STATUS "vu_micro.cpp LP64 copy (2 casts): ${lp64_hash}")
set_source_files_properties("${GT4_MEMCARD_RUNTIME_SOURCE}/sio2.cpp" PROPERTIES INCLUDE_DIRECTORIES "${DONOR}/runtime/src/iop")
set_source_files_properties("${GT4_IPU_RUNTIME_SOURCE}/ipu.cpp" PROPERTIES INCLUDE_DIRECTORIES "${DONOR}/runtime/src;${NATIVE}")
set_source_files_properties("${GT4_SPU2_RUNTIME_SOURCE}/spu2.cpp" PROPERTIES INCLUDE_DIRECTORIES "${DONOR}/runtime/src/iop;${NATIVE}")
file(GLOB vu1_generated_parts "${GT4_VU1_GENERATED_DIR}/vu1_generated_*.cpp")
list(SORT vu1_generated_parts)
file(GLOB vu0_generated_parts "${GT4_VU0_GENERATED_DIR}/vu0_generated_*.cpp")
list(SORT vu0_generated_parts)
set(vu_generated "${GT4_VU1_GENERATED_DIR}/vu1_generated.cpp" ${vu1_generated_parts}
    "${GT4_VU0_GENERATED_DIR}/vu0_generated.cpp" ${vu0_generated_parts})
set_source_files_properties(${vu_generated} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
add_library(ps2runtime STATIC ${RUNTIME_FILES} "${PORT}/gs.cpp" "${NATIVE}/gpu_unavailable.cpp" "${NATIVE}/mtvu_bridge.cpp"
    ${vu_generated} "${GT4_ROOT}/tools/vu_recomp/src/sha256.cpp")
target_include_directories(ps2runtime BEFORE PUBLIC "${GT4_FAST_HEADERS_DIR}")
target_include_directories(ps2runtime PUBLIC "${DONOR}/runtime/include" "${DONOR}/runtime/src" "${DONOR}/runtime/src/gs"
    "${NATIVE}" "${PORT}" "${GT4_ROOT}/tools/vu_recomp/src" "${GT4_VULKAN_INCLUDE}")
target_compile_definitions(ps2runtime PUBLIC GT4_PERF_PROBE PS2_GPU GT4_VULKAN_SHADER_DIR="")
target_link_libraries(ps2runtime PUBLIC gt4_pacing gt4_quickfix gt4_install_paths gt4_memcard gt4_license_guard
    gt4_perf_probe gt4_mtvu_runtime gt4_gs_stream_client)

# ---- libgt4.so: the entry (SDL_main), the SDL3 window host and the translated-code hooks ------------------------------
set_source_files_properties("${PORT}/main.cpp" PROPERTIES COMPILE_DEFINITIONS "main=gt4_donor_main"
    COMPILE_OPTIONS "-include;${NATIVE}/platform/msvc_compat.hpp")
add_library(gt4 SHARED "${PORT}/main.cpp" "${NATIVE}/game_start.cpp" "${NATIVE}/window_sdl.cpp"
    "${NATIVE}/android_main.cpp" "${NATIVE}/android_diagnostics.cpp" "${NATIVE}/camera_360_hook.cpp" "${NATIVE}/fov_adjust_hook.cpp"
    "${NATIVE}/display_aspect_hook.cpp")
target_compile_definitions(gt4 PRIVATE PS2_WINDOW GT4_MEMCARD_FLUSH_OVERLAY GT4_PLATFORM_SDL3)
target_include_directories(gt4 PRIVATE "${PORT}" "${NATIVE}")
target_link_libraries(gt4 PRIVATE ps2game ps2runtime native_host_io gt4_platform gt4_sdl3 android log dl)
target_link_options(gt4 PRIVATE ${GT4_ANDROID_LINK_FLAGS})
# TASK-180 test3: the Spec II library is libgt4spec2.so, as the port plan names it; v2.00 stays libgt4.so.
if(GT4_TARGET STREQUAL "spec2")
    set_target_properties(gt4 PROPERTIES OUTPUT_NAME gt4spec2)
endif()
