cmake_minimum_required(VERSION 3.7)

set(APP_VERSION 0.1.3)
set(APP_ID "org.osynth.osyntho")

# those are defaults
option(GIT_REVISION "Append the current git revision to the app's version string" ON)
option(BLE_DIRECT_CONNECT "Skip BLE scan and connect directly by address on Windows" ON)

# change the values here
# NOTE: intentional — these normal-variable set()s shadow the cache options
# above, so a -D value passed on the configure command line has no effect.
# Edit the values here to change a flag.
set(GIT_REVISION true)
set(BLE_DIRECT_CONNECT true)

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

if (WIN32)
  set(CONDITIONAL_WIN32_BLUETOOTHMANAGER
    src/bluetoothmanager2.cpp src/bluetoothmanager2.h
  )
endif()

if (ANDROID OR LINUX OR APPLE)
  set(CONDITIONAL_ANDROID_BLUETOOTHMANAGER src/bluetoothmanager.cpp src/bluetoothmanager.h)
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
    QML_FILES qml/SettingsItemToggable.qml
    QML_FILES qml/SettingsItemNumber.qml
    QML_FILES qml/SettingsSwitch.qml
    SOURCES src/translator.cpp
    SOURCES src/translator.h
    SOURCES src/apputils.h src/apputils.cpp
    SOURCES src/graphicscaps.h src/graphicscaps.cpp
    QML_FILES qml/WindowStateSaver.qml
    QML_FILES qml/SpinBoxDouble.qml
    SOURCES src/ble/synthprotocol.h

    # Phase 3 — synth control UI
    QML_FILES qml/Knob.qml
    QML_FILES qml/EnumSelector.qml
    QML_FILES qml/ParamControl.qml
    QML_FILES qml/ParamGroup.qml
    QML_FILES qml/PanelFlow.qml
    QML_FILES qml/ModMatrixSlot.qml
    QML_FILES qml/Keyboard.qml
    QML_FILES qml/AdsrEnvelope.qml
    QML_FILES qml/HomeScreen.qml
    QML_FILES qml/ToneScreen.qml
    QML_FILES qml/FilterEnvScreen.qml
    QML_FILES qml/ModScreen.qml
    QML_FILES qml/FxScreen.qml
    QML_FILES qml/ArpSeqScreen.qml
    QML_FILES qml/SequencerScreen.qml
    QML_FILES qml/StepField.qml
    QML_FILES qml/SyncedComboBox.qml
    QML_FILES qml/TrackSheet.qml
    QML_FILES qml/PlockDialog.qml
    QML_FILES qml/SeqSetDialog.qml
    QML_FILES qml/DrumsScreen.qml
    QML_FILES qml/DrumPads.qml
    QML_FILES qml/TransportStrip.qml
    QML_FILES qml/ParamValue.qml
    QML_FILES qml/LooperScreen.qml
    QML_FILES qml/PresetsScreen.qml
    QML_FILES qml/PatchLibraryScreen.qml

    # ENABLE_TYPE_COMPILER

    # removes qml code from binary, keeps compiled versions
    DISCARD_QML_CONTENTS
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
if (ANDROID)
  set_target_properties(${PROJECT_NAME} PROPERTIES
      QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/android-build"
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
      # Also declared as package= in assets/android-build/AndroidManifest.xml;
      # keep the two in sync. (QT_ANDROID_PACKAGE_NAME needs Qt >= 6.8; on
      # older Qt it is an inert custom property and the manifest value rules.)
      QT_ANDROID_PACKAGE_NAME "org.osynth.osyntho"
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

if (WIN32)
  add_subdirectory(${CMAKE_SOURCE_DIR}/simpleble/simpleble)
  if (BLE_DIRECT_CONNECT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BLE_DIRECT_CONNECT)
  endif()
endif()

if (WIN32)
  set(CONDITIONAL_SIMPLEBLE "simpleble::simpleble")
endif()

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE Qt${QT_VERSION_MAJOR}::Multimedia
    # PRIVATE Qt${QT_VERSION_MAJOR}::Positioning
    # PRIVATE Qt${QT_VERSION_MAJOR}::Quick
    PRIVATE Qt${QT_VERSION_MAJOR}::QuickControls2
    # PRIVATE Qt${QT_VERSION_MAJOR}::QuickPrivate
    PRIVATE Qt${QT_VERSION_MAJOR}::Sql
    PRIVATE Qt${QT_VERSION_MAJOR}::Bluetooth
    PRIVATE Qt${QT_VERSION_MAJOR}::Concurrent
    PRIVATE Qt${QT_VERSION_MAJOR}::Network
    PRIVATE Qt${QT_VERSION_MAJOR}::CorePrivate # since Qt 6.10
    # simpleble::simpleble
    # simpleble::simpleble-c
    PRIVATE ${CONDITIONAL_SIMPLEBLE}
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
