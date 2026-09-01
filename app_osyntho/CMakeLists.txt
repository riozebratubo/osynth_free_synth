cmake_minimum_required(VERSION 3.7)

set(APP_VERSION 0.2.0)
# The controller build's identity, and the base the standalone one is derived
# from. Do not use it directly -- APP_ID / APP_DISPLAY_NAME are set per variant
# further down, once OSYNTHO_EMBEDDED is known.
set(APP_ID_BASE "org.osynth.osyntho")

# those are defaults
option(GIT_REVISION "Append the current git revision to the app's version string" ON)
option(BLE_DIRECT_CONNECT "Skip BLE scan and connect directly by address on Windows" ON)

# Build the synth INTO the app instead of talking to one over BLE.
#
# With this on, port/host compiles the firmware's own audio engine as a library
# and src/embeddedmanager.cpp reaches it through SynthCtl v1 -- the same
# protocol the BLE backends carry. SynthController and every QML screen sit
# above IBluetoothManager and are untouched by the choice.
#
# Off by default: the normal osyntho is a controller for the instrument, and
# that is the build most people want. Configure with -DOSYNTHO_EMBEDDED=ON for
# the standalone one.
option(OSYNTHO_EMBEDDED "Embed the synth engine and run standalone" ON)

# change the values here
# NOTE: intentional — these normal-variable set()s shadow the cache options
# above, so a -D value passed on the configure command line has no effect.
# Edit the values here to change a flag.
set(GIT_REVISION true)
set(BLE_DIRECT_CONNECT true)

# --- Variant identity --------------------------------------------------------
# The controller and the standalone build are two different applications as far
# as every OS is concerned, and each OS keys an installed app off exactly one
# string: the Android package name, the macOS/iOS bundle id, the .desktop and
# icon basename on Linux, and -- on all of them -- the QLocalServer name the
# single-instance guard claims. Share that string and the two cannot coexist:
# Android refuses to install the second package, and on the desktop the second
# one launched finds the first's socket, raises its window and exits.
#
# So the standalone build gets a suffix of its own. The controller keeps the
# plain id -- it is the one already published, and changing it would strand
# existing installs on a package no update ever matches again.
#
# APP_DISPLAY_NAME is also per variant, and it is not only cosmetic: main.cpp
# passes it to QCoreApplication::setApplicationName(), which is what makes
# AppDataLocation (<org>/<app>) differ between the two. Without that split the
# guard's own warning comes true from the other direction -- two processes on
# one SQLite file, now by design instead of by accident. Keep them together.
#
# APP_VARIANT is the same choice as a bare word, for places that need a path
# segment rather than a name: the Windows static-Qt drop at the bottom of this
# file writes to <repo>/current/${APP_VARIANT}/.
if (OSYNTHO_EMBEDDED)
  set(APP_ID "${APP_ID_BASE}.standalone")
  set(APP_DISPLAY_NAME "Osyntho Standalone")
  set(APP_VARIANT "standalone")
else()
  set(APP_ID "${APP_ID_BASE}")
  set(APP_DISPLAY_NAME "Osyntho")
  set(APP_VARIANT "controller")
endif()
message(STATUS "osyntho: app id '${APP_ID}', display name '${APP_DISPLAY_NAME}'")

if (APPLE AND NOT IOS)
  set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Target architecture: arm64, x86_64, or x86_64;arm64")
  set_property(CACHE CMAKE_OSX_ARCHITECTURES PROPERTY STRINGS arm64 x86_64 "x86_64;arm64")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "")
endif()

if (IOS)
  # iOS device builds default to arm64 (32-bit was dropped at iOS 11).
  # For Apple-Silicon-Mac simulator builds keep arm64; for legacy Intel-Mac
  # simulator use x86_64. The Xcode generator picks iphoneos vs.
  # iphonesimulator SDK from the active scheme, not from this variable.
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS target architecture: arm64 (device or Apple-Silicon sim) or x86_64 (Intel-Mac sim)")
  set_property(CACHE CMAKE_OSX_ARCHITECTURES PROPERTY STRINGS arm64 x86_64)

  # Must be set BEFORE project(); CMake bakes this into the toolchain.
  # 13.0 is the floor — older releases can be selected by overriding the
  # cache var, but the project's third-party deps (SimpleBLE etc.) assume 13+.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum supported iOS version (13.0 or newer)")
endif()

project(
  osyntho
  VERSION ${APP_VERSION}
  DESCRIPTION "Osynth companion app"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# fix qt creator 14, 15 qml lint issues
set(QT_QML_GENERATE_QMLLS_INI ON)

message(STATUS "Trying to find Qt 6...")
find_package(QT NAMES Qt6 QUIET)

if(NOT DEFINED QT_VERSION_MAJOR OR QT_VERSION_MAJOR LESS 6)
  message(STATUS "Could not find Qt 6, aborting compilation.")
endif()

find_package(Qt6 6.7 # this is the min qt6 version required
  REQUIRED COMPONENTS
    # Quick
    QuickControls2
    # QuickPrivate
    Sql
    Bluetooth
    # Positioning
    Multimedia
    Concurrent
    Network
    CorePrivate
    # LinguistTools # not using it anymore
)

qt_standard_project_setup(REQUIRES 6.5)

set(CMAKE_AUTORCC ON)

# https://doc.qt.io/qt-6/qt-cmake-policies.html#qt-cmake-policies
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

if (WIN32)
  set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_SOURCE_DIR}/assets/app_icon_windows.rc")
endif()

qt_add_executable(
    ${PROJECT_NAME}
    main.cpp
    main_resources.qrc
    ${APP_ICON_RESOURCE_WINDOWS}
    # ${APP_RES}
    # ${QT_RES}
)

if (ANDROID)
qt_add_android_permission(${PROJECT_NAME}
    NAME android.permission.BLUETOOTH_SCAN
    ATTRIBUTES
        minSdkVersion 31
        # usesPermissionFlags neverForLocation
)
qt_add_android_permission(${PROJECT_NAME}
    NAME android.permission.ACCESS_COARSE_LOCATION
)
qt_add_android_permission(${PROJECT_NAME}
    NAME android.permission.WRITE_EXTERNAL_STORAGE
)
endif()

if (OSYNTHO_EMBEDDED)
  # The engine replaces the radio entirely: neither BLE backend is compiled,
  # so an embedded build carries no Bluetooth code at all rather than carrying
  # it unused.
  # protocolparity.cpp is compiled only here, and only here can it be: it
  # includes both the app's copy of the protocol constants and the firmware's,
  # which share a binary in no other configuration. It emits no code -- it is
  # entirely static_assert -- and exists so that the two cannot drift again.
  set(CONDITIONAL_EMBEDDED_MANAGER
      src/embeddedmanager.cpp src/embeddedmanager.h
      src/ble/protocolparity.cpp)
else()
  if (WIN32)
    set(CONDITIONAL_WIN32_BLUETOOTHMANAGER
      src/bluetoothmanager2.cpp src/bluetoothmanager2.h
    )
  endif()

  if (ANDROID OR LINUX OR APPLE)
    set(CONDITIONAL_ANDROID_BLUETOOTHMANAGER src/bluetoothmanager.cpp src/bluetoothmanager.h)
  endif()
endif()

set(GIT_REV "")
if(GIT_REVISION)
  if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
      COMMAND git rev-parse --short HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE GIT_REV
      ERROR_VARIABLE GIT_REV_ERR
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    if(GIT_REV)
      message(STATUS "Git revision: ${GIT_REV}")
      string(PREPEND GIT_REV "+g")
    else()
      message(FATAL_ERROR "Failed to retrieve git revision: ${GIT_REV_ERR}")
    endif()
  else()
    message(
      WARNING
        "The GIT_REVISION option was set, but this is not a git repository.")
  endif()
endif()

target_compile_definitions(
  ${PROJECT_NAME}
  PUBLIC APP_VERSION_NUMBER="${PROJECT_VERSION}"
  PUBLIC APP_VERSION="${PROJECT_VERSION}${GIT_REV}"
  PUBLIC APP_ID="${APP_ID}"
  # Every user-visible name and every per-variant path in the app comes from
  # here, so that "which build is this?" has one answer and not four.
  PUBLIC APP_DISPLAY_NAME="${APP_DISPLAY_NAME}"
)

list(APPEND QML_IMPORT_PATH "/qml")
list(REMOVE_DUPLICATES QML_IMPORT_PATH)
set(QML_IMPORT_PATH ${QML_IMPORT_PATH}
    CACHE STRING "Qt Creator qml import paths"
    FORCE
)

# Session-state singleton (UI.qml) — must be marked before qt_add_qml_module
# registers it so the generated qmldir declares it as a singleton type.
set_source_files_properties(qml/UI.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# --- DIAGNOSTIC TOGGLE -------------------------------------------------------
# ON disables qmlcachegen's ahead-of-time compilation for the whole module. The
# QML still runs (interpreted byte code), so this changes no behaviour that the
# app itself defines — it only stops the compiler emitting C++ for bindings and
# functions.
#
# Use it to split a fault in two: if a misbehaviour disappears with this ON, it
# lives in generated code (a wrong type annotation, or a codegen bug); if it
# survives, the generated code is exonerated and the fault is in the QML or C++
# as written.
#
# Turn it back OFF before measuring: with AOT off, tools/qml_aot_score.py has
# nothing to read and the .aotstats left in the build tree go stale.
option(OSYNTHO_NO_QML_AOT "Disable QML ahead-of-time compilation (diagnostic)" OFF)
if(OSYNTHO_NO_QML_AOT)
  set(QT_QML_NO_CACHEGEN ON)
  message(STATUS "osyntho: QML AOT compilation DISABLED (OSYNTHO_NO_QML_AOT=ON)")
endif()
# -----------------------------------------------------------------------------

# --- BISECT HELPER -----------------------------------------------------------
# A semicolon-separated list of QML files to exclude from AOT compilation while
# leaving it on for everything else. Narrows "the fault is in generated code"
# down to a single document without editing any source:
#
#   -DOSYNTHO_QML_SKIP_CACHEGEN="qml/Keyboard.qml"
#   -DOSYNTHO_QML_SKIP_CACHEGEN="qml/Keyboard.qml;qml/Toolbar.qml"
#
# Empty means every file is compiled, which is the shipping configuration.
#
# A normal variable, not a cache one: a cache entry set on an earlier configure
# would otherwise win over any edit made here, which is exactly the trap this
# helper exists to avoid.
#
# >>> BISECT IN PROGRESS: Keyboard.qml is excluded from AOT. <<<
# Set back to "" once the offending document is identified.
set(OSYNTHO_QML_SKIP_CACHEGEN "")
foreach(_skip IN LISTS OSYNTHO_QML_SKIP_CACHEGEN)
  set_source_files_properties(${_skip} PROPERTIES QT_QML_SKIP_CACHEGEN TRUE)
  message(STATUS "osyntho: AOT skipped for ${_skip}")
endforeach()
# -----------------------------------------------------------------------------

qt_add_qml_module(
    ${PROJECT_NAME}
    # URI osyntho
    URI org.osynth.osyntho
    VERSION 1.0
    SOURCES src/business/idatabase.h
    SOURCES src/business/databaseclient.h
    SOURCES src/business/database.h src/business/database.cpp
    RESOURCES main_resources.qrc
    SOURCES src/theme.h src/theme.cpp
    SOURCES src/app.h src/app.cpp
    SOURCES ${CONDITIONAL_WIN32_BLUETOOTHMANAGER}
    SOURCES ${CONDITIONAL_ANDROID_BLUETOOTHMANAGER}
    SOURCES ${CONDITIONAL_EMBEDDED_MANAGER}
    SOURCES src/singleinstance/singleinstance.h src/singleinstance/singleinstance.cpp
    SOURCES src/synthcontroller.h src/synthcontroller.cpp
    QML_FILES qml/Main.qml
    QML_FILES qml/UI.qml
    QML_FILES qml/Toolbar.qml
    QML_FILES qml/Toast.qml
    QML_FILES qml/ToastManager.qml
    QML_FILES qml/SettingsScreen.qml
    QML_FILES qml/BluetoothDeviceSelectorScreen.qml
    SOURCES src/ibluetoothmanager.h
    # Registers IBluetoothManager as the BluetoothManager QML singleton;
    # the interface itself must not name a backend. See src/qmlforeign.h.
    SOURCES src/qmlforeign.h src/qmlforeign.cpp
    QML_FILES qml/SettingsSwitch.qml
    SOURCES src/translator.cpp
    SOURCES src/translator.h
    SOURCES src/apputils.h src/apputils.cpp
    SOURCES src/nativefiledialog.h src/nativefiledialog.cpp
    SOURCES src/graphicscaps.h src/graphicscaps.cpp
    QML_FILES qml/WindowStateSaver.qml
    QML_FILES qml/SpinBoxDouble.qml
    SOURCES src/ble/synthprotocol.h
    # QML value types for the parameter API (see src/paramtypes.h).
    SOURCES src/paramtypes.h
    SOURCES src/seqtypes.h
    SOURCES src/chordtypes.h
    SOURCES src/graphtypes.h
    SOURCES src/loopwav.h src/loopwav.cpp

    # Phase 3 — synth control UI
    QML_FILES qml/Knob.qml
    QML_FILES qml/EnumSelector.qml
    QML_FILES qml/ParamControl.qml
    QML_FILES qml/ParamGroup.qml
    QML_FILES qml/HoldSampleCard.qml
    QML_FILES qml/VocoderPresets.qml
    QML_FILES qml/PanelFlow.qml
    QML_FILES qml/ModMatrixSlot.qml
    QML_FILES qml/Keyboard.qml
    QML_FILES qml/AdsrEnvelope.qml
    QML_FILES qml/HomeScreen.qml
    QML_FILES qml/ToneScreen.qml
    QML_FILES qml/FilterEnvScreen.qml
    QML_FILES qml/ModScreen.qml
    QML_FILES qml/InputScreen.qml
    QML_FILES qml/FxScreen.qml
    QML_FILES qml/ArpSeqScreen.qml
    QML_FILES qml/ChordScreen.qml
    QML_FILES qml/SequencerScreen.qml
    QML_FILES qml/StepField.qml
    QML_FILES qml/SyncedComboBox.qml
    QML_FILES qml/SyncedButton.qml
    QML_FILES qml/TrackSheet.qml
    QML_FILES qml/PlockDialog.qml
    QML_FILES qml/SeqSetDialog.qml
    QML_FILES qml/DrumsScreen.qml
    QML_FILES qml/DrumPads.qml
    QML_FILES qml/TransportStrip.qml
    QML_FILES qml/ParamValue.qml
    QML_FILES qml/LooperScreen.qml
    QML_FILES qml/GraphScreen.qml
    QML_FILES qml/PresetsScreen.qml
    QML_FILES qml/PatchLibraryScreen.qml

    # ENABLE_TYPE_COMPILER

    # removes qml code from binary, keeps compiled versions
    # DISCARD_QML_CONTENTS
    SOURCES src/business/settings.h
    SOURCES src/business/settings.cpp
    SOURCES src/business/isettings.h
    SOURCES src/business/settingsclient.h   
)

include_directories(src/model)
include_directories(src)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER ${APP_ID}
    # Substituted into assets/{macos,ios}/Info.plist as CFBundleName /
    # CFBundleDisplayName, next to the identifier above. Both plists take these
    # from CMake rather than spelling them out, so a variant cannot ship under
    # the other one's name.
    MACOSX_BUNDLE_BUNDLE_NAME "${APP_DISPLAY_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    # QT_ANDROID_ABIS "arm64-v8a;armeabi-v7a"
)

if (APPLE AND NOT IOS)
  set_target_properties(${PROJECT_NAME} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/assets/macos/Info.plist"
  )
endif()

if (IOS)
  set_target_properties(${PROJECT_NAME} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/assets/ios/Info.plist"
      XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
      # Match the top-of-file CMAKE_OSX_DEPLOYMENT_TARGET. Keeping both in sync
      # avoids Xcode silently overriding the toolchain-level minimum with a
      # per-target one and producing binaries that crash on older devices.
      XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
      XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "$<IF:$<CONFIG:Debug>,YES,NO>"
  )

  # Code signing. Xcode refuses to build for any iOS target — even the
  # simulator — without either a development team or CODE_SIGNING_ALLOWED=NO.
  # Configure-time options:
  #   -DIOS_DEVELOPMENT_TEAM=ABCDE12345   use an explicit Apple Developer Team
  #   (unset)                              disable signing (simulator-only)
  if (NOT "${IOS_DEVELOPMENT_TEAM}" STREQUAL "")
    message(STATUS "iOS signing: Apple Development Team = ${IOS_DEVELOPMENT_TEAM}")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${IOS_DEVELOPMENT_TEAM}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    )
  else()
    message(STATUS "iOS signing: no IOS_DEVELOPMENT_TEAM set — disabling signing (simulator-only builds will work)")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED  "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY    ""
    )
  endif()
endif()

# Sets the manifest, app icon and res/ (incl. the FileProvider paths) on Android.
#
# The manifest is generated rather than shipped ready-made: it names the package
# in four places (package=, the FileProvider and androidx-startup authorities,
# and the dynamic-receiver permission) and all four must carry this variant's
# APP_ID for the two APKs to install side by side. So assets/android-build/ is
# copied into the build tree, AndroidManifest.xml.in is configured into the copy,
# and androiddeployqt is pointed there.
#
# file(COPY) runs at configure time, so an edit under assets/android-build/res/
# reaches the APK only after a re-configure. The template itself is fine --
# configure_file() registers it as a configure dependency automatically.
if (ANDROID)
  set(OSYNTHO_ANDROID_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/android-package-source")
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/android-build/"
       DESTINATION "${OSYNTHO_ANDROID_PACKAGE_DIR}"
       PATTERN "AndroidManifest.xml.in" EXCLUDE)
  # @ANDROID_EXTRA_PERMISSIONS@ in the template. See the comment beside it
  # there for why the microphone is asked for at all, and why only here: the
  # controller build embeds no engine, so it opens no capture device.
  if (OSYNTHO_EMBEDDED)
    set(ANDROID_EXTRA_PERMISSIONS
        "<uses-permission android:name=\"android.permission.RECORD_AUDIO\" />")
  else()
    set(ANDROID_EXTRA_PERMISSIONS "")
  endif()

  configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/android-build/AndroidManifest.xml.in"
      "${OSYNTHO_ANDROID_PACKAGE_DIR}/AndroidManifest.xml"
      @ONLY)
  set_target_properties(${PROJECT_NAME} PROPERTIES
      QT_ANDROID_PACKAGE_SOURCE_DIR "${OSYNTHO_ANDROID_PACKAGE_DIR}"
  )
endif()

if (ANDROID)
  # Android versioning derived from APP_VERSION so a release can never ship
  # with a stale manifest again: 0.9.8.11 -> versionName "0.9.8.11",
  # versionCode 90811 (MAJOR*1000000 + MINOR*10000 + PATCH*100 + TWEAK).
  # Monotonic while each component after MAJOR stays below 100 (checked
  # below). androiddeployqt overrides AndroidManifest.xml with these values.
  string(REPLACE "." ";" APP_VERSION_PARTS "${APP_VERSION}")
  list(LENGTH APP_VERSION_PARTS APP_VERSION_PARTS_LEN)
  list(GET APP_VERSION_PARTS 0 APP_VERSION_MAJOR)
  list(GET APP_VERSION_PARTS 1 APP_VERSION_MINOR)
  list(GET APP_VERSION_PARTS 2 APP_VERSION_PATCH)
  if (APP_VERSION_PARTS_LEN GREATER 3)
    list(GET APP_VERSION_PARTS 3 APP_VERSION_TWEAK)
  else()
    set(APP_VERSION_TWEAK 0)
  endif()
  if (APP_VERSION_MINOR GREATER 99 OR APP_VERSION_PATCH GREATER 99 OR APP_VERSION_TWEAK GREATER 99)
    message(FATAL_ERROR "APP_VERSION component > 99 breaks Android versionCode monotonicity: ${APP_VERSION}")
  endif()
  math(EXPR ANDROID_VERSION_CODE
       "${APP_VERSION_MAJOR} * 1000000 + ${APP_VERSION_MINOR} * 10000 + ${APP_VERSION_PATCH} * 100 + ${APP_VERSION_TWEAK}")

  set_target_properties(${PROJECT_NAME} PROPERTIES
      # Same value the generated manifest carries as package= -- both come from
      # APP_ID, so they cannot drift. (QT_ANDROID_PACKAGE_NAME needs Qt >= 6.8;
      # on older Qt it is an inert custom property and the manifest value rules,
      # which is why the manifest is templated rather than left to this.)
      QT_ANDROID_PACKAGE_NAME "${APP_ID}"
      QT_ANDROID_VERSION_NAME "${APP_VERSION}"
      QT_ANDROID_VERSION_CODE ${ANDROID_VERSION_CODE}
  )
endif()

# Trim the Android APK by excluding Qt plugins this app never uses.
if (ANDROID)
  qt_import_plugins(${PROJECT_NAME}
    EXCLUDE
      Qt6::QFFmpegMediaPlugin
      Qt6::QVirtualKeyboardPlugin
      # Unused image formats. The app only deals with camera-captured JPEGs and
      # SVG/PNG UI assets (PNG is handled by Qt Gui core, no plugin). qicns/qico
      # are macOS/Windows icon formats — dead weight on Android. If you ever load
      # a gif/tiff/webp/tga/bmp image at runtime, drop the matching line.
      Qt6::QGifPlugin
      Qt6::QICNSPlugin
      Qt6::QICOPlugin
      Qt6::QTgaPlugin
      Qt6::QTiffPlugin
      Qt6::QWbmpPlugin
      Qt6::QWebpPlugin
  )
endif()

if (WIN32 AND NOT OSYNTHO_EMBEDDED)
  add_subdirectory(${CMAKE_SOURCE_DIR}/simpleble/simpleble)
  if (BLE_DIRECT_CONNECT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BLE_DIRECT_CONNECT)
  endif()
  set(CONDITIONAL_SIMPLEBLE "simpleble::simpleble")
endif()

# Native File Dialog (nfd/) — the platform's own open/save dialogs, wrapped by
# src/nativefiledialog.cpp. Built on Windows and Linux only: Android picks files
# through the Storage Access Framework and the rest stay on the QML FileDialog,
# and NFD_AVAILABLE is what tells the wrapper which of the two it is.
# LINUX is false for Android builds (CMAKE_SYSTEM_NAME is "Android" there).
if (WIN32 OR LINUX)
  if (WIN32)
    set(NFD_PLATFORM_SOURCES nfd/nfd_win.cpp)
  else()
    # zenity: shells out to the binary, so nothing to find at configure time.
    # The wrapper checks for it at runtime and falls back if it is missing.
    # nfd_zenity.c is C, and the project only enables CXX by default.
    enable_language(C)
    set(NFD_PLATFORM_SOURCES nfd/nfd_zenity.c)
  endif()

  add_library(nfd STATIC nfd/nfd_common.cpp ${NFD_PLATFORM_SOURCES})
  set_target_properties(nfd PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  # nfd/       — the backends include "nfd_common.h"
  # nfd/include — and both they and our wrapper include "nfd.h"
  target_include_directories(nfd PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/nfd
    ${CMAKE_CURRENT_SOURCE_DIR}/nfd/include
  )
  if (WIN32)
    # IFileDialog/COM (ole32, uuid) and SHCreateItemFromParsingName (shell32).
    target_link_libraries(nfd PRIVATE ole32 uuid shell32)
  endif()

  set(CONDITIONAL_NFD "nfd")
  target_compile_definitions(${PROJECT_NAME} PRIVATE NFD_AVAILABLE)
endif()

# ---------------------------------------------------------------------------
# The embedded engine (OSYNTHO_EMBEDDED)
#
# port/host builds the firmware's own components -- the same .cpp files the
# ESP-IDF build compiles -- against shims that stand in for ESP-IDF. See
# port/host/README.md.
# ---------------------------------------------------------------------------
if (NOT OSYNTHO_EMBEDDED)
  # Only a build that talks to a real instrument needs the Bluetooth module.
  set(CONDITIONAL_QT_BLUETOOTH "Qt${QT_VERSION_MAJOR}::Bluetooth")
endif()

if (OSYNTHO_EMBEDDED)
  add_subdirectory(${CMAKE_SOURCE_DIR}/../port/host
                   ${CMAKE_BINARY_DIR}/osynth_core)
  target_compile_definitions(${PROJECT_NAME} PRIVATE OSYNTHO_EMBEDDED)
  set(CONDITIONAL_OSYNTH_CORE osynth_core)
endif()

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE ${CONDITIONAL_OSYNTH_CORE}
    PRIVATE Qt${QT_VERSION_MAJOR}::Multimedia
    # PRIVATE Qt${QT_VERSION_MAJOR}::Positioning
    # PRIVATE Qt${QT_VERSION_MAJOR}::Quick
    PRIVATE Qt${QT_VERSION_MAJOR}::QuickControls2
    # PRIVATE Qt${QT_VERSION_MAJOR}::QuickPrivate
    PRIVATE Qt${QT_VERSION_MAJOR}::Sql
    PRIVATE ${CONDITIONAL_QT_BLUETOOTH}
    PRIVATE Qt${QT_VERSION_MAJOR}::Concurrent
    PRIVATE Qt${QT_VERSION_MAJOR}::Network
    PRIVATE Qt${QT_VERSION_MAJOR}::CorePrivate # since Qt 6.10
    # simpleble::simpleble
    # simpleble::simpleble-c
    PRIVATE ${CONDITIONAL_SIMPLEBLE}
    PRIVATE ${CONDITIONAL_NFD}
)

if (APPLE AND NOT IOS)
  target_link_libraries(${PROJECT_NAME} PRIVATE "-framework AppKit")
endif()

# Not using LinguistTools, using own implementation
# qt_add_translations(
#   TARGETS ${PROJECT_NAME}
#   INCLUDE_DIRECTORIES src/ qml/
# )

# find_package(simpleble REQUIRED CONFIG)
# target_link_libraries(${PROJECT_NAME} simpleble::simpleble)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

add_custom_command(TARGET ${PROJECT_NAME}  POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_CURRENT_BINARY_DIR}/assets)

add_custom_command(TARGET ${PROJECT_NAME}  POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                # list of files to copy
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/graph.ico
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/graph.svg

                # destination dir
                ${CMAKE_CURRENT_BINARY_DIR}/assets)

# fix empty qmldir file not found on org.osynth
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/qmldir
        ${CMAKE_CURRENT_BINARY_DIR}/org/osynth/qmldir
)

# --- Windows static-Qt drop --------------------------------------------------
# A static Qt is the one configuration whose osyntho.exe stands on its own: no
# Qt DLLs beside it, no windeployqt pass, nothing to assemble before it runs.
# That makes it the only build worth lifting straight out of the build tree, so
# it also lands at <repo>/current/<variant>/ -- a fixed path per variant to run
# from or hand to someone, instead of a build directory whose name encodes the
# kit and the configuration. The build tree keeps its own copy either way; this
# is a copy, not a relocation.
#
# The two variants get their own subdirectory for the same reason they get their
# own app id: both executables are called osyntho.exe, so one directory would
# mean each build silently replacing the other.
#
# Static vs shared is read off Qt6::Core's imported target type. Qt's own
# QT_BUILD_SHARED_LIBS is set by Qt6BuildInternalsExtra.cmake, which an
# application project never loads, so it is not available here -- whereas the
# imported library is STATIC_LIBRARY or SHARED_LIBRARY in every Qt 6 kit.
if (WIN32 AND TARGET Qt6::Core)
  get_target_property(OSYNTHO_QT_CORE_TYPE Qt6::Core TYPE)
  if (OSYNTHO_QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
    # ../ is the repository root -- the same assumption add_subdirectory() makes
    # for port/host above.
    get_filename_component(OSYNTHO_REPO_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
    set(OSYNTHO_CURRENT_DIR "${OSYNTHO_REPO_DIR}/current/${APP_VARIANT}")
    message(STATUS
        "osyntho: static Qt kit -- the .exe will also be copied to ${OSYNTHO_CURRENT_DIR}")

    # copy_if_different rather than copy: a rebuild that produced the same bytes
    # leaves the file alone, so a copy someone is running from is not replaced
    # underneath them (Windows refuses that anyway, and the build would fail).
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OSYNTHO_CURRENT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "${OSYNTHO_CURRENT_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMENT "Copying the static build to current/${APP_VARIANT}/"
        VERBATIM)
  else()
    message(STATUS
        "osyntho: shared Qt kit -- not copying to current/${APP_VARIANT} "
        "(the .exe there would need the Qt DLLs beside it)")
  endif()
endif()

get_target_property(MY_DEPENDENCIES ${PROJECT_NAME} "MANUALLY_ADDED_DEPENDENCIES")
message(MY_DEPENDENCIES="${MY_DEPENDENCIES}")

# deployment

# @todo TESTS!!!
# option(BUILD_TESTS "Build unit tests" OFF)
# if(BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(tests)
#
#     # Make every test executable build AFTER the main app target, so a full
#     # build with BUILD_TESTS=ON produces the app first and the tests afterwards
#     # (and an app build error surfaces before any test is compiled). This walks
#     # every subdirectory of tests/ so new test dirs are covered automatically.
#     if(TARGET ${PROJECT_NAME})
#         get_property(_test_subdirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
#                      PROPERTY SUBDIRECTORIES)
#         foreach(_subdir IN LISTS _test_subdirs)
#             get_property(_subdir_targets DIRECTORY ${_subdir}
#                          PROPERTY BUILDSYSTEM_TARGETS)
#             foreach(_t IN LISTS _subdir_targets)
#                 get_target_property(_t_type ${_t} TYPE)
#                 if(_t_type STREQUAL "EXECUTABLE")
#                     add_dependencies(${_t} ${PROJECT_NAME})
#                 endif()
#             endforeach()
#         endforeach()
#     endif()
# endif()
