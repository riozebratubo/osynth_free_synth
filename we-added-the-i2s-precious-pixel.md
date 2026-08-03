# Stereo line-in via PCM1808, full-duplex with the PCM5102A (S31)

## Context

The I2S sink added in S2 gives osynth a proper line **out** through a PCM5102A, but
the PCM5102A is a DAC — it has no input. The synth currently has no audio input of
any kind: `audio_sink_t` (`components/audio_io/audio_sink.h:23-28`) and
`audio_render_fn` (`components/audio_io/include/audio_io.h:27`) are both
output-only, and the looper records the internal master bus and nothing else
(`components/looper/looper.cpp:1059-1090`).

The ESP32-S3's I2S controller is full-duplex: TX and RX allocated from a single
`i2s_new_channel()` call land on the same port and share BCLK and WS. So a
**PCM1808 stereo ADC in slave mode** can hang off the two clock lines the DAC
already drives, costing one GPIO for DIN and one for MCLK. Because RX and TX share
the clock, the input is sample-locked to the output by construction — no drift, no
resampling, ever.

Outcome: a routable stereo line input with four positions — `off`, `mon` (dry
monitor, heard but never recorded), `fx` (into the FX chain, reverberated *and*
recorded), `dry` (recorded dry, no FX). External gear runs through the synth's FX
and lands in looper takes. Default off; the existing output path is untouched when
the feature is disabled.

---

## Verified facts (checked against ESP-IDF v6.0.1 source, not assumed)

These decide whether the approach works at all:

1. **Changing `slot_bit_width` does not change any buffer format.**
   `i2s_std_set_slot` sizes the DMA buffer from `data_bit_width`, not the slot
   width (`esp_driver_i2s/i2s_std.c:153` → `i2s_common.c:446-458`,
   `bytes_per_sample = (data_bit_width + 7) / 8`). So setting
   `slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT` with `data_bit_width = 16` moves
   BCLK from 32·fs to 64·fs while **`s_out` stays `int16_t` and `i2s_write()`
   (`sink_i2s.cpp:69-80`) is untouched**. `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` is
   not set for esp32s3, so there is no cache-line rounding either.

2. **Duplex is decided by `memcmp` of the whole `i2s_std_config_t`**
   (`i2s_std.c:279`), and on the S3 a mismatch is **silent** — `ESP_LOGD` only
   (`i2s_std.c:306`), because the hard error is `#if SOC_I2S_HW_VERSION_1`
   (`i2s_std.c:343-345`) and the S3 is v2. Both channels would then be independent
   masters driving the same pins. Two consequences: pass one **static** config
   object to both init calls (static storage zeroes padding, which `memcmp`
   covers), and **assert** duplex actually constituted.

3. **The later-initialized channel is demoted** to `I2S_ROLE_SLAVE` with
   `full_duplex_slave = true` (`i2s_std.c:310-318`). Initialize **TX first** so the
   DAC keeps generating BCLK/WS/MCLK exactly as today.

4. **`i2s_channel_read` with `timeout_ms = 0` cannot block**
   (`i2s_common.c:1396-1441`: zero-tick semaphore take, zero-tick `xQueueReceive`,
   returns `ESP_ERR_TIMEOUT` with a partial `bytes_read`). The TX blocking write
   stays the sole pacer.

5. **RX latency cannot accumulate.** The RX ISR drops the oldest descriptor when
   the queue is full (`i2s_common.c:635-641`) and `i2s_channel_read` skips forward
   when `uxQueueSpacesAvailable <= 1` (`i2s_common.c:1417`). Queue depth is
   `dma_desc_num - 1` = 3 (`i2s_common.c:330`), so the backlog self-clamps at 1-2
   buffers ≈ 1.3-2.7 ms. Combined with the shared clock, **no drain logic is
   needed**.

6. **`simd_mix_i16lr_f32` already exists** (`components/synth_core/include/synth_simd.h:29-48`)
   with the exact signature needed — `l[i] += p[2i]*g; r[i] += p[2i+1]*g` — and has
   **zero callers** in the tree. Reuse it; do not write a conversion loop.

7. **Param IDs 0x0008 and 0x0009 are free.** 0x0000-0x0001 are globals
   (`synth_params.h:42-43`), 0x0002-0x0007 are preset triggers
   (`presets.h:54-59`), and `PID_SPACE_END = 0x0800`.

Clock arithmetic with 32-bit slots: BCLK = 48000 × 2 × 32 = **3.072 MHz (64·fs)**,
MCLK = 48000 × 256 = **12.288 MHz (256·fs)**, `bclk_div = 4` exactly — so the
"cannot perform integer division" warning (`i2s_std.c:44-46`) will not fire, and
12.288 MHz is one of the PCM1808's three legal SCKI rates.

---

## Design decisions

**Extend `sink_i2s.cpp` in place — do not add an `audio_source_t` vtable.**
Both handles come from one `i2s_new_channel()` call, must share one config object,
share GPIOs, and share a teardown path. A separate `source_i2s.cpp` would need a
private header re-exporting the handle, the config and the failure state — a split
with negative information hiding. `audio_sink_t` earns its vtable because four
sinks compete at runtime; there is exactly one source and no fallback. Worse, the
contracts are opposite: the sink's `write()` **blocks and paces**
(`audio_sink.h:1-13`); the source's `read()` must never do either. Two free
functions with an explicit contract comment beat a lookalike vtable.

**Line-in is a render-chain *stage*, like the drum bus — not a widened callback.**
`components/drums/` already solves this exact shape: render once into scratch,
expose `drums_pre_fx()` / `drums_post_fx()` / `drums_render_click()`, and let
`render_chain` (`main/main.cpp:107-129`) call each at the right point. Line-in is
structurally identical — one capture, three possible mix points, one active.
Widening `audio_render_fn` would change a public typedef, and a config-dependent
function-pointer signature is a stack-corruption footgun if a TU ever sees the
wrong `sdkconfig.h`. It would also leave gain smoothing and route decode stranded
in `main.cpp` while the buffers and RX handle live in `audio_io`.

**Three `Smooth` instances, one per position** (`synth_smooth.h:65-90`), each
targeting `in.gain` when selected and `0` otherwise. Route switching crossfades
instead of clicking, gain drags don't zipper, and each mix function reduces to one
compare when inactive — the same `kSilent` gate the FX bus uses (`fx.cpp:196-198`).

**No float input buffers.** One `int16_t s_in[SYNTH_BLOCK_SIZE*2]` (256 B) plus
`simd_mix_i16lr_f32`, which does convert + gain + accumulate in one 4-wide pass.

**No fourth stage meter.** `main.cpp:109-115` already argues this for the drums
(*"a fourth number would not have changed any diagnosis so far"*), and it holds
harder here: line-in is a fixed ~0.7% that doesn't vary with anything the player
does. `in_peak` and `in_starves` are the diagnostics that pay.

---

## Implementation

Sequenced so each step compiles; audible behaviour changes only at step 7.

### 1. `components/synth_core/Kconfig.projbuild`
After `OSYNTH_USB_AUDIO_TAP` (ends :41), keeping the I2S cluster together:
- `OSYNTH_ENABLE_I2S_LINE_IN` — bool, `default n`,
  `depends on OSYNTH_ENABLE_I2S_DAC && IDF_TARGET_ESP32S3`
- `OSYNTH_I2S_DIN_GPIO` — int, default `15`
- `OSYNTH_I2S_MCLK_GPIO` — int, default `14`

Help text in the established voice: the full-duplex arrangement, why BCLK moves
32·fs → 64·fs, why the PCM1808 needs MCLK even as a slave, the four route
positions and what each records, ~8 ms monitor latency, ~0.7% block budget.

### 2. `components/synth_core/include/synth_config.h`
`SYNTH_ENABLE_LINE_IN` after the `SYNTH_ENABLE_USB_TAP` block (:45-49), repeating
the capability gate rather than trusting Kconfig — that block's own comment
explains the idiom. Gate on `CONFIG_OSYNTH_ENABLE_I2S_LINE_IN &&
SYNTH_ENABLE_I2S_DAC && CONFIG_IDF_TARGET_ESP32S3`.

The classic-ESP32 exclusion is real, not defensive: there, MCLK can't go through
the GPIO matrix (`i2s_common.c:950-957` routes it via `esp_clock_output_start`,
restricted to CLK_OUT/strapping pins), and it's HW v1 where duplex mismatch is a
hard error.

### 3. `components/synth_core/include/synth_params.h`
After `PID_ENGINE_TYPE` (:43), **unconditionally** (so `skip_id` needs no `#if`):
`PID_LINE_IN_ROUTE = 0x0008`, `PID_LINE_IN_GAIN = 0x0009`, with a comment noting
0x0002-0x0007 belong to the preset triggers.

### 4. `components/audio_io/sink_i2s.cpp` — the substantive change
Rewrite the header comment (:1-8) to cover both directions.

Add under `#if SYNTH_ENABLE_LINE_IN`: `OSYNTH_I2S_DIN` / `OSYNTH_I2S_MCLK` macros
from the `CONFIG_*` values (matching `midi_serial.c:28`'s idiom), and
`i2s_chan_handle_t s_rx = nullptr;`.

Restructure `i2s_start()`:
1. Build the slot config as a value; under `#if SYNTH_ENABLE_LINE_IN` set
   `slot.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT` (comment citing fact 1 above —
   this line looks alarming and needs to explain why the buffer format is safe).
2. Declare the shared `i2s_std_config_t` with **static storage duration**; pass the
   same object to both init calls (fact 2).
3. `i2s_new_channel(&chan_cfg, &s_tx, &s_rx)`, `.mclk`/`.din` set; then
   `i2s_channel_init_std_mode(s_tx, ...)` **then** `(s_rx, ...)` (fact 3).
4. **Assert duplex constituted**: `i2s_channel_get_info(s_tx, &info)` →
   require `info.pair_chan != nullptr`. Without this, fact 2's silent failure ships.
5. On any RX-side failure: `i2s_del_channel` **both**, null both, log a warning,
   fall through to today's exact simplex path. Do not delete only RX —
   `i2s_del_channel` clears `controller->full_duplex` (`i2s_common.c:393-399`) and
   leaves TX half-configured.
6. Enable **RX first, then TX**. BCLK/WS only start with the master; arming RX
   first makes frame alignment deterministic from frame 0. TX-first opens a race
   where RX latches mid-frame and comes up with **L/R swapped permanently**,
   intermittently across boots.
7. Extend the start log (:64-65) with the real clock numbers and `duplex ok`, so a
   bad build is visible in the monitor.

Add `audio_source_i2s_ready()` and `audio_source_i2s_read()` (`SYNTH_RENDER_IRAM`,
zero timeout, reports bytes read so the caller can zero-fill the tail).

### 5. `components/audio_io/audio_sink.h`
Declare both under `#if SYNTH_ENABLE_LINE_IN` next to `audio_sink_i2s()` (:47-50),
with the never-block/never-pace contract spelled out — it inverts the sink contract
this header opens with, so the contrast is worth two sentences.

### 6. `components/audio_io/include/audio_io.h`
- `Session 31:` paragraph in the file header, matching the existing narrative.
- `in_peak` (float, pre-gain, reset on read) and `in_starves` (uint32) added to
  `audio_io_stats_t` (:29-59). Pre-gain because ADC clipping happens in the
  analogue domain and no firmware gain undoes it — that's the whole point.
- Declare the three `audio_io_line_in_*` functions.
- Drive-by while here: `audio_io_quiet_ms()` and its 13-line comment are
  **duplicated verbatim** at :91 and :105. Delete the second copy.

### 7. `components/audio_io/audio_io.cpp`
Statics next to `s_buf_l`/`s_out` (:57-59): `s_in[128]`, cached
`std::atomic<float>*` for both params, `s_line_in_ok`, `Smooth s_in_sm[3]`,
`float s_in_g[3]`.

`line_in_capture()` — called at the top of `audio_task` **after `c0`** (:104) so
its cost lands honestly in `dsp_load_pct`, and **before `s_render`** (:109) so the
block reaches the first stage: guard on `s_line_in_ok`, read + zero-fill tail +
`in_starves` on error, integer max-abs peak into `s_stats.in_peak` inside the
existing `s_stats_mux` section (:139-142), then tick all three smoothers.

Three `SYNTH_RENDER_IRAM` mix functions, each `if (g <= kSilent) return;` +
`simd_mix_i16lr_f32(s_in, g * (1.0f/32768.0f), l, r, frames)`. Empty bodies in the
`#else` branch so `render_chain` needs no `#if`.

In `audio_io_start()`, after the sink-start/fallback block (:212-218):
`s_line_in_ok = (s_sink == audio_sink_i2s()) && audio_source_i2s_ready() && both
param pointers non-null`. If the I2S sink fell back to null, line-in is inert but
the params stay registered — log once.

### 8. `main/main.cpp`
- `kInRouteNames[] = {"off","mon","fx","dry"}` next to `kEngineNames` (:64-69).
- Two `ParamDesc` rows in `kGlobals[]` (:70-76) under `#if SYNTH_ENABLE_LINE_IN`,
  using the in-array conditional idiom `kEngineNames` already uses at :66-68.
  `in.route`: Enum, 0..3, default 0. `in.gain`: Float, **Linear**, 0..4, default
  1.0 — Linear because Exp requires `min > 0` (`smooth_exp` divides by `s.cur`,
  `synth_smooth.h:102`) and 0 must mean silent.
- Both ids into `kPersisted[]` (:87-89) — the route and gain are the rig's wiring,
  not a patch. Same reasoning the file already gives for `master.volume`.
- Registration must stay in `main.cpp`, not `audio_io_start()`: the boot-order
  comment at :5-18 requires all registration before `persist_init()` (:165), and
  `audio_io_start()` runs at :170. Registering later means saved values never
  return.
- Three calls in `render_chain` (:107-129): `_fx` after `drums_pre_fx` (before the
  `c1` marker, so its cost folds into `voi` alongside the drums), `_dry` after
  `drums_post_fx`, `_mon` after `looper_process`. Extend the doc comment (:93-106)
  to explain the positions against the looper's record tap.
- Heartbeat (:211-225): add `in pk %.2f, starve %u`. Boot banner (:133-136): add
  `line_in:%d`.

### 9. `components/presets/presets.cpp`
Add both ids to `skip_id()` next to `PID_MASTER_VOLUME` (:303). Without this,
`fetch_snapshot` captures every registered id and **loading a preset would silently
unmute a live microphone or change its gain**.

### 10. `app_osyntho/qml/FxScreen.qml`
One line: `ParamGroup { title: "Line in"; prefix: "in." }`. The app builds its UI
from `PARAM_INFO` (BLE opcode `0x03`) and `ParamGroup` self-hides when
`ids.length === 0` — so **no protocol version bump, no `synthprotocol.h` change**,
and the card vanishes on builds without line-in. Checked `in.` against every
registered prefix in the tree: no collision.

### 11. Docs
`private_docs/HARDWARE.md` — DIN/MCLK rows in the S3 pin table (:64-76) plus a
wiring section (straps, clock numbers, route semantics, ~8-9 ms monitor latency,
analogue levels, ground-loop warning, and the note that line-in reaches the USB
capture stream in all three positions). `PARAM_MAP.md` — two rows, marked
Persisted + preset-skipped. `ARCHITECTURE.md` — signal-flow diagram (:38-56),
feature table, `audio_io` row. `SESSIONS.md` — new S31 entry in the house format;
note that S29 and S30 exist in code but never got entries.

---

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| **PCM5102A rejects 64·fs / 32-bit slots** — touches the working output path | Blocking | Bench step 1 with the ADC unwired. Fallback: revert slot width, check whether the PCM1808 tolerates 32·fs, or move the ADC to `I2S_NUM_1` |
| **RX `left_align` takes the wrong 16 bits** of the 32-bit slot | High | TX is symmetric and the register semantics say it works, but I found no IDF test covering `data_bit < slot_bit` on **RX**. Signature: input ~48 dB down and grainy. Fallback is 32-bit data both ways + software shift, which *does* change `s_out` to int32 — so test this before building anything on top |
| **`i2s_ll_share_bck_ws` is literally `tx_conf.sig_loopback`** on the S3 (`i2s_ll.h:1161-1164`) — may loop TX data into RX | High | Negative test: DIN grounded, `in.route = mon`, output must be unchanged. If you hear the synth doubled, use `I2S_NUM_1` |
| **Duplex silently fails to constitute**; both channels become masters on one pin set | High, silent | Static shared config object + `pair_chan` assertion + teardown to TX-only |
| **PCM1808 straps wrong** — MD0/MD1 not both GND makes the ADC a *master*, fighting the ESP32 on BCK/LRCK (bus contention, possible pin damage) | High | **Meter MD0, MD1, FMT to GND before applying power.** Cheap breakouts vary; some pull these high |
| **RX enabled after TX** → L/R swapped, intermittent across boots | Medium | RX-first ordering; bench with a hard-left mono source |
| **Preset load flips `in.route`** and unmutes a live input | Medium | `skip_id()` (step 9) |
| Ground loop between the input source and a USB-connected host | Medium | Documented; star ground and/or a DI on the input |

---

## Wiring

| PCM1808 | ESP32-S3 | Note |
|---|---|---|
| SCKI | GPIO14 | 12.288 MHz = 256·fs. **Mandatory even in slave mode** |
| BCK | GPIO16 | shared with the PCM5102A |
| LRCK | GPIO17 | shared with the PCM5102A |
| DOUT | GPIO15 | ADC → ESP32 |
| MD0 / MD1 | GND | slave mode |
| FMT | GND | I2S (Philips) — matches `bit_shift = true` |
| VCC / GND | 3V3 / GND | star ground with the DAC; check whether your board has its own LDO for 5 V |

GPIO14/15 are free and adjacent to the existing 16/17/18 block, which keeps the
connector sane. **GPIO33-37 are octal PSRAM and must not be touched** — that PSRAM
is where the looper's buffers live.

---

## Verification (for you to run — I won't build or flash)

```
idf.py set-target esp32s3
idf.py menuconfig    # osynth Synthesizer → Stereo line input → set DIN=15, MCLK=14
idf.py build flash monitor
```

**Step 0 — regression, line-in OFF.** Nothing may change: same sound, same `dsp %`,
same boot log. This is the guarantee the default build is untouched.

**Step 1 — line-in ON, ADC not wired (DIN tied to GND).** The step that de-risks
the BCLK change. Boot log should report `16-bit data in 32-bit slots (bclk 3.072
MHz = 64fs, mclk 12.288 MHz = 256fs) … duplex ok`. **The DAC must play exactly as
before** — same pitch, same level, no distortion. No `cannot perform integer
division` warning. Heartbeat after the first second: `in pk 0.00, starve 0` —
`starve` climbing means RX isn't clocking. Scope, if you have one: BCK 3.072 MHz,
LRCK 48 kHz, MCLK 12.288 MHz. **Negative test:** set `in.route = mon`; output must
be *unchanged*. Doubling or flanging means `sig_loopback` is looping TX into RX.

**Step 2 — ADC wired.** 1 kHz sine at ~−10 dBV into **L only**, `in.route = mon`,
`in.gain = 1`. Heard clean, left only (right-only ⇒ enable ordering). `in pk` ~0.15
(if ~0.001 and grainy ⇒ slot alignment). Sweep gain 0→4: no zipper. Toggle route
off↔mon repeatedly: no clicks. Swap to R only.

**Step 3 — routing semantics.** Reverb up, looper armed:

| Route | Heard | In the take |
|---|---|---|
| `off` | no | no |
| `mon` | yes, dry | **no** |
| `fx` | yes, reverberated | yes, with the reverb print |
| `dry` | yes, dry | yes, without reverb |

The `mon` test matters most: record a loop while talking into the input, set
`in.route = off`, play back — your voice must be absent. Play the synth in all four
positions; it must be unaffected.

**Step 4 — stability, one hour on `mon` with a continuous source.** `in pk` stays
live, **`starve` stays 0**, `underruns` grows no faster than a line-in-off build,
`dsp` sits ~0.7 points higher. **Latency must not creep**: clap at the start and
again after an hour — same perceived delay (~8-9 ms). Creep would contradict the
self-clamping backlog.

**Step 5 — fallbacks.** Pull SCKI while running: `in pk` → 0, `starve` climbs, **the
DAC keeps playing**; reconnect and it recovers. Load a preset with `in.route = fx`:
route and gain must not change. Power cycle: both come back from NVS. USB-tap
build: the input appears in the host recording in all three positions. Finally
`idf.py set-target esp32 && idf.py build` — the symbol must not appear, the params
must not register, and the app must show no "Line in" card.
