# osynth Pin Map

Default GPIO assignment per board. **Every pin marked configurable can be
changed in `idf.py menuconfig` → "osynth Synthesizer"**; only the USB pins and
the classic ESP32's internal DAC are fixed in silicon.

Defaults come from `components/synth_core/Kconfig.projbuild` and the
per-target `sdkconfig.defaults.*` files. Wiring detail, straps, levels and the
reasoning behind each choice: `private_docs/HARDWARE.md`.

---

## ESP32-S3 — primary target

Devkit with native USB exposed (e.g. ESP32-S3-DevKitC-1 N8R8).
`sdkconfig.defaults.esp32s3` ships with the **ES8388 codec** front end and
line input **on**.

| Pin | Function | Goes to | menuconfig symbol |
| --- | --- | --- | --- |
| GPIO5  | Serial MIDI RX | 6N138 optocoupler output | `OSYNTH_SERIAL_MIDI_RX_GPIO` |
| GPIO8  | Codec I2C SDA | ES8388 **CDATA** | `OSYNTH_ES8388_I2C_SDA_GPIO` |
| GPIO9  | Codec I2C SCL | ES8388 **CCLK** | `OSYNTH_ES8388_I2C_SCL_GPIO` |
| GPIO10 | SD card CS | micro-SD | `OSYNTH_SD_CS_GPIO` |
| GPIO11 | SD card MOSI | micro-SD | `OSYNTH_SD_MOSI_GPIO` |
| GPIO12 | SD card SCK | micro-SD | `OSYNTH_SD_SCK_GPIO` |
| GPIO13 | SD card MISO | micro-SD | `OSYNTH_SD_MISO_GPIO` |
| GPIO14 | I2S MCLK | PCM1808 SCKI / ES8388 MCLK | `OSYNTH_I2S_MCLK_GPIO` |
| GPIO15 | I2S DIN (capture) | PCM1808 DOUT / ES8388 ASDOUT | `OSYNTH_I2S_DIN_GPIO` |
| GPIO16 | I2S BCLK | PCM5102A BCK / ES8388 SCLK | `OSYNTH_I2S_BCLK_GPIO` |
| GPIO17 | I2S LRCK/WS | PCM5102A LCK / ES8388 LRCK | `OSYNTH_I2S_WS_GPIO` |
| GPIO18 | I2S DOUT (playback) | PCM5102A DIN / ES8388 DSDIN | `OSYNTH_I2S_DOUT_GPIO` |
| GPIO19 | USB D− | host / computer | **fixed** — native USB-OTG |
| GPIO20 | USB D+ | host / computer | **fixed** — native USB-OTG |

**Do not use:**

- **GPIO33–37** — octal PSRAM. The looper's track buffers live there.
- **GPIO0, 3, 45, 46** — strapping pins.
- GPIO19/20 belong to the USB PHY the synth enumerates on.

**Ports on a two-port devkit:** the "USB" (native) port is the audio/MIDI
device; the "UART"/"COM" port is for `idf.py flash monitor`.

**In USB host mode (S35)** GPIO19/20 are the same two pins, doing the same job
in the other direction — the role is a runtime setting (`usb.mode`, on the
app's osynth page), not a different pinout, and flashing still happens over the
UART port either way. What host mode *does* need is 5 V on the socket's VBUS to
power the controller: that comes from the board, not from any GPIO this
firmware drives, so there is no pin here to assign. A bus-powered controller on
a socket whose VBUS is not fed will never enumerate.

---

## ESP32-P4 (+ ESP32-C6 for BLE)

**These defaults target a module-on-carrier board, not a wide-header devkit.**
The header on that board exposes only **GPIO1–5, 20, 32, 33, 45, 46, 47** plus a
pair of pads labelled **ES_I2C_SDA / ES_I2C_SCL**, and its micro-SD socket is
already wired. On a devkit with more pins out, any free pins work — all five
I2S signals route through the GPIO matrix. Check every row against your
schematic before wiring either way.

Ships with the **ES8388 codec** front end and line input **on**, driving an
external codec module, and the **microphone input on** (`OSYNTH_ENABLE_MIC_IN`,
default y on this target only) reading GPIO4. The on-board 3.5 mm jack is an
**ES8311** and is *not* what this configures — nor is the board's on-board
microphone; see "the two codecs" below.

| Pin | Function | Goes to | menuconfig symbol |
| --- | --- | --- | --- |
| GPIO14–19 | SDIO → C6 | on-board ESP32-C6 | ESP-Hosted — **claimed, see below** |
| GPIO20 | Serial MIDI RX | 6N138 optocoupler output | `OSYNTH_SERIAL_MIDI_RX_GPIO` |
| GPIO24 | USB D− | host / computer | **fixed** — USB-OTG FS PHY (port 0) |
| GPIO25 | USB D+ | host / computer | **fixed** — USB-OTG FS PHY (port 0) |
| — | USB host / device | the OTG socket (high-speed) | `usb.mode` — runtime, see below |
| GPIO1 | I2S BCLK | PCM5102A BCK | `OSYNTH_I2S_BCLK_GPIO` |
| GPIO32 | I2S DOUT (playback) | PCM5102A **DIN** | `OSYNTH_I2S_DOUT_GPIO` |
| GPIO33 | I2S LRCK/WS | PCM5102A LCK | `OSYNTH_I2S_WS_GPIO` |
| GPIO10 | Mic LRCK/WS | on-board mic block | `OSYNTH_MIC_WS_GPIO` — **on by default here** |
| GPIO48 | Mic DIN | on-board mic — ES8311 **ASDOUT** | `OSYNTH_MIC_DIN_GPIO` |
| GPIO9 | (unused) | ES8311 **DSDIN** — the board's speaker path | — |
| GPIO11 | (unused) | the speaker amplifier's **enable** — not a data pin | — |
| GPIO12 | Mic BCLK | on-board mic block | `OSYNTH_MIC_BCLK_GPIO` |
| GPIO13 | Mic MCLK | on-board mic block | `OSYNTH_MIC_MCLK_GPIO` |
| GPIO39–44 | micro-SD | on-board socket | SDMMC slot-1 IOMUX — **see below** |
| GPIO45–47 | — | **nothing — see the warning below** | |
| GPIO54 | C6 reset | on-board ESP32-C6 | ESP-Hosted — **claimed** |
| ES_I2C_SDA | Codec I2C SDA | ES8388 CDATA | `OSYNTH_ES8388_I2C_SDA_GPIO` (7) |
| ES_I2C_SCL | Codec I2C SCL | ES8388 CCLK | `OSYNTH_ES8388_I2C_SCL_GPIO` (8) |

Ships with a **PCM5102A** (`OSYNTH_FRONTEND_DISCRETE`), three signal wires and
no MCLK. Tie the module's **SCK to GND** — that is how its internal PLL engages,
and floating gives silence. Note the naming trap: the module's pin marked `DIN`
takes the P4's *DOUT*.

That leaves **GPIO4, 5 and 20** free for local UI and serial MIDI once MCLK
took GPIO2 and DIN took GPIO3 — and **GPIO4 is now the microphone's data pin**,
so it is really GPIO5 and 20. GPIO2–5 are the JTAG pads (MTCK/MTDI/MTMS/MTDO)
— fine as GPIOs, but they cost external JTAG.

**The mic default here needs no wiring to be safe.** Sharing clocks means it
only *reads* GPIO1 and GPIO33, and `in.source` defaults to `line`, so a board
with nothing on GPIO4 sounds exactly as it did.

> **The clock source on this target took three goes, and one of them
> bootlooped.** The 32-bit slots the ES8388 front end forces need a slave
> clock above ~48.9 MHz, and on a P4 pinned to rev <3 every candidate fails
> differently: `DEFAULT` is broken in IDF 6.0.2 (`= 0`, and nothing translates
> it, so the source frequency reads zero), `PLL_160M` is fast enough but
> `i2s_ll_get_clk_src()` gates it on `CHIP_SUPPORT_MIN_REV >= 300` and
> **`HAL_ASSERT`s** rather than returning an error — an abort, mid-init, so it
> bootloops — and `EXTERNAL` needs a faster pin clock than the board has. What
> works **for a slave** is **XTAL with a divided declared rate**: a slave's
> declared rate never reaches a pin, so halving it halves the internal clock
> demand and changes nothing on the wire. Expect `clk xtal, declared rate/2` on
> the shared-clock (external MEMS) path. Full write-up in
> `sdkconfig.defaults.esp32p4`.
>
> **A master takes APLL instead, and the ban on it was wrong** (S37d). It
> applied to a slave, whose divided declared rate really would ask for a
> different frequency; a master asks for `mclk = 256 x fs`, the identical figure
> `sink_i2s.cpp` asks for on the output port. And IDF cannot retune an occupied
> APLL in any case — `esp_clk_tree_src_set_freq_hz()` returns
> `ESP_ERR_INVALID_STATE` and hands back the frequency it is already running at.
> This matters because on a master that MCLK is a *pin*: from the 40 MHz XTAL,
> 12.288 MHz needs a fractional divider (3 + 49/192), so the clock an ES8311
> derives its whole ADC timing from dithers between 75 ns and 100 ns periods.
> Expect `clk apll` on the on-board-mic path — and, as a free consequence, a
> capture sample-locked to playback rather than in a second clock domain.

> ### ⚠️ GPIO45, 46 and 47 do not drive on this carrier
>
> They read a static **0.6 V** when configured as outputs and nothing connected,
> against 3.3 V on every other candidate. Held by something on the board — the
> "free header pins" list this target was originally configured from was never
> checked against a schematic.
>
> This cost a week. With MCLK/BCLK/WS on those three pins, an ES8388 produced
> grossly distorted audio (it has its own clock reference, so it converts
> anyway) and a PCM5102A produced silence (it must PLL-lock to BCK, and could
> not). Two converters, two symptoms, one cause — and everything upstream was
> verified correct while the fault sat in the pads.
>
> **Before trusting any pin on an unfamiliar board, measure it.** Set
> `OSYNTH_GPIO_OUTPUT_SCAN` in `components/audio_io/sink_i2s.cpp`: it drives a
> list of candidates high with nothing connected, so a multimeter tells you in
> two minutes what days of listening cannot. Include pins you already trust as
> controls. Measured result here — usable: **1, 2, 3, 4, 5, 20, 32, 33**;
> unusable: **45, 46, 47**.

**Why 45/46/47 looked free on a board that has an SD socket:** they are
SD1_CDATA4–6 in the IO MUX, the 8-bit-mode data lines of SDMMC slot 1. A
micro-SD card only has four, so a socket wired to that IOMUX takes 39–42 +
CLK 43 + CMD 44 and leaves these three over. Three adjacent spare pins is why
the clock-rate signals live there and the two data lines went to 32/33.

**The two codecs.** The external ES8388 and the board's on-board ES8311 share
the ES_I2C bus but not the I2S port. osynth drives only the ES8388
(`components/codec/` implements no other), so the ES8311 stays in its
power-up state: unconfigured and silent, jack dead. Nothing collides — one
master, and the addresses differ.

That also settles what the board's **on-board microphone** is not:
it is clocked by the board's own I2S pin group (~GPIO9–13), not the 1/2/3/32/33
this firmware drives, so `OSYNTH_MIC_SHARE_CLOCKS` would read the wrong pins
and its real clock pins are neither on the header nor in any schematic to hand.
**Settled in S37h**: it is the ADC half of the ES8311, its data pin is GPIO48,
and GPIO11 is the speaker amplifier's enable. The rest of this paragraph is the
reasoning that got there, which was sound even where the pin was not. It was
unresolved whether that pin carried a bare MEMS mic or the ADC half of
the ES8311, which `components/codec/` has no driver for — an ESPHome
`adc_type: external` entry does not distinguish them, and the bus scan does:
0x18 present means a codec that must be configured over I2C before it emits
anything. The default mic input above is an **external** mic on GPIO4, not this
one.

Reaching it means having its port *master* the board's own pins rather than
slaving to the ES8388's, and that is what this target now defaults to:
`OSYNTH_MIC_SHARE_CLOCKS=n`, **DIN 48, BCLK 12, WS 10, MCLK 13** — the numbers
from the board repo's own `i2s_audio:` block, not inferred. Mastering is also
simpler here: a master needs only 12.288 MHz internally, which XTAL clears, so
none of the divided-rate workaround applies. The cost is a second clock domain
that drifts slowly against the DAC, which the DMA ring absorbs.

**Those pins are necessary and not sufficient.** The board's mic is an
*analogue* microphone into an **ES8311 at I2C 0x18** (its own config: `platform:
es8311`, `use_mclk: True`, `microphone_type: analog`, `mic_gain: 24DB`) — which
is also why GPIO13 carries MCLK at all, since no digital MEMS part has such a
pin. That codec powers up unconfigured and silent and converts nothing until
its registers are written, and `components/codec/` drives the ES8388 and
nothing else. Until an ES8311 init exists, this port reads exactly what it read
before the pins were right: full blocks, no starves, every sample zero.

The external-mic path — a digital MEMS part on `OSYNTH_MIC_SHARE_CLOCKS` —
needs no codec at all and is proven working end to end. The boot-time bus scan is the way to read
it: **0x18** the on-board ES8311, **0x10/0x11** the ES8388, **0x33** the M5
module's STM32, **0x40/0x41** an ES7210 mic array. Only 0x10/0x11 matters to
the driver; the rest is the board identifying itself.

**The on-board SD socket needs its pins remapped.** osynth's storage backend is
SDSPI, and an on-board socket is wired for SDMMC. It costs no header pins to
fix — SPI2 goes through the GPIO matrix, so the firmware reaches pins the
header never brings out, and every SD card speaks SPI. Drive the same socket
with the SPI role mapping: **CS←DAT3 (42), SCK←CLK (43), MOSI←CMD (44),
MISO←DAT0 (39)**. Left commented out in `sdkconfig.defaults.esp32p4` because it
is unverified on this board; confirm the socket really is on slot 1 and that
DAT1/DAT2 are pulled up before enabling it.

**USB host mode (S35) uses the same socket, not the other controller.** The
tempting reading of the two rows above is that the P4's second USB controller
could host while the first stays a device. It cannot on this carrier: GPIO24/25
are the *full-speed* controller and they reach no connector, so only the
high-speed controller has a socket — the same one the device role enumerates
on. Host and device are therefore alternatives here exactly as on the S3, and
`usb.mode` picks between them at boot. See
`components/usb_dev/tusb/usb_descriptors.h` for the measurement behind that.

**Do not use:**

- **GPIO24/25** — USB-OTG full-speed PHY.
- **GPIO34–38** — strapping pins (37/38 are also console UART0).
- **GPIO10–13** — UART1 IOMUX pins.
- **GPIO14/15** — LP-UART.
- **GPIO14–19 and GPIO54** — the SDIO block to the C6, with
  `ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`: CLK[18] CMD[19] D0[14] D1[15] D2[16]
  D3[17], slave reset [54]. This block differs per board and is the most likely
  collision — the I2S defaults used to land right on it. Resolve it from the
  schematic first; nothing else can be chosen around it blind, and if you change
  the ESP-Hosted board choice, every row above needs re-checking against the new
  block.

The collision that block causes does not announce itself as a pin conflict:
audio comes up first and the C6 link then fails its card init with
`sdmmc_io_reset: unexpected return: 0x108`, with nothing in the log naming a
pin. The Kconfig defaults for MCLK (16) and DIN (20) land inside it, which is
why `sdkconfig.defaults.esp32p4` sets **all five** I2S pins explicitly rather
than leaving the two that are only live on a codec front end to their defaults.

---

## Classic ESP32

4 MB flash devkit. No USB-OTG: audio out via the internal 8-bit DAC (default)
or I2S, control via BLE, MIDI via the serial input. No line input, no ES8388,
no looper persistence (all need the S3/P4 or PSRAM).

| Pin | Function | Goes to | menuconfig symbol |
| --- | --- | --- | --- |
| GPIO25 | Internal DAC ch 1 (**L**) | AC-couple → amp | **fixed** — default audio out |
| GPIO26 | Internal DAC ch 2 (**R**) | AC-couple → amp | **fixed** — default audio out |
| GPIO27 | I2S BCLK | PCM5102A BCK | `OSYNTH_I2S_BCLK_GPIO` |
| GPIO32 | I2S LRCK/WS | PCM5102A LCK | `OSYNTH_I2S_WS_GPIO` |
| GPIO33 | I2S DOUT | PCM5102A DIN | `OSYNTH_I2S_DOUT_GPIO` |
| GPIO35 | Serial MIDI RX | 6N138 optocoupler output | `OSYNTH_SERIAL_MIDI_RX_GPIO` — input-only pin |

**Do not use:**

- **GPIO6–11** — SPI flash.
- **GPIO12** — MTDI, flash-voltage strap.

The internal DAC sits at a ~1.65 V DC bias — AC-couple each channel (~10 µF in
series) before an amplifier or line input.

---

## Which pins are actually live

A pin only exists if its feature is enabled:

| Pins | Only when |
| --- | --- |
| I2S BCLK / WS / DOUT | `OSYNTH_ENABLE_I2S_DAC` |
| I2S DIN | `OSYNTH_ENABLE_I2S_LINE_IN` (S3/P4 only) |
| I2S MCLK | `OSYNTH_ENABLE_I2S_LINE_IN` **or** `OSYNTH_FRONTEND_ES8388` |
| Mic DIN | `OSYNTH_ENABLE_MIC_IN` (S3/P4 only) |
| Mic BCLK / WS | `OSYNTH_ENABLE_MIC_IN` **and** `OSYNTH_MIC_SHARE_CLOCKS`=n — otherwise the mic reads the I2S BCLK/WS pins above |
| Codec I2C SDA/SCL | `OSYNTH_FRONTEND_ES8388` |
| SD card ×4 | `OSYNTH_LOOP_STORE_SD` **or** `OSYNTH_DRUM_SD_KITS` (shared bus) |
| Serial MIDI RX | `OSYNTH_ENABLE_SERIAL_MIDI` |
| Internal DAC (ESP32) | no I2S DAC enabled |
| USB D−/D+ as a **host** | `usb.mode` = host — a runtime setting, and only offered when an I2S DAC carries the audio (otherwise the USB *device* is the audio clock) |

Local UI (LCD + encoders + buttons) has **no pins assigned yet** — the
component compiles to a stub.

### Microphone input (S37)

A digital MEMS mic (INMP441, ICS-43434, SPH0645) runs on the **second** I2S
controller — the S3 has two and the P4 three, and the line input holds the
first. Off by default (`OSYNTH_ENABLE_MIC_IN`).

It does **not** get a second DIN on the DAC's port: a port has one RX channel
and one DIN pin, and the line input has them. That is the whole reason this is
a second controller rather than a second pin.

| Mic pin | Goes to | With `MIC_SHARE_CLOCKS=y` (default) |
| --- | --- | --- |
| SD / DOUT | `OSYNTH_MIC_DIN_GPIO` | the one pin the feature costs |
| SCK / BCLK | the I2S BCLK pin | shared with the DAC — nothing to wire but the tap |
| WS / LRCL | the I2S LRCK/WS pin | likewise |
| L/R | **GND or 3V3, never floating** | picks the slot; match `OSYNTH_MIC_SLOT` |
| VDD / GND | 3V3 / GND | |

Sharing clocks is the default because it is strictly better: one pin instead of
three, and the capture is sample-locked to playback exactly the way the line
input is. The GPIO matrix allows one output driver and any number of input taps
on a pad, and the I2S driver only enables the input on a pin it takes as an
input, so a slave reading BCLK/WS does not disturb the master driving them.

Suggested free pins — **S3**: 1, 2, 4, 6, 7, 21, 38–44, 47, 48 (default 4).
**P4 carrier**: GPIO4 or 5, the only header pins the I2S block left over that
have no other claim (GPIO20 is the serial-MIDI default).

`in.source` picks which device feeds the input chain — `line`, `mic`, or
**`both`**, which mixes the two — with `in.micgain` as the microphone's own
level trim. Both parameters are registered only where both devices are compiled
in, and both are persisted. Every device is captured every block regardless, so
the heartbeat meters each one separately (`in line …/… g… mic …/… g…`) and an
unselected microphone still reads: the `g` figure after each pair is that
device's live mix gain, which is what separates "not selected" from "selected
and silent" from "turned all the way down".

**First thing to check on a silent new mic:** the L/R strap against
`OSYNTH_MIC_SLOT`. Reading the slot the mic does not drive is silence from a
microphone that is working perfectly, and it looks identical to a wiring fault.

---

## Board-specific wiring

### PCM5102A (discrete DAC, best-sounding option)

| PCM5102A | Wire to |
| --- | --- |
| BCK / LCK / DIN | the three I2S pins above |
| SCK | **GND** (internal PLL) |
| FMT | GND — I2S / Philips |
| XSMT | 3V3 — un-mute |
| FLT / DEMP | GND |
| VIN / GND | 3V3 / GND |

### PCM1808 (discrete ADC, adds line in)

| PCM1808 | Wire to |
| --- | --- |
| SCKI | MCLK pin — 12.288 MHz, **mandatory even in slave mode** |
| BCK / LRCK | shared with the PCM5102A |
| DOUT | I2S DIN pin |
| MD0 / MD1 | **GND** — slave mode |
| FMT | GND — I2S / Philips |

**Meter MD0 and MD1 to GND before powering up.** Floating or high, the PCM1808
comes up as *master* and drives BCK/LRCK against the ESP32 — bus contention on
two pins with both sides pushing.

### ES8388 codec (ESP32-A1S, LyraT, "audio kit" boards)

| ES8388 | ESP32-S3 default | ESP32-P4 default |
| --- | --- | --- |
| MCLK | GPIO14 | GPIO45 |
| SCLK | GPIO16 | GPIO46 |
| LRCK | GPIO17 | GPIO47 |
| DSDIN | GPIO18 | GPIO32 |
| ASDOUT | GPIO15 | GPIO33 |
| CDATA | GPIO8 — **1k pull-up to 3V3** | ES_I2C_SDA — pads already pulled up |
| CCLK | GPIO9 — **1k pull-up to 3V3** | ES_I2C_SCL — pads already pulled up |
| CE | GND (addr 0x10) **or** 3V3 (0x11) — **never to a GPIO** | same |

On the P4 the control bus is the board's own, shared with an on-board ES8311 —
do **not** add the 1k pair there, the board fits its own and the codec module
brings more in parallel.

The firmware probes both addresses, so CE either way is fine. Control pins are
named CCLK/CDATA, not SCL/SDA. Some boards gate their speaker amp with a
PA-enable GPIO this firmware does not drive — if headphones work and speakers
stay silent, that pin is why.

### M5Stack Module Audio (M144) → ESP32-S3 devkit / ESP32-P4 module

Jumper wires into the 30-pin header. Odd pins in one row, even in the other.

| M-Bus pin | Signal | ESP32-S3 | ESP32-P4 |
| --- | --- | --- | --- |
| 12 | SYS_3.3V | 3V3 | 3V3 |
| 1, 3, 5 | GND | a pin marked **GND** — see below | same |
| 17 | BUS_SDA (CDATA) | GPIO8 | ES_I2C_SDA |
| 18 | BUS_SCL (CCLK) | GPIO9 | ES_I2C_SCL |
| 21 | I2S_LRCK | GPIO17 | GPIO47 |
| 22 | I2S_MCLK | GPIO14 | GPIO45 |
| 23 | I2S_MAIN_DOUT | GPIO18 | GPIO32 |
| 24 | I2S_SCLK | GPIO16 | GPIO46 |
| 26 | I2S_MAIN_DIN | GPIO15 | GPIO33 |

Everything else unconnected: pin 6 (RST), pin 28 (5V), pins 25/27/29 (HPWR).

- **Slide switch S1 must be in position "B"** — it swaps which of pins 22/24
  carries MCLK. Wrong position gives no sound.
- **Ground is one node, not three pins.** M-Bus 1/3/5 are three tie points to
  the *same* net, and the host end wants a pin actually marked GND. If the
  header has only one GND left, put all three wires on it — sharing a ground
  pin is normal and correct. Two or three separate GND pins are better only
  because they lower the resistance of the return, which is worth having with
  three clocks up to 12.288 MHz on flying leads.
- **Never substitute a GPIO for GND.** This is not a pedantic point and it does
  not fail loudly. An unconfigured GPIO is a high-impedance input, so the
  module's only return to the host is through the pad's ESD protection diodes —
  a threshold device, and nonlinear. The result is audio that plays but is
  grossly distorted at *every* level, into *every* load, at *any* sample rate,
  with a comb-filtered "phaser" character as the two channels return through
  each other; intermittent I2C NACKs; and an AUDIO_VDD that measures perfectly
  healthy, because a meter referenced to the module's own local ground reads a
  sensible number while the module floats relative to the host. Every one of
  those was observed on the P4 bring-up before the ground was found.
- **On the P4 module-on-carrier, header pin numbers and GPIO numbers are
  different things, and both appear as bare integers.** Header position 16 on
  the board this was brought up on is a GND pad; GPIO16 is SDIO D2 to the C6.
  A schematic row reading `163 -> GND` is a connector position, not a GPIO.
  Read every row of the table above as a *GPIO* number and find it on the
  silkscreen rather than counting header positions. The boot log prints the one
  block where this is dangerous, so check it against your wiring:
  `sdio_wrapper: GPIOs: CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17]
  Slave_Reset[54]` — grounding any of those shorts a line both chips drive, and
  it surfaces as an ESP-Hosted boot loop (`sdmmc_io_rw_extended ... 0x109`, then
  `0x107`, then `Restarting host`) with nothing audio-related in the log.
- Set `OSYNTH_ES8388_INPUT` to **DIFF** and `OSYNTH_ES8388_OUTPUT` to **OUT1**
  (already in `sdkconfig.defaults.esp32s3` and `.esp32p4`). The input jack is
  differential;
  single-ended reads exactly half the amplitude on one channel.
- J1 (TRS, "LINE/MIC") is the **input**; J2 (TRRS) is the **output**.

### SD card (SPI mode, FAT)

CS/SCK/MOSI/MISO straight through, 3.3 V, common ground. Most breakouts carry
the MISO/DAT0 pull-up; on a bare socket add 10 kΩ on CS and MISO. The firmware
does not format cards.

### DIN-5 MIDI input

**Never wire a DIN cable to a GPIO directly.** Use the standard optocoupler
circuit (6N138: DIN pins 4/5 → 220 Ω + opto LED, output pulled to **3.3 V**,
then to the RX pin). A 3.3 V TTL MIDI source can connect straight to RX.
