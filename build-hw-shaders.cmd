@echo off
rem GT4Recomp private test build: builds the CoreGS 2.0 shader C++ standalone (the Android library reuses them).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set VULKAN_SDK=C:\VulkanSDK\1.4.341.1
cmake -S native\hw_gs\shaders -B build\hw-shaders -G Ninja ^
  -DCMAKE_C_COMPILER="C:\Program Files\LLVM\bin\clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:\Program Files\LLVM\bin\clang-cl.exe" ^
  -DGT4_HW_GLSLANG="C:\VulkanSDK\1.4.341.1\Bin\glslangValidator.exe"
if errorlevel 1 exit /b 1
cmake --build build\hw-shaders -j 8
exit /b %errorlevel%
