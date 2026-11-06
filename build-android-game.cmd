@echo off
rem GT4Recomp private test build: libgt4.so (arm64-v8a) with the ARMSX2 GS renderer. Inputs are read-only from the
rem main checkout; the translated units were regenerated into this clone's private/ folder (pinned hashes checked).
setlocal
set "GT4_EE_UNITS_DIR=C:/Antigravity/gt4-private-armsx2gs/private/v200-units-rc14b"
set "GT4_HW_SHADER_CPP_DIR=C:/Antigravity/gt4-private-armsx2gs/build/hw-shaders"
call "%~dp0android\native\configure-android.cmd" arm64-v8a "C:\Antigravity\gt4-private-armsx2gs\build\android-arm64"
exit /b %errorlevel%
