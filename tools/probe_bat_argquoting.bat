@echo off
:: Checks how deploy_android.bat accumulates quoted -D args and how they arrive
:: at the called program (paths with spaces + a semicolon-separated ABI list).
setlocal EnableExtensions EnableDelayedExpansion
set "ANDROID_PLATFORM=android-28"
set "SDK_BUILD_TOOLS=35.0.0"
set "ABIS=arm64-v8a;armeabi-v7a"
set "SDK=C:\Users\me\AppData\Local\Android\Sdk"
set "EXTRA_ARGS="
if defined ANDROID_PLATFORM set "EXTRA_ARGS=!EXTRA_ARGS! "-DANDROID_PLATFORM=!ANDROID_PLATFORM!""
if defined SDK_BUILD_TOOLS  set "EXTRA_ARGS=!EXTRA_ARGS! "-DQT_ANDROID_SDK_BUILD_TOOLS_REVISION=!SDK_BUILD_TOOLS!""
echo RAW EXTRA_ARGS=[!EXTRA_ARGS!]
call :showargs "-DQT_ANDROID_ABIS=!ABIS!" "-DANDROID_SDK_ROOT=!SDK!" !EXTRA_ARGS!
exit /b 0

:showargs
set "n=0"
:loop
if "%~1"=="" goto :eof
set /a n+=1
echo   arg!n! = [%~1]
shift
goto :loop
