#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Android standalone: make the output survive a refused capture device.

Applied 2026-09-01. Idempotent in the loud sense: every replacement asserts its
anchor is present and unique, so a second run fails instead of doubling edits.

The bug
-------
The standalone app was silent on Android. Nothing in the UI showed it and
nothing in any log said it, and the two halves of that have the same cause.

port/host opens ONE duplex miniaudio device -- playback and capture share a
clock, the way the S3's I2S port's two halves do -- and miniaudio's AAudio
backend opens the CAPTURE stream first (ma_device_init_streams__aaudio).
Android refuses a capture stream to an app that has not been granted
RECORD_AUDIO, which the manifest never declared and the app never asked for.
So ma_device_init() failed before the playback stream was ever attempted,
audio_io.cpp did what it does with a sink that will not start -- fell back to
the null sink, deliberately, so the instrument still runs -- and a perfectly
good DAC went quiet behind a permission the synth does not need to make a
sound.

The reason it was invisible: Android closes stderr, and the host port's ESP_LOGx
shim writes there. "no audio device could be opened" and "sink 'host' failed to
start; falling back to null sink" were both printed, into nothing.

What this changes
-----------------
1. sink_miniaudio.cpp  retry output-only when the duplex device is refused.
   This is the fix that matters: sound must never depend on a microphone.
2. esp_sys_host.cpp    ESP_LOGx -> logcat on Android, so the next failure of
   port/host/CMakeLists  this kind says so out loud. Needs -llog.
3. AndroidManifest.xml.in  declare RECORD_AUDIO, standalone variant only.
   app_osyntho/CMakeLists  (The controller build has no engine and no capture.)
4. app.{h,cpp}         request the microphone BEFORE starting the engine, and
   start it on every outcome. The device is opened inside start(), so an
   answer that arrives afterwards would only take effect next launch.
5. Info.plist (macOS, iOS)  NSMicrophoneUsageDescription. Apple terminates an
   app that requests a permission with no usage string, so 4 needs this.
6. port/host/README.md  the platform table said "Audio in: yes" for Android.
"""

import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(path):
    """Returns (text with LF newlines, the newline the file actually uses)."""
    raw = io.open(os.path.join(ROOT, path), encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in raw else "\n"
    return raw.replace("\r\n", "\n"), nl


def write(path, text, nl):
    io.open(os.path.join(ROOT, path), "w", encoding="utf-8", newline="").write(
        text.replace("\n", nl))


def sub(text, old, new):
    assert old in text, "anchor not found:\n" + old
    assert text.count(old) == 1, "anchor is not unique:\n" + old
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# 1. The sink: a refused capture device costs the input, not the output.
# ---------------------------------------------------------------------------
def sink():
    p = "port/host/src/sink_miniaudio.cpp"
    s, nl = read(p)

    s = sub(
        s,
        '''    /* NULL context: miniaudio picks the platform's default backend and the
     * default output device. Choosing a device is a setting the app should
     * own, not something to bake in here. */
    if (ma_device_init(nullptr, &cfg, &g_device) != MA_SUCCESS) {
        ESP_LOGE(TAG, "no audio device could be opened");
        return ESP_FAIL;
    }
''',
        '''    /* NULL context: miniaudio picks the platform's default backend and the
     * default output device. Choosing a device is a setting the app should
     * own, not something to bake in here. */
    ma_result mr = ma_device_init(nullptr, &cfg, &g_device);

#if SYNTH_ENABLE_LINE_IN
    /* Whether the device that opened has a capture half, and the whole reason
     * there are two attempts here rather than one.
     *
     * A refused input must never cost the synth its output. On Android an app
     * without RECORD_AUDIO is refused the capture stream, and miniaudio's
     * AAudio backend opens the capture half of a duplex device FIRST -- so the
     * playback stream was never even attempted, ma_device_init() failed, and
     * audio_io.cpp fell back to the null sink exactly as it is meant to. A
     * working DAC went silent behind a permission the synth does not need in
     * order to make a sound. The same shape appears on macOS with the
     * microphone refused in Settings, and on any machine whose default input
     * was unplugged between boot and here.
     *
     * So: ask for both, and if that is refused ask for the output alone. What
     * is lost is the line input, the vocoder's modulator and the granular
     * capture -- audio_source_i2s_ready() answers false below and audio_io
     * leaves `in.route` and `in.gain` registered and inert, which is already
     * its behaviour for a device that was refused. Everything else plays. */
    bool capture_up = (mr == MA_SUCCESS);
    if (!capture_up) {
        ESP_LOGW(TAG,
                 "no capture device (%s); opening the output alone -- line "
                 "input, vocoder and granular capture are off",
                 ma_result_description(mr));
        /* Reusing cfg: ma_device_init() reads deviceType out of it, and every
         * other field is the same request. The capture.* fields left set are
         * ignored for a playback device. */
        cfg.deviceType = ma_device_type_playback;
        mr = ma_device_init(nullptr, &cfg, &g_device);
    }
#endif

    if (mr != MA_SUCCESS) {
        ESP_LOGE(TAG, "no audio device could be opened (%s)",
                 ma_result_description(mr));
        return ESP_FAIL;
    }
''')

    s = sub(
        s,
        '''#if SYNTH_ENABLE_LINE_IN
        /* Sized from the same two numbers, and starting EMPTY -- the opposite
         * of the playback ring, which starts primed. Nothing has been captured
         * yet, and priming this with silence would hand the renderer a block of
         * nothing and call it input. */
        std::lock_guard<std::mutex> cl(g_cap_mutex);
        g_cap_frames = g_ring_frames;
''',
        '''#if SYNTH_ENABLE_LINE_IN
        /* Sized from the same two numbers, and starting EMPTY -- the opposite
         * of the playback ring, which starts primed. Nothing has been captured
         * yet, and priming this with silence would hand the renderer a block of
         * nothing and call it input.
         *
         * Zero frames where the capture half was refused above, which is what
         * data_callback tests before it touches this at all. */
        std::lock_guard<std::mutex> cl(g_cap_mutex);
        g_cap_frames = capture_up ? g_ring_frames : 0;
''')

    s = sub(
        s,
        '''#if SYNTH_ENABLE_LINE_IN
    /* Only now: a reader that arrived between init and here would find the
     * buffer empty and count a starve for no reason. */
    g_cap_open = true;
#endif
''',
        '''#if SYNTH_ENABLE_LINE_IN
    /* Only now: a reader that arrived between init and here would find the
     * buffer empty and count a starve for no reason. And never at all where
     * the device that opened has no capture half -- audio_io reads this to
     * decide whether the input exists. */
    g_cap_open = capture_up;
#endif
''')

    write(p, s, nl)


# ---------------------------------------------------------------------------
# 2. The log shim: stderr goes nowhere on Android.
# ---------------------------------------------------------------------------
def logging():
    p = "port/host/src/esp_sys_host.cpp"
    s, nl = read(p)

    s = sub(
        s,
        '''#if defined(_MSC_VER)
/* MoveFileExA, for the rename shim below.''',
        '''#if defined(__ANDROID__)
/* Android closes stderr, so every line the log shim writes would otherwise be
 * formatted and thrown away -- which is exactly what happened to "no audio
 * device could be opened" while the standalone app sat silent on a phone and
 * said nothing about it anywhere. logcat is where a phone's diagnostics live. */
#include <android/log.h>
#endif

#if defined(_MSC_VER)
/* MoveFileExA, for the rename shim below.''')

    s = sub(
        s,
        '''const char* level_letter(esp_log_level_t l) {''',
        '''#if defined(__ANDROID__)
/* IDF's levels onto Android's. The same order, different numbers, and no
 * arithmetic relationship between the two worth relying on. */
int android_priority(esp_log_level_t l) {
    switch (l) {
        case ESP_LOG_ERROR: return ANDROID_LOG_ERROR;
        case ESP_LOG_WARN: return ANDROID_LOG_WARN;
        case ESP_LOG_INFO: return ANDROID_LOG_INFO;
        case ESP_LOG_DEBUG: return ANDROID_LOG_DEBUG;
        default: return ANDROID_LOG_VERBOSE;
    }
}
#endif

const char* level_letter(esp_log_level_t l) {''')

    s = sub(
        s,
        '''    const long ms = (long)(elapsed_us() / 1000);
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::fprintf(stderr, "%s (%ld) %s: %s\\n", level_letter(level), ms,
                 tag != nullptr ? tag : "?", msg);
    std::fflush(stderr);
}
''',
        '''    const long ms = (long)(elapsed_us() / 1000);
    std::lock_guard<std::mutex> lk(g_log_mutex);
#if defined(__ANDROID__)
    /* The tag becomes logcat's tag rather than part of the message, so
     * `adb logcat -s sink_ma audio_io osynth_host` filters these the way it
     * filters any other Android component. The level letter and the
     * boot-relative milliseconds stay in the text: that is the IDF shape, and
     * keeping it is what lets a phone log and a serial log be read side by
     * side. */
    __android_log_print(android_priority(level), tag != nullptr ? tag : "?",
                        "%s (%ld) %s", level_letter(level), ms, msg);
#else
    std::fprintf(stderr, "%s (%ld) %s: %s\\n", level_letter(level), ms,
                 tag != nullptr ? tag : "?", msg);
    std::fflush(stderr);
#endif
}
''')

    # The two remaining direct writes to stderr, routed through the shim above
    # so they reach logcat too. The ESP_ERROR_CHECK one matters most: it is the
    # last thing printed before abort(), and on Android it was printed nowhere.
    s = sub(
        s,
        '''    std::fprintf(stderr, "ESP_ERROR_CHECK failed: %s (0x%x) at %s:%d\\n  %s\\n",
                 esp_err_to_name(rc), (unsigned)rc, file, line, expr);
    std::fflush(stderr);
    std::abort();''',
        '''    esp_log_write_host(ESP_LOG_ERROR, "ESP_ERROR_CHECK",
                       "failed: %s (0x%x) at %s:%d -- %s", esp_err_to_name(rc),
                       (unsigned)rc, file, line, expr);
    std::abort();''')

    s = sub(
        s,
        '''    std::fprintf(stderr, "W (host) esp_restart(): ignored, no firmware to restart\\n");
    std::fflush(stderr);''',
        '''    esp_log_write_host(ESP_LOG_WARN, "host",
                       "esp_restart(): ignored, no firmware to restart");''')

    write(p, s, nl)


def cmake_host():
    p = "port/host/CMakeLists.txt"
    s, nl = read(p)
    s = sub(
        s,
        '''elseif(ANDROID)
    # Nothing to link. AAudio is the preferred backend and is resolved at run
''',
        '''elseif(ANDROID)
    # liblog, for __android_log_print: src/esp_sys_host.cpp sends every ESP_LOGx
    # line to logcat on Android, because the runtime closes stderr and the
    # engine's whole boot log -- including the reason the audio device did not
    # open -- was being written into nothing. It is part of the NDK sysroot, so
    # there is no package to install and nothing to resolve at run time.
    target_link_libraries(osynth_core PUBLIC log)

    # Nothing to link for audio. AAudio is the preferred backend and is resolved at run
''')
    write(p, s, nl)


# ---------------------------------------------------------------------------
# 3. The permission: declared for the standalone variant only.
# ---------------------------------------------------------------------------
def manifest():
    p = "app_osyntho/assets/android-build/AndroidManifest.xml.in"
    s, nl = read(p)
    s = sub(
        s,
        '''    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
''',
        '''    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />

    <!-- RECORD_AUDIO, and only on the standalone build: CMake expands this to
         nothing for the controller, which embeds no engine and opens no
         capture device. Asking for a microphone in an app that never opens one
         is a review flag and a lie to the user.

         What it buys is the INPUT, not the sound. The engine opens one duplex
         device (playback and capture share a clock, as the S3's two I2S halves
         do) and miniaudio opens the capture stream first, so being refused it
         used to fail the whole device and leave the synth silent. The sink now
         retries output-only, so without this permission the app still plays;
         what it loses is the line input, the vocoder and granular capture. -->
    @ANDROID_EXTRA_PERMISSIONS@
''')
    write(p, s, nl)


def cmake_app():
    p = "app_osyntho/CMakeLists.txt"
    s, nl = read(p)
    s = sub(
        s,
        '''  configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/android-build/AndroidManifest.xml.in"
''',
        '''  # @ANDROID_EXTRA_PERMISSIONS@ in the template. See the comment beside it
  # there for why the microphone is asked for at all, and why only here: the
  # controller build embeds no engine, so it opens no capture device.
  if (OSYNTHO_EMBEDDED)
    set(ANDROID_EXTRA_PERMISSIONS
        "<uses-permission android:name=\\"android.permission.RECORD_AUDIO\\" />")
  else()
    set(ANDROID_EXTRA_PERMISSIONS "")
  endif()

  configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/android-build/AndroidManifest.xml.in"
''')
    write(p, s, nl)


# ---------------------------------------------------------------------------
# 4. Ask for it before the device is opened, and start the engine either way.
# ---------------------------------------------------------------------------
def app_header():
    p = "app_osyntho/src/app.h"
    s, nl = read(p)
    s = sub(
        s,
        '''  void requestPermissions();
  void deleteTemporaryAppFiles();''',
        '''  void requestPermissions();
  // Standalone builds only: brings the in-process engine up, once the
  // microphone permission has been resolved on the platforms that have one.
  // Compiles to nothing in the controller build.
  void startEmbeddedEngine();
  void deleteTemporaryAppFiles();''')
    write(p, s, nl)


def app_source():
    p = "app_osyntho/src/app.cpp"
    s, nl = read(p)

    s = sub(
        s,
        '''#ifdef OSYNTHO_EMBEDDED
  // Last, and inside the constructor rather than in main(): every connect()
  // above is already made, so the manager's startup signals -- infoRead and
  // connectedChanged -- have somewhere to land. Both are emitted queued, so
  // they are delivered after this returns and the object is fully built.
  EmbeddedManager::instance().start();
#endif
}''',
        '''  // Last, and inside the constructor rather than in main(): every connect()
  // above is already made, so the manager's startup signals -- infoRead and
  // connectedChanged -- have somewhere to land. Both are emitted queued, so
  // they are delivered after this returns and the object is fully built.
  startEmbeddedEngine();
}''')

    s = sub(
        s,
        '''void App::deleteTemporaryAppFiles() {''',
        '''void App::startEmbeddedEngine() {
#ifdef OSYNTHO_EMBEDDED
  auto start = [] { EmbeddedManager::instance().start(); };

#if QT_CONFIG(permissions)
  // The microphone is asked for BEFORE the engine starts, and that order is
  // the whole reason this function exists.
  //
  // The engine opens a single duplex audio device inside start(): the capture
  // half is claimed by the same call that claims the output. Android and iOS
  // both refuse a capture stream to an app that has not been granted the
  // permission, and both answer asynchronously -- so asking afterwards would
  // hand the answer to a device that is already open, and the input would only
  // begin working on the *next* launch.
  //
  // Denied is not a failure and stops nothing. The sink retries output-only
  // (port/host/src/sink_miniaudio.cpp), so the synth plays; what a refusal
  // costs is the line input, the vocoder's modulator and the granular capture.
  // Which is why every branch below starts the engine.
  //
  // Apple terminates a process that requests a permission whose Info.plist has
  // no usage string, so assets/macos/Info.plist and assets/ios/Info.plist both
  // carry NSMicrophoneUsageDescription. Do not remove one without the other.
  if (qApp == nullptr) {
    start();  // DI/test construction: no application object to ask through.
    return;
  }

  QMicrophonePermission microphonePermission;
  switch (qApp->checkPermission(microphonePermission)) {
    case Qt::PermissionStatus::Undetermined:
      // Delivered on the event loop, i.e. once main() reaches exec(). Nothing
      // in the UI depends on the engine being up before then: the manager
      // announces itself with the same queued infoRead/connectedChanged a BLE
      // link does, and the app is already written to treat those as late.
      qApp->requestPermission(microphonePermission, this,
                              [start](const QPermission&) { start(); });
      break;
    case Qt::PermissionStatus::Denied:
    case Qt::PermissionStatus::Granted:
      // Every run after the first answers synchronously, so the engine starts
      // here with no delay at all.
      start();
      break;
  }
#else
  // Windows and Linux: nothing stands in front of the audio device.
  start();
#endif
#endif  // OSYNTHO_EMBEDDED
}

void App::deleteTemporaryAppFiles() {''')

    write(p, s, nl)


# ---------------------------------------------------------------------------
# 5. Apple usage strings -- without these, step 4 terminates the app.
# ---------------------------------------------------------------------------
def plists():
    p = "app_osyntho/assets/macos/Info.plist"
    s, nl = read(p)
    s = sub(
        s,
        '''    <key>NSBluetoothPeripheralUsageDescription</key>
    <string>Osyntho uses Bluetooth to connect to your Osynth synthesizer.</string>
''',
        '''    <key>NSBluetoothPeripheralUsageDescription</key>
    <string>Osyntho uses Bluetooth to connect to your Osynth synthesizer.</string>
    <!-- Required, not optional: App::startEmbeddedEngine() requests the
         microphone permission on the standalone build, and macOS terminates a
         process that requests a permission with no usage string here. The
         controller build never asks, and an unused string costs it nothing. -->
    <key>NSMicrophoneUsageDescription</key>
    <string>Osyntho uses the audio input as a sound source: the line input, the vocoder and the granular engine all read from it.</string>
''')
    write(p, s, nl)

    p = "app_osyntho/assets/ios/Info.plist"
    s, nl = read(p)
    s = sub(
        s,
        '''    <key>NSCameraUsageDescription</key>''',
        '''    <!-- Required, not optional: App::startEmbeddedEngine() requests the
         microphone permission on the standalone build, and iOS terminates a
         process that requests a permission with no usage string here. Written
         for Osyntho, unlike the strings around it. -->
    <key>NSMicrophoneUsageDescription</key>
    <string>Osyntho uses the audio input as a sound source: the line input, the vocoder and the granular engine all read from it.</string>
    <key>NSCameraUsageDescription</key>''')
    write(p, s, nl)


# ---------------------------------------------------------------------------
# 6. The platform table said "Audio in: yes" for Android. It was not free.
# ---------------------------------------------------------------------------
def readme():
    p = "port/host/README.md"
    s, nl = read(p)
    s = sub(
        s,
        '''| Android arm64-v8a | yes | AAudio | yes | — | **built** (NDK 27, Clang) |
| Android armeabi-v7a | yes | AAudio | yes | — | **built** (NDK 27, Clang) |''',
        '''| Android arm64-v8a | yes | AAudio | with `RECORD_AUDIO` | — | **built** (NDK 27, Clang) |
| Android armeabi-v7a | yes | AAudio | with `RECORD_AUDIO` | — | **built** (NDK 27, Clang) |''')

    s = sub(
        s,
        '''**MIDI is Windows, macOS and Linux only.**''',
        '''**Android needs `RECORD_AUDIO`, and must not need it to make a sound.** The
engine opens one duplex device, and miniaudio's AAudio backend opens the
capture half *first* -- so an app without the permission was refused the whole
device, `audio_io` fell back to the null sink exactly as designed, and a working
DAC was silenced by a permission the synth does not need in order to play a
note. `sink_start()` now retries output-only when the duplex device is refused;
the standalone APK declares the permission and `App::startEmbeddedEngine()`
requests it before the device is opened, because the answer arrives
asynchronously and one that lands after `start()` would only take effect on the
next launch. Refused, the app still plays: what is lost is the line input, the
vocoder's modulator and the granular capture.

**Android logging goes to logcat.** The runtime closes stderr, so the whole boot
log -- including the line above about the device -- used to be formatted and
discarded, which is why a silent app was also a silent diagnosis.
`esp_sys_host.cpp` calls `__android_log_print` instead and keeps the IDF shape
in the message, so:

```
adb logcat -s sink_ma audio_io osynth_host
```

**MIDI is Windows, macOS and Linux only.**''')
    write(p, s, nl)


def main():
    sink()
    logging()
    cmake_host()
    manifest()
    cmake_app()
    app_header()
    app_source()
    plists()
    readme()
    print("android_audio_out_fallback: all edits applied")


if __name__ == "__main__":
    main()
