#!/usr/bin/env python3
"""S37e - ESP32-P4 on-board microphone: the two things S37d's data leaves open.

S37d's boot log settled a lot: APLL took (clean 12.288 MHz MCLK), the GPIO
matrix routed all four pads to the right I2S1 signals (32 MCLK, 35 I_BCK,
36 I_WS out; 34 I_SD in), the codec identifies itself as `chip id 8311 ver 01`
so the register reads are genuine, and `raw 000000/000000` under +18 dB with
someone shouting at the board says the data pin is at *exactly* zero, forever.

That leaves two candidates, and this addresses one of each kind:

 1. **A diagnostic.** Nothing has ever established that GPIO10/12/13 physically
    toggle. A master fills its DMA ring from its internal clock whether or not
    a single pad is connected, so `starve 1/1` proves nothing about copper, and
    this board has form (45/46/47 read 0.6 V and cost a week). probe_clock_pins()
    enables the pad input alongside the peripheral output — gpio_input_enable()
    touches only the IE bit — and polls each pad a few thousand times. A pin
    carrying BCLK, WS or MCLK reads both levels; a dead one reads one level.

 2. **A candidate fix.** codec_es8311.cpp writes whole registers where the
    vendor read-modify-writes, and two of those masks are not about unknown
    state — they are reserved/mode bits the driver deliberately does not own.
    es8311_config_sample() keeps `REG07 & 0xC0`, and REG07 is the register
    Espressif's own header calls **"tri-state, lrck divider"**. We write 0x00
    over it. Same story for `REG06 & 0xE0`. Every masked write in the vendor's
    open()/set_fs()/start() now carries its mask, and a register dump taken
    *before* the table runs records what the power-on defaults actually were —
    which is the measurement that would have caught this class of bug at the
    start.

Kept per the project's artifacts rule.
"""
import io
import sys


def edit(path, subs):
    with io.open(path, encoding='utf-8', newline='') as f:
        src = f.read()
    for old, new in subs:
        if new in src:
            print('  (already applied) %r' % old[:50])
            continue
        n = src.count(old)
        if n != 1:
            sys.exit('%s: anchor count %d: %r' % (path, n, old[:70]))
        src = src.replace(old, new)
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(src)
    print('patched %s' % path)


DASH = u'—'

# ---------------------------------------------------------------- source_mic
edit('components/audio_io/source_mic.cpp', [(
    u'''                 (int)pin, hi, lo);
    }
}

esp_err_t audio_source_mic_start(void) {
''',
    u'''                 (int)pin, hi, lo);
    }
}

#if OSYNTH_MIC_DUMP_GPIO
/* Do this port's clock pads actually toggle? (S37e)
 *
 * The one question every other instrument here has quietly assumed the answer
 * to. A *master* fills its DMA ring from the peripheral's internal clock, so a
 * starve counter that stays at zero says the engine is running and says nothing
 * at all about whether one pad is connected to anything. The GPIO dump above is
 * the same kind of evidence one layer out: it proves the matrix routed the
 * signals, not that the pins carry them.
 *
 * And on this board that gap is not theoretical. GPIO45/46/47 read a static
 * 0.6 V as outputs with nothing attached, held by something on the carrier that
 * no header listing mentioned, and it cost a week of looking at converters.
 *
 * So: enable the pad's *input* next to the peripheral's output {D}
 * gpio_input_enable() sets the IE bit and touches neither the output enable nor
 * the matrix {D} and read the pin a few thousand times. MCLK at 12.288 MHz, BCLK
 * at 3.072 MHz and WS at 48 kHz are all fast enough relative to this loop that
 * a live pad returns both levels within a few hundred samples. One level for
 * the whole window means the pad is not switching, and no register in any codec
 * can help that.
 *
 * DIN is in the list too, as a second opinion on the `raw` field in the
 * heartbeat: same reading, arrived at without the I2S receiver in the path. */
void probe_clock_pins(void) {
    struct Probe {
        gpio_num_t pin;
        const char* name;
    };
    const Probe kProbes[] = {
        {OSYNTH_MIC_BCLK, "bclk"},
        {OSYNTH_MIC_WS, "ws"},
        {OSYNTH_MIC_MCLK, "mclk"},
        {OSYNTH_MIC_DIN, "din"},
    };
    const int kSamples = 4096;
    for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        const gpio_num_t pin = kProbes[i].pin;
        if ((int)pin < 0) continue;
        if (gpio_input_enable(pin) != ESP_OK) continue;
        int ones = 0;
        for (int n = 0; n < kSamples; ++n) ones += gpio_get_level(pin);
        if (ones == 0 || ones == kSamples) {
            ESP_LOGW(TAG,
                     "pad probe: %s GPIO%d is STUCK at %d over %d samples {D} "
                     "this pad is not switching, so nothing on the other end of "
                     "it is being clocked",
                     kProbes[i].name, (int)pin, ones ? 1 : 0, kSamples);
        } else {
            ESP_LOGI(TAG, "pad probe: %s GPIO%d toggles (high %d%% of %d)",
                     kProbes[i].name, (int)pin, ones * 100 / kSamples,
                     kSamples);
        }
    }
}
#endif

esp_err_t audio_source_mic_start(void) {
'''.replace('{D}', DASH)), (
    u'''    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
#endif
''',
    u'''    (void)gpio_dump_io_configuration(
        stdout, pin_mask(OSYNTH_MIC_DIN) | pin_mask(OSYNTH_MIC_BCLK) |
                    pin_mask(OSYNTH_MIC_WS) | pin_mask(OSYNTH_MIC_MCLK));
    probe_clock_pins();
#endif
''')])

# --------------------------------------------------------------- codec_es8311
CODEC = 'components/codec/codec_es8311.cpp'

edit(CODEC, [
    # -- masked writes -------------------------------------------------------
    (u'''esp_err_t wr(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), kTimeoutMs);
}
''',
     u'''/* One register write, with `keep` naming the bits to carry over from whatever
 * the chip currently holds (S37e).
 *
 * keep == 0 is the plain absolute write this file was built on, and stays the
 * common case. The masked form exists because "absolute writes, no
 * read-modify-write" turned out to conflate two different reasons the vendor
 * reads first, and only one of them was the one being rejected:
 *
 *   - *Unknown state.* The vendor supports being called on a chip someone else
 *     has touched. This runs once from reset and does not need that, which is
 *     the argument the file header makes and it is still right.
 *   - *Bits that are not the driver's to set.* es8311_config_sample() keeps
 *     `REG07 & 0xC0` and `REG06 & 0xE0` and never writes either field {D} not
 *     because their values are unknown, but because they are reserved or
 *     mode-carrying and the reset default is the correct one. Writing a whole
 *     register clears them, and REG07 is the one Espressif's own header calls
 *     "tri-state, lrck divider". A tri-stated ASDOUT and a disconnected ASDOUT
 *     read identically: full blocks, no starves, every sample zero.
 *
 * So the masks below are transcribed from the vendor alongside the values, and
 * the reset-state dump in codec_mic_init() records what they preserved. */
esp_err_t wr(uint8_t reg, uint8_t val, uint8_t keep = 0) {
    if (keep != 0) {
        uint8_t cur = 0;
        const esp_err_t rerr = i2c_master_transmit_receive(s_dev, &reg, 1, &cur,
                                                           1, kTimeoutMs);
        if (rerr != ESP_OK) return rerr;
        val = (uint8_t)((cur & keep) | (val & (uint8_t)~keep));
    }
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), kTimeoutMs);
}
'''.replace('{D}', DASH)),

    # -- dump_regs takes a label --------------------------------------------
    (u'''void dump_regs(void) {''', u'''void dump_regs(const char* when) {'''),
    (u'''        int n = snprintf(line, sizeof(line), "regs %02x:", base);''',
     u'''        int n = snprintf(line, sizeof(line), "%s regs %02x:", when, base);'''),
    (u'''    ESP_LOGI(TAG, "regs 44: %02x %02x", v44, v45);''',
     u'''    ESP_LOGI(TAG, "%s regs 44: %02x %02x", when, v44, v45);'''),
    (u'''    ESP_LOGI(TAG, "chip id %02x%02x ver %02x (expect 8311)", id1, id2, ver);''',
     u'''    ESP_LOGI(TAG, "%s chip id %02x%02x ver %02x (expect 8311)", when, id1,
             id2, ver);'''),

    # -- the struct gains a mask --------------------------------------------
    (u'''struct RegVal {
    uint8_t reg;
    uint8_t val;
};
''',
     u'''struct RegVal {
    uint8_t reg;
    uint8_t val;
    /* Bits to carry over from the chip's current value; 0 for a plain absolute
     * write, which is most of them. Transcribed from the mask in the vendor
     * function each line came from {D} see wr() for why those masks are not the
     * same thing as tolerating an unknown state. */
    uint8_t keep;
};
'''.replace('{D}', DASH)),

    # -- open(): REG06 -------------------------------------------------------
    (u'''    {REG_CLK_MGR1, kClkMgr1}, /* mclk from the pin, all enables on */
    {REG_CLK_MGR6, kClkMgr6}, /* sclk not inverted */
''',
     u'''    {REG_CLK_MGR1, kClkMgr1}, /* mclk from the pin, all enables on */
    /* Clear bit 5 (sclk not inverted) and *only* bit 5. This is the vendor's
     * first touch of REG06, and it leaves bits 7:6 at their power-on value —
     * which the absolute 0x03 that used to be here overwrote with zeros. */
    {REG_CLK_MGR6, 0x00, 0xDF},
'''),

    # -- set_fs(): the serial port and the coefficient writes ---------------
    (u'''    /* ================= es8311_set_fs() ================= */
    /* 16-bit words, I2S format, both directions. */
    {REG_SDPIN, kSdpI2s},
    {REG_SDPOUT, kSdpI2s},
    /* config_sample() for 12.288 MHz MCLK at 48 kHz, in its own order. */
    {REG_CLK_MGR2, kClkMgr2},
    {REG_CLK_MGR5, kClkMgr5},
    {REG_CLK_MGR3, kClkMgr3},
    {REG_CLK_MGR4, kClkMgr4},
    {REG_CLK_MGR7, kClkMgr7},
    {REG_CLK_MGR8, kClkMgr8},
    {REG_CLK_MGR6, kClkMgr6},
''',
     u'''    /* ================= es8311_set_fs() ================= */
    /* Word length and I2S format, both directions, with bits 7:5 kept: the
     * vendor reaches this value through two read-modify-writes
     * (es8311_set_bits_per_sample() then es8311_config_fmt()), and neither of
     * them owns the top three bits. */
    {REG_SDPIN, kSdpI2s, 0xE0},
    {REG_SDPOUT, kSdpI2s, 0xE0},
    /* config_sample() for 12.288 MHz MCLK at 48 kHz, in its own order and with
     * its own masks. The two that matter are REG07 (bits 7:6, which the
     * vendor's header names *tri-state*) and REG06 (bits 7:5); the rest are
     * transcribed for the same reason, which is that a mask in a reference
     * driver is a statement about ownership and not about caution. */
    {REG_CLK_MGR2, kClkMgr2, 0x07},
    {REG_CLK_MGR5, kClkMgr5},
    {REG_CLK_MGR3, kClkMgr3, 0x80},
    {REG_CLK_MGR4, kClkMgr4, 0x80},
    {REG_CLK_MGR7, kClkMgr7, 0xC0},
    {REG_CLK_MGR8, kClkMgr8},
    {REG_CLK_MGR6, kClkMgr6, 0xE0},
'''),

    # -- start(): the two mute-clearing writes -------------------------------
    (u'''    {REG_RESET, kResetRun},
    {REG_CLK_MGR1, kClkMgr1},
    {REG_SDPIN, kSdpI2s},
    /* bit 6 clear is what takes the ADC's serial output out of mute — the
     * vendor's `adc_iface &= ~BITS(6)` for ADC work modes. 0x0C has it clear
     * already, and it is written here rather than assumed. */
    {REG_SDPOUT, kSdpI2s},
''',
     u'''    {REG_RESET, kResetRun},
    {REG_CLK_MGR1, kClkMgr1},
    /* Clear bit 6 and nothing else, which is literally all the vendor does
     * here: `dac_iface &= 0xBF` / `adc_iface &= ~BITS(6)`, the write that takes
     * the serial outputs out of mute. Re-writing the whole word here (which is
     * what naming kSdpI2s did) would undo the masked set_fs() values above. */
    {REG_SDPIN, 0x00, 0xBF},
    {REG_SDPOUT, 0x00, 0xBF},
'''),

    # -- the write loop passes the mask --------------------------------------
    (u'''        err = wr(kInit[i].reg, kInit[i].val);''',
     u'''        err = wr(kInit[i].reg, kInit[i].val, kInit[i].keep);'''),

    # -- reset-state dump before the table runs ------------------------------
    (u'''    const size_t n = sizeof(kInit) / sizeof(kInit[0]);
    for (size_t i = 0; i < n; ++i) {''',
     u'''    /* The register file *before* anything is written to it.
     *
     * This is the measurement that was missing while the "init" dump was being
     * compared against the vendor line by line: matching the vendor's end state
     * says nothing about which bits got there by being written and which were
     * simply the reset defaults the vendor never touches. Diff the two dumps
     * and every masked write above is accounted for — and any *other* register
     * that changed without appearing in kInit is a finding on its own.
     *
     * It also proves the chip answers before it has been configured, which is
     * one more thing that no longer has to be inferred. */
    dump_regs("reset");

    const size_t n = sizeof(kInit) / sizeof(kInit[0]);
    for (size_t i = 0; i < n; ++i) {'''),

    (u'''    dump_regs();
    return ESP_OK;''',
     u'''    dump_regs("init");
    return ESP_OK;'''),
])

print('done')
