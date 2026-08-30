# Third-party sources vendored into the host port

## miniaudio

- **Version:** v0.11.25 (2026-03-04)
- **Upstream:** https://github.com/mackron/miniaudio
- **Licence:** public domain (Unlicense) *or* MIT-0, at your option. The full
  text is at the end of `miniaudio.h`.
- **Why it is here:** the host port needs a real audio device on Windows,
  macOS, Linux, Android and iOS. miniaudio is one header plus one source file
  and covers WASAPI, CoreAudio, ALSA/PulseAudio, AAudio and OpenSL from a
  single API, which is exactly the shape `audio_sink_t` wants.
- **Licence compatibility:** both options are permissive with no reciprocal
  obligation, so linking it does not affect osyntho's MIT terms. This is the
  opposite of the `components/fx_gpl` situation, where the licence is the
  reason that component is excluded.
- **Local modifications:** none. `miniaudio.h` is byte-for-byte upstream.
  `miniaudio.c` is ours, and only sets the `MA_NO_*` switches and defines
  `MINIAUDIO_IMPLEMENTATION` before including the header.

To update: replace `miniaudio.h` from upstream and re-run the host build. If
the device API changes shape, `port/host/src/sink_miniaudio.cpp` is the only
file that touches it.

## RtMidi

- **Version:** master as of 2026-08 (v6.x line)
- **Upstream:** https://github.com/thestk/rtmidi
- **Licence:** MIT, plus a **non-binding** request that modifications be sent
  upstream. The clause says so in as many words ("This is, however, not a
  binding provision of this license"), so it imposes no obligation and does
  not affect osyntho's MIT terms.
- **Why it is here:** host MIDI input, so a USB keyboard can play the
  standalone synth. One header and one source covering WinMM, CoreMIDI and
  ALSA -- the same shape as miniaudio, and the reason neither needed a package
  manager.
- **Platform coverage:** Windows, macOS and Linux. **Not Android or iOS** --
  RtMidi has no backend for either, and mobile MIDI is a different problem
  (USB host or BLE MIDI) rather than a missing build flag. The standalone
  engine runs there; it just has no external keyboard input.
- **Local modifications:** none. Both files are byte-for-byte upstream; the
  platform selection is made with compile definitions from
  `port/host/CMakeLists.txt`.
