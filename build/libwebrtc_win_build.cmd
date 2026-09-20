@echo off

setlocal enabledelayedexpansion

set arch=
set profile=release
set commit=

:arg_loop
if "%1" == "" goto end_arg_loop
if "%1" == "--arch" (
    set "arch=%~2"
    shift & shift & goto arg_loop
)
if "%1" == "--profile" (
    set "profile=%~2"
    shift & shift & goto arg_loop
)
if "%1" == "--commit" (
    set "commit=%~2"
    shift & shift & goto arg_loop
)
echo Error: Unknown argument '%1'
exit /b 1
:end_arg_loop

if not "!arch!" == "x64" if not "!arch!" == "arm64" (
    echo Error: Invalid value for --arch. Must be 'x64' or 'arm64'.
    exit /b 1
)
if not "!profile!" == "debug" if not "!profile!" == "release" (
    echo Error: Invalid value for --profile. Must be 'debug' or 'release'.
    exit /b 1
)

echo "Building  libwebrtc dll - Windows"
echo "Arch: !arch!"
echo "Profile: !profile!"
echo "Commit: !commit!"

if not exist depot_tools (
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
  if errorlevel 1 exit /b 1
)

set COMMAND_DIR=%~dp0
set PATH=%cd%\depot_tools;%PATH%
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
set GYP_GENERATORS=ninja,msvs-ninja
set GYP_MSVS_VERSION=2022
set OUTPUT_DIR=src\out-!arch!-!profile!
set ARTIFACTS_DIR=%cd%\libwebrtc-!arch!-!profile!
set vs2019_install=C:\Program Files\Microsoft Visual Studio\2022\Enterprise

set "default_branch="
for /f "tokens=2 delims=@" %%i in ('findstr /c:"url" "%COMMAND_DIR%.gclient"') do (
  for /f "tokens=1 delims='" %%j in ("%%i") do set "default_branch=%%j"
)
if "!default_branch!" == "" (
  echo Error: cannot extract default branch from %COMMAND_DIR%.gclient
  exit /b 1
)
if not "!commit!" == "" (
  set "checkout_ref=!commit!"
) else (
  set "checkout_ref=!default_branch!"
)
echo "Checkout ref: !checkout_ref!"

if not exist src (
  call git clone https://github.com/webrtc-sdk/webrtc.git src
if errorlevel 1 exit /b 1
)

pushd src
if errorlevel 1 exit /b 1
call git fetch
if errorlevel 1 exit /b 1
call git checkout -f "!checkout_ref!"
if errorlevel 1 exit /b 1
call git clean -df
if errorlevel 1 exit /b 1
popd

call gclient.bat sync --with_branch_heads --jobs=8
if errorlevel 1 exit /b 1


if not exist src/libwebrtc (
  md "src/libwebrtc"
  xcopy "%COMMAND_DIR%..\include" "src\libwebrtc\include" /E /I /Y
  xcopy "%COMMAND_DIR%..\src" "src\libwebrtc\src" /E /I /Y
  xcopy "%COMMAND_DIR%..\patches" "src\libwebrtc\patches" /E /I /Y
  xcopy "%COMMAND_DIR%..\test" "src\libwebrtc\test" /E /I /Y
  copy "%COMMAND_DIR%..\BUILD.gn" "src\libwebrtc\"
  copy "%COMMAND_DIR%..\LICENSE" "src\libwebrtc\"
)

cd src
copy .vpython3 ..
call git apply "libwebrtc\patches\tsnx_hardware_clock_api.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
call git apply "libwebrtc\patches\custom_audio_source_m144.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
call git apply "libwebrtc\patches\external_recording_demand.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
call git apply "libwebrtc\patches\external_pcm_playout.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
call git apply "libwebrtc\patches\pcm_playout_sdk.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
call git apply "libwebrtc\patches\concurrent_pcm_playout.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
call git apply "libwebrtc\patches\add_libwebrtc_build_target.patch" -v --ignore-space-change --ignore-whitespace --whitespace=nowarn || exit /b 1
cd ..

if not exist "%ARTIFACTS_DIR%\lib" (
  mkdir "%ARTIFACTS_DIR%\lib"
)

if not exist "%ARTIFACTS_DIR%\include" (
  mkdir "%ARTIFACTS_DIR%\include"
)

set "debug=false"
if "!profile!" == "debug" (
  set "debug=true"
)

rem generate ninja for release
call gn.bat gen %OUTPUT_DIR% --root="src" ^
  --args="is_debug=!debug! is_clang=true target_cpu=\"!arch!\" use_custom_libcxx=false rtc_libvpx_build_vp9=true enable_libaom=true rtc_include_tests=true rtc_build_examples=false rtc_build_tools=false is_component_build=false rtc_enable_protobuf=false rtc_use_h264=true ffmpeg_branding=\"Chrome\" symbol_level=0 enable_iterator_debugging=false"
if errorlevel 1 exit /b 1

rem Build the artifact and its focused wrapper/core ownership tests.
ninja.exe -C %OUTPUT_DIR% libwebrtc libwebrtc_cpp_api_unittests external_recording_demand_unittests pcm_playout_unittests pcm_factory_unittests

if errorlevel 1 exit /b 1

if "!arch!" == "x64" (
  %OUTPUT_DIR%\libwebrtc_cpp_api_unittests.exe --gtest_filter=AudioProcessing.*:AudioDevice.RecordingStateReadbackHasNoCaptureSideEffect:AudioDevice.ActivePlayoutRouteReadbackIsGraceful
  if errorlevel 1 exit /b 1

  %OUTPUT_DIR%\pcm_playout_unittests.exe
  if errorlevel 1 exit /b 1
  %OUTPUT_DIR%\pcm_factory_unittests.exe
  if errorlevel 1 exit /b 1
  %OUTPUT_DIR%\external_recording_demand_unittests.exe
  if errorlevel 1 exit /b 1
) else (
  echo ARM64 tests compiled; execution requires an ARM64 host.
)

rem Retain same-build tests for x64 and ARM64 qualification without devices.
set "TESTS_DIR=%cd%\win-!arch!-!profile!-tests"
if not exist "!TESTS_DIR!" mkdir "!TESTS_DIR!"
for %%F in (libwebrtc.dll libwebrtc_cpp_api_unittests.exe external_recording_demand_unittests.exe pcm_playout_unittests.exe pcm_factory_unittests.exe) do (
  copy /Y "%OUTPUT_DIR%\%%F" "!TESTS_DIR!\" >nul
  if errorlevel 1 exit /b 1
)

rem copy static library for release build
copy "%OUTPUT_DIR%\libwebrtc.dll.lib" "%ARTIFACTS_DIR%\lib"
copy "%OUTPUT_DIR%\libwebrtc.dll" "%ARTIFACTS_DIR%\lib"

rem copy license
copy "src\libwebrtc\LICENSE" "%ARTIFACTS_DIR%\"

rem copy header
xcopy "src\libwebrtc\include\*.h" "%ARTIFACTS_DIR%\include" /S /I /F /H
if errorlevel 1 exit /b 1
