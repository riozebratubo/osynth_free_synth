#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stop miniaudio handing the sink a silent null device and calling it success.

Applied 2026-09-01, straight after android_audio_out_fallback.py and for the
same silence. That script assumed a refused capture device made ma_device_init()
FAIL. Reading ma_device_init_ex() (miniaudio.h) shows it does not, and this is
the half that actually explains why the standalone Android app was silent while
every layer above it reported success.

ma_device_init(NULL, ...) means "pick a backend for me". It walks the backend
list in priority order, keeping the first that opens the device -- and
ma_backend_null is the last entry in that walk ("Must always be the last item.
Lowest priority"). The null backend is a device in every respect the API cares
about: it opens, it honours the sample rate, it fires the data callback on a
timer, and it throws every frame away.

So on Android: AAudio's context initialised, its capture stream was refused (no
RECORD_AUDIO), the context was uninitialised and the walk moved on; OpenSL|ES
refused it for the same reason; and miniaudio returned MA_SUCCESS holding a
silent device. sink_start() succeeded. audio_io never reached its null-sink
fallback, because nothing failed. The engine rendered, wrote, and paced against
a clock that led nowhere -- and the one line that said so, "null via null", went
to the stderr Android discards.

The fix is to name the backends rather than let miniaudio choose "any, then
none": every backend it was built with EXCEPT the null one. A machine with no
audio hardware now fails here, which is what audio_io's own null sink is for --
the same silence, but chosen in the one place that knows to log it.
"""

import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SINK = "port/host/src/sink_miniaudio.cpp"


def read(path):
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


def main():
    s, nl = read(SINK)

    s = sub(
        s,
        '''    /* NULL context: miniaudio picks the platform's default backend and the
     * default output device. Choosing a device is a setting the app should
     * own, not something to bake in here. */
    ma_result mr = ma_device_init(nullptr, &cfg, &g_device);
''',
        '''    /* Every backend miniaudio was compiled with EXCEPT the null one, and that
     * exclusion is the entire reason this list is built by hand.
     *
     * ma_device_init() with a null context calls ma_device_init_ex(), which
     * walks the backends in priority order and keeps the first that opens the
     * device. ma_backend_null is the last entry in that walk, and it is a
     * device in every respect this code can test: it opens, it honours the
     * sample rate, it calls the data callback on a timer, and it throws every
     * frame away.
     *
     * That is what the standalone app got on Android. AAudio was refused the
     * capture stream (no RECORD_AUDIO), OpenSL|ES was refused it for the same
     * reason, and miniaudio returned MA_SUCCESS holding a silent device. Every
     * layer above reported success -- audio_io never reached its own null-sink
     * fallback, because nothing had failed -- and the synth rendered into
     * nothing for as long as the app was open.
     *
     * Named backends turn that into an error the retry below can act on. Where
     * there genuinely is no audio hardware, sink_start() now returns ESP_FAIL
     * and audio_io installs its OWN null sink: the same silence, chosen and
     * logged in the one place that knows to say so.
     *
     * Built rather than written out, because it has to track whatever this
     * miniaudio was compiled with; ma_backend_null is documented as the last
     * enumerator, so counting up to it is exactly "all the real ones". The
     * default *device* is still miniaudio's to pick -- which output to use is
     * a setting the app should own, not something to bake in here. */
    ma_backend backends[ma_backend_null];
    for (ma_uint32 i = 0; i < (ma_uint32)ma_backend_null; ++i) {
        backends[i] = (ma_backend)i;
    }

    ma_result mr = ma_device_init_ex(backends, (ma_uint32)ma_backend_null,
                                     nullptr, &cfg, &g_device);
''')

    s = sub(
        s,
        '''        cfg.deviceType = ma_device_type_playback;
        mr = ma_device_init(nullptr, &cfg, &g_device);
''',
        '''        cfg.deviceType = ma_device_type_playback;
        mr = ma_device_init_ex(backends, (ma_uint32)ma_backend_null, nullptr,
                               &cfg, &g_device);
''')

    write(SINK, s, nl)
    print("android_null_backend_reject: applied")


if __name__ == "__main__":
    main()
