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
 * bits, RINSEL the next two. */
#if defined(CONFIG_OSYNTH_ES8388_IN_LINE1)
constexpr uint8_t kAdcInput = 0x00; /* LIN1 / RIN1 */
#elif defined(CONFIG_OSYNTH_ES8388_IN_DIFF)
constexpr uint8_t kAdcInput = 0xf0; /* differential */
#else
constexpr uint8_t kAdcInput = 0x50; /* LIN2 / RIN2 — the line jack on most boards */
#endif

/* DACPOWER: LOUT1 = 0x04, ROUT1 = 0x10, LOUT2 = 0x08, ROUT2 = 0x20. */
#if defined(CONFIG_OSYNTH_ES8388_OUT1)
constexpr uint8_t kDacOutputs = 0x14;
constexpr const char* kOutputName = "LOUT1/ROUT1";
#elif defined(CONFIG_OSYNTH_ES8388_OUT2)
constexpr uint8_t kDacOutputs = 0x28;
constexpr const char* kOutputName = "LOUT2/ROUT2";
#else
constexpr uint8_t kDacOutputs = 0x3c; /* both pairs */
constexpr const char* kOutputName = "LOUT1/ROUT1 + LOUT2/ROUT2";
#endif

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

    /* Reference buffers up, then the digital blocks out of reset. */
    {REG_CONTROL2, 0x50},
    {REG_CHIPPOWER, 0x00},

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
    {REG_ADCCONTROL3, 0x02},
    /* Same serial format as the DAC: I2S, 16-bit, MCLK 256x fs. */
    {REG_ADCCONTROL4, 0x0c},
    {REG_ADCCONTROL5, 0x02},
    {REG_ADCVOL_L, 0x00},
    {REG_ADCVOL_R, 0x00},

    /* ADC up, microphone bias off.
     *
     * DEPARTURE 3: Espressif's init leaves the PGA at 0xbb — about +33 dB,
     * past the documented 24 dB maximum — because its driver assumes a
     * microphone. Into a line input that is roughly 30 dB of clipping. The
     * PGA is set from in.pga after this table instead, defaulting to 0 dB. */
    {REG_ADCPOWER, kAdcPower},
};

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
const char* s_name = "es8388?";

esp_err_t write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

#if SYNTH_ENABLE_LINE_IN
/* ADCCONTROL1 carries two 4-bit PGA codes, left in the high nibble and right
 * in the low, 3 dB per step. `code` is the parameter's enum index, which is
 * the hardware code directly — that is the point of exposing it as an enum
 * rather than a dB number the firmware would have to round. */
esp_err_t set_pga(uint8_t code) {
    if (code > 8) code = 8; /* 8 = +24 dB, the documented maximum */
    return write_reg(REG_ADCCONTROL1, (uint8_t)((code << 4) | code));
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
    esp_err_t err = write_reg(REG_LOUT1VOL, v);
    if (err == ESP_OK) err = write_reg(REG_ROUT1VOL, v);
    if (err == ESP_OK) err = write_reg(REG_LOUT2VOL, v);
    if (err == ESP_OK) err = write_reg(REG_ROUT2VOL, v);
    return err;
}

/* Runs on whichever control task called ParamStore::set(), never the audio
 * task (which is forbidden from calling set() at all). Each register write is
 * ~60 us at 100 kHz, against a BLE knob drag already coalesced to ~20 Hz, so
 * doing it synchronously here costs less than a queue would. */
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
    uint16_t addr = 0;
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
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000; /* the rate Espressif's driver uses */
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
            ESP_LOGE(TAG, "register 0x%02x = 0x%02x failed (%s); stopping with "
                          "the codec muted",
                     (unsigned)kInit[i].reg, (unsigned)kInit[i].val,
                     esp_err_to_name(err));
            return err;
        }
    }

    /* Apply the restored/default values before unmuting, then follow them.
     * These first writes are not optional extras alongside the listener: the
     * listener only ever sees *future* set() calls, and persist_init()
     * restored any saved value long before this component existed. */
    osynth::ParamStore& params = osynth::ParamStore::instance();
#if SYNTH_ENABLE_LINE_IN
    (void)set_pga((uint8_t)(params.get(osynth::PID_LINE_IN_PGA) + 0.5f));
#endif
    (void)set_out_level(params.get(osynth::PID_OUT_LEVEL));
    if (params.addListener(on_param, nullptr) < 0) {
        ESP_LOGW(TAG, "listener table full; in.pga and out.level will not "
                      "reach the chip");
    }

    /* Unmute last, onto a codec that is fully configured and already being
     * clocked by a running I2S port. */
    err = write_reg(REG_DACCONTROL3, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unmute failed: %s", esp_err_to_name(err));
        return err;
    }

    s_name = "es8388";
    ESP_LOGI(TAG,
             "up at 0x%02x (sda %d scl %d): I2S slave, 16-bit, mclk 256fs, "
             "in %s, out %s",
             (unsigned)addr, OSYNTH_ES8388_SDA, OSYNTH_ES8388_SCL, kInputName,
             kOutputName);
    return ESP_OK;
}

const char* codec_name(void) { return s_name; }

#else /* discrete front end: the converters configure themselves from straps */

esp_err_t codec_init(void) { return ESP_OK; }
const char* codec_name(void) { return "none"; }

#endif /* SYNTH_ENABLE_CODEC_ES8388 */
