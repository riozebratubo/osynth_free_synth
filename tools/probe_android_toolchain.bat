@echo off
:: Isolated copy of the JDK / Ninja / CMake detection block from
:: deploy_android.bat, so it can be exercised without reaching the build step.
setlocal EnableExtensions EnableDelayedExpansion
set "QT_ROOT=C:\Qt"
set "JAVA_HOME="

if defined JAVA_HOME if not exist "!JAVA_HOME!\bin\javac.exe" (
    echo WARN: JAVA_HOME has no javac - ignoring
    set "JAVA_HOME="
)
if not defined JAVA_HOME (
    for %%p in (
        "%ProgramFiles%\Android\Android Studio\jbr"
        "%ProgramFiles%\Android\Android Studio\jre"
    ) do if not defined JAVA_HOME if exist "%%~p\bin\javac.exe" set "JAVA_HOME=%%~p"
)
if not defined JAVA_HOME (
    for %%r in (
        "%ProgramFiles%\Java"
        "%ProgramFiles%\Java\latest"
        "%ProgramFiles%\Eclipse Adoptium"
        "%ProgramFiles%\Microsoft"
        "%ProgramFiles%\Amazon Corretto"
    ) do (
        for /f "delims=" %%d in ('dir /b /ad "%%~r" 2^>nul') do (
            if exist "%%~r\%%d\bin\javac.exe" set "JAVA_HOME=%%~r\%%d"
        )
    )
)
if not defined JAVA_HOME (
    echo RESULT: no JDK found
) else (
    echo RESULT JAVA_HOME=!JAVA_HOME!
)

if not defined NINJA if exist "!QT_ROOT!\Tools\Ninja\ninja.exe" set "NINJA=!QT_ROOT!\Tools\Ninja\ninja.exe"
if not defined NINJA for /f "delims=" %%p in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%p"
echo RESULT NINJA=!NINJA!

if not defined CMAKE_EXE if exist "!QT_ROOT!\Tools\CMake_64\bin\cmake.exe" set "CMAKE_EXE=!QT_ROOT!\Tools\CMake_64\bin\cmake.exe"
if not defined CMAKE_EXE for /f "delims=" %%p in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%p"
echo RESULT CMAKE=!CMAKE_EXE!

if defined JAVA_HOME "!JAVA_HOME!\bin\javac.exe" -version
exit /b 0
