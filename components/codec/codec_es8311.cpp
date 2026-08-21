/*
 * osynth — ES8311 microphone codec, ADC only (Session 37c).
 *
 * The ESP32-P4 carrier's on-board microphone is an *analogue* element wired to
 * an ES8311's preamp, not a digital MEMS part on an I2S bus. Its board config
 * says so outright — `microphone_type: analog`, `mic_gain: 24DB` — and that is
 * what makes this file unavoidable: the analogue signal terminates on the
 * codec's pins and reaches nothing else, so the only path to it is that chip's
 * own ADC, and the chip converts nothing until its registers are written.
 * Driving the four I2S pins at it (DIN 48, BCLK 12, WS 10, MCLK 13) is
 * necessary and not sufficient. DIN read 11 until S37h, from an `i2s_audio:`
 * block attributed to the wrong board; GPIO11 on this one is the speaker
 * amplifier's enable, which is why it measured as a hard zero that nothing in
 * this file could ever have moved.
 *
 * **ADC only, and that is the whole point.** The DAC half is powered down here
 * and never touched again. Audio output stays entirely with the ES8388 on the
 * main port: different chip, different I2C address (0x18 against 0x10/0x11),
 * different I2S controller. Nothing about the output path changes because this
 * file ran — the ES8311 is a microphone preamp with a serial port as far as
 * osynth is concerned, and never drives a speaker pin.
 *
 * Register values come from Espressif's own esp_codec_dev driver (device/es8311)
 * rather than from reasoning about the datasheet. The clock coefficients in
 * particular are a table indexed by the MCLK/LRCK ratio, and the one row this
 * build needs — 12.288 MHz MCLK at 48 kHz — is transcribed below with the
 * arithmetic left where the vendor put it.
 *
 * Two deliberate departures from that driver:
 *
 *  - **Absolute writes, no read-modify-write.** The vendor reads a register,
 *    masks, and writes it back, because it supports being called on a chip in
 *    an unknown state. This runs once, from reset, on a chip nobody else
 *    touches, so every value below is the whole register. That matches
 *    codec_es8388.cpp next door — one transfer per register, no retries, no
 *    read-back — and the reasoning in codec.h for why that is the right shape
 *    for a bring-up fault to be visible through.
 *  - **The word length is derived from the port's slot width**, never set
 *    independently, and the codec runs 32-bit slots here because that is what
 *    its own clock coefficients assume. This started at 16-bit — the vendor's
 *    32-bit write is `adc_iface |= 0x10`, which only reaches the datasheet's
 *    100 if bits 3:2 were already clear and lands on 111 from the 0x0C reset
 *    default, so an absolute 0x0C looked like the safe choice. It was safe and
 *    it was wrong: the coefficient row's bclk_div is 4, meaning the codec was
 *    configured expecting BCLK = MCLK/4 = 64 x fs, and 16-bit slots feed it
 *    32 x fs. Half the bit clock its own configuration was computed for, no
 *    error anywhere, silence as the only symptom. Knowing the field encoding
 *    (000 = 24-bit, 011 = 16-bit, 100 = 32-bit) removes the reason to avoid
 *    32-bit, and kSdpI2s now follows SYNTH_MIC_SLOT_BITS so the two cannot
 *    drift apart again.
 *
 * Ordering: called *after* audio_io_start(), so the mic port is already
 * driving MCLK on GPIO13 when the clock manager below is written. That is not
 * where it started — it sat beside codec_init(), which on the P4 puts it
 * before the port because of OSYNTH_CODEC_INIT_BEFORE_I2S, and a codec whose
 * clock manager is configured from a pin carrying no clock reaches its
 * operating point afterwards rather than under the writes that set it. See
 * OSYNTH_ES8311_INIT_BEFORE_I2S in codec.h for the switch back, and for why
 * that is a different question from the ES8388's identically-named one.
 */
#include "codec.h"

#include <stdint.h>
#include <stdio.h> /* snprintf(), for the register dump */

#include "esp_log.h"

#include "codec_priv.h"
#include "synth_config.h"

#if SYNTH_ENABLE_CODEC_ES8311

static const char* TAG = "es8311";

namespace {

constexpr uint16_t kAddr = 0x18; /* CE tied low; 0x19 is the other strap */
constexpr int kTimeoutMs = 100;

i2c_master_dev_handle_t s_dev = nullptr;
bool s_up = false;

/* ---- registers (esp_codec_dev device/es8311/es8311_reg.h) ---- */
enum : uint8_t {
    REG_RESET       = 0x00,
    REG_CLK_MGR1    = 0x01, /* clock source for internal mclk, core enables */
    REG_CLK_MGR2    = 0x02, /* mclk pre-divider and pre-multiplier          */
    REG_CLK_MGR3    = 0x03, /* adc fs mode + adc osr                        */
    REG_CLK_MGR4    = 0x04, /* dac osr                                      */
    REG_CLK_MGR5    = 0x05, /* adc and dac dividers                         */
    REG_CLK_MGR6    = 0x06, /* bclk invert + bclk divider                   */
    REG_CLK_MGR7    = 0x07, /* tri-state, lrck divider high                 */
    REG_CLK_MGR8    = 0x08, /* lrck divider low                             */
    REG_SDPIN       = 0x09, /* DAC serial port format/width                 */
    REG_SDPOUT      = 0x0A, /* ADC serial port format/width                 */
    REG_SYS_0B      = 0x0B, /* system                                       */
    REG_SYS_0C      = 0x0C, /* system                                       */
    REG_SYS_0D      = 0x0D, /* power up/down                                */
    REG_SYS_0E      = 0x0E, /* analogue power, ADC modulator                */
    REG_SYS_10      = 0x10, /* VMID / bias, analogue reference               */
    REG_SYS_11      = 0x11, /* analogue reference, charge pump              */
    REG_SYS_12      = 0x12, /* DAC enable                                   */
    REG_SYS_13      = 0x13,
    REG_SYS_14      = 0x14, /* DMIC select, analogue PGA select             */
    REG_ADC_15      = 0x15, /* adc ramp rate                                */
    REG_ADC_16      = 0x16, /* PGA gain                                     */
    REG_ADC_17      = 0x17, /* adc digital volume                           */
    REG_ADC_1B      = 0x1B, /* alc automute, hpf stage 1                    */
    REG_ADC_1C      = 0x1C, /* equalizer, hpf stage 2                       */
    REG_DAC_37      = 0x37, /* dac ramp rate                                */
    REG_GPIO_44     = 0x44, /* dac-to-adc loopback (test)                   */
    REG_GP_45       = 0x45,
};

/* PGA gain: 0..7 is 0/6/12/18/24/30/36/42 dB, straight into REG_ADC_16. The
 * board asks for 24 dB, which is index 4 — an analogue electret needs most of
 * that before the converter sees anything worth converting. Kconfig carries it
 * because it is the one value here that depends on the microphone rather than
 * on the chip. */
constexpr uint8_t kMicGainStep = CONFIG_OSYNTH_ES8311_MIC_GAIN;

/* ---- keep the DAC half alive as the ADC's reference ----
 *
 * 1 configures this chip the way the board's own firmware does: DAC enabled
 * (REG12 = 0x00) and REG44 = 0x58, which the vendor's header calls the
 * *internal reference signal* and describes as "right channel filled with dac
 * output". 0 is the strictly-ADC-only arrangement — DAC powered down, right
 * channel left empty — which is what this file did first.
 *
 * ADC-only is the tidier configuration and it is not what is known to work on
 * this hardware. The board runs its ES8311 in both directions at once (its
 * ESPHome config gives the same codec to a speaker on GPIO9 and a microphone
 * on GPIO48), so the only arrangement anyone has demonstrated on this part has
 * the DAC powered and that reference in place. Espressif's own driver leaves
 * REG12 at its reset default for ADC-only work modes, so "ADC-only" there is
 * an untested corner rather than a supported mode.
 *
 * Costs nothing to leave on. osynth drives no data at this codec's DOUT, so
 * its DAC converts silence into an amplifier this firmware never enables — the
 * board's speaker PA is a separate GPIO (11 on this board) that nothing here
 * touches. Audio output remains entirely the ES8388's on the other port. */
#define OSYNTH_ES8311_DAC_REF 1

#if OSYNTH_ES8311_DAC_REF
constexpr uint8_t kReg12 = 0x00; /* DAC enabled */
constexpr uint8_t kReg44 = 0x58; /* ADCL + DACR: the internal reference */
#else
constexpr uint8_t kReg12 = 0x02; /* DAC powered down */
constexpr uint8_t kReg44 = 0x08; /* right channel left empty */
#endif

/* The one coefficient row this build needs, from esp_codec_dev's coeff_div[]:
 *
 *   { mclk 12288000, rate 48000, pre_div 1, pre_multi 1, adc_div 1, dac_div 1,
 *     fs_mode 0, lrck_h 0x00, lrck_l 0xff, bclk_div 4, adc_osr 0x10,
 *     dac_osr 0x10 }
 *
 * MCLK is 256 x fs because that is what the mic port drives (I2S_MCLK_MULTIPLE
 * _256, source_mic.cpp), so this row is selected by construction rather than
 * looked up — there is exactly one rate and one MCLK on this path. Written out
 * as the register values the vendor's arithmetic produces from it, with that
 * arithmetic named beside each one so the transcription can be checked.
 *
 * lrck_h/lrck_l describe the *master* case, where the codec would divide MCLK
 * to make LRCK itself; here it is the slave and LRCK arrives from the P4, so
 * they are set for consistency with the vendor's sequence and nothing depends
 * on them.
 *
 * **bclk_div is not in that category, and treating it as though it were is
 * what cost this file three rounds.** It says the codec's clocking was
 * computed for BCLK = MCLK/4 = 3.072 MHz = 64 x fs, and that is a statement
 * about the wire, not merely about who divides. Feed it 32 x fs from 16-bit
 * slots and every register still reads back correct, the port still fills its
 * DMA at the right rate, and the ADC still emits nothing. Which is why the
 * word length above is derived from SYNTH_MIC_SLOT_BITS rather than chosen:
 * the port and this row have to agree about how many bit clocks a frame is. */
constexpr uint8_t kClkMgr2 = 0x00; /* (pre_div-1)<<5 | pre_multi_code<<3 = 0 */
constexpr uint8_t kClkMgr5 = 0x00; /* (adc_div-1)<<4 | (dac_div-1) = 0       */
constexpr uint8_t kClkMgr3 = 0x10; /* fs_mode<<6 | adc_osr(0x10)             */
constexpr uint8_t kClkMgr4 = 0x10; /* dac_osr(0x10)                          */
constexpr uint8_t kClkMgr7 = 0x00; /* lrck_h                                 */
constexpr uint8_t kClkMgr8 = 0xFF; /* lrck_l                                 */
constexpr uint8_t kClkMgr6 = 0x03; /* sclk not inverted | (bclk_div 4 - 1)   */

/* REG_CLK_MGR1 = 0x3F: internal mclk sourced from the MCLK *pin* (bit 7 clear,
 * which is what `use_mclk` means), not inverted (bit 6 clear), and every clock
 * enable in the low six bits on. Turning this to 0xBF would run the core off
 * BCLK instead — wrong here, since the board routes a real MCLK to GPIO13 and
 * that is why that pin exists at all. */
constexpr uint8_t kClkMgr1 = 0x3F;

/* REG_RESET = 0x80: out of reset, slave. Bit 6 is master mode and normally
 * stays clear — the P4's mic port is the master on this bus (it drives BCLK,
 * WS and MCLK), so the codec must not also try to.
 *
 * SYNTH_MIC_CODEC_MASTER_TEST (synth_config.h) inverts that, and the two ends
 * move together: source_mic.cpp stops driving BCLK and WS in the same build.
 * See that switch for what the arrangement is for. */
#if SYNTH_MIC_CODEC_MASTER_TEST
constexpr uint8_t kResetRun = 0xC0;
#else
constexpr uint8_t kResetRun = 0x80;
#endif

/* Serial port: I2S format (bits 1:0 = 00) and the word length in bits 4:2,
 * where the datasheet's encoding is 000 = 24-bit, 011 = 16-bit, 100 = 32-bit.
 *
 * **Derived from the port's slot width, never set independently.** The two have
 * to agree and nothing at runtime would notice if they did not: a codec told
 * 16-bit words while the port frames 32-bit slots emits into the wrong half of
 * every frame, which reads as silence or noise depending on where the bits
 * land, and looks exactly like every other fault this port has produced.
 *
 * 32-bit is what the clock coefficients below actually want. Their bclk_div is
 * 4, i.e. BCLK = MCLK/4 = 3.072 MHz = 64 x fs, and only a 32-bit slot pair
 * gives that. Driving 16-bit slots against that row means feeding the codec
 * half the bit clock its own configuration was computed for — which is what
 * this file did for three rounds, and the one mismatch left after the register
 * file matched the vendor byte for byte.
 *
 * Written as one absolute value per width rather than the vendor's
 * `iface |= 0x10`, which only reaches 100 if bits 3:2 were already clear and
 * lands on 111 — not a width at all — from the 0x0C reset default. That
 * fragility is why 16-bit was picked first; knowing the field encoding removes
 * the reason. */
#if SYNTH_MIC_SLOT_BITS == 32
constexpr uint8_t kSdpI2s = 0x10; /* bits 4:2 = 100, 32-bit */
#else
constexpr uint8_t kSdpI2s = 0x0C; /* bits 4:2 = 011, 16-bit */
#endif

/* One register write, with `keep` naming the bits to carry over from whatever
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
 *     `REG07 & 0xC0` and `REG06 & 0xE0` and never writes either field — not
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

/* Read the register file back and print it, exactly as codec_es8388.cpp does.
 *
 * That dump earned its place next door and earns it here for the same reason:
 * a table of writes that all returned ESP_OK proves the *bus* worked, and says
 * nothing about whether the chip kept the values. A codec that NACKs is loud;
 * one that accepts a write and reverts it, or that is held in reset and
 * answers anyway, is silent — and produces exactly what this port is
 * producing, full blocks of zeros from an init that logged success.
 *
 * Read failures are printed as -- rather than abandoning the dump: a chip that
 * answers some registers and not others is itself the finding. */
void dump_regs(const char* when) {
    /* 0x00-0x1C is clocking, serial format, power and the whole ADC path;
     * 0x44-0x45 is the output routing that decides what reaches SDOUT. The
     * DAC block between them is powered down and would only be noise here. */
    static const uint8_t kFirst = 0x00, kLast = 0x1C;
    char line[96];
    for (uint8_t base = kFirst; base <= kLast; base += 8) {
        int n = snprintf(line, sizeof(line), "%s regs %02x:", when, base);
        for (uint8_t i = 0; i < 8 && (base + i) <= kLast; ++i) {
            const uint8_t reg = (uint8_t)(base + i);
            uint8_t val = 0;
            const esp_err_t err = i2c_master_transmit_receive(
                s_dev, &reg, 1, &val, 1, kTimeoutMs);
            n += snprintf(line + n, sizeof(line) - (size_t)n,
                          err == ESP_OK ? " %02x" : " --", val);
        }
        ESP_LOGI(TAG, "%s", line);
    }
    uint8_t v44 = 0, v45 = 0;
    uint8_t r = 0x44;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &v44, 1, kTimeoutMs);
    r = 0x45;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &v45, 1, kTimeoutMs);
    ESP_LOGI(TAG, "%s regs 44: %02x %02x", when, v44, v45);

    /* Chip ID and version, which is the register dump's own credential.
     *
     * Everything the dump above is used for — "the values match the vendor's
     * driver byte for byte" — rests on these being real reads of a real
     * ES8311, and nothing had checked that. 83 11 is the part; anything else,
     * and especially 00 00 or ff ff, means the reads are an artefact and every
     * conclusion drawn from them has to go. The version byte is worth having
     * beside it because the analogue front end differs across revisions. */
    uint8_t id1 = 0, id2 = 0, ver = 0;
    r = 0xFD;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &id1, 1, kTimeoutMs);
    r = 0xFE;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &id2, 1, kTimeoutMs);
    r = 0xFF;
    (void)i2c_master_transmit_receive(s_dev, &r, 1, &ver, 1, kTimeoutMs);
    ESP_LOGI(TAG, "%s chip id %02x%02x ver %02x (expect 8311)", when, id1,
             id2, ver);
}

/* The init sequence, in the vendor's order.
 *
 * That order is load-bearing in two places and arbitrary everywhere else: the
 * reset and clock-source writes have to precede the coefficient writes (the
 * clock manager latches on a running core), and REG_SYS_0D powering the
 * analogue block up has to come *after* the PGA and input selection, or the
 * stage reaches its operating point before the registers that define it — the
 * same ordering trap the ES8388's line input hit (codec.h). */
struct RegVal {
    uint8_t reg;
    uint8_t val;
    /* Bits to carry over from the chip's current value; 0 for a plain absolute
     * write, which is most of them. Transcribed from the mask in the vendor
     * function each line came from — see wr() for why those masks are not the
     * same thing as tolerating an unknown state. */
    uint8_t keep;
};

const RegVal kInit[] = {
    /* ================= es8311_open() ================= */
    /* Power the analogue side *down* first. This is where the second attempt
     * went wrong: it opened with REG00 = 0x1F/0x00, a digital reset the
     * vendor never performs here, and left REG0D wherever it was. The chip
     * then held every subsequent write — the register dump proved it — and
     * converted nothing, because the analogue block was never taken through
     * the down-then-configure-then-up cycle its power-up actually needs.
     * 0xFA is the same value es8311_suspend() parks it at. */
    {REG_SYS_0D, 0xFA, 0},

    /* Twice, and not a typo — the vendor's own comment is "due to occasional
     * failures during the first I2C write with the ES8311 chip". Kept because
     * this is exactly the sort of thing that is cheap to honour and expensive
     * to rediscover.
     *
     * 0x08 is also the value this register keeps, and the vendor's header says
     * exactly what the choice is: with `no_dac_ref` false it writes 0x58 and
     * "right channel filled with dac output", with it true 0x08 and "right
     * channel leave empty". **Both leave the ADC on the left channel** — the
     * difference is only what the unused half of the frame carries. So 0x08 is
     * right for a capture whose DAC is powered down, and this register was
     * never a candidate for the silence, which is worth recording because it
     * looked like one for two rounds. */
    {REG_GPIO_44, 0x08, 0},
    {REG_GPIO_44, 0x08, 0},

    /* Clock manager and analogue reference, all while still powered down. */
    {REG_CLK_MGR1, 0x30, 0},
    {REG_CLK_MGR2, kClkMgr2, 0},
    {REG_CLK_MGR3, kClkMgr3, 0},
    /* 0x20 | gain, not the gain alone. The vendor writes 0x24 here and its
     * separate set_mic_gain() later writes the bare 0..7 step, clobbering
     * bit 5 — so which value the chip ends on depends on whether the caller
     * ever asks for a gain. Keeping bit 5 means this build lands on the
     * vendor's own initialised value (0x24 at 24 dB) rather than on the
     * result of an optional call, which is the safer of the two to copy. */
    {REG_ADC_16, (uint8_t)(0x20 | kMicGainStep), 0},
    {REG_CLK_MGR4, kClkMgr4, 0},
    {REG_CLK_MGR5, kClkMgr5, 0},
    {REG_SYS_0B, 0x00, 0},
    {REG_SYS_0C, 0x00, 0},
    {REG_SYS_10, 0x1F, 0}, /* VMID and bias up */
    {REG_SYS_11, 0x7F, 0},

    /* Out of reset, slave (bit 6 clear — the P4's mic port is the master). */
    {REG_RESET, kResetRun, 0},
    {REG_CLK_MGR1, kClkMgr1, 0}, /* mclk from the pin, all enables on */
    /* Clear bit 5 (sclk not inverted) and *only* bit 5. This is the vendor's
     * first touch of REG06, and it leaves bits 7:6 at their power-on value —
     * which the absolute 0x03 that used to be here overwrote with zeros. */
    {REG_CLK_MGR6, 0x00, 0xDF},

    {REG_GPIO_44, kReg44, 0}, /* the mode-carrying write; see OSYNTH_ES8311_DAC_REF */
    {REG_SYS_13, 0x10, 0},
    {REG_ADC_1B, 0x0A, 0}, /* alc automute + HPF stage 1 */
    {REG_ADC_1C, 0x6A, 0}, /* equalizer off, HPF stage 2 */

    /* ================= es8311_set_fs() ================= */
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
    {REG_CLK_MGR5, kClkMgr5, 0},
    {REG_CLK_MGR3, kClkMgr3, 0x80},
    {REG_CLK_MGR4, kClkMgr4, 0x80},
    {REG_CLK_MGR7, kClkMgr7, 0xC0},
    {REG_CLK_MGR8, kClkMgr8, 0},
    {REG_CLK_MGR6, kClkMgr6, 0xE0},

    /* ================= es8311_start() ================= */
    {REG_RESET, kResetRun, 0},
    {REG_CLK_MGR1, kClkMgr1, 0},
    /* Clear bit 6 and nothing else, which is literally all the vendor does
     * here: `dac_iface &= 0xBF` / `adc_iface &= ~BITS(6)`, the write that takes
     * the serial outputs out of mute. Re-writing the whole word here (which is
     * what naming kSdpI2s did) would undo the masked set_fs() values above. */
    {REG_SDPIN, 0x00, 0xBF},
    {REG_SDPOUT, 0x00, 0xBF},
    {REG_ADC_17, 0xBF, 0}, /* ADC digital volume */
    {REG_SYS_0E, 0x02, 0}, /* analogue up, ADC modulator enabled */
    /* The output belongs to the ES8388 on the other port. Left enabled this
     * chip's DAC drives the board's own output pins from GPIO9, which nothing
     * here writes — so this is correctness, not tidiness. The vendor writes
     * 0x00 here for DAC work modes; 0x02 is its powered-down value. */
    {REG_SYS_12, kReg12, 0},
    /* Analogue PGA path selected, DMIC input off (bit 6 clear) — this mic is
     * analogue, which is the whole reason this file exists. */
    {REG_SYS_14, 0x1A, 0},
    {REG_SYS_0D, 0x01, 0}, /* analogue up */
    {REG_ADC_15, 0x40, 0}, /* adc ramp rate */
    {REG_DAC_37, 0x08, 0},
    {REG_GP_45, 0x00, 0},
};

} // namespace

esp_err_t codec_mic_init(void) {
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = codec_i2c_bus(&bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no control bus (%s); on-board mic stays silent",
                 esp_err_to_name(err));
        return err;
    }

    if (i2c_master_probe(bus, kAddr, 100) != ESP_OK) {
        /* Not an error worth failing a boot over, and worth saying precisely:
         * this address is the board's own codec, so a board that does not
         * answer here is a board without one, not a fault. */
        ESP_LOGW(TAG, "no ES8311 at 0x%02x — no on-board microphone here",
                 (unsigned)kAddr);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kAddr;
    dev_cfg.scl_speed_hz = 100000;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not add device 0x%02x: %s", (unsigned)kAddr,
                 esp_err_to_name(err));
        s_dev = nullptr;
        return err;
    }

    /* The register file *before* anything is written to it.
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
    /* **Warm boots do not reset this chip.** It keeps its rail across a P4
     * reset, so on anything but a cold start this dump is the previous boot's
     * end state, not a power-on default — which is exactly what it came back
     * as the first time it ran, byte for byte identical to the `init` dump
     * below. Two things follow, and neither is fixable in code: the masked
     * writes above cannot be seen to preserve anything until the board has been
     * fully unpowered, and every earlier "tried it, still silent" result was
     * taken on a chip carrying whatever the attempt before it left behind. */
    dump_regs("reset");

    const size_t n = sizeof(kInit) / sizeof(kInit[0]);
    for (size_t i = 0; i < n; ++i) {
        err = wr(kInit[i].reg, kInit[i].val, kInit[i].keep);
        if (err != ESP_OK) {
            /* Same philosophy as the ES8388 next door: report where it stopped
             * and stop. A codec that NACKs mid-table is a control-bus fault,
             * and a driver that retries around it makes the next symptom
             * harder to read rather than the board work. */
            ESP_LOGE(TAG,
                     "register 0x%02x = 0x%02x failed at step %u/%u (%s) — "
                     "on-board mic will read silence",
                     kInit[i].reg, kInit[i].val, (unsigned)(i + 1), (unsigned)n,
                     esp_err_to_name(err));
            return err;
        }
    }

    s_up = true;
    ESP_LOGI(TAG,
             "ES8311 at 0x%02x: analogue mic, PGA %d dB, %d-bit I2S slave off "
             "MCLK, DAC %s (audio output stays on the ES8388 either way)",
             (unsigned)kAddr, kMicGainStep * 6, SYNTH_MIC_SLOT_BITS,
             OSYNTH_ES8311_DAC_REF ? "on as the ADC's reference"
                                   : "powered down");
    dump_regs("init");
    return ESP_OK;
}

const char* codec_mic_name(void) { return s_up ? "es8311" : "es8311?"; }

#else /* no on-board mic codec in this build */

esp_err_t codec_mic_init(void) { return ESP_OK; }
const char* codec_mic_name(void) { return "none"; }

#endif /* SYNTH_ENABLE_CODEC_ES8311 */
