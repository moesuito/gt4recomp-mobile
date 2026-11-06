@echo off
rem GT4Recomp private Android test build: stages the ARMSX2 GS library and the resources it reads into the app.
rem usage: stage-armsx2gs.cmd <arm64-v8a> <armsx2-core build folder>
rem   libgt4gs.so (stripped of line tables, symbol table kept) into app\src\main\jniLibs\<ABI>\
rem   GameIndex.yaml and the OpenGL ES shader sources into app\src\main\assets\armsx2gs\resources\ (the app copies them to
rem   its external files folder on Play). Never committed.
setlocal
set "ABI=%~1"
set "BUILD=%~2"
if "%ABI%"=="" goto usage
if "%BUILD%"=="" goto usage
if "%GT4_ANDROID_NDK%"=="" set "GT4_ANDROID_NDK=%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125"
set "VENDOR=%~dp0..\private\armsx2-vendor"
set "APP=%~dp0..\launcher\crates\gt4l-tauri\gen\android\app"
set "JNI=%APP%\src\main\jniLibs\%ABI%"
set "ASSETS=%APP%\src\main\assets\armsx2gs\resources"
if not exist "%JNI%" mkdir "%JNI%"
if not exist "%ASSETS%\shaders\opengl" mkdir "%ASSETS%\shaders\opengl"
"%GT4_ANDROID_NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-debug -o "%JNI%\libgt4gs.so" "%BUILD%\gt4gs\libgt4gs.so" || exit /b 1
copy /y "%VENDOR%\bin\resources\GameIndex.yaml" "%ASSETS%\GameIndex.yaml" >nul || exit /b 1
copy /y "%VENDOR%\bin\resources\shaders\opengl\*.glsl" "%ASSETS%\shaders\opengl\" >nul || exit /b 1
echo staged %ABI%: libgt4gs.so, GameIndex.yaml and the OpenGL ES shaders
exit /b 0
:usage
echo usage: stage-armsx2gs.cmd ^<arm64-v8a^> ^<armsx2-core build folder^>
exit /b 2
