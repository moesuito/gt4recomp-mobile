@echo off
rem GT4Recomp private Android test build: stages the ARMSX2 GS library and the resources it reads into the app.
rem usage: stage-armsx2gs.cmd <arm64-v8a> <armsx2-core build folder>
rem   libgt4gs.so (stripped of line tables, symbol table kept) into app\src\main\jniLibs\<ABI>\
rem   GameIndex.yaml and the WHOLE resources\shaders tree into app\src\main\assets\armsx2gs\resources\ (the app copies
rem   them to its external files folder on Play). The shaders tree includes shaders\common (fxaa.fx and the FidelityFX
rem   headers) as well as shaders\opengl: the GLES device reads fxaa.fx while it is being created, and without it the
rem   device fails at start-up and the game shows no picture (owner's S23 log, 2026-11-06).
rem Never committed.
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
if not exist "%ASSETS%" mkdir "%ASSETS%"
"%GT4_ANDROID_NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-debug -o "%JNI%\libgt4gs.so" "%BUILD%\gt4gs\libgt4gs.so" || exit /b 1
copy /y "%VENDOR%\bin\resources\GameIndex.yaml" "%ASSETS%\GameIndex.yaml" >nul || exit /b 1
rem The fonts the Performance HUD and the OSD use: the same RobotoMono-Medium the CoreGS HUD shows (owner, 2026-11-06,
rem round 5: the ARMSX2 panel used the plain Roboto fallback because these were missing).
if exist "%ASSETS%\fonts" rmdir /s /q "%ASSETS%\fonts"
xcopy /e /i /y /q "%VENDOR%\bin\resources\fonts" "%ASSETS%\fonts" >nul || exit /b 1
rem Replace the staged tree so a removed resource cannot linger from an earlier staging.
if exist "%ASSETS%\shaders" rmdir /s /q "%ASSETS%\shaders"
xcopy /e /i /y /q "%VENDOR%\bin\resources\shaders" "%ASSETS%\shaders" >nul || exit /b 1
echo staged %ABI%: libgt4gs.so, GameIndex.yaml, shaders (opengl + common), fonts (RobotoMono)
exit /b 0
:usage
echo usage: stage-armsx2gs.cmd ^<arm64-v8a^> ^<armsx2-core build folder^>
exit /b 2
