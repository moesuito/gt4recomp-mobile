# HWR, the MIT hardware GS renderer (TASK-027). Chapter 12.10: built only with GT4_HW_GS=ON
# (default OFF until qualified). Included from native/CMakeLists.txt.
#
# Layout for later milestones: add sources to gt4_hw_backend (or a new sub-library with
# target_link_libraries(... gt4_hw_backend)). Only hw_gs_backend.cpp and the backend-internal
# files may include Vulkan headers; the public headers stay Vulkan-free.

set(GT4_HW_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK root with Include/ and Lib/vulkan-1.lib")
if(NOT EXISTS "${GT4_HW_VULKAN_SDK}/Lib/vulkan-1.lib")
    message(FATAL_ERROR "GT4_HW_GS needs a Vulkan SDK: set GT4_HW_VULKAN_SDK or VULKAN_SDK")
endif()

# Pure option parsing (GT4_RENDERER, GT4_HW_*): no Vulkan, no Windows types.
add_library(gt4_hw_options STATIC hw_gs_options.cpp)
target_compile_options(gt4_hw_options PRIVATE /W4)
target_compile_definitions(gt4_hw_options PRIVATE _CRT_SECURE_NO_WARNINGS)

# CPU side of the write-back reference (TASK-027 M4): GS front end (M3), shadow local memory (M2),
# CLUT (M3), pixel library (per-pixel state), display (M6) and the CPU sink wrapping the memory model.
# Pure C++, no Vulkan. The same sources are listed by cpuref/CMakeLists.txt for the standalone build.
set(HW_GS_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(gt4_hw_core STATIC
    cpuref/cpuref_memory.cpp
    cpuref/cpuref_draw.cpp
    memory/hw_memory.cpp
    clut/hw_clut.cpp
    frontend/hw_gs_registers.cpp
    frontend/hw_gif_parser.cpp
    frontend/hw_transfers.cpp
    frontend/hw_batch.cpp
    frontend/hw_front_end.cpp
    pixel/hw_gs_pixel.cpp
    display/hw_gs_display.cpp
    core2/core2_upload.cpp
    core2/core2_depth_alias.cpp)
target_include_directories(gt4_hw_core PUBLIC
    "${HW_GS_CORE_DIR}/cpuref" "${HW_GS_CORE_DIR}/memory" "${HW_GS_CORE_DIR}/clut"
    "${HW_GS_CORE_DIR}/frontend" "${HW_GS_CORE_DIR}/pixel" "${HW_GS_CORE_DIR}/display")
target_compile_options(gt4_hw_core PRIVATE /W4 /permissive-)

# GPU write-back drawer (Vulkan entry points resolved at run time) and its device entry-point table.
add_library(gt4_hw_gpu STATIC
    writeback/hw_gs_writeback_gpu.cpp
    writeback/hw_gs_vulkan_device_table.cpp)
target_include_directories(gt4_hw_gpu PRIVATE "${GT4_HW_VULKAN_SDK}/Include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gt4_hw_gpu PUBLIC gt4_hw_shaders)
target_compile_options(gt4_hw_gpu PRIVATE /W4)
target_compile_definitions(gt4_hw_gpu PRIVATE _CRT_SECURE_NO_WARNINGS)

# Renderer of the write-back mode: front end, batch translation, GPU draw, SLM write-back, display.
add_library(gt4_hw_writeback STATIC writeback/hw_gs_writeback.cpp
    writeback/hw_gs_present_scheduler.cpp
    core2/core2_renderer.cpp core2/core2_gpu.cpp core2/core2_replace.cpp)
target_link_libraries(gt4_hw_writeback PUBLIC gt4_hw_core gt4_hw_gpu hw_gs_lod hw_gs_replace)
# CORE2 (the core rewrite, CORE2-DESIGN.md): its GPU file includes Vulkan headers.
target_include_directories(gt4_hw_writeback PRIVATE "${GT4_HW_VULKAN_SDK}/Include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(gt4_hw_writeback PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(gt4_hw_writeback PRIVATE /W4)

# M4 fixtures: CPU-side de-index/corner rules and GPU equivalence against the CPU reference.
# Exit code 77 means "no compute-capable Vulkan 1.2 device" and is reported as Skipped, never as a pass.
add_executable(hw_writeback_tests writeback/tests/hw_writeback_tests.cpp)
target_include_directories(hw_writeback_tests PRIVATE "${GT4_HW_VULKAN_SDK}/Include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hw_writeback_tests PRIVATE gt4_hw_writeback gt4_hw_vulkan_loader hw_gs_cpuref
    "${GT4_HW_VULKAN_SDK}/Lib/vulkan-1.lib")
target_compile_options(hw_writeback_tests PRIVATE /W4 -Wno-missing-field-initializers)
target_compile_definitions(hw_writeback_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# M6 replay tool: one recorded stream -> the GPU write-back renderer -> one PPM per frame. It reuses the
# capture reader of the CPU reference library (hw_gs_cpuref), so both paths replay identical records.
add_executable(hw_writeback_replay writeback/hw_writeback_replay.cpp)
target_include_directories(hw_writeback_replay PRIVATE "${GT4_HW_VULKAN_SDK}/Include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hw_writeback_replay PRIVATE gt4_hw_writeback gt4_hw_vulkan_loader hw_gs_cpuref
    "${GT4_HW_VULKAN_SDK}/Lib/vulkan-1.lib")
target_compile_options(hw_writeback_replay PRIVATE /W4 -Wno-missing-field-initializers)
target_compile_definitions(hw_writeback_replay PRIVATE _CRT_SECURE_NO_WARNINGS)

foreach(hw_writeback_test IN ITEMS hw_flat_provoking hw_sprite_corners hw_present_scheduler
        hw_present_scheduler_hold hw_present_scheduler_rate hw_aux_frame_guest_untouched
        hw_pass_timings hw_gpu_coverage_vectors
        hw_gpu_equivalence_flat hw_gpu_equivalence_texture hw_gpu_equivalence_region
        hw_gpu_equivalence_bilinear hw_gpu_equivalence_tfx_fog hw_gpu_equivalence_depth_alpha
        hw_gpu_equivalence_scissor hw_gpu_raster_blend hw_gpu_raster_pabe hw_gpu_raster_atest
        hw_gpu_raster_date hw_gpu_raster_frame hw_gpu_gouraud_report
        hw_gpu_writeback_aliased_depth hw_readback_pending_draw hw_replay_parity_double_buffer
        hw_presentation_field_phase hw_cache_equals_writeback hw_scale_exact hw_postfx_fxaa hw_scale_display_shadow hw_scale_fullscreen_passes hw_scale_interlaced_present hw_core2_interlaced_present hw_core2_cpu_sprites hw_core2_gpu_clut hw_core2_palette_segments hw_core2_tone_early_drop hw_core2_drawn_page_test hw_core2_width_grace hw_core2_glm_memo hw_core2_patch_bits hw_core2_patch_blocks hw_core2_over_target_memo hw_core2_mipmaps hw_core2_present_scale hw_core2_forced_bilinear hw_core2_specialised hw_core2_upload_memo hw_core2_depth_clear hw_core2_scanmsk hw_core2_display_memory hw_core2_shader_cache hw_core2_prepare_shaders hw_core2_shader_guard hw_core2_whole_clears hw_core2_rop hw_core2_road_sheen hw_core2_delayed_downloads hw_core2_compute_fills hw_core2_self_texture hw_core2_self_copy_draw hw_core2_texture_replacement hw_core2_overlap_order hw_core2_point_quads hw_core2_sync_trace hw_core2_post_field_compose hw_core2_data_paths hw_core2_effect_toggles
        hw_scale_target_extent
        hw_tone_hle_exact
        hw_glm_decode hw_glm_swizzle hw_glm_clut hw_glm_unswizzle hw_glm_upload_merge
        hw_glm_host_upload)
    add_test(NAME ${hw_writeback_test} COMMAND hw_writeback_tests ${hw_writeback_test})
    set_tests_properties(${hw_writeback_test} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endforeach()

# Loader of the Vulkan entry points at run time: shared by the backend and the M4 fixtures, so every
# call goes through the loader and never through another library's import.
add_library(gt4_hw_vulkan_loader STATIC hw_gs_vulkan_loader.cpp)
target_include_directories(gt4_hw_vulkan_loader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${GT4_HW_VULKAN_SDK}/Include")
target_compile_options(gt4_hw_vulkan_loader PRIVATE /W4)
target_compile_definitions(gt4_hw_vulkan_loader PRIVATE _CRT_SECURE_NO_WARNINGS)

# Backend, dispatch front end and device code. Vulkan is linked from the SDK import library.
# TASK-135: the performance HUD. The numbers, the text and the panel (no Vulkan), and what Windows gives them.
add_library(gt4_hw_hud STATIC hud/hw_hud.cpp hud/hw_hud_system.cpp)
target_link_libraries(gt4_hw_hud PUBLIC gdi32 user32)
target_compile_options(gt4_hw_hud PRIVATE /W4)
target_compile_definitions(gt4_hw_hud PRIVATE _CRT_SECURE_NO_WARNINGS)
add_executable(hw_hud_fixture hud/hw_hud_fixture.cpp)
target_link_libraries(hw_hud_fixture PRIVATE gt4_hw_hud)
target_compile_options(hw_hud_fixture PRIVATE /W4)
add_test(NAME hw_hud COMMAND hw_hud_fixture)

add_library(gt4_hw_backend STATIC hw_gs_backend.cpp hw_gs_dispatch.cpp hud/hw_hud_layer.cpp)
target_include_directories(gt4_hw_backend PRIVATE "${GT4_HW_VULKAN_SDK}/Include")
target_link_libraries(gt4_hw_backend PUBLIC gt4_hw_hud gt4_hw_options gt4_pacing gt4_hw_writeback gt4_hw_vulkan_loader "${GT4_HW_VULKAN_SDK}/Lib/vulkan-1.lib")
target_compile_options(gt4_hw_backend PRIVATE /W4)
target_compile_definitions(gt4_hw_backend PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(hw_options_fixture hw_options_fixture.cpp)
target_link_libraries(hw_options_fixture PRIVATE gt4_hw_options)
target_compile_options(hw_options_fixture PRIVATE /W4)

# The shared presentation_rect lives in native_vulkan.cpp. The fixture compiles that file directly
# (it loads the Vulkan loader at run time) instead of linking the donor runtime library.
add_executable(hw_present_rect_fixture hw_present_rect_fixture.cpp "${CMAKE_CURRENT_SOURCE_DIR}/../native_vulkan.cpp")
target_include_directories(hw_present_rect_fixture PRIVATE "${GT4_HW_VULKAN_SDK}/Include")
target_link_libraries(hw_present_rect_fixture PRIVATE gt4_pacing gt4_perf_probe gt4_install_paths gt4_vulkan_extensions user32)
target_compile_definitions(hw_present_rect_fixture PRIVATE GT4_VULKAN_SHADER_DIR="" _CRT_SECURE_NO_WARNINGS)
target_compile_options(hw_present_rect_fixture PRIVATE /W4)

# IN-414: core 2's HOST->LOCAL fast path against memory::hostToLocal (CPU only).
add_executable(hw_core2_upload_fixture core2/core2_upload_fixture.cpp)
target_include_directories(hw_core2_upload_fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hw_core2_upload_fixture PRIVATE gt4_hw_core)
target_compile_options(hw_core2_upload_fixture PRIVATE /W4)
add_test(NAME hw_core2_upload_swizzle COMMAND hw_core2_upload_fixture)

# IN-535: the colour pixels a depth fill at another base replaces, against memory::pixelSlot (CPU only).
add_executable(hw_core2_depth_alias_fixture core2/core2_depth_alias_fixture.cpp)
target_include_directories(hw_core2_depth_alias_fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hw_core2_depth_alias_fixture PRIVATE gt4_hw_core)
target_compile_options(hw_core2_depth_alias_fixture PRIVATE /W4)
add_test(NAME hw_core2_depth_alias COMMAND hw_core2_depth_alias_fixture)

# IN-514: the deadline of the flare table read (CPU only).
add_executable(hw_flare_table_fixture hw_flare_table_fixture.cpp)
target_compile_options(hw_flare_table_fixture PRIVATE /W4)
add_test(NAME hw_flare_table COMMAND hw_flare_table_fixture)

# IN-521: the display queue control (CPU only).
add_executable(hw_present_latency_fixture hw_present_latency_fixture.cpp)
target_compile_options(hw_present_latency_fixture PRIVATE /W4)
add_test(NAME hw_present_latency COMMAND hw_present_latency_fixture)
add_executable(hw_present_spacing_fixture hw_present_spacing_fixture.cpp)
target_compile_options(hw_present_spacing_fixture PRIVATE /W4)
add_test(NAME hw_present_spacing COMMAND hw_present_spacing_fixture)
# TASK-174: the lock of a queue shared by the GS thread and the presentation thread, and a model of that presentation (CPU only).
add_executable(hw_queue_lock_fixture hw_queue_lock_fixture.cpp)
target_compile_options(hw_queue_lock_fixture PRIVATE /W4)
add_test(NAME hw_queue_lock COMMAND hw_queue_lock_fixture)
set_tests_properties(hw_queue_lock PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    PASS_REGULAR_EXPRESSION "hw_queue_lock: all checks passed")
# TASK-179: the device profile (CPU only).
add_executable(hw_device_profile_fixture hw_device_profile_fixture.cpp)
target_compile_options(hw_device_profile_fixture PRIVATE /W4)
add_test(NAME hw_device_profile COMMAND hw_device_profile_fixture)
# TASK-179: the swapchain pre-transform and its blit (CPU only).
add_executable(hw_present_transform_fixture hw_present_transform_fixture.cpp)
target_compile_options(hw_present_transform_fixture PRIVATE /W4)
add_test(NAME hw_present_transform COMMAND hw_present_transform_fixture)

add_executable(hw_gs_smoke hw_gs_smoke.cpp)
target_include_directories(hw_gs_smoke PRIVATE "${GT4_HW_VULKAN_SDK}/Include")
target_link_libraries(hw_gs_smoke PRIVATE gt4_hw_backend user32)
target_compile_options(hw_gs_smoke PRIVATE /W4)
target_compile_definitions(hw_gs_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)

# HWR shader toolchain and GPU pixel exactness fixture (TASK-027, M4). Pinned glslang, IN-201.
add_subdirectory(shaders)

# TASK-027 hookup on main's GS stream seam (task/027-coregs2): GT4_RENDERER=hw sends the stream hooks to the
# backend in process (native/gs_stream/gs_stream_sink.hpp). Linked into gt4_gs_stream_client by native/CMakeLists.txt.
# hw_cpu_sampler.cpp: the TASK-027 CPU survey sampler (GT4_CPU_SAMPLER, diagnostic, off by default).
add_library(gt4_hw_stream_sink STATIC hw_gs_stream_sink.cpp hw_gs_prepare.cpp hw_cpu_sampler.cpp)
target_include_directories(gt4_hw_stream_sink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(gt4_hw_stream_sink PUBLIC gt4_hw_backend)
target_compile_options(gt4_hw_stream_sink PRIVATE /W4)
target_compile_definitions(gt4_hw_stream_sink PRIVATE _CRT_SECURE_NO_WARNINGS)

# Pure libraries and their CTest fixtures, built inside the game tree (they also build standalone; see each folder).
# cpuref compiles memory, clut, frontend, pixel and display itself and registers their fixtures (memory_*, m3_*,
# pixel_*, display_*), so those folders are not added again here.
add_subdirectory(cpuref)
add_subdirectory(cache)
add_subdirectory(decide)
add_subdirectory(lod)
add_subdirectory(replace)

add_executable(hw_stream_sink_fixture hw_stream_sink_fixture.cpp)
target_link_libraries(hw_stream_sink_fixture PRIVATE gt4_gs_stream_client)
target_compile_options(hw_stream_sink_fixture PRIVATE /W4)

# TASK-027 CPU survey: the GT4_CPU_SAMPLER sampler on a known busy thread (registered as hw_cpu_sampler).
add_executable(hw_cpu_sampler_fixture hw_cpu_sampler_fixture.cpp hw_cpu_sampler.cpp)
target_compile_options(hw_cpu_sampler_fixture PRIVATE /W4)
target_compile_definitions(hw_cpu_sampler_fixture PRIVATE _CRT_SECURE_NO_WARNINGS)

# IN-468: profile-guided optimisation of the GS pipeline libraries only (the EE, VU and IOP code is not touched).
# GT4_HW_PGO=generate instruments them (build only gt4vulkan, the fixtures do not link the profile runtime; a run writes
# LLVM_PROFILE_FILE at a normal exit); GT4_HW_PGO=use optimises from GT4_HW_PGO_PROFILE (llvm-profdata merge output).
# GT4_HW_THINLTO=ON adds cross-file inlining between them. Off by default: a build without a profile is unchanged.
set(GT4_HW_PGO "" CACHE STRING "GS libraries PGO: empty, generate or use")
set(GT4_HW_PGO_PROFILE "" CACHE FILEPATH "Merged profile for GT4_HW_PGO=use")
option(GT4_HW_THINLTO "ThinLTO for the GS libraries" OFF)
set(GT4_HW_PGO_TARGETS gt4_hw_core gt4_hw_gpu gt4_hw_writeback gt4_hw_backend gt4_hw_stream_sink)
if(GT4_HW_PGO STREQUAL "generate")
    get_filename_component(GT4_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB GT4_PROFILE_RUNTIME "${GT4_LLVM_BIN}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    if(NOT GT4_PROFILE_RUNTIME)
        message(FATAL_ERROR "GT4_HW_PGO=generate needs clang_rt.profile-x86_64.lib")
    endif()
    foreach(hw_target IN LISTS GT4_HW_PGO_TARGETS)
        target_compile_options(${hw_target} PRIVATE "/clang:-fprofile-generate")
    endforeach()
    target_link_libraries(gt4_hw_core PUBLIC "${GT4_PROFILE_RUNTIME}")
    target_link_options(gt4_hw_core PUBLIC "/include:__llvm_profile_runtime")
elseif(GT4_HW_PGO STREQUAL "use")
    if(NOT EXISTS "${GT4_HW_PGO_PROFILE}")
        message(FATAL_ERROR "GT4_HW_PGO=use needs GT4_HW_PGO_PROFILE")
    endif()
    foreach(hw_target IN LISTS GT4_HW_PGO_TARGETS)
        target_compile_options(${hw_target} PRIVATE "/clang:-fprofile-use=${GT4_HW_PGO_PROFILE}"
                               "/clang:-Wno-profile-instr-out-of-date" "/clang:-Wno-profile-instr-unprofiled"
                               "/clang:-Wno-backend-plugin")
    endforeach()
endif()
if(GT4_HW_THINLTO)
    foreach(hw_target IN LISTS GT4_HW_PGO_TARGETS)
        target_compile_options(${hw_target} PRIVATE "/clang:-flto=thin")
    endforeach()
endif()
