/*
 * osynth — ES8388 codec bring-up over I2C (Session 31b). See codec.h for why
 * this component exists and when it runs.
 *
 * The register values below are not invented: they follow Espressif's own
 * ES8388 driver (esp-adf, components/audio_hal/driver/es8388), which is the
 * sequence every shipping ES8388 board runs, with three deliberate
 * departures — each marked at its line. Register names match Espressif's
 * es8388_reg.h so the two can be read side by side.
 *
 * What this does *not* do is touch the audio path. Both of the codec's
 * digital volumes are parked at 0 dB and its analogue bypass is left
 * disconnected, so the only thing between the render chain and the output
 * pins is a converter. Gain lives where it always lived: master.volume
 * before the sink, in.gain after the capture. A signal chain acquires a
 * mystery 6 dB by having two stages that both think they are the level
 * control.
 */
#include "codec.h"

#if SYNTH_ENABLE_CODEC_ES8388

#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "synth_params.h"

static const char* TAG = "es8388";

#define OSYNTH_ES8388_SDA ((gpio_num_t)CONFIG_OSYNTH_ES8388_I2C_SDA_GPIO)
#define OSYNTH_ES8388_SCL ((gpio_num_t)CONFIG_OSYNTH_ES8388_I2C_SCL_GPIO)

namespace {

/* 7-bit addresses; the chip's CE pin picks one. Boards differ and neither is
 * wrong, so both get probed rather than made into a menuconfig question the
 * user would have to answer with a multimeter. */
constexpr uint16_t kAddrCe0 = 0x10;
constexpr uint16_t kAddrCe1 = 0x11;

enum : uint8_t {
    REG_CONTROL1     = 0x00,
    REG_CONTROL2     = 0x01,
    REG_CHIPPOWER    = 0x02,
    REG_ADCPOWER     = 0x03,
    REG_DACPOWER     = 0x04,
    REG_MASTERMODE   = 0x08,
    REG_ADCCONTROL1  = 0x09, /* PGA gain, both channels */
    REG_ADCCONTROL2  = 0x0a, /* analogue input select */
    REG_ADCCONTROL3  = 0x0b,
    REG_ADCCONTROL4  = 0x0c, /* ADC serial format + word length */
    REG_ADCCONTROL5  = 0x0d, /* ADC MCLK/fs ratio */
    REG_ADCVOL_L     = 0x10,
    REG_ADCVOL_R     = 0x11,
    REG_DACCONTROL1  = 0x17, /* DAC serial format + word length */
    REG_DACCONTROL2  = 0x18, /* DAC MCLK/fs ratio */
    REG_DACCONTROL3  = 0x19, /* DAC mute */
    REG_DACVOL_L     = 0x1a,
    REG_DACVOL_R     = 0x1b,
    REG_DACCONTROL16 = 0x26, /* mixer source select */
    REG_DACCONTROL17 = 0x27, /* left mixer */
    REG_DACCONTROL20 = 0x2a, /* right mixer */
    REG_DACCONTROL21 = 0x2b, /* shared LRCK */
    REG_DACCONTROL23 = 0x2d, /* VROI */
    REG_LOUT1VOL     = 0x2e,
    REG_ROUT1VOL     = 0x2f,
    REG_LOUT2VOL     = 0x30,
    REG_ROUT2VOL     = 0x31,
};

/* ADCCONTROL2: which analogue pins reach the ADC. LINSEL is the top two
 * bits, RINSEL the next two.
 *
 * ADCCONTROL3 goes with it. M5's setADCInput() writes both — 0x00 alongside
 * LIN1/RIN1 and 0x80 alongside LIN2/RIN2 — and its init comment spells the
 * pairs out as "(0x00 0x00)" and "(0x50 0x80)". esp-adf writes ADCCONTROL2
 * alone and leaves ADCCONTROL3 at a constant, which is what this did until
 * S31d: LINE2 selected the pins but never set the bit that goes with them.
 * Nothing here exercised LINE2 once the M144 moved this build to LINE1, but
 * it is the right setting for an A1S or a LyraT and it was quietly wrong.
 *
 * Bit 1 reads back set whatever is written to it, so it is included rather
 * than fought: the init table and a register dump then agree. */
#if defined(CONFIG_OSYNTH_ES8388_IN_LINE1)
constexpr uint8_t kAdcInput = 0x00;    /* LIN1 / RIN1 */
constexpr uint8_t kAdcControl3 = 0x02;
#elif defined(CONFIG_OSYNTH_ES8388_IN_DIFF)
/* Each channel is the difference across its pair rather than one pin against
 * ground. ADCCONTROL3's DS bit picks which pair the difference is taken from
 * and 0 is pair 1, which is what M5 names ADC_INPUT_DIFFERENCE1. This is the
 * correct mode for the M5Stack Module Audio (M144): its jack is wired
 * differentially, and a single-ended selection there recovers exactly half the
 * amplitude on that channel — the 6 dB imbalance that S31d spent a long time
 * mistaking for a summing node and trying to trim. */
constexpr uint8_t kAdcInput = 0xf0;
constexpr uint8_t kAdcControl3 = 0x02;
#else
constexpr uint8_t kAdcInput = 0x50;    /* LIN2 / RIN2 — the line jack on most boards */
constexpr uint8_t kAdcControl3 = 0x82;
#endif

/* DACPOWER (0x04) enable bits: LOUT1 = 0x20, ROUT1 = 0x10, LOUT2 = 0x08,
 * ROUT2 = 0x04 — so a *pair* is OUT1 = 0x30, OUT2 = 0x0c, both = 0x3c.
 *
 * This was wrong until S31d, and wrong in a way that hid: the bits were taken
 * from esp-adf's es_dac_output_t, which names 0x04 LOUT1 and 0x10 ROUT1. Those
 * two do OR to a plausible-looking 0x14, and 0x3c for "everything" is correct
 * either way, so the default OUT_BOTH build worked and nothing ever exercised
 * the other two. It is a channel split, not a pair split: 0x14 is bit 4 and
 * bit 2, which is ROUT1 + ROUT2 — the right-hand driver of *both* pairs, and
 * mono-right on any ES8388 board, not just this one. Selecting OUT1 on an
 * M5Stack M144 is what finally showed it, as audio in the right ear only.
 *
 * The layout above is what the chip actually does, confirmed twice over: it
 * matches M5's own es_dac_output_t (OUT1 = 0x30, OUT2 = 0x0C), and it is the
 * only assignment under which 0x14 gives right-only, which is what the
 * hardware did.
 *
 * kOutVolRegs is the same choice expressed as the volume registers that
 * matter, so out.level writes the pair that is actually driving something and
 * not the pair that is not. That halving is worth having: every register in
 * this list is one more chance for a flaky bus to land half an update, and a
 * half-landed *stereo* update is heard as the image jumping to one side. */
#if defined(CONFIG_OSYNTH_ES8388_OUT1)
constexpr uint8_t kDacOutputs = 0x30;
constexpr const char* kOutputName = "LOUT1/ROUT1";
constexpr uint8_t kOutVolRegs[] = {REG_LOUT1VOL, REG_ROUT1VOL};
#elif defined(CONFIG_OSYNTH_ES8388_OUT2)
constexpr uint8_t kDacOutputs = 0x0c;
constexpr const char* kOutputName = "LOUT2/ROUT2";
constexpr uint8_t kOutVolRegs[] = {REG_LOUT2VOL, REG_ROUT2VOL};
#else
constexpr uint8_t kDacOutputs = 0x3c; /* both pairs */
constexpr const char* kOutputName = "LOUT1/ROUT1 + LOUT2/ROUT2";
constexpr uint8_t kOutVolRegs[] = {REG_LOUT1VOL, REG_ROUT1VOL, REG_LOUT2VOL,
                                   REG_ROUT2VOL};
#endif
constexpr size_t kOutVolCount = sizeof(kOutVolRegs) / sizeof(kOutVolRegs[0]);

/* ADCVOL_L / ADCVOL_R: per-channel ADC digital volume, 0x00 = 0 dB counting
 * down in 0.5 dB steps to 0xc0 (-96 dB). Attenuation only, which is exactly
 * why the input balance is corrected here rather than in the PGA: nothing this
 * writes can clip, and 0.5 dB is fine enough to actually match a pair.
 *
 * The louder channel comes down to meet the quieter one. Costs a little
 * resolution on that side — 6 dB is a bit of the 16-bit word — against a
 * converter with far more range than a line input uses. The PGA alternative
 * raises the quiet channel instead and costs nothing in theory, but its step
 * is 3 dB and over-correcting clips it analogue-side, which sounds metallic
 * and cannot be undone. See OSYNTH_ES8388_ADC_BALANCE. */
constexpr int kAdcBalance = CONFIG_OSYNTH_ES8388_ADC_BALANCE;
constexpr uint8_t kAdcVolL =
    (kAdcBalance < 0) ? (uint8_t)((-kAdcBalance > 192) ? 192 : -kAdcBalance) : 0;
constexpr uint8_t kAdcVolR =
    (kAdcBalance > 0) ? (uint8_t)((kAdcBalance > 192) ? 192 : kAdcBalance) : 0;

/* The ADC is powered down entirely on a playback-only build: it is in the
 * chip either way, and leaving its analogue front end biased for a signal
 * nothing reads is current spent on nothing. 0xff is everything down; 0x09
 * is everything up *except* the microphone bias, which a line input has no
 * use for — see the departure note in kInit. */
#if SYNTH_ENABLE_LINE_IN
constexpr uint8_t kAdcPower = 0x09;
#if defined(CONFIG_OSYNTH_ES8388_IN_LINE1)
constexpr const char* kInputName = "LIN1/RIN1";
#elif defined(CONFIG_OSYNTH_ES8388_IN_DIFF)
constexpr const char* kInputName = "differential";
#else
constexpr const char* kInputName = "LIN2/RIN2";
#endif
#else
constexpr uint8_t kAdcPower = 0xff;
constexpr const char* kInputName = "off (ADC powered down)";
#endif

struct RegWrite {
    uint8_t reg;
    uint8_t val;
};

const RegWrite kInit[] = {
    /* Mute first. Everything below moves power rails around, and the output
     * pins should be quiet while that happens. */
    {REG_DACCONTROL3, 0x04},

    /* Hold the DAC/ADC state machines and the digital engine in reset for the
     * whole of the sequence below, and release them at the very end.
     *
     * This is what the ES8388 User Guide's own example does, and what M5's
     * driver does — 0xff first, everything configured, 0x00 last. Until S31d
     * this wrote 0x00 here instead, following esp-adf, which configures a
     * running chip. Both work; only one of them is the manufacturer's. The
     * control port is unaffected either way, so every write below still lands
     * exactly as before. */
    {REG_CHIPPOWER, 0xff},

    /* Reference buffers up. */
    {REG_CONTROL2, 0x50},

    /* Undocumented registers, verbatim from Espressif's driver, where the
     * comment says they disable the internal DLL to fix 8 kHz playback.
     * Nothing to do with 48 kHz. Kept anyway: three redundant writes are a
     * better bet than being the only board that diverges from the sequence
     * this chip is known to come up under. */
    {0x35, 0xa0},
    {0x37, 0xd0},
    {0x39, 0xd0},

    /* Slave. The ESP32 drives BCLK, WS and MCLK — see sink_i2s.cpp. */
    {REG_MASTERMODE, 0x00},

    /* ---- DAC / line out ---- */
    /* Outputs off while the path is built; playback + record enabled. */
    {REG_DACPOWER, 0xc0},
    {REG_CONTROL1, 0x12},

    /* I2S (Philips), 16-bit words, MCLK at 256x fs. All three match what
     * sink_i2s.cpp actually generates; the word length is why the port can
     * stay 16-bit even though the slots are 32. */
    {REG_DACCONTROL1, 0x18},
    {REG_DACCONTROL2, 0x02},

    /* Output mixer: DAC only, 0 dB, no analogue bypass.
     *
     * DEPARTURE 1 from Espressif's driver, which offers a "line" mode that
     * routes LIN/RIN straight to the output stages in the analogue domain.
     * That would defeat the entire point of this feature — the input is
     * meant to come back through the FX bus and the looper under in.route,
     * not around them. So the line inputs are left out of the mixer and the
     * only thing reaching the outputs is the DAC. */
    {REG_DACCONTROL16, 0x00},
    {REG_DACCONTROL17, 0x90},
    {REG_DACCONTROL20, 0x90},

    /* One LRCK for both converters, taken from the ADC side. They already
     * share a WS line on the wire — this makes the chip's internals agree.
     *
     * DEPARTURE 2: Espressif writes 0xc0 here in its line mode, which also
     * arms the analogue bypass of departure 1. 0x80 is the shared-LRCK half
     * on its own. */
    {REG_DACCONTROL21, 0x80},
    {REG_DACCONTROL23, 0x00}, /* VROI: the lower output reference resistance */

    /* Digital volumes at 0 dB in both directions (the register counts down
     * in 0.5 dB steps, so 0 is unity). See the file header on why nothing
     * here is used as a level control. */
    {REG_DACVOL_L, 0x00},
    {REG_DACVOL_R, 0x00},

    /* Analogue output level, i.e. nominal line level out of LOUT/ROUT.
     *
     * The datasheet's range is -45 dB to +4.5 dB in 1.5 dB steps, so the
     * register counts 0..33 and 0x1e (30) is 0 dB. Espressif's driver
     * comments this as "0x00: -30dB, 0x1E: 0dB, 0x21: 3dB" — only the middle
     * number is right, and 0x21 is +4.5 dB, not +3. Anything above 0 dB can
     * clip on a loud signal, per the same section of the datasheet.
     *
     * A safe starting point only: set_out_level() applies `out.level` from
     * here before the unmute below, so this is what the pins sit at during
     * the rest of the sequence rather than what they end up at. */
    {REG_LOUT1VOL, 0x1e},
    {REG_ROUT1VOL, 0x1e},
    {REG_LOUT2VOL, 0x1e},
    {REG_ROUT2VOL, 0x1e},

    /* Outputs on. */
    {REG_DACPOWER, kDacOutputs},

    /* ---- ADC / line in ---- */
    /* Down while the input path is selected. */
    {REG_ADCPOWER, 0xff},
    {REG_ADCCONTROL2, kAdcInput},
    /* Paired with ADCCONTROL2 above — see kAdcControl3. */
    {REG_ADCCONTROL3, kAdcControl3},
    /* Same serial format as the DAC: I2S, 16-bit, MCLK 256x fs.
     *
     * The field layout is DATSEL[7:6], ADCLRP[5], ADCWL[4:2], FORMAT[1:0]. M5's
     * driver writes 0x2c here, i.e. the same with ADCLRP set, and S31d tried it
     * on the theory that a lopsided capture was a framing error. It is not: the
     * bit simply moved the signal from the right slot to the left and left the
     * other side on a low-level residue either way. One live analogue input,
     * not a misframed pair — and with ADCLRP clear it lands in the slot it
     * belongs in. esp-adf's value, kept. */
    {REG_ADCCONTROL4, 0x0c},
    {REG_ADCCONTROL5, 0x02},
    /* 0 dB on both unless the board needs a channel matched — see kAdcVolL. */
    {REG_ADCVOL_L, kAdcVolL},
    {REG_ADCVOL_R, kAdcVolR},

    /* ADC up, microphone bias off.
     *
     * DEPARTURE 3: Espressif's init leaves the PGA at 0xbb — about +33 dB,
     * past the documented 24 dB maximum — because its driver assumes a
     * microphone. Into a line input that is roughly 30 dB of clipping. The
     * PGA is set from in.pga after this table instead, defaulting to 0 dB. */
    {REG_ADCPOWER, kAdcPower},

    /* Everything is configured: release the digital engine. Pairs with the
     * 0xff at the top, and must stay last — the point of the hold is that
     * nothing above it runs on a live state machine. */
    {REG_CHIPPOWER, 0x00},
};

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
uint16_t s_addr = 0; /* whichever of kAddrCe0/kAddrCe1 answered the probe */
const char* s_name = "es8388?";

/* One transfer, one chance, same as the vendor's driver — which does a plain
 * beginTransmission/write/write/endTransmission and checks nothing.
 *
 * S31d spent a while doing otherwise: retries, then read-back verification,
 * then a background reconciler that kept trying until the chip agreed. All of
 * it was compensating for one hand-wired rig whose control bus stops accepting
 * writes while the I2S port runs, and none of it fixed that — it only changed
 * the shape of the failure, and each layer made the next symptom harder to
 * read. A driver that reports "this write failed" is more useful than one that
 * hides a bad bus behind machinery, because the bus is the thing to fix.
 *
 * The one ordering that did matter is kept: codec_init() runs before the port
 * starts, where the bus is reliable. See codec.h. */
constexpr int kTimeoutMs = 100;

esp_err_t write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), kTimeoutMs);
}

esp_err_t read_reg(uint8_t reg, uint8_t* val) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, kTimeoutMs);
}

#if SYNTH_ENABLE_LINE_IN
/* The M5Stack Module Audio (M144) carries an STM32G030 at 0x33 alongside the
 * codec, and it owns two analogue switches in front of LIN1: the LINE/MIC jack
 * and the headset microphone, which share that pin. M5's README is explicit
 * that the two must be kept mutually exclusive, and its own example opens the
 * jack and closes the headset mic before selecting LIN1/RIN1.
 *
 * Without this the module captures in mono. RIN1 comes straight off the jack's
 * right channel and works; LIN1 sits behind the switch and reads only a few dB
 * of bleed, which looks like a framing error on the meter and is not one.
 * Register 0x00 is documented as defaulting open, but it is flash-backed on the
 * STM32, so a module that was ever set otherwise stays that way. Setting it is
 * a RAM-level write — the flash write-back is a separate command (0xf0) that
 * this deliberately does not send, so nothing about the module is changed
 * permanently.
 *
 * Silently skipped on any board without that chip: a bare ES8388 NACKs the
 * probe and this returns, which is the correct behaviour everywhere else. */
constexpr uint16_t kM5ModuleAddr = 0x33;
constexpr uint8_t kM5RegMicStatus = 0x00;   /* LINE/MIC jack: 1 = open */
constexpr uint8_t kM5RegHpMode = 0x10;      /* 0 = CTIA, 1 = OMTP */
constexpr uint8_t kM5RegHpMicStatus = 0x11; /* headset mic: 0 = closed */
constexpr uint8_t kM5RegHpInsert = 0x20;    /* headset detect, read-only */
constexpr uint8_t kM5RegFwVersion = 0xfe;

/* Read one of the STM32's registers; returns 0xff on any failure, which is not
 * a value any of the ones read here take. */
uint8_t m5_read(i2c_master_dev_handle_t dev, uint8_t reg) {
    uint8_t v = 0xff;
    if (i2c_master_transmit_receive(dev, &reg, 1, &v, 1, kTimeoutMs) != ESP_OK) {
        return 0xff;
    }
    return v;
}

void configure_m5_module(void) {
    if (i2c_master_probe(s_bus, kM5ModuleAddr, 50) != ESP_OK) return;

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kM5ModuleAddr;
    cfg.scl_speed_hz = 400000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(s_bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "M5 module at 0x%02x answered but would not open",
                 (unsigned)kM5ModuleAddr);
        return;
    }

    /* What the module is actually set to, before touching it. Both of these
     * switches sit on LIN1 and both survive a reset — 0x00 is flash-backed —
     * so the state a module comes up in is not necessarily the documented
     * default, and every conclusion drawn from writing them blind is only as
     * good as that assumption. */
    ESP_LOGI(TAG,
             "M5 module 0x%02x before: mic 0x%02x, hp-mode 0x%02x, hp-mic "
             "0x%02x, inserted 0x%02x, fw 0x%02x",
             (unsigned)kM5ModuleAddr, (unsigned)m5_read(dev, kM5RegMicStatus),
             (unsigned)m5_read(dev, kM5RegHpMode),
             (unsigned)m5_read(dev, kM5RegHpMicStatus),
             (unsigned)m5_read(dev, kM5RegHpInsert),
             (unsigned)m5_read(dev, kM5RegFwVersion));

    const uint8_t mic[2] = {kM5RegMicStatus,
#if defined(CONFIG_OSYNTH_ES8388_M5_LINE_MIC)
                            1
#else
                            0
#endif
    };
    const uint8_t hp_mode[2] = {kM5RegHpMode,
#if defined(CONFIG_OSYNTH_ES8388_M5_HP_OMTP)
                                1
#else
                                0
#endif
    };
    const uint8_t hp_mic[2] = {kM5RegHpMicStatus, 0};

    esp_err_t err = i2c_master_transmit(dev, hp_mic, 2, kTimeoutMs);
    if (err == ESP_OK) err = i2c_master_transmit(dev, hp_mode, 2, kTimeoutMs);
    if (err == ESP_OK) err = i2c_master_transmit(dev, mic, 2, kTimeoutMs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "M5 module 0x%02x after:  mic 0x%02x, hp-mode 0x%02x, "
                      "hp-mic 0x%02x",
                 (unsigned)kM5ModuleAddr, (unsigned)m5_read(dev, kM5RegMicStatus),
                 (unsigned)m5_read(dev, kM5RegHpMode),
                 (unsigned)m5_read(dev, kM5RegHpMicStatus));
    } else {
        ESP_LOGW(TAG, "M5 module 0x%02x: setup failed (%s)",
                 (unsigned)kM5ModuleAddr, esp_err_to_name(err));
    }
    (void)i2c_master_bus_rm_device(dev);
}
#endif /* SYNTH_ENABLE_LINE_IN */

/* Every register the init table touches, read straight back off the chip.
 * Called once at the end of codec_init(), while the port is still stopped and
 * the bus is known good — so it is both a record of what the chip was actually
 * left holding and a check on the read path itself. */
void dump_regs(void) {
    char line[80];
    for (uint8_t base = 0x00; base < 0x36; base += 8) {
        size_t n = 0;
        for (uint8_t i = 0; i < 8 && (uint8_t)(base + i) < 0x36; ++i) {
            uint8_t v = 0;
            const esp_err_t err = read_reg((uint8_t)(base + i), &v);
            const int w = (err == ESP_OK)
                              ? snprintf(line + n, sizeof(line) - n, " %02x",
                                         (unsigned)v)
                              : snprintf(line + n, sizeof(line) - n, " --");
            if (w < 0 || (size_t)w >= sizeof(line) - n) break;
            n += (size_t)w;
        }
        ESP_LOGI(TAG, "regs %02x:%s", (unsigned)base, line);
    }
}

/* Everything that answers on the bus, logged only when something has already
 * gone wrong. A chip that ACKs its address and then NACKs a write looks exactly
 * like a chip that is not there at all once the sequence has aborted, and the
 * address map is the one piece of evidence that tells those apart — an empty
 * scan means wiring, a full one means the codec is present and refusing.
 *
 * Expected maps: a bare ES8388 answers at 0x10 or 0x11 and nothing else. On the
 * M5Stack Module Audio (M144) a healthy bus is 0x10 (the codec, CE strapped
 * low) *and* 0x33 (its STM32G030) — if 0x33 is missing too, the fault is the
 * bus, not the codec. */
void log_bus_scan(i2c_master_bus_handle_t bus, const char* when) {
    char found[96];
    size_t n = 0;
    for (uint16_t a = 0x08; a <= 0x77; ++a) {
        if (i2c_master_probe(bus, a, 20) != ESP_OK) continue;
        const int w =
            snprintf(found + n, sizeof(found) - n, " 0x%02x", (unsigned)a);
        if (w < 0 || (size_t)w >= sizeof(found) - n) break;
        n += (size_t)w;
    }
    ESP_LOGE(TAG, "bus scan (%s) on sda %d scl %d:%s", when,
             (int)OSYNTH_ES8388_SDA, (int)OSYNTH_ES8388_SCL,
             n != 0 ? found : " nothing answered");
}

#if SYNTH_ENABLE_LINE_IN
/* ADCCONTROL1 carries two 4-bit PGA codes, left in the high nibble and right
 * in the low, 3 dB per step. `code` is the parameter's enum index, which is
 * the hardware code directly — that is the point of exposing it as an enum
 * rather than a dB number the firmware would have to round. */
esp_err_t set_pga(uint8_t code) {
    if (code > 8) code = 8; /* 8 = +24 dB, the documented maximum */
    /* The two nibbles are independent, so the board's channel imbalance is
     * corrected here rather than left for a digital trim to paper over: this
     * is ahead of the converter, which is the only place a correction costs
     * no resolution. See OSYNTH_ES8388_PGA_BALANCE. */
    int l = (int)code + CONFIG_OSYNTH_ES8388_PGA_BALANCE;
    int r = (int)code;
    if (l < 0) { r -= l; l = 0; } /* a negative balance lifts the right instead */
    if (l > 8) l = 8;
    if (r > 8) r = 8;
    return write_reg(REG_ADCCONTROL1, (uint8_t)((l << 4) | r));
}
#endif

/* Output driver level. The datasheet's range is -45 dB to +4.5 dB in 1.5 dB
 * steps, so the register counts 0..33 with 30 = 0 dB — this is where the
 * parameter's dB value gets quantised, and it is the only place that knows
 * the step size.
 *
 * All four registers are written, not just the enabled pair: an unpowered
 * driver ignores its volume, and keeping them in step means a build that
 * changes OSYNTH_ES8388_OUTPUT does not come up at a level nobody set. */
esp_err_t set_out_level(float db) {
    if (db < -45.0f) db = -45.0f;
    if (db > 4.5f) db = 4.5f;
    int code = (int)((db + 45.0f) / 1.5f + 0.5f);
    if (code < 0) code = 0;
    if (code > 33) code = 33;
    const uint8_t v = (uint8_t)code;
    /* Only the pair that is actually driving something, so a stereo level is
     * two writes and not four. Both are attempted even if the first fails, and
     * the first error is the one returned: stopping early is what would leave
     * the left channel at the new level and the right at the old, which is
     * heard as the image jumping to one side rather than as a level that did
     * not change. */
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < kOutVolCount; ++i) {
        const esp_err_t e = write_reg(kOutVolRegs[i], v);
        if (err == ESP_OK) err = e;
    }
    return err;
}

/* Runs on whichever control task called ParamStore::set(), never the audio
 * task (which is forbidden from calling set() at all). Each register write is
 * ~60 us at 100 kHz, against a BLE knob drag already coalesced to ~20 Hz, so
 * doing it synchronously here costs less than a queue would.
 *
 * A failure is reported and dropped, not retried. On a healthy bus this does
 * not fail; on an unhealthy one the warning is the useful output, because the
 * bus is what needs fixing. */
void on_param(uint16_t id, float value, osynth::ParamOrigin, void*) {
    esp_err_t err = ESP_OK;
    switch (id) {
#if SYNTH_ENABLE_LINE_IN
        case osynth::PID_LINE_IN_PGA:
            err = set_pga((uint8_t)(value + 0.5f));
            break;
#endif
        case osynth::PID_OUT_LEVEL:
            err = set_out_level(value);
            break;
        default:
            return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "param 0x%04x -> codec failed: %s", (unsigned)id,
                 esp_err_to_name(err));
    }
}

} // namespace

esp_err_t codec_init(void) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1; /* let the driver pick a free controller */
    bus_cfg.sda_io_num = OSYNTH_ES8388_SDA;
    bus_cfg.scl_io_num = OSYNTH_ES8388_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    /* A courtesy, not a substitute. The ESP32's internal pull-ups are around
     * 45k; the ES8388 datasheet asks for 1k on both CCLK and CDATA. Enabling
     * these gives a board without real resistors some chance of enumerating
     * instead of failing the probe with no clue why — but a bus that is
     * intermittent, or works at boot and not later, wants the 1k fitted. */
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus on sda %d scl %d failed: %s", OSYNTH_ES8388_SDA,
                 OSYNTH_ES8388_SCL, esp_err_to_name(err));
        s_bus = nullptr;
        return err;
    }

    /* On a CE=1 board the first probe fails and the I2C driver logs its own
     * "unexpected nack" error before the second one succeeds. That line is
     * expected and harmless — the one that matters is whether the summary
     * below says the codec came up. */
    uint16_t& addr = s_addr;
    if (i2c_master_probe(s_bus, kAddrCe0, 100) == ESP_OK) {
        addr = kAddrCe0;
    } else if (i2c_master_probe(s_bus, kAddrCe1, 100) == ESP_OK) {
        addr = kAddrCe1;
    } else {
        /* Loud, because the board is about to be completely silent and this
         * is the only place that knows why — but not fatal: the synth still
         * renders, still answers BLE, and still streams over USB on a tap
         * build. A soldering fault should not cost the user their patches. */
        ESP_LOGE(TAG, "no ES8388 answering on sda %d scl %d (tried 0x%02x and "
                      "0x%02x) — the analogue output will be silent",
                 OSYNTH_ES8388_SDA, OSYNTH_ES8388_SCL, (unsigned)kAddrCe0,
                 (unsigned)kAddrCe1);
        log_bus_scan(s_bus, "probe failed");
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    /* 400 kHz, matching M5's own driver, which passes 400000 as its default
     * I2C speed and successfully writes volume registers with the I2S port
     * already running. Espressif's driver uses 100 kHz, which is what this was
     * until S31d — and it is the last configuration difference left between
     * this build and a setup known to work on the same module. Revert to
     * 100000 if the bus gets worse rather than better; longer wires and weak
     * pull-ups are less forgiving of the faster edges this needs. */
    dev_cfg.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device 0x%02x failed: %s", (unsigned)addr,
                 esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        s_dev = nullptr;
        return err;
    }

    for (size_t i = 0; i < sizeof(kInit) / sizeof(kInit[0]); ++i) {
        err = write_reg(kInit[i].reg, kInit[i].val);
        if (err != ESP_OK) {
            /* Stop at the first failure rather than push on through a
             * half-configured power sequence: the chip answered the probe,
             * so a write failing here means the bus went bad mid-sequence,
             * and the remaining writes would be guesses about what state it
             * is actually in. The DAC stays muted, which is where the table
             * starts. */
            ESP_LOGE(TAG, "register 0x%02x = 0x%02x failed at step %u/%u on the "
                          "device that answered at 0x%02x (%s); stopping with "
                          "the codec muted",
                     (unsigned)kInit[i].reg, (unsigned)kInit[i].val,
                     (unsigned)(i + 1),
                     (unsigned)(sizeof(kInit) / sizeof(kInit[0])),
                     (unsigned)addr, esp_err_to_name(err));
            log_bus_scan(s_bus, "write failed");
            return err;
        }
    }

    /* Apply the restored/default values before unmuting, then follow them.
     * These first writes are not optional extras alongside the listener: the
     * listener only ever sees *future* set() calls, and persist_init()
     * restored any saved value long before this component existed. */
    osynth::ParamStore& params = osynth::ParamStore::instance();
#if SYNTH_ENABLE_LINE_IN
    /* Board-specific, and a no-op on anything that is not an M144. */
    configure_m5_module();
    (void)set_pga((uint8_t)(params.get(osynth::PID_LINE_IN_PGA) + 0.5f));
#endif
    (void)set_out_level(params.get(osynth::PID_OUT_LEVEL));
    if (params.addListener(on_param, nullptr) < 0) {
        ESP_LOGW(TAG, "listener table full; in.pga and out.level will not "
                      "reach the chip");
    }

    /* Unmute here, on the stopped-port bus, and not after audio_io_start().
     * Leaving it for afterwards was the first thing tried and it does not
     * work: on a jumper-wired M144 the single unmute write NACKs five times in
     * about a millisecond once the clocks are running, and a codec that is
     * perfectly configured and permanently muted is no better than one that
     * never answered.
     *
     * Unmuting a DAC that has no clock yet is safe rather than merely
     * tolerable. With no MCLK it is not converting, so its outputs are sitting
     * at VMID exactly as they were while muted; when audio_io_start() brings
     * the clocks up a moment later the first thing it converts is the render
     * chain's silence, because no note can have been played yet. There is no
     * step for the output caps to pass on. */
    err = write_reg(REG_DACCONTROL3, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unmute failed: %s — configured but silent",
                 esp_err_to_name(err));
        return err;
    }

    s_name = "es8388";
    ESP_LOGI(TAG,
             "up at 0x%02x (sda %d scl %d): I2S slave, 16-bit, mclk 256fs, "
             "in %s, out %s",
             (unsigned)addr, OSYNTH_ES8388_SDA, OSYNTH_ES8388_SCL, kInputName,
             kOutputName);
    dump_regs();
    return ESP_OK;
}

const char* codec_name(void) { return s_name; }

#else /* discrete front end: the converters configure themselves from straps */

esp_err_t codec_init(void) { return ESP_OK; }
const char* codec_name(void) { return "none"; }

#endif /* SYNTH_ENABLE_CODEC_ES8388 */
