# The ARMSX2 GS import

Status: 2026-11-06. This repository embeds ARMSX2's GS renderer as tracked source under `third_party/armsx2/`, so the
Android build does not depend on a local, untracked copy. ARMSX2 derives from PCSX2; the imported files are
GPL-3.0-or-later (see `third_party/armsx2/COPYING.GPLv3` and the repository `NOTICE`).

## What is imported

Upstream: [ARMSX2](https://github.com/moesuito/armsx2) at commit
`1049db37fba4a08b9849e0ab061302d563565f7f` (2026-11-04).

| Path under `third_party/armsx2/` | Why |
|---|---|
| `common/` | the shared core (types, logging, file system, strings, heap arrays) |
| `pcsx2/` | the emulator core and the GS (GLES and Vulkan devices, GSdx, shaders, GameDB, ARM64 support) |
| `3rdparty/libretro/`, `3rdparty/include/` | the root third-party pieces the core's CMake links and includes |
| `platforms/android/app/src/main/cpp/3rdparty/` | the 30 libraries the Android core build compiles: ccc, cpuinfo, cubeb, demangler, discord-rpc, fast_float, fmt, freesurround, freetype, glad, imgui, libchdr, libjpeg-turbo, libpng, libwebp, libzip, lz4, lzma, oboe, plutosvg, plutovg, rapidjson, rapidyaml, rcheevos, sdl3, shaderc, simpleini, soundtouch, vulkan, zstd |
| `platforms/android/app/src/main/cpp/cmake/` | the Android build's CMake modules |
| `cmake/` | the build modules the core includes (`EmbedResources`, `ShaderToCpp`, ...) |
| `bin/resources/shaders/`, `bin/resources/fonts/`, `bin/resources/GameIndex.yaml` | the shaders the GS compiles, the ImGui fonts and the game database the library reads |
| `COPYING.GPLv3` | the GPL text |

Not imported, because the Android GS library neither compiles nor ships them: `.git`, the Qt and other frontends
(`pcsx2-qt/`, `pcsx2-sdl/`, `pcsx2-libretro/`, `pcsx2-*-runner/`), `platforms/ios/`, the Android app's Java/Kotlin
sources and prebuilt `jniLibs/`, `updater/`, `tests/`, `tools/`, the SDL3 and Oboe sample/test trees, the unused
third-party trees (`vixl`, `zydis`, `xbyak`, `adrenotools`, `librashader`, `raininterface`, `miniupnpc`, `promptfont`,
the Windows-only ones) and every file over 50 MB.

Approximate size of the import: **7,200 files, about 120 MiB**, largest file 3.6 MiB.

The shader compiler sources (`shaderc`'s dependencies) are **not** committed. shaderc itself ships inside ARMSX2's tree;
its `third_party/` dependencies are fetched by `tools/fetch_armsx2_shader_deps.py` at the revisions its `DEPS` file
pins:

| Repository | Revision |
|---|---|
| glslang | `09c541ee5b22bbac307987b50d86ec2b4f683d75` |
| SPIRV-Headers | `465055f6c9128772e20082e893d974146acf7a02` |
| SPIRV-Tools | `e4bceacf59fdfe742047e94e41ef65a48999a0dd` |

They land in `.../3rdparty/shaderc/third_party/{glslang,spirv-headers,spirv-tools}` and are git-ignored. The script
checks that the imported shaderc's `DEPS` matches the pins and fails loudly otherwise.

## The two commits

- **`third_party: import the ARMSX2 GS subtree at 1049db37fb`** — the upstream files, unmodified. A later update
  re-imports the same selection from the new commit.
- **`Android: re-apply the GT4Recomp patches on the ARMSX2 import`** — every change the GT4Recomp Android work made to
  the tree. Re-applied on top of a fresh import.

## The patches, and why

| File(s) | Patch | Reason |
|---|---|---|
| `pcsx2/GT4GSStreamHook.h`, `pcsx2/VMManager.cpp`, `pcsx2/GSDumpReplayer.cpp` | the `gt4gs::` stream hook: `AutoDetectSource` routes to the stream, `IsReplayingDump` answers true while it runs, serial/CRC answer for a stream, `InitializeCPUProviders` uses the stream CPU provider | libgt4gs is fed the game's GS stream in-process; there is no BIOS, disc or GS dump to boot from |
| `pcsx2/GSDumpReplayer.cpp` (`RenderUI`), `pcsx2/GS/Renderers/Common/GSRenderer.cpp` (`BeginPresent`, `EndPresentFrame`) | return before the dump overlay when there is no dump file; end the present and draw only the GT4Recomp HUD while a stream runs | the first VSync read the null dump and aborted the GS thread (owner's S23, 2026-11-06); the library draws no emulator UI |
| `pcsx2/GT4GSNoJitStubs.cpp`, `pcsx2/gen_GT4GS_nojit_stubs.py` | empty definitions for the 272 opcode-table `rec*` entries plus `dVifUnpack<0/1>`, `dVifReset`, `SaveStateBase::vuJITFreeze`, `eeDivideSignificandArm64` and the six `vuAot*` reporters, generated from `R5900OpcodeTables.h` | `GT4GS_NO_JIT`: the ARM64 recompiler sources are not compiled, but the opcode tables and savestates still name them |
| `pcsx2/CMakeLists.txt` | under `GT4GS_NO_JIT`, do not compile `pcsx2arm64Sources` or the scanline generators and do not link `vixl`; add the stub file | no run-time code generation: the ARM64 recompilers and the software-rasterizer generators are left out of the library (0 vixl symbols) |
| `pcsx2/Memory.cpp` | no executable code memory is mapped under `GT4GS_NO_JIT` | the library maps no W^X arena and no code cache |
| `pcsx2/VMManager.cpp` | no recompiler provider is initialized and every CPU is an interpreter under `GT4GS_NO_JIT` | nothing generates or runs JIT code |
| `pcsx2/vtlb.cpp` | `vtlb_BackpatchLoadStore` answers false under `GT4GS_NO_JIT` | there is no generated code to backpatch |
| `pcsx2/EECycleRate.cpp` | the ARM64 repatch path is skipped under `GT4GS_NO_JIT` | no generated code to patch |
| `pcsx2/GS/Renderers/Common/GSFunctionMap.h`, `pcsx2/GS/Renderers/SW/GSDrawScanline.{h,cpp}` | the generators are forward-declared and `ENABLE_JIT_RASTERIZER` is off under `GT4GS_NO_JIT`; `GetDefaultFunction` returns null | the C++ software rasterizer is the only path; the generator sources are not compiled |
| `common/HeapArray.h` | `operator<=>` written out element by element | NDK 26.1 ships libc++ 17, which has no `std::lexicographical_compare_three_way` |
| `pcsx2/EeFpuModel.h` | `GT4GS_NO_PRESERVE_ALL` selects the wide-spill form | NDK 26.1 is clang 17, whose AArch64 backend refuses `__attribute__((preserve_all))`; the attribute exists for recompilers this library never runs |
| `platforms/android/app/src/main/cpp/CMakeLists.txt`, `CMakeLists-app-emucore.txt` | the private entry point replaces the emulator app's, builds the core plus the GT4Recomp frontend, forces `GT4GS_NO_JIT` and `GT4GS_NO_PRESERVE_ALL` | the app is GT4Recomp, not the ARMSX2 emulator app |
| `platforms/android/app/src/main/cpp/cmake/SearchForStuff.cmake` | do not add `3rdparty/vixl` when `GT4GS_NO_JIT` is set | the tree is not needed and is not imported |

Not part of the vendored tree, but part of the same renderer work:

- `android/armsx2gs/` — the in-process frontend (`libgt4gs.so`): stream queue, replayer, HUD, C ABI.
- `android/stage-armsx2gs.cmd` — stages the built library plus `GameIndex.yaml`, the shaders (`common` and `opengl`) and
  the ImGui fonts, including `RobotoMono-Medium.ttf`, into the APK. The fonts are what make the HUD use CoreGS 2.0's
  monospace face and metrics.
- `-DUSE_VULKAN=ON` in the core build and the `shaderc` link in `android/armsx2gs/CMakeLists.txt` — the Vulkan renderer
  compiles its shaders at run time; the GLES renderer stays available.

## Building the library from the tracked tree

```
python tools/fetch_armsx2_shader_deps.py

cmake -S third_party/armsx2/platforms/android/app/src/main/cpp -B build/armsx2-core -G Ninja
  -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DANDROID=true
  -DCMAKE_BUILD_TYPE=Release
  -DUSE_VULKAN=ON -DUSE_OPENGL=ON -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_GSRUNNER=OFF
  -DGT4GS_FRONTEND_DIR=<this repository>/android/armsx2gs
cmake --build build/armsx2-core --target gt4gs -j 10

android\stage-armsx2gs.cmd arm64-v8a build\armsx2-core
```

`GT4GS_NO_JIT` and `GT4GS_NO_PRESERVE_ALL` are set by the entry point itself; no extra compiler flags are needed.
`GT4_ARMSX2_ROOT` makes `stage-armsx2gs.cmd` read a local ARMSX2 tree instead of the tracked one.

Proof from the tracked tree (2026-11-06): a clean configure and `libgt4gs.so` build succeeded, `llvm-nm` reports
**0 `vixl::` symbols and 0 recompiler entries**, and the APK built with it runs Gran Turismo 4 v2.00 in the emulator
through the Vulkan ARMSX2 renderer (`build/evidence/t180-tracked-import/`, not committed).

## Maintenance

1. Fetch the new ARMSX2 commit and re-import the same selection (see the list above; keep the 50 MB and no-prebuilt
   rules).
2. Re-apply the patches in this document, in the same order.
3. Re-run `tools/fetch_armsx2_shader_deps.py`; if the new shaderc pins different revisions, update `PINS` in the script
   (it refuses to run when the `DEPS` file disagrees).
4. Rebuild `libgt4gs.so`, stage it and test the APK before merging.
