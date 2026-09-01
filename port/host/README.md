# osynth host port

Compiles the firmware's audio engine — the same `components/*` sources, not a
copy — as a plain static library for Windows, Linux, macOS, Android and iOS.
It exists so the osyntho app can embed the synth and run standalone, with no
hardware in reach.

## How it works

The DSP sources include ESP-IDF headers (`esp_log.h`, `esp_err.h`,
`freertos/task.h`, `sdkconfig.h`, …). This directory provides headers with
**those same names**, placed first on the include path. No `components/` source
is edited and none is copied; the only firmware change the port needed was one
capability branch in `synth_core/include/synth_config.h`.

That works because the audio code was already separated: not one of
`synth_core`, `engines`, `fx`, `fx_gpl`, `graph`, `drums`, `looper`, `seqarp`
or `chord` contains a target test, no DSP file touches a register or a DMA
descriptor, and `fx.cpp` — the largest file in the tree — makes no RTOS, heap
or timer call at all.

## Layout

```
include/            headers that shadow ESP-IDF, by name
  sdkconfig.h       ← the one policy file: what this build is and is not
  esp_err.h  esp_log.h  esp_attr.h  esp_system.h
  esp_heap_caps.h  esp_memory_utils.h   budgeted pools, not plain malloc
  esp_timer.h  esp_cpu.h  esp_rom_sys.h  esp_rom_crc.h
  freertos/FreeRTOS.h  task.h  semphr.h  queue.h
src/                their implementations
CMakeLists.txt      the osynth_core static library
```

Start at `include/sdkconfig.h`. Everything else here is mechanism; that file is
the decisions.

## Two things that are not simple aliases

**`MALLOC_CAP_SPIRAM` is a budget, not `malloc`.** `looper_init()`
(`looper.cpp:2231`) sizes the loop pool from
`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` and registers the answer as
`loop.maxlen`'s default — the number the app draws as "max n s". Given a
desktop's real free RAM it would promise hours and then try to reserve them.
So the pool has a declared size, allocations draw it down, and a request past
it returns `NULL` exactly as a full PSRAM heap would. Set it with
`heap_caps_host_set_budgets()` before any component initialises; a mobile build
should lower it.

**`portMUX` is a spinlock, not a `std::mutex`.** All eleven muxes guard a few
instructions and two of them are taken from the audio thread. A mutex there
would enter the kernel on contention and deschedule the render thread inside a
section written on the assumption that it cannot be.

## Building the library

Not part of the ESP-IDF build; configure it directly.

```sh
cmake -S port/host -B build_host
cmake --build build_host --config Release
```

On Windows, `toolsuild_host.bat` does both and runs `vcvars64.bat` first
(`toolsuild_host.bat clean` to start over; set `OSYNTH_VCVARS` if Visual
Studio is somewhere else). Note the Visual Studio generator is multi-config: it
ignores `CMAKE_BUILD_TYPE` and wants `--config` at build time.

Needs CMake ≥ 3.16, a C++20 compiler and Python 3 (for `tools/gen_wavetables.py`, which generates
`factory_wavetables.h` exactly as the firmware build does).

On Linux, also the ALSA development headers — `libasound2-dev` (Debian/Ubuntu),
`alsa-lib-devel` (Fedora), `alsa-lib` (Arch). They are RtMidi's, not
miniaudio's: audio dlopens ALSA and PulseAudio at run time and needs no
package, MIDI input `#include`s `<alsa/asoundlib.h>` and does. CMake checks for
them and says so by name rather than letting the compile fail.

## Checks

```sh
python tools/check_host_includes.py         # every include resolves
python tools/check_host_includes.py --all   # the same over all of components/
python tools/check_host_symbols.py          # what the library still leaves open
python tools/check_android_symbols.py      # the same, for an NDK build
python tools/check_android_symbols.py --abi armeabi-v7a

build_host/Release/osynth_host_demo            # play a figure; render stats
build_host/Release/osynth_host_demo --proto    # SynthCtl round trip, 15 checks
build_host/Release/osynth_host_demo --input    # meter the capture device
build_host/Release/osynth_host_demo --storage-write
build_host/Release/osynth_host_demo --storage-check
```

`--proto` is the one that matters most after touching `ctrl_proto` or a
transport: it drives the protocol against the live engine and checks the
answers, including the multi-frame `PARAM_INFO` discovery the app does on every
connect. It writes its request frames by hand from the *app's* view of the wire
format rather than including the implementation's own constants -- a test that
shared them would agree by construction, and those two files are maintained
separately.

The storage pair is two processes on purpose: what is being tested is that
state survives a restart, which one run cannot show.

`--input` routes the capture device to `mon` and meters it for five seconds.
It earned its place immediately: the first capture implementation kept a single
block and truncated each device period to it, which metered *correctly* -- real
audio, moving peaks -- while discarding 224 of every 480 frames and starving
the reader on 44% of blocks. Only the starve and drop counters showed it.

`check_host_includes.py --all` is the map of what each remaining component
needs; it prints the `#if` nesting around each unresolved header, because an
include reached only through a condition this build turns off is not a missing
shim.

`check_host_symbols.py` matters because `osynth_core` is a **static library**:
it archives successfully with symbols still open, so a green build says nothing
about completeness. Reading dumpbin's UNDEF column alone does not answer it
either -- a cross-object reference inside one library shows as UNDEF -- so the
script subtracts the defined set and splits the remainder into osynth symbols
and toolchain noise.

`check_android_symbols.py` is the same check under the NDK, and it exists
because that gap was not hypothetical. The drum kit ROM is declared two ways --
the objcopy symbols `_binary_drumkit_bin_start`/`_end` for the firmware, plain
identifiers for the host, which generates the array with `tools/bin2c.py`. The
`#if` choosing between them keyed on `_MSC_VER`, so it was right on Windows and
wrong everywhere else: Android builds with Clang, took the objcopy branch, and
referenced two symbols no one defines. `libosynth_core.a` archived cleanly
anyway. The failure surfaced only when the app linked it into
`libosyntho_arm64-v8a.so`, several steps and one platform later. The condition
now keys on `SYNTH_TARGET_HOST`, which is what actually decides, and this script
answers the question at the archive.

It resolves against the NDK sysroot *and* compiler-rt, which is not optional
bookkeeping: `libclang_rt.builtins` holds the ARM64 outline atomics that every
`std::atomic` goes through and the `__aeabi_` helpers 32-bit ARM needs for
integer division. Omitting them reported 35 ordinary builtins as findings and
buried the two that were real.

## State

**The whole synth runs on Windows.** `osynth_host_demo` boots 397 parameters
-- six engines, the 146-parameter FX bus, the drum bus with its factory kit,
the 8x8x256 sequencer, chord mode, presets, the looper and the MIDI router --
renders the *complete* firmware chain, and plays it through a real device:
0 underruns, 0 sink starves, 0 sink errors, DSP load ~0.3-1.1%.

**Storage round-trips.** `--storage-write` then `--storage-check` saves a
setting and a preset, restarts, and reads both back. The preset file is
byte-compatible with the firmware's: `OSP1`, version 3, a 32-byte header --
which is also the runtime proof that `synth_pack.h` gives MSVC the same struct
layout GCC produces.

**Builds clean.** 31 translation units, no errors under MSVC 19.4x / x64
Release. The executable links with no unresolved symbols.

**The firmware still builds.** The port touched six firmware files plus one new
header, so this is not optional: `idf.py -B build_regress_p4 build` for
esp32p4 succeeds with zero errors and links with DIRAM at 34.9% (372 KB
remaining). Every host-side change is behind `#ifdef _MSC_VER`,
`defined(SYNTH_TARGET_HOST)`, or a macro that expands to what was there before,
so GCC compiles exactly what it compiled prior to the port. Re-run this after
touching anything under `components/`.

**In:** `synth_core`, `engines` (all six), `fx`, `graph`, `seqarp`, `chord`,
`drums` (bus, factory kit, sampler engine), `midi` (parser/router),
`usb_host_midi/usb_mode.cpp` and `midi/midi_serial.c` (both target-independent
by design — each is a stub `#else` branch on this build, and `midi.c` calls
into both outside any guard), `audio_io`'s render core, the null sink, and the
miniaudio sink.

`drums` arrived earlier than planned and not by choice: `engines.cpp` registers
the sampler engine, `seq_play.cpp` targets drum slots, `midi.c` routes drum
notes and `fx.cpp` reads the compressor's sidechain key — so no executable can
link without it. Its SD kit store stays compiled out; see
`CONFIG_OSYNTH_SAMPLE_KITS` in `include/sdkconfig.h`, which explains what is
staged off and what restores it.

`ctrl_proto` is in and now **links** — SynthCtl v1, the protocol the app already speaks.
Splitting it out of `ble_ctrl.cpp` was the point of plan step 3, and it was a
move rather than a rewrite: 1,389 lines of handlers with not one reference to a
NimBLE type. : the storage components its handlers reach are ported, so
`check_host_symbols.py` reports **no osynth symbols unresolved** — the library
is self-contained.

**The app builds with it.** `cmake -DOSYNTHO_EMBEDDED=ON` produces an
`osyntho.exe` with the synth inside it: it boots the engine, opens the audio
device and reaches it through SynthCtl v1 in process.

**Audio input works**, as one duplex miniaudio device so capture and playback
share a clock the way the I2S port's two halves do. That is what lights up the
granular engine, the vocoder, the two noise-reduction units, the graph's LineIn
node, the looper's line recording and the sampler's pre-roll -- none of which
needed a line of change, because they test `SYNTH_ENABLE_AUDIO_IN` and this
feeds it.

**MIDI input works** on Windows, macOS and Linux (RtMidi): every input port is
opened and merged into the firmware's own router, so a keyboard drives chord
mode, the arpeggiator, the sequencer's recorder and the per-engine CC maps
exactly as it does on the instrument. Android and iOS have no RtMidi backend --
mobile MIDI is USB host or BLE MIDI, a different problem.

**Never:** `codec`, the four hardware sinks, `source_mic`, `usb_dev`,
`usb_host_midi`, `local_ui`, `midi_serial.c`. These are peripherals, not ports.

**`fx_gpl` is excluded for its licence, not its features.** MVerb and DuskVerb
are GPL-3; osyntho is MIT, and linking them in would make the app a GPL-3
combined work. Its include directory stays on the path so `fx.cpp` can include
`fx_gpl.h` unconditionally — the same arrangement
`components/fx_gpl/CMakeLists.txt` uses when the option is off.

## MSVC

The firmware is built by GCC on all three ESP targets and by Clang on the Apple
and Android hosts, so it uses GCC spellings freely. MSVC is the one toolchain
in the set that does not know them. Five appear in the DSP, and they are
handled in two places:

| Spelling | Uses | Handled by |
| --- | --- | --- |
| `__restrict__` | 70 | `include/host_compat.h`, force-included (`/FI`) |
| `__builtin_expect` | 2 | same, via `synth_config.h`'s own `#ifndef SYNTH_LIKELY` guard |
| `__attribute__((vector_size))` | 2 | an `#ifdef _MSC_VER` branch in `synth_core/include/synth_simd.h` |
| `__builtin_convertvector` | 3 | same branch |
| `##__VA_ARGS__` | the log macros | avoided: the format string stays inside `__VA_ARGS__` |
| `__attribute__((noinline))` | 2 | `NOINLINE_ATTR` in `include/esp_attr.h` maps to `__declspec(noinline)` |
| `__attribute__((packed))` | 7 | `synth_core/include/synth_pack.h` — see below |
| `taskENTER_CRITICAL` | 106 | `include/freertos/FreeRTOS.h` (only the `port*` spelling was shimmed at first) |
| `asm("_binary_...")` label | 2 | `drum_kit.cpp` declares pointers under MSVC; `tools/bin2c.py` emits the blob |
| `strlcpy` | 20 | `include/host_compat.h` — full BSD semantics |
| `<strings.h>` / `strcasecmp` | 3 | `include/strings.h`, which `#include_next`s the real one on POSIX |

The `synth_simd.h` branch supplies types with the same names and the same three
operations the two kernels use — aggregate init, element read, elementwise
multiply — so **both function bodies compile unchanged**. It produces scalar
code, which is what that header's own comment says these lower to on Xtensa
anyway; the win being bought is the 4× unroll, the hoisted loads and the
aliasing freedom, and all three survive.

## Two real defects the port surfaced

Neither is a shim gap. Both were latent in the firmware and only GCC's
tolerance was hiding them.

**1. Struct packing (7 structs, fixed).** Every on-disk format was declared
`struct __attribute__((packed))`. MSVC does not understand that and does not
warn — it silently changes the layout, so a preset written by one build stops
loading in the other. A data bug wearing the clothes of a successful build.
Now spelled through `synth_core/include/synth_pack.h`
(`OSYNTH_PACK_PUSH` / `OSYNTH_PACKED` / `OSYNTH_PACK_POP`), which is the
attribute on GCC and `#pragma pack(1)` on MSVC. Six of the seven already had a
`static_assert` on their size; `preset_pair_t` (`presets_priv.h:14`) did not,
and it is the one every `.osp`, `.oss` and `.osw` file is a *stream* of — a
padded version would have read every stored patch two bytes further off from
the first pair onward. It has one now.

**2. A `#if` inside a macro argument list (fixed).** `seq_model.cpp` chose
`" PSRAM"` or `" internal"` with a `#if CONFIG_SPIRAM` *inside* an `ESP_LOGI()`
call. A preprocessing directive inside a macro invocation is undefined
behaviour in both C and C++ (C++20 [cpp.replace]/11); GCC accepts it as an
extension, MSVC rejects it. The conditional is now hoisted into a local above
the call — behaviour-identical everywhere.

## Platform status

| Platform | Engine | Audio out | Audio in | MIDI in | Verified how |
| --- | --- | --- | --- | --- | --- |
| Windows x64 | yes | WASAPI | yes | WinMM | **built and run** |
| Android arm64-v8a | yes | AAudio | with `RECORD_AUDIO` | — | **built** (NDK 27, Clang) |
| Android armeabi-v7a | yes | AAudio | with `RECORD_AUDIO` | — | **built** (NDK 27, Clang) |
| Linux x64 | yes | ALSA/Pulse | yes | ALSA seq | not built here |
| macOS | yes | CoreAudio | yes | CoreMIDI | not built here |
| iOS | yes | CoreAudio | yes | — | not built here |

Building both Android ABIs is what makes the middle rows more than a guess: it
is a different compiler (Clang, not MSVC), a different libc++, and two
architectures, so it takes the *other* branch of every `#if defined(_MSC_VER)`
in this directory and compiles `synth_simd.h`'s vector extensions for NEON
rather than x86. Linux and macOS use those same non-MSVC paths.

What is still unproven for Linux, macOS and iOS is the platform glue that has
no Android equivalent -- the CoreAudio and ALSA link lines, and the iOS
Objective-C requirement below. Those are written from miniaudio's own build
notes rather than tested.

**The sink names its backends, and never the null one.** `ma_device_init()` with
a null context walks miniaudio's backends in priority order and keeps the first
that opens the device -- and `ma_backend_null` is the last entry in that walk.
It opens, it honours the sample rate, it calls the data callback on a timer, and
it discards every frame. So a device that no real backend could open came back
as `MA_SUCCESS`, `sink_start()` succeeded, `audio_io` never reached its own
null-sink fallback because nothing had failed, and the engine rendered into
nothing. `sink_start()` now passes an explicit list of every backend except that
one, so "no device" is an error again and `audio_io` chooses -- and logs -- the
silence itself.

**Android needs `RECORD_AUDIO`, and must not need it to make a sound.** The
engine opens one duplex device, and miniaudio's AAudio backend opens the capture
half *first*, so an app without the permission is refused the whole device
before the playback stream is ever attempted. That was the standalone APK: no
permission declared, both real backends refused, and (see above) a null device
handed back in their place. `sink_start()` now retries output-only when the
duplex device is refused; the APK declares the permission and
`App::startEmbeddedEngine()` requests it *before* the device is opened, because
the answer arrives asynchronously and one that lands after `start()` would only
take effect on the next launch. Refused, the app still plays: what is lost is
the line input, the vocoder's modulator and the granular capture.

**Android logging goes to logcat.** The runtime closes stderr, so the whole boot
log -- including the line above about the device -- used to be formatted and
discarded, which is why a silent app was also a silent diagnosis.
`esp_sys_host.cpp` calls `__android_log_print` instead and keeps the IDF shape
in the message, so:

```
adb logcat -s sink_ma audio_io osynth_host
```

**MIDI is Windows, macOS and Linux only.** RtMidi has no Android or iOS
backend, so `OSYNTH_HOST_MIDI_IN` is 0 there and `midi_in_host.cpp` compiles to
stubs. Mobile MIDI is USB host or BLE MIDI -- a different problem, not a
missing flag.

### Two things the Apple builds need that nothing else does

**iOS compiles `miniaudio.c` as Objective-C.** It drives an AVAudioSession, so
it includes `<AVFoundation/AVFoundation.h>`. The CMake sets `-x objective-c`
and links AVFoundation; without that the build fails inside a vendored file
with errors that look nothing like their cause.

**Both Apple targets set `MA_NO_RUNTIME_LINKING`** and link the three
CoreAudio frameworks explicitly. miniaudio's default is to `dlopen` them, and
on macOS that fails Apple's *notarization* unless the app carries the
allow-unsigned-executable-memory entitlement -- a far worse trade than naming
three frameworks.

## The protocol has one owner now

`components/ctrl_proto/include/ctrl_proto_wire.h` holds the opcodes and status
codes. The app still has its own copy in `src/ble/synthprotocol.h`, and has to
-- a BLE build talks to an instrument over the air and cannot include a
firmware header.

What the standalone build adds is that both are in one binary, so they can be
*compared*. `app_osyntho/src/ble/protocolparity.cpp` is 40 `static_assert`s
doing exactly that, compiled only when `OSYNTHO_EMBEDDED` is on. It emits no
code.

It exists because care alone had already failed once: the app's engine enum
stopped at `ENG_GRANULAR = 5` while the firmware had added the sampler at 6.
The app drew a seventh engine it could not name, and selecting it asked the
synth for a preset list it then crashed serving. Neither build said a word.

The asserts were verified by breaking them on purpose -- removing
`ENG_SAMPLER` and renumbering one opcode -- and both were caught by name:

```
error C2338: static assertion failed: 'SynthCtl drift: OP_KIT_EDIT differs
             between app and firmware'
```

## The path-length trap

Every storage component builds its paths with `snprintf` into a fixed buffer
sized for the firmware's short mount points -- `/lfs` (40 bytes in presets),
`/sd/osynth` (48 in loop_store, 64 in loop_stream). A host data directory is a
full user-profile path and fits in none of them.

The failure is silent, and worse than a crash: `snprintf` truncates rather than
overflowing, so the write lands on a *different, valid* path. The first run of
the storage test wrote a 32-byte settings blob to a file called `pre` beside
the presets folder -- the 39-character truncation of the preset path.

All three components now size their buffers from a named `kPathMax` that
differs per target. Port another component that builds a path, and size its
buffers the same way.

`drum_kit.cpp` got the same treatment when sample kits were turned on -- its
kit paths were 128 to 192 bytes, sized for `/sd/osynth/kits`.

## Embedding it in the app

```sh
cmake -S app_osyntho -B build_embedded -DOSYNTHO_EMBEDDED=ON       -DCMAKE_PREFIX_PATH=<your Qt>
cmake --build build_embedded
```

`app_osyntho/src/embeddedmanager.cpp` implements the app's `IBluetoothManager`
over this library: `write()` queues a command frame to a protocol thread,
replies come back as `receivedData`, and `connected` is always true.
`SynthController` and all 43 QML files are untouched -- they cannot tell an
in-process engine from a BLE link, which is the point.

The one `#ifdef` in app code is in `qmlforeign.cpp`'s `create()`, beside the
existing Qt-Bluetooth-vs-SimpleBLE switch. QML has no preprocessor, so the
screens that need a real radio bind their `visible` to a new `App.standalone`
property instead.

An embedded build links **no** Bluetooth code at all -- neither backend is
compiled, and neither `Qt6::Bluetooth` nor SimpleBLE is linked.

## Latency

The sink's ring is sized in **device periods**, not render blocks, and that
correction is worth knowing about. WASAPI shared mode rounds its period up to
about 10 ms whatever is asked of it: we request 256 frames and get 480. A ring
of four 256-frame blocks looks generous and is in fact 2.1 device periods, so
any scheduling delay longer than one period starved it — which on Windows with
an ordinary-priority thread happened once or twice a run.

Four device periods is ~40 ms. Getting below that means raising the render
thread's priority or asking for an exclusive-mode device: both per-platform,
neither belonging in a first-sound port.

That the ring absorbs the scheduler is also what lets the **render block stay
the firmware's 64 frames**. The port ran at 256 for a while on the theory that
a general-purpose OS cannot meet a 1.33 ms deadline — but the render thread
has no per-block deadline here; it blocks on a ring several device periods
deep and simply produces four small blocks where it produced one large one.
Same work, same slack, four times the wakeups. What 256 *did* cost was parity:
`kParamSlew`, `kMixSlew` and `kTimeSlew` are one-pole coefficients **per
block**, and the compressor's gain, the FX LFOs, the phaser's coefficients,
the granular delay's grain spawns and every ADSR stage boundary all land on
the **block grid** — so the standalone build's effects smoothed four times
slower and stepped four times coarser than the instrument's. Two numbers are
therefore deliberately separate: `SYNTH_BLOCK_SIZE` is the renderer's, and
`kDevicePeriodFrames` (256) is what the device is asked for.

The device is also started by the render chain's **first block**, not by
`sink_start()`. At `sink_start()` the render task does not exist yet —
`audio_io_start()` creates it afterwards — so a device started there pulls
against a ring nobody is filling.
