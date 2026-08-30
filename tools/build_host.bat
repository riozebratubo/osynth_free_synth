@echo off
REM Configure and build the osynth host port (port/host) with MSVC.
REM
REM vcvars64.bat has to run in the same shell as cmake, and chaining the two
REM through `cmd /c` from another shell loses the quoting around the Program
REM Files path -- hence a script rather than a one-liner.
REM
REM Usage, from the repo root:
REM     tools\build_host.bat              build (configures if needed)
REM     tools\build_host.bat clean        delete build_host first
REM
REM Override the toolchain location with OSYNTH_VCVARS if Visual Studio is
REM somewhere else.

setlocal

if "%OSYNTH_VCVARS%"=="" (
  set "OSYNTH_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
)

if not exist "%OSYNTH_VCVARS%" (
  echo ERROR: vcvars64.bat not found at:
  echo   %OSYNTH_VCVARS%
  echo Set OSYNTH_VCVARS to its path and re-run.
  exit /b 1
)

cd /d "%~dp0.."

if /i "%~1"=="clean" (
  echo Removing build_host...
  if exist build_host rmdir /s /q build_host
)

call "%OSYNTH_VCVARS%" >nul
if errorlevel 1 (
  echo ERROR: vcvars64.bat failed.
  exit /b 1
)

REM The Visual Studio generator is multi-config: it ignores CMAKE_BUILD_TYPE at
REM configure time and takes --config at build time instead. Both are passed so
REM this script behaves the same under a single-config generator (Ninja, make).
if "%OSYNTH_CONFIG%"=="" set "OSYNTH_CONFIG=Release"

if not exist build_host\CMakeCache.txt (
  cmake -S port/host -B build_host -DCMAKE_BUILD_TYPE=%OSYNTH_CONFIG%
  if errorlevel 1 exit /b 1
)

cmake --build build_host --config %OSYNTH_CONFIG%
exit /b %errorlevel%
