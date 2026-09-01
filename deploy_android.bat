@echo off
:: deploy_android.bat - Build the Osyntho companion app into a single Android
:: APK carrying BOTH arm64-v8a and armeabi-v7a native libraries.
::
:: Usage:   deploy_android.bat [output_dir] [options]
:: Example: deploy_android.bat --clean
::          deploy_android.bat C:\somewhere\else --release --sign
::
:: The APK lands in private_releases\app<version>\ unless [output_dir] says
:: otherwise, alongside the other artifacts built for that version.
::
:: Unlike deploy_linux.sh / deploy_macos_multi.sh, which package an *existing*
:: build dir, this script configures and builds too. It has to: the set of ABIs
:: baked into an APK is decided at CMake configure time by QT_ANDROID_ABIS, so
:: there is no "already built app" to bolt a second architecture onto after the
:: fact.
::
:: How the multi-ABI APK is produced (Qt6AndroidMacros.cmake):
::   * The Qt kit used for the configure decides the PRIMARY ABI
::     (android_arm64_v8a -> arm64-v8a). That ABI is always in the APK.
::   * QT_ANDROID_ABIS lists every ABI the APK should carry. Qt drops the
::     primary from that list and spawns one ExternalProject per remaining ABI,
::     each configured against the sibling Qt kit for that ABI
::     (<Qt>/<version>/android_armv7 for armeabi-v7a, and so on).
::   * androiddeployqt then collects all of them into one APK under
::     <build>/android-build/, with libs/<abi>/libosyntho_<abi>.so per ABI.
:: So the sibling Qt kit for EVERY requested ABI must be installed, with the
:: same modules (Bluetooth, Multimedia, Sql, ...) as the primary one. The script
:: checks that up front instead of letting the build fail 10 minutes in.
::
:: Options:
::   --abis "a;b"      ABI list (default "arm64-v8a;armeabi-v7a"). The FIRST
::                     entry selects the primary Qt kit, so "--abis x86_64"
::                     really does build an x86_64-only APK.
::   --debug           Debug build. This is the DEFAULT: Gradle signs debug APKs
::                     with the automatic debug keystore, so what comes out
::                     installs on a phone straight away.
::   --release         Release build - optimised and stripped, but UNSIGNED
::                     unless a keystore is configured, and Android will not
::                     install an unsigned APK (see --sign).
::   --sign            Require APK signing. Fails if QT_ANDROID_KEYSTORE_PATH is
::                     unset. Signing is enabled automatically whenever that
::                     variable IS set, so this flag is about failing loudly
::                     rather than shipping an unsigned artifact by accident.
::   --aab             Also build the Play Store bundle (.aab).
::   --standalone      Build the standalone app (OSYNTHO_EMBEDDED=ON): the synth
::                     engine compiled in, no BLE. Its own package name
::                     (org.osynth.osyntho.standalone), so it installs alongside
::                     the controller instead of replacing it.
::   --controller      Build the BLE controller (OSYNTHO_EMBEDDED=OFF),
::                     package org.osynth.osyntho. This is the DEFAULT.
::   --check           Resolve and print the toolchain, then stop. Nothing is
::                     configured, built or deleted - use it to find out whether
::                     a machine can build the APK at all.
::   --clean           Wipe the build dir before configuring.
::   --build-dir DIR   Where to build (default app_osyntho\build\android-<tag>).
::   -h, --help        This text.
::
:: Environment overrides (all optional, all auto-detected otherwise):
::   QT_ROOT                   Qt install root                (default C:\Qt)
::   QT_VERSION                Qt version dir under QT_ROOT   (default: newest)
::   QT_DIR                    Full path to the Qt version dir, e.g.
::                             C:\Qt\6.11.0 - overrides QT_ROOT/QT_VERSION
::   ANDROID_SDK_ROOT          Android SDK  (default %LOCALAPPDATA%\Android\Sdk)
::   ANDROID_NDK_ROOT          Android NDK  (default: newest under SDK\ndk)
::   JAVA_HOME                 JDK 17+ used by Gradle
::   ANDROID_PLATFORM          NDK API level, e.g. android-28 (Qt's default)
::   SDK_BUILD_TOOLS           Pin build-tools, e.g. 35.0.0. Qt otherwise picks
::                             the highest installed, which can be a preview
::                             revision that Qt's Gradle plugin rejects.
::   QT_ANDROID_KEYSTORE_PATH        )
::   QT_ANDROID_KEYSTORE_ALIAS       ) read by androiddeployqt --sign
::   QT_ANDROID_KEYSTORE_STORE_PASS  )
::   QT_ANDROID_KEYSTORE_KEY_PASS    )

setlocal EnableExtensions EnableDelayedExpansion

:: Output is deliberately plain text, unlike the ANSI colours in
:: deploy_linux.sh / deploy_macos_multi.sh. cmd has no literal ESC, and the
:: usual "prompt $E" capture trick silently yields the string "rem" when the
:: script's output is redirected to a file - which is exactly when a build log
:: full of "rem[0;32m" is least welcome.

:: -- App identity -----------------------------------------------------------
:: APP_NAME is the CMake project()/target name, and also android.app.lib_name in
:: assets/android-build/AndroidManifest.xml.in. APP_FILE_BASE is the display-name
:: identity without spaces, used for artifact filenames (same as the Linux and
:: macOS scripts, so releases line up).
set "APP_NAME=osyntho"
:: Defaults for the controller build. --standalone rewrites the last three
:: below, once the arguments have been parsed: the two are separate Android
:: packages precisely so that installing one does not remove the other.
set "APP_DISPLAY_NAME=Osyntho"
set "APP_FILE_BASE=Osyntho"
set "APP_ID=org.osynth.osyntho"

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

:: The Qt app lives in app_osyntho\; this script sits at the repository root
:: next to the firmware's CMakeLists.txt (an ESP-IDF project that has nothing to
:: do with the APK). Fall back to SCRIPT_DIR if the script is ever moved in.
if exist "%SCRIPT_DIR%\app_osyntho\CMakeLists.txt" (
    set "APP_SRC_DIR=%SCRIPT_DIR%\app_osyntho"
) else (
    set "APP_SRC_DIR=%SCRIPT_DIR%"
)

:: -- Defaults ---------------------------------------------------------------
:: The point of this script: one APK, both ARM ABIs. armeabi-v7a keeps 32-bit
:: devices working; arm64-v8a is what every current phone runs and what Play
:: requires. x86/x86_64 are emulator-only here, so they are opt-in via --abis.
if not defined QT_ANDROID_ABIS (
    set "ABIS=arm64-v8a;armeabi-v7a"
) else (
    set "ABIS=%QT_ANDROID_ABIS%"
)
:: Debug by default: Gradle signs debug APKs with the automatic debug keystore,
:: so the artifact installs. A Release build without a keystore is unsigned and
:: Android refuses it, which is a poor thing for a bare invocation to produce.
set "BUILD_TYPE=Debug"
:: OFF matches the CMake default: the controller is the build most people want,
:: and it is the package already published. Passed explicitly all the same, so
:: that the package name this script prints is the one the APK actually gets
:: even when the build dir was last configured the other way round.
set "EMBEDDED=OFF"
set "DO_CHECK=0"
set "DO_CLEAN=0"
set "DO_AAB=0"
set "REQUIRE_SIGN=0"
set "OUTPUT_DIR="
set "BUILD_DIR="

:: -- Parse arguments --------------------------------------------------------
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="-h"          goto :usage
if /i "%~1"=="--help"      goto :usage
if /i "%~1"=="/?"          goto :usage
if /i "%~1"=="--check"     ( set "DO_CHECK=1"        & shift & goto :parse_args )
if /i "%~1"=="--clean"     ( set "DO_CLEAN=1"        & shift & goto :parse_args )
if /i "%~1"=="--debug"     ( set "BUILD_TYPE=Debug"   & shift & goto :parse_args )
if /i "%~1"=="--release"   ( set "BUILD_TYPE=Release" & shift & goto :parse_args )
if /i "%~1"=="--aab"       ( set "DO_AAB=1"          & shift & goto :parse_args )
if /i "%~1"=="--standalone" ( set "EMBEDDED=ON"      & shift & goto :parse_args )
if /i "%~1"=="--controller" ( set "EMBEDDED=OFF"     & shift & goto :parse_args )
if /i "%~1"=="--sign"      ( set "REQUIRE_SIGN=1"    & shift & goto :parse_args )
if /i "%~1"=="--abis"      ( set "ABIS=%~2"          & shift & shift & goto :parse_args )
if /i "%~1"=="--build-dir" ( set "BUILD_DIR=%~2"     & shift & shift & goto :parse_args )
set "_arg=%~1"
if "!_arg:~0,1!"=="-" ( call :die "Unknown option: %~1  -- run with --help" & exit /b 1 )
if defined OUTPUT_DIR ( call :die "Too many positional arguments: %~1" & exit /b 1 )
set "OUTPUT_DIR=%~1"
shift
goto :parse_args
:args_done

if not defined ABIS ( call :die "--abis was given an empty list" & exit /b 1 )

:: -- Variant identity -------------------------------------------------------
:: Mirrors the APP_ID / APP_DISPLAY_NAME derivation in
:: app_osyntho\CMakeLists.txt. Kept in step with it by hand; the values are
:: printed below, so a drift shows up before the build rather than as an
:: install that refuses with a conflicting-provider error.
if /i "!EMBEDDED!"=="ON" (
    set "APP_ID=org.osynth.osyntho.standalone"
    set "APP_DISPLAY_NAME=Osyntho Standalone"
    set "APP_FILE_BASE=Osyntho-Standalone"
    set "VARIANT=standalone - embedded synth engine"
) else (
    set "VARIANT=controller - BLE"
)

:: The first ABI selects the primary Qt kit, so it is the one that decides which
:: android_* directory we configure against.
for /f "tokens=1 delims=;" %%a in ("!ABIS!") do set "PRIMARY_ABI=%%a"
call :abi_to_kit "!PRIMARY_ABI!" PRIMARY_KIT
if not defined PRIMARY_KIT ( call :die "Unknown ABI '!PRIMARY_ABI!' - expected arm64-v8a, armeabi-v7a, x86 or x86_64" & exit /b 1 )

:: Filename tag: the ABI list with ';' turned into '-', so the artifact name
:: says exactly what is inside it (Osyntho-0.1.7-arm64-v8a-armeabi-v7a.apk).
set "ABI_TAG=!ABIS:;=-!"

:: -- App version ------------------------------------------------------------
:: Read from app_osyntho\CMakeLists.txt rather than repeated here: a hard-coded
:: copy drifts from the app within a release or two, and then every artifact is
:: stamped with a version the app never had. Same source the sibling scripts
:: use, and the same value CMake turns into the Android versionCode.
set "APP_VERSION="
for /f "tokens=3 delims=() " %%v in ('findstr /r /c:"set(APP_VERSION" "%APP_SRC_DIR%\CMakeLists.txt" 2^>nul') do (
    if not defined APP_VERSION set "APP_VERSION=%%v"
)
if not defined APP_VERSION set "APP_VERSION=0.0.0"

:: -- Directories ------------------------------------------------------------
:: private_releases\app<version>\ is where a release for this version is
:: assembled - the Windows osyntho.exe already lands there, and
:: tools\build_release.py drops the firmware images in the matching
:: firmware<version>\. Everything under private_releases\ is gitignored
:: (/private* in .gitignore), so artifacts never get committed by accident.
:: Set at this point in the script, not with the other defaults, because
:: APP_VERSION is only known once CMakeLists.txt has been read just above.
if not defined OUTPUT_DIR set "OUTPUT_DIR=%SCRIPT_DIR%\private_releases\app!APP_VERSION!"
:: The variant is part of the default path. The two configurations differ in
:: far more than a define -- one links Qt Bluetooth, the other compiles the
:: whole synth engine -- so sharing a cache between them buys nothing and
:: costs a package name silently left over from the previous run.
set "VARIANT_TAG="
if /i "!EMBEDDED!"=="ON" set "VARIANT_TAG=-standalone"
if not defined BUILD_DIR  set "BUILD_DIR=%APP_SRC_DIR%\build\android-!ABI_TAG!-!BUILD_TYPE!!VARIANT_TAG!"

:: Create it now rather than after the build: an unwritable output dir should
:: cost a second, not a full multi-ABI compile. --check is exempt so that it
:: really does leave the filesystem alone.
if "!DO_CHECK!"=="0" (
    if not exist "!OUTPUT_DIR!" mkdir "!OUTPUT_DIR!" 2>nul
    if not exist "!OUTPUT_DIR!" ( call :die "Cannot create output dir: !OUTPUT_DIR!" & exit /b 1 )
)
:: Resolve to absolute paths before anything cd's around.
for %%i in ("!OUTPUT_DIR!") do set "OUTPUT_DIR=%%~fi"
for %%i in ("!BUILD_DIR!")  do set "BUILD_DIR=%%~fi"

call :step "Osyntho Android build"
call :info "App          : !APP_DISPLAY_NAME! !APP_VERSION!"
call :info "Variant      : !VARIANT!"
call :info "Package      : !APP_ID!"
call :info "ABIs         : !ABIS!   [primary: !PRIMARY_ABI!]"
call :info "Build type   : !BUILD_TYPE!"
call :info "App source   : !APP_SRC_DIR!"
call :info "Build dir    : !BUILD_DIR!"
call :info "Output dir   : !OUTPUT_DIR!"

:: -- Detect Qt --------------------------------------------------------------
call :step "Detecting Qt for Android..."

if not defined QT_ROOT set "QT_ROOT=C:\Qt"

if not defined QT_DIR (
    if defined QT_VERSION (
        set "QT_DIR=!QT_ROOT!\!QT_VERSION!"
    ) else (
        rem Pick the newest version dir that actually has the primary ABI kit.
        rem Compare numerically: 'dir /o-n' would rank 6.9.0 above 6.11.0
        rem because it sorts as text.
        set "_best=0"
        for /f "delims=" %%d in ('dir /b /ad "!QT_ROOT!\6.*" 2^>nul') do (
            if exist "!QT_ROOT!\%%d\android_!PRIMARY_KIT!\bin\qt-cmake.bat" (
                for /f "tokens=1-3 delims=." %%a in ("%%d") do (
                    set "_p3=%%c"
                    if not defined _p3 set "_p3=0"
                    set /a "_key=%%a*1000000 + %%b*1000 + _p3" >nul 2>&1
                    if !_key! GTR !_best! (
                        set "_best=!_key!"
                        set "QT_DIR=!QT_ROOT!\%%d"
                    )
                )
            )
        )
    )
)

if not defined QT_DIR (
    call :die "No Qt install with an android_!PRIMARY_KIT! kit found under !QT_ROOT!."
    echo     Install it with the Qt Maintenance Tool ^(Qt 6.x ^> Android^), or point
    echo     the script at it:  set QT_DIR=C:\Qt\6.11.0
    exit /b 1
)

set "QT_PRIMARY_DIR=!QT_DIR!\android_!PRIMARY_KIT!"
set "QT_CMAKE=!QT_PRIMARY_DIR!\bin\qt-cmake.bat"
if not exist "!QT_CMAKE!" ( call :die "qt-cmake.bat not found: !QT_CMAKE!" & exit /b 1 )
call :info "Qt version dir : !QT_DIR!"
call :info "Primary kit    : !QT_PRIMARY_DIR!"

:: Every non-primary ABI is built by an ExternalProject against its own kit, and
:: Qt only reports the missing toolchain at generate time with a wall of text.
:: Check here so the failure names the exact kit to install.
call :step "Checking Qt kits for every requested ABI..."
for %%a in (!ABIS!) do (
    call :abi_to_kit "%%a" _kit
    if not defined _kit ( call :die "Unknown ABI '%%a' in --abis" & exit /b 1 )
    if not exist "!QT_DIR!\android_!_kit!\lib\cmake\Qt6\qt.toolchain.cmake" (
        call :die "No Qt kit for ABI '%%a' - expected !QT_DIR!\android_!_kit!"
        echo     Add it in the Qt Maintenance Tool, under the same Qt version.
        exit /b 1
    )
    rem The app links Bluetooth/Multimedia/Sql/Concurrent/Network; a kit missing
    rem one of them configures fine and then fails deep inside the ABI's
    rem external project, where the error does not mention the ABI at all.
    for %%m in (Bluetooth Multimedia Sql Concurrent Network QuickControls2) do (
        if not exist "!QT_DIR!\android_!_kit!\lib\cmake\Qt6%%m" (
            call :warn "Qt6%%m missing from the %%a kit - the build will fail for that ABI"
        )
    )
    rem No '-^>' arrow here: inside a for body cmd sees the '^>' as a redirect
    rem even in a quoted argument, and the line dies with "Access is denied".
    call :info "  %%a uses !QT_DIR!\android_!_kit!"
    set "_kit="
)

:: -- Detect Android SDK / NDK / JDK ----------------------------------------
call :step "Detecting Android SDK, NDK and JDK..."

:: Two Android SDKs on one machine is normal rather than exceptional: an
:: Android-Studio-shaped %LOCALAPPDATA%\Android\Sdk sitting next to the one Qt
:: Creator manages. Frequently only one of them has an NDK, and taking the first
:: root that merely exists lands on the other one - so score candidates by
:: whether they carry an NDK and only fall back to "exists" if none do.
set "SDK_CANDIDATES="%LOCALAPPDATA%\Android\Sdk" "C:\Android\Sdk" "%ProgramData%\Android\Sdk" "C:\Android\android-sdk""
if not defined ANDROID_SDK_ROOT if defined ANDROID_HOME set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
if not defined ANDROID_SDK_ROOT (
    set "_sdk_fallback="
    for %%s in (!SDK_CANDIDATES!) do (
        if not defined ANDROID_SDK_ROOT if exist "%%~s\platform-tools" (
            if not defined _sdk_fallback set "_sdk_fallback=%%~s"
            call :find_ndk "%%~s" _ndk_probe
            if defined _ndk_probe set "ANDROID_SDK_ROOT=%%~s"
            set "_ndk_probe="
        )
    )
    if not defined ANDROID_SDK_ROOT set "ANDROID_SDK_ROOT=!_sdk_fallback!"
)
if not defined ANDROID_SDK_ROOT (
    call :die "Android SDK not found. Install it via Android Studio or Qt Creator,"
    echo     then re-run, or:  set ANDROID_SDK_ROOT=C:\path\to\Sdk
    exit /b 1
)
:: Trailing backslashes turn into escapes once these end up in quoted -D args.
if "!ANDROID_SDK_ROOT:~-1!"=="\" set "ANDROID_SDK_ROOT=!ANDROID_SDK_ROOT:~0,-1!"

:: The NDK is a separate SDK package and is NOT installed by default.
if not defined ANDROID_NDK_ROOT if defined ANDROID_NDK set "ANDROID_NDK_ROOT=%ANDROID_NDK%"
if not defined ANDROID_NDK_ROOT call :find_ndk "!ANDROID_SDK_ROOT!" ANDROID_NDK_ROOT
if not defined ANDROID_NDK_ROOT (
    call :die "Android NDK not found under !ANDROID_SDK_ROOT!\ndk"
    rem Before sending anyone off to download 2 GB, check whether one of the
    rem other SDK roots on this machine already has an NDK - having a complete
    rem SDK next to a partial one is exactly how you end up reading this.
    for %%s in (!SDK_CANDIDATES!) do (
        call :find_ndk "%%~s" _other_ndk
        if defined _other_ndk (
            echo     There IS an NDK in another SDK on this machine:
            echo       set ANDROID_SDK_ROOT=%%~s
            echo     and re-run. The script normally prefers that root by itself.
            exit /b 1
        )
    )
    echo     Qt 6.11's Android libraries are built with NDK r27c, so install that
    echo     revision unless you know you need another.
    if exist "!ANDROID_SDK_ROOT!\cmdline-tools\latest\bin\sdkmanager.bat" (
        echo       "!ANDROID_SDK_ROOT!\cmdline-tools\latest\bin\sdkmanager.bat" "ndk;27.2.12479018"
    ) else (
        echo     This SDK has no cmdline-tools\latest\bin\sdkmanager.bat to install it
        echo     with, so use Qt Creator: Preferences ^> Devices ^> Android ^> SDK Manager,
        echo     or point the script at an NDK you already have:
        echo       set ANDROID_NDK_ROOT=C:\path\to\ndk\27.2.12479018
    )
    exit /b 1
)
if "!ANDROID_NDK_ROOT:~-1!"=="\" set "ANDROID_NDK_ROOT=!ANDROID_NDK_ROOT:~0,-1!"

:: Gradle needs a JDK 17+ (JAVA_HOME), not just a JRE. Android Studio ships one
:: as jbr\; the Oracle/Adoptium/Microsoft layouts are the other common ones.
:: Test for javac.exe, not java.exe: a JRE has the latter and Gradle dies on it
:: much later, complaining about a missing tools.jar instead of a missing JDK.
if defined JAVA_HOME if not exist "!JAVA_HOME!\bin\javac.exe" (
    call :warn "JAVA_HOME points at !JAVA_HOME! but has no bin\javac.exe - not a JDK, ignoring it"
    set "JAVA_HOME="
)
if not defined JAVA_HOME (
    for %%p in (
        "%ProgramFiles%\Android\Android Studio\jbr"
        "%ProgramFiles%\Android\Android Studio\jre"
    ) do if not defined JAVA_HOME if exist "%%~p\bin\javac.exe" set "JAVA_HOME=%%~p"
)
if not defined JAVA_HOME (
    rem Scan the usual vendor roots; the highest-named JDK in the last root that
    rem has one wins. Any JDK 17+ works, so this does not try to be clever.
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
    call :die "No JDK found. Gradle needs JAVA_HOME pointing at a JDK 17 or newer."
    echo     Install one ^(Android Studio ships a JDK in its jbr\ folder^), then:
    echo       set JAVA_HOME=C:\Program Files\Java\jdk-21
    exit /b 1
)
if "!JAVA_HOME:~-1!"=="\" set "JAVA_HOME=!JAVA_HOME:~0,-1!"

:: androiddeployqt shells out to Gradle, which reads these from the environment.
:: ANDROID_HOME is set alongside ANDROID_SDK_ROOT because parts of the Android
:: toolchain still look at the older name.
set "ANDROID_HOME=%ANDROID_SDK_ROOT%"
:: %PATH%, not !PATH!: delayed expansion would eat any '!' a directory on the
:: existing PATH happens to contain.
set "PATH=%JAVA_HOME%\bin;%PATH%"

:: -- Build tools ------------------------------------------------------------
:: Ninja: Qt's multi-ABI support forwards CMAKE_MAKE_PROGRAM to each per-ABI
:: external project, so one Ninja found here serves every ABI.
if not defined NINJA if exist "!QT_ROOT!\Tools\Ninja\ninja.exe" set "NINJA=!QT_ROOT!\Tools\Ninja\ninja.exe"
if not defined NINJA for /f "delims=" %%p in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%p"
if not defined NINJA (
    call :die "Ninja not found. The Qt installer ships it at !QT_ROOT!\Tools\Ninja\ninja.exe"
    echo     Install the 'Ninja' component in the Qt Maintenance Tool, or put ninja on PATH.
    exit /b 1
)

:: cmake for the --build step. qt-cmake.bat picks its own for the configure.
if not defined CMAKE_EXE if exist "!QT_ROOT!\Tools\CMake_64\bin\cmake.exe" set "CMAKE_EXE=!QT_ROOT!\Tools\CMake_64\bin\cmake.exe"
if not defined CMAKE_EXE for /f "delims=" %%p in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%p"
if not defined CMAKE_EXE ( call :die "cmake not found - install the CMake component with Qt, or put cmake on PATH" & exit /b 1 )

call :info "Android SDK  : !ANDROID_SDK_ROOT!"
call :info "Android NDK  : !ANDROID_NDK_ROOT!"
call :info "JDK          : !JAVA_HOME!"
call :info "CMake        : !CMAKE_EXE!"
call :info "Ninja        : !NINJA!"

:: -- Signing ----------------------------------------------------------------
:: androiddeployqt reads the keystore from the four QT_ANDROID_KEYSTORE_* env
:: vars when CMake passes --sign (QT_ANDROID_SIGN_APK=ON). A Release build with
:: no keystore produces an UNSIGNED apk, which Android refuses to install
:: (INSTALL_PARSE_FAILED_NO_CERTIFICATES) - so say so before the build, not
:: after ten minutes of compiling.
set "SIGN_ARG="
if defined QT_ANDROID_KEYSTORE_PATH (
    set "SIGN_ARG=-DQT_ANDROID_SIGN_APK=ON"
    call :info "Signing      : !QT_ANDROID_KEYSTORE_PATH! [alias !QT_ANDROID_KEYSTORE_ALIAS!]"
) else (
    if "!REQUIRE_SIGN!"=="1" (
        call :die "--sign was given but QT_ANDROID_KEYSTORE_PATH is not set."
        echo     Set all four before re-running:
        echo       set QT_ANDROID_KEYSTORE_PATH=C:\keys\osyntho.keystore
        echo       set QT_ANDROID_KEYSTORE_ALIAS=osyntho
        echo       set QT_ANDROID_KEYSTORE_STORE_PASS=...
        echo       set QT_ANDROID_KEYSTORE_KEY_PASS=...
        exit /b 1
    )
    if /i "!BUILD_TYPE!"=="Release" (
        call :warn "No keystore configured - this Release APK will be UNSIGNED and will not install."
        call :warn "  Drop --release for a debug-signed build that does, or set"
        call :warn "  QT_ANDROID_KEYSTORE_PATH and friends and re-run with --sign."
    )
)

:: Optional -D args, assembled here rather than inline in the configure call so
:: that --check below can show exactly what would be passed.
set "EXTRA_ARGS="
if defined ANDROID_PLATFORM set "EXTRA_ARGS=!EXTRA_ARGS! "-DANDROID_PLATFORM=!ANDROID_PLATFORM!""
if defined SDK_BUILD_TOOLS  set "EXTRA_ARGS=!EXTRA_ARGS! "-DQT_ANDROID_SDK_BUILD_TOOLS_REVISION=!SDK_BUILD_TOOLS!""
:: Always passed, never left to the CMake default: an existing build dir would
:: otherwise keep whatever OSYNTHO_EMBEDDED its cache already holds, and the
:: package name reported above would be a guess.
set "EXTRA_ARGS=!EXTRA_ARGS! "-DOSYNTHO_EMBEDDED=!EMBEDDED!""

:: -- --check: stop before touching anything ---------------------------------
:: Everything above only reads. Stopping here answers "is this machine set up to
:: build the APK, and with which toolchain?" in a second instead of a build, and
:: without --clean deleting a build tree on the way.
if "!DO_CHECK!"=="1" (
    echo.
    call :info "--check: toolchain resolved, stopping before configure."
    call :info "The configure would be:"
    echo.
    echo     call "!QT_CMAKE!" -S "!APP_SRC_DIR!" -B "!BUILD_DIR!" -G Ninja
    echo          "-DCMAKE_MAKE_PROGRAM=!NINJA!"
    echo          "-DCMAKE_BUILD_TYPE=!BUILD_TYPE!"
    echo          "-DQT_ANDROID_ABIS=!ABIS!"
    echo          "-DANDROID_SDK_ROOT=!ANDROID_SDK_ROOT!"
    echo          "-DANDROID_NDK_ROOT=!ANDROID_NDK_ROOT!" !SIGN_ARG! !EXTRA_ARGS!
    echo.
    exit /b 0
)

:: -- Clean ------------------------------------------------------------------
if "!DO_CLEAN!"=="1" (
    if exist "!BUILD_DIR!" (
        rem Only ever delete a directory that looks like our own build tree.
        rem --build-dir takes a user path, and this used to be the kind of line
        rem that eats whatever it is pointed at.
        if exist "!BUILD_DIR!\CMakeCache.txt" (
            call :step "Cleaning !BUILD_DIR!"
            rmdir /s /q "!BUILD_DIR!"
        ) else (
            call :warn "Not cleaning !BUILD_DIR! - no CMakeCache.txt there, so it is not a build tree"
        )
    )
)

:: -- Configure --------------------------------------------------------------
call :step "Configuring [!BUILD_TYPE!, ABIs: !ABIS!]..."

:: qt-cmake.bat is a batch file: without `call` the script would transfer to it
:: and never come back here. It exports CMAKE_TOOLCHAIN_FILE for the primary
:: ABI kit and forwards everything else to cmake.
:: QT_ANDROID_ABIS is quoted whole because cmd treats a bare ';' as an argument
:: separator, which would hand CMake a truncated list.
call "!QT_CMAKE!" ^
    -S "!APP_SRC_DIR!" ^
    -B "!BUILD_DIR!" ^
    -G Ninja ^
    "-DCMAKE_MAKE_PROGRAM=!NINJA!" ^
    "-DCMAKE_BUILD_TYPE=!BUILD_TYPE!" ^
    "-DQT_ANDROID_ABIS=!ABIS!" ^
    "-DANDROID_SDK_ROOT=!ANDROID_SDK_ROOT!" ^
    "-DANDROID_NDK_ROOT=!ANDROID_NDK_ROOT!" ^
    !SIGN_ARG! !EXTRA_ARGS!
if errorlevel 1 ( call :die "CMake configure failed - see the output above" & exit /b 1 )

:: -- Build ------------------------------------------------------------------
:: 'apk' is Qt's global convenience target; it depends on osyntho_make_apk,
:: which runs androiddeployqt after every per-ABI external project has been
:: built and its .so copied into android-build\libs\<abi>\.
call :step "Building the APK - this compiles the app once per ABI, so it takes a while..."
"!CMAKE_EXE!" --build "!BUILD_DIR!" --target apk
if errorlevel 1 ( call :die "APK build failed - see the output above" & exit /b 1 )

if "!DO_AAB!"=="1" (
    call :step "Building the AAB bundle..."
    "!CMAKE_EXE!" --build "!BUILD_DIR!" --target aab
    if errorlevel 1 call :warn "AAB build failed - the APK above is still valid"
)

:: -- Collect artifacts ------------------------------------------------------
call :step "Collecting artifacts..."

:: androiddeployqt copies the packaged APK to <build>\android-build\<target>.apk.
set "APK_SRC=!BUILD_DIR!\android-build\!APP_NAME!.apk"
if not exist "!APK_SRC!" (
    rem Fall back to Gradle's own output tree if Qt's copy step did not run.
    set "APK_SRC="
    for /f "delims=" %%f in ('dir /b /s "!BUILD_DIR!\android-build\build\outputs\apk\*.apk" 2^>nul') do (
        if not defined APK_SRC set "APK_SRC=%%f"
    )
)
if not defined APK_SRC ( call :die "No APK produced under !BUILD_DIR!\android-build" & exit /b 1 )
if not exist "!APK_SRC!" ( call :die "No APK produced under !BUILD_DIR!\android-build" & exit /b 1 )

set "APK_OUT=!OUTPUT_DIR!\!APP_FILE_BASE!-!APP_VERSION!-!ABI_TAG!.apk"
copy /y "!APK_SRC!" "!APK_OUT!" >nul
if errorlevel 1 ( call :die "Could not copy !APK_SRC! to !APK_OUT!" & exit /b 1 )
call :info "APK: !APK_OUT!"

set "AAB_OUT="
if "!DO_AAB!"=="1" (
    for /f "delims=" %%f in ('dir /b /s "!BUILD_DIR!\android-build\build\outputs\bundle\*.aab" 2^>nul') do (
        if not defined AAB_OUT (
            set "AAB_OUT=!OUTPUT_DIR!\!APP_FILE_BASE!-!APP_VERSION!.aab"
            copy /y "%%f" "!OUTPUT_DIR!\!APP_FILE_BASE!-!APP_VERSION!.aab" >nul
        )
    )
    if defined AAB_OUT ( call :info "AAB: !AAB_OUT!" ) else ( call :warn "No .aab found under android-build\build\outputs\bundle" )
)

:: -- Verify the ABIs actually landed in the APK -----------------------------
:: The whole point of the script is "both ABIs in ONE apk", and a silently
:: single-ABI APK looks completely normal until it refuses to install on a
:: 32-bit phone. Read lib/<abi>/ straight out of the zip and compare.
:: The PowerShell output goes through a file rather than `for /f`: the command
:: is full of parentheses, and cmd's `for /f (...)` set parser ends the set at
:: the first unescaped ')' inside the command string.
call :step "Verifying ABIs inside the APK..."
set "ABI_LIST_FILE=!BUILD_DIR!\apk-abis.txt"
set "APK_ABIS="
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $z=[IO.Compression.ZipFile]::OpenRead('!APK_OUT!'); $h=@{}; foreach($e in $z.Entries){ if($e.FullName -like 'lib/*/*'){ $h[($e.FullName -split '/')[1]]=1 } }; $z.Dispose(); [string]::Join(' ', $h.Keys)" > "!ABI_LIST_FILE!" 2>nul
if exist "!ABI_LIST_FILE!" set /p APK_ABIS=<"!ABI_LIST_FILE!"

if not defined APK_ABIS (
    call :warn "Could not read the APK contents - skipping the ABI check"
) else (
    call :info "ABIs in the APK: !APK_ABIS!"
    set "ABI_MISSING=0"
    for %%a in (!ABIS!) do (
        echo !APK_ABIS! | findstr /c:"%%a" >nul
        if errorlevel 1 (
            call :warn "  MISSING: %%a is not in the APK"
            set "ABI_MISSING=1"
        )
    )
    if "!ABI_MISSING!"=="0" call :info "All requested ABIs are present."
)

:: -- Report -----------------------------------------------------------------
set "APK_SIZE=0"
for %%f in ("!APK_OUT!") do set "APK_SIZE=%%~zf"
set /a "APK_MB=APK_SIZE/1048576" >nul 2>&1

echo.
rem No literal '!' in messages: delayed expansion is on, so "Done!  !APK_OUT!"
rem pairs the '!' after Done with the next one and swallows the variable.
call :info "Done. !APK_OUT!  [!APK_MB! MB]"
echo.
echo   Install : "!ANDROID_SDK_ROOT!\platform-tools\adb.exe" install -r "!APK_OUT!"
echo   Logs    : "!ANDROID_SDK_ROOT!\platform-tools\adb.exe" logcat -s Qt:* !APP_NAME!:* *:E
echo   Remove  : "!ANDROID_SDK_ROOT!\platform-tools\adb.exe" uninstall !APP_ID!
echo.
if /i "!BUILD_TYPE!"=="Release" if not defined QT_ANDROID_KEYSTORE_PATH (
    echo   This APK is UNSIGNED, so adb install will fail with
    echo   INSTALL_PARSE_FAILED_NO_CERTIFICATES. Rebuild without --release to
    echo   sideload, or configure QT_ANDROID_KEYSTORE_* and re-run with --sign
    echo   to ship it.
    echo.
)
echo   On the phone, Osyntho asks for Bluetooth and location permission on first
echo   scan. Denying them leaves the synth permanently undiscoverable - re-enable
echo   under Settings ^> Apps ^> !APP_DISPLAY_NAME! ^> Permissions, not by reinstalling.
exit /b 0

:: ---------------------------------------------------------------------------
:: Helpers. Callers that must stop the script use:  call :die "..." ^& exit /b 1
:: because `exit /b` inside a :label only returns from the call.
:: ---------------------------------------------------------------------------

:find_ndk
:: find_ndk <sdk_root> <out_var> - newest usable NDK under <sdk_root>, or
:: <out_var> left undefined. Qt's toolchain file chainloads
:: <ndk>\build\cmake\android.toolchain.cmake, so that file - not the directory -
:: is what makes an NDK real; a half-finished download leaves the dir behind.
:: 'dir /b' lists ascending, so the last hit is the highest revision.
set "%~2="
for /f "delims=" %%d in ('dir /b /ad "%~1\ndk" 2^>nul') do (
    if exist "%~1\ndk\%%d\build\cmake\android.toolchain.cmake" set "%~2=%~1\ndk\%%d"
)
if not defined %~2 if exist "%~1\ndk-bundle\build\cmake\android.toolchain.cmake" set "%~2=%~1\ndk-bundle"
goto :eof

:abi_to_kit
:: abi_to_kit <abi> <out_var> - map an Android ABI to Qt's android_<suffix> kit
:: directory name. Mirrors _qt_internal_get_android_abi_prefix_path in
:: Qt6AndroidMacros.cmake; leaves <out_var> undefined for an unknown ABI.
set "%~2="
if /i "%~1"=="arm64-v8a"   set "%~2=arm64_v8a"
if /i "%~1"=="armeabi-v7a" set "%~2=armv7"
if /i "%~1"=="x86"         set "%~2=x86"
if /i "%~1"=="x86_64"      set "%~2=x86_64"
goto :eof

:info
echo [deploy] %~1
goto :eof

:warn
echo [deploy] WARNING: %~1
goto :eof

:step
echo [deploy] ^>^>^> %~1
goto :eof

:die
echo.
rem Redirect first, so the message does not pick up a trailing space before '1'.
1>&2 echo ERROR: %~1
goto :eof

:usage
echo Usage: %~nx0 [output_dir] [options]
echo.
echo Builds the Osyntho app into ONE Android APK containing arm64-v8a and
echo armeabi-v7a native libraries, via Qt's QT_ANDROID_ABIS multi-ABI support.
echo.
echo Options:
echo   --abis "a;b"      ABI list, first entry picks the primary Qt kit
echo                     [default: arm64-v8a;armeabi-v7a]
echo   --debug           Debug build - debug-signed, installs directly [DEFAULT]
echo   --release         Release build - unsigned unless a keystore is configured
echo   --sign            Fail unless QT_ANDROID_KEYSTORE_PATH ^& co. are set
echo   --aab             Also build the Play Store .aab bundle
echo   --standalone      Embedded synth engine, package org.osynth.osyntho.standalone
echo   --controller      BLE controller, package org.osynth.osyntho [DEFAULT]
echo   --check           Print the resolved toolchain and stop - builds nothing
echo   --clean           Wipe the build dir first
echo   --build-dir DIR   Build tree location
echo   -h, --help        This text
echo.
echo Output dir defaults to private_releases\app^<version^>\ next to this script.
echo Environment overrides: QT_ROOT QT_VERSION QT_DIR ANDROID_SDK_ROOT
echo ANDROID_NDK_ROOT JAVA_HOME ANDROID_PLATFORM SDK_BUILD_TOOLS
echo QT_ANDROID_KEYSTORE_PATH QT_ANDROID_KEYSTORE_ALIAS
echo QT_ANDROID_KEYSTORE_STORE_PASS QT_ANDROID_KEYSTORE_KEY_PASS
exit /b 0
