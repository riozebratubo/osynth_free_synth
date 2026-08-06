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

---

## ESP32-P4 (+ ESP32-C6 for BLE)

**These defaults are chosen to be safe, not to match any particular devkit.**
No P4 board pinout is assumed — check every row against your schematic before
wiring. Ships with the **discrete** front end (PCM5102A); the on-board 3.5 mm
jack is most likely an ES8311, which osynth does not drive.

| Pin | Function | Goes to | menuconfig symbol |
| --- | --- | --- | --- |
| GPIO7  | Codec I2C SDA | ES8388 CDATA | `OSYNTH_ES8388_I2C_SDA_GPIO` |
| GPIO8  | Codec I2C SCL | ES8388 CCLK | `OSYNTH_ES8388_I2C_SCL_GPIO` |
| GPIO16 | I2S MCLK | PCM1808 SCKI / ES8388 MCLK | `OSYNTH_I2S_MCLK_GPIO` |
| GPIO17 | I2S BCLK | PCM5102A BCK | `OSYNTH_I2S_BCLK_GPIO` |
| GPIO18 | I2S LRCK/WS | PCM5102A LCK | `OSYNTH_I2S_WS_GPIO` |
| GPIO19 | I2S DOUT (playback) | PCM5102A DIN | `OSYNTH_I2S_DOUT_GPIO` |
| GPIO20 | I2S DIN (capture) | PCM1808 DOUT | `OSYNTH_I2S_DIN_GPIO` |
| GPIO21 | Serial MIDI RX | 6N138 optocoupler output | `OSYNTH_SERIAL_MIDI_RX_GPIO` |
| GPIO24 | USB D− | host / computer | **fixed** — USB-OTG FS PHY (port 0) |
| GPIO25 | USB D+ | host / computer | **fixed** — USB-OTG FS PHY (port 0) |
| GPIO39 | SD card CS | micro-SD | `OSYNTH_SD_CS_GPIO` |
| GPIO40 | SD card SCK | micro-SD | `OSYNTH_SD_SCK_GPIO` |
| GPIO41 | SD card MOSI | micro-SD | `OSYNTH_SD_MOSI_GPIO` |
| GPIO42 | SD card MISO | micro-SD | `OSYNTH_SD_MISO_GPIO` |
| board-specific | SDIO → C6 | ESP-Hosted (BLE + Wi-Fi link) | ESP-Hosted component |

**Do not use:**

- **GPIO24/25** — USB-OTG full-speed PHY.
- **GPIO37/38** — console UART0.
- **GPIO10–13** — UART1 IOMUX pins.
- **GPIO14/15** — LP-UART.
- **The SDIO block to the C6** — differs per board and is the most likely
  collision. Resolve it from the schematic first; nothing else can be chosen
  around it blind.

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
| Codec I2C SDA/SCL | `OSYNTH_FRONTEND_ES8388` |
| SD card ×4 | `OSYNTH_LOOP_STORE_SD` **or** `OSYNTH_DRUM_SD_KITS` (shared bus) |
| Serial MIDI RX | `OSYNTH_ENABLE_SERIAL_MIDI` |
| Internal DAC (ESP32) | no I2S DAC enabled |

Local UI (LCD + encoders + buttons) has **no pins assigned yet** — the
component compiles to a stub.

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

| ES8388 | ESP32-S3 default |
| --- | --- |
| MCLK | GPIO14 |
| SCLK | GPIO16 |
| LRCK | GPIO17 |
| DSDIN | GPIO18 |
| ASDOUT | GPIO15 |
| CDATA | GPIO8 — **1k pull-up to 3V3** |
| CCLK | GPIO9 — **1k pull-up to 3V3** |
| CE | GND (addr 0x10) **or** 3V3 (0x11) — **never to a GPIO** |

The firmware probes both addresses, so CE either way is fine. Control pins are
named CCLK/CDATA, not SCL/SDA. Some boards gate their speaker amp with a
PA-enable GPIO this firmware does not drive — if headphones work and speakers
stay silent, that pin is why.

### M5Stack Module Audio (M144) → ESP32-S3 devkit

Jumper wires into the 30-pin header. Odd pins in one row, even in the other.

| M-Bus pin | Signal | ESP32-S3 |
| --- | --- | --- |
| 12 | SYS_3.3V | 3V3 |
| 1, 3, 5 | GND | GND — **solder all three, short and direct** |
| 17 | BUS_SDA (CDATA) | GPIO8 |
| 18 | BUS_SCL (CCLK) | GPIO9 |
| 21 | I2S_LRCK | GPIO17 |
| 22 | I2S_MCLK | GPIO14 |
| 23 | I2S_MAIN_DOUT | GPIO18 |
| 24 | I2S_SCLK | GPIO16 |
| 26 | I2S_MAIN_DIN | GPIO15 |

Everything else unconnected: pin 6 (RST), pin 28 (5V), pins 25/27/29 (HPWR).

- **Slide switch S1 must be in position "B"** — it swaps which of pins 22/24
  carries MCLK. Wrong position gives no sound.
- **Three grounds, not one.** One thin ground jumper for three clocks up to
  12.288 MHz makes the codec NACK I2C the moment the I2S port is enabled.
- Set `OSYNTH_ES8388_INPUT` to **DIFF** and `OSYNTH_ES8388_OUTPUT` to **OUT1**
  (already in `sdkconfig.defaults.esp32s3`). The input jack is differential;
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
