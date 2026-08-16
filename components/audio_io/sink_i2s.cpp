/*
 * osynth — the I2S port: audio out to a DAC, and (S31) audio in from an ADC.
 *
 * Output (SYNTH_ENABLE_I2S_DAC): standard-mode master, 16-bit stereo Philips
 * frames. The blocking i2s_channel_write() against the DMA queue is what
 * paces the audio task.
 *
 * Input (SYNTH_ENABLE_LINE_IN): a stereo ADC in *slave* mode on the same
 * port. TX and RX allocated from a single i2s_new_channel() call land on one
 * controller and share BCLK and WS, so the capture is sample-locked to the
 * output by construction — no drift, no resampling, ever.
 *
 * This file knows nothing about *which* converters are out there. It has one
 * question to answer — does anything on this bus need a system clock? — and
 * SYNTH_I2S_MCLK_MODE answers it (synth_config.h):
 *
 *   off  a bare PCM5102A: 16-bit slots, BCLK 32x fs, no MCLK. It recovers
 *        its clock from BCK through an internal PLL — tie its SCK to GND.
 *   on   a PCM1808 ADC or an ES8388 codec is present: 32-bit slots, BCLK
 *        64x fs, MCLK at 256x fs on a pin of its own.
 *
 * An ES8388 additionally has to be told all this over I2C, since nothing
 * about it is strapped — that is the `codec` component's job, and it runs
 * after this port is already clocking. Pinout: docs/HARDWARE.md.
 */
#include "audio_sink.h"

#if SYNTH_ENABLE_I2S_DAC

#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soc/gpio_num.h"
#include "soc/soc_caps.h"

/* Capture this chip's own I2S output and check it, at startup, with nothing
 * connected. See i2s_loopback_selftest() below for what it proves and why it
 * exists. 1 to run it; it costs about a second of boot and then gets out of
 * the way. Deliberately a source-level switch rather than Kconfig: it is a
 * bring-up instrument, not a feature. */
#define OSYNTH_I2S_LOOPBACK_TEST 0

/* ---- link-level A/B switches (ESP32-P4 distortion, see
 * private_docs/P4_AUDIO_DISTORTION.md) --------------------------------------
 *
 * Everything here changes how the signal reaches the converter, not what the
 * signal is. They exist because that is the only region left: the render chain
 * is proven bit-perfect at the USB tap, the codec's registers are verified
 * against a dump, and the loopback self-test proves this chip emits correct
 * frames at its own pad. What is unproven is the trip from that pad to the
 * converter's pins, and these are the four things software can do about it.
 *
 * One at a time, and put each back before trying the next — the whole point is
 * to know which one moved the needle.
 *
 * DRIVE_CAP  GPIO drive strength on the outputs, 0-3; IDF's default is 2.
 *            Try *both* directions and do not assume stronger is better: 3
 *            sharpens edges into a line that may already be ringing, 0 or 1
 *            slows them, which is what an unterminated jumper usually wants.
 *            -1 leaves IDF's default alone.
 * INVERT_BCLK  Moves the converter's sampling edge by half a bit period. The
 *            classic fix for a setup/hold violation at the far end, and free to
 *            try: a link marginal on one edge is usually comfortable on the
 *            other.
 * INVERT_WS  Same idea for the word clock. A frame boundary latched at the
 *            wrong moment puts part of one sample into the other channel, heard
 *            as a swirling, phase-shifted mess rather than as an obvious fault.
 * SLOT16     Drops back to 16-bit slots (BCLK 32x fs, 1.536 MHz at 48 kHz)
 *            while keeping MCLK. Halves the edges per frame on the busiest
 *            signal, and changes the *framing* rather than only its rate —
 *            which the 24 kHz experiment did not. The ES8388 is told 16-bit
 *            words either way so it takes either slot width; do NOT use this
 *            with a PCM1808, which always emits 24 bits in a 32-bit slot. */
/* Find out which pins on this board can drive at all, before trusting any of
 * them with a clock. Drives a list of candidates high and stops — the I2S port
 * is not opened, so there is no audio under this switch. See gpio_output_scan()
 * below. Run it with NOTHING wired to the pins. */
#define OSYNTH_GPIO_OUTPUT_SCAN 0

#define OSYNTH_I2S_DRIVE_CAP   (-1)
#define OSYNTH_I2S_INVERT_BCLK 0
#define OSYNTH_I2S_INVERT_WS   0
#define OSYNTH_I2S_SLOT16      0

static const char* TAG = "sink_i2s";

/* All five from Kconfig (defaults per target). Codec boards arrive with
 * their pinout already soldered, so a hardcoded triple would rule most of
 * them out. */
#define OSYNTH_I2S_BCLK ((gpio_num_t)CONFIG_OSYNTH_I2S_BCLK_GPIO)
#define OSYNTH_I2S_WS   ((gpio_num_t)CONFIG_OSYNTH_I2S_WS_GPIO)
#define OSYNTH_I2S_DOUT ((gpio_num_t)CONFIG_OSYNTH_I2S_DOUT_GPIO)
#if SYNTH_ENABLE_LINE_IN
#define OSYNTH_I2S_DIN ((gpio_num_t)CONFIG_OSYNTH_I2S_DIN_GPIO)
#endif
#if SYNTH_I2S_MCLK_MODE
#define OSYNTH_I2S_MCLK ((gpio_num_t)CONFIG_OSYNTH_I2S_MCLK_GPIO)
#endif

namespace {

i2s_chan_handle_t s_tx = nullptr;
#if SYNTH_ENABLE_LINE_IN
/* NULL when the RX half did not come up; the output then runs simplex and
 * audio_source_i2s_ready() says no. */
i2s_chan_handle_t s_rx = nullptr;
#endif

/* Opens the port. `with_rx` allocates and enables the capture channel;
 * `with_mclk` puts the port in 32-bit slots with a system clock. They are
 * separate because they come apart in two real cases: an ES8388 configured
 * for playback only still needs MCLK, and a PCM1808 whose RX half failed
 * should hand the PCM5102A back its original clocking rather than keep a
 * doubled BCLK it never asked for.
 *
 * On failure both handles are freed and nulled, so a caller can simply try
 * again with different arguments. */
esp_err_t i2s_open(const i2s_chan_config_t* chan_cfg, bool with_rx,
                   bool with_mclk) {
    i2s_chan_handle_t* rx_arg = nullptr;
#if SYNTH_ENABLE_LINE_IN
    if (with_rx) rx_arg = &s_rx;
#endif

    /* *One* config object, handed to both init calls below.
     *
     * Not a formality. The pairing itself is free — i2s_new_channel() with
     * both handles marks the controller full-duplex before anything is
     * configured — but the hardware then genuinely shares one BCLK, one WS
     * and one clock divider between the two directions. The driver programs
     * them from whichever config it is given, and it does not check that the
     * second agrees with the first (on this chip a mismatch is not even an
     * error; i2s_std.c logs at DEBUG and carries on). Passing the same object
     * twice is how the two are guaranteed to agree, rather than kept in sync
     * by hand at two call sites. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SYNTH_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = OSYNTH_I2S_BCLK,
            .ws = OSYNTH_I2S_WS,
            .dout = OSYNTH_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = (bool)OSYNTH_I2S_INVERT_BCLK,
                .ws_inv = (bool)OSYNTH_I2S_INVERT_WS,
            },
        },
    };

    /* Clock source. I2S_STD_CLK_DEFAULT_CONFIG asks for I2S_CLK_SRC_DEFAULT,
     * which is documented as "auto select maximum clock source" — on most
     * targets that lands on a 160 MHz PLL and is fine.
     *
     * On the ESP32-P4 it is not obviously fine, and the reason is in IDF's own
     * enum (soc/esp32p4/clk_tree_defs.h):
     *
     *     I2S_CLK_SRC_PLL_160M = SOC_MOD_CLK_PLL_F160M,
     *         only supported on P4 hw_ver3
     *
     * osynth's P4 target is pinned to rev <3 (ESP32P4_SELECTS_REV_LESS_V3, see
     * sdkconfig.defaults.esp32p4 — the CPLL caps at 360 MHz there), so that
     * source is exactly the one the silicon does *not* have. Whatever DEFAULT
     * resolves to on such a part, it is worth not leaving to inference on the
     * one signal an ES8388 cannot tolerate being wrong.
     *
     * APLL is a fractional-N PLL meant for audio and can synthesise 12.288 MHz
     * (256 x 48 kHz) exactly, where a 160 MHz source needs 13.0208..., i.e. a
     * fractional divider and the jitter that comes with it. A DAC's sigma-delta
     * modulator turns MCLK jitter into distortion that does not care about
     * signal level, load impedance or sample rate — which is the exact shape of
     * a P4 bring-up fault that survived every other explanation while the same
     * codec played cleanly from an S3.
     *
     * Guarded on the capability rather than the target: SOC_I2S_SUPPORTS_APLL is
     * 1 on the P4 and absent on the S3, so the S3 keeps the source it has always
     * used and this cannot regress it. */
#if SOC_I2S_SUPPORTS_APLL
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif

#if SYNTH_ENABLE_LINE_IN
    if (with_rx) std_cfg.gpio_cfg.din = OSYNTH_I2S_DIN;
#endif

#if SYNTH_I2S_MCLK_MODE
    if (with_mclk) {
        std_cfg.gpio_cfg.mclk = OSYNTH_I2S_MCLK;
        /* 16-bit samples carried in 32-bit slots. A PCM1808 always emits 24
         * data bits inside a 32-bit slot and has no mode that says otherwise;
         * an ES8388 is told the word length over I2C and would take either,
         * but runs the same arrangement so there is one configuration to
         * reason about. Left-aligned (the Philips default on this hardware),
         * the top 16 bits of the slot are exactly the sample we want and the
         * converter's remaining bits fall off the end of it.
         *
         * This looks like it should change the buffer format, and it does
         * not: i2s_get_buf_size() sizes the DMA buffer from `data_bit_width`,
         * never from the slot width (esp_driver_i2s/i2s_common.c), so `s_out`
         * in audio_io.cpp stays int16_t and i2s_write() below is untouched.
         * What it does change is the wire: 64 BCLK per frame instead of 32.
         *
         * ws_width has to move with it. Standard mode drives this chip's TDM
         * engine, where ws_width is the WS *high time in BCLK cycles*
         * (i2s_hal_std_set_tx_slot -> i2s_ll_tx_set_ws_width), while the
         * frame half-period follows the slot width. Leaving the default 16
         * against a 32-bit slot would emit WS high for 16 cycles and low for
         * 48 — not I2S, and not something any of these chips would decode. */
#if OSYNTH_I2S_SLOT16
        /* A/B switch: keep MCLK but hand the converter 16-bit slots. Both
         * fields move together for the same reason they do below — ws_width is
         * the WS high time in BCLK cycles, so it has to match the slot. */
        std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
        std_cfg.slot_cfg.ws_width = 16;
#else
        std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        std_cfg.slot_cfg.ws_width = 32;
#endif
    }
#endif

    esp_err_t err = i2s_new_channel(chan_cfg, &s_tx, rx_arg);
    if (err != ESP_OK) {
        s_tx = nullptr;
#if SYNTH_ENABLE_LINE_IN
        s_rx = nullptr;
#endif
        return err;
    }

    /* TX first. The *later* channel to initialize is the one the driver
     * demotes to I2S_ROLE_SLAVE (i2s_std.c), and the output side has to stay
     * the master that generates BCLK, WS and MCLK. */
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);

#if SYNTH_ENABLE_LINE_IN
    if (err == ESP_OK && rx_arg != nullptr) {
        err = i2s_channel_init_std_mode(s_rx, &std_cfg);

        /* Confirm the two really are a pair before either starts driving
         * pins. It should be true by construction, and it is cheap: the
         * alternative to a pair is two independent masters on one set of
         * pins, which is a hardware fault rather than a quiet degradation,
         * and nothing downstream would report it. */
        if (err == ESP_OK) {
            i2s_chan_info_t info = {};
            err = i2s_channel_get_info(s_tx, &info);
            if (err == ESP_OK && info.pair_chan == nullptr) {
                ESP_LOGE(TAG, "TX and RX did not constitute a duplex pair");
                err = ESP_ERR_INVALID_STATE;
            }
        }
    }

    /* RX before TX. BCLK and WS only start moving when the master (TX) is
     * enabled, so arming the receiver first makes frame alignment
     * deterministic from the very first frame. The other order races: RX can
     * latch mid-frame and come up with L and R swapped — permanently, and
     * intermittently across boots. */
    bool rx_running = false;
    if (err == ESP_OK && rx_arg != nullptr) {
        err = i2s_channel_enable(s_rx);
        rx_running = (err == ESP_OK);
    }
#endif

    if (err == ESP_OK) err = i2s_channel_enable(s_tx);

    if (err != ESP_OK) {
#if SYNTH_ENABLE_LINE_IN
        /* i2s_del_channel() refuses a running channel, so disable what came
         * up first — then delete *both*. Deleting only RX clears the
         * controller's full_duplex flag and leaves TX configured for a
         * pairing that no longer exists. */
        if (rx_running) i2s_channel_disable(s_rx);
        if (s_rx != nullptr) i2s_del_channel(s_rx);
        s_rx = nullptr;
#endif
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return err;
    }

#if OSYNTH_I2S_DRIVE_CAP >= 0
    /* After the channels are up, not before: the driver configures these pins
     * itself as part of init, so a drive strength set earlier is overwritten. */
    {
        gpio_num_t driven[4];
        size_t n = 0;
        driven[n++] = OSYNTH_I2S_BCLK;
        driven[n++] = OSYNTH_I2S_WS;
        driven[n++] = OSYNTH_I2S_DOUT;
#if SYNTH_I2S_MCLK_MODE
        if (with_mclk) driven[n++] = OSYNTH_I2S_MCLK;
#endif
        for (size_t i = 0; i < n; ++i) {
            (void)gpio_set_drive_capability(
                driven[i], (gpio_drive_cap_t)OSYNTH_I2S_DRIVE_CAP);
        }
        ESP_LOGW(TAG, "drive strength forced to %d on %u output pins (A/B "
                      "switch — see OSYNTH_I2S_DRIVE_CAP)",
                 OSYNTH_I2S_DRIVE_CAP, (unsigned)n);
    }
#endif
#if OSYNTH_I2S_INVERT_BCLK || OSYNTH_I2S_INVERT_WS || OSYNTH_I2S_SLOT16
    ESP_LOGW(TAG, "link A/B active: bclk_inv %d ws_inv %d slot16 %d",
             OSYNTH_I2S_INVERT_BCLK, OSYNTH_I2S_INVERT_WS, OSYNTH_I2S_SLOT16);
#endif

    if (with_mclk) {
        /* Read the ratios out of the config rather than hardcoding them, so the
         * line stays true when OSYNTH_I2S_SLOT16 changes the slot width. */
        const unsigned slot_bits = (unsigned)std_cfg.slot_cfg.slot_bit_width;
        ESP_LOGI(TAG,
                 "up: %d Hz, 16-bit data in %u-bit slots (bclk %u Hz = %ufs, "
                 "mclk %u Hz = 256fs), %s",
                 SYNTH_SAMPLE_RATE, slot_bits,
                 (unsigned)(SYNTH_SAMPLE_RATE * 2u * slot_bits), 2u * slot_bits,
                 (unsigned)(SYNTH_SAMPLE_RATE * 256u),
                 with_rx ? "duplex ok" : "output only");
    } else {
        ESP_LOGI(TAG, "up: %d Hz 16-bit stereo (bclk %u Hz = 32fs, no mclk), %s",
                 SYNTH_SAMPLE_RATE, (unsigned)(SYNTH_SAMPLE_RATE * 32u),
                 with_rx ? "duplex ok" : "output only");
    }
    /* Read back out of the config rather than from the macros, so an unused
     * pin prints as I2S_GPIO_UNUSED (-1) instead of needing its own #if. */
    ESP_LOGI(TAG, "pins: bclk %d ws %d dout %d din %d mclk %d",
             (int)std_cfg.gpio_cfg.bclk, (int)std_cfg.gpio_cfg.ws,
             (int)std_cfg.gpio_cfg.dout, (int)std_cfg.gpio_cfg.din,
             (int)std_cfg.gpio_cfg.mclk);
    /* Which source actually got used, so the choice above is visible in a log
     * rather than something to infer from the build. */
    ESP_LOGI(TAG, "clock source: %d (%s)", (int)std_cfg.clk_cfg.clk_src,
             std_cfg.clk_cfg.clk_src == I2S_CLK_SRC_DEFAULT ? "default" :
#if SOC_I2S_SUPPORTS_APLL
             std_cfg.clk_cfg.clk_src == I2S_CLK_SRC_APLL ? "APLL" :
#endif
             "other");
    return ESP_OK;
}

#if OSYNTH_I2S_LOOPBACK_TEST
/* Does this chip emit what it was given?
 *
 * Everything between the render chain and i2s_channel_write() can be checked
 * from the CPU side — the USB tap reads the very buffer the sink is handed, so
 * a clean USB stream proves the samples are right *as far as the driver*. What
 * it cannot see is the DMA, the serializer and the pin. This closes that gap
 * without a converter, a scope or a single jumper: IDF routes the TX signal
 * back into RX inside the GPIO matrix when dout == din (i2s_std.c, "Loopback if
 * dout = din"), so the port captures its own output at the pad.
 *
 * A known pattern goes out, comes back, and is compared. Clean means the P4's
 * output path is sound and the fault is past the pin — the wire, or the
 * converter's interface to it. Corrupt means the fault is on-chip and there is
 * finally something reproducible to bisect the target's settings against, which
 * is a far better position than swapping modules.
 *
 * Written for the ESP32-P4 bring-up where an M5Stack M144 that plays cleanly
 * from an S3 is unusable, identically across two modules, with the register
 * dump verified and the sample buffer proven bit-perfect. It is target-agnostic
 * though: run it on the S3 first to see what a passing result looks like.
 *
 * Off by default. It takes the port for about a second at startup and leaves it
 * closed, so audio comes up normally afterwards. */
esp_err_t i2s_loopback_selftest(void) {
    /* Ramp in the left slot, its complement in the right. The ramp makes the
     * arbitrary RX start offset findable; the complement catches a channel swap
     * or a slot misalignment, which a symmetric pattern would hide. */
    constexpr size_t kFrames = 256;
    constexpr size_t kSamples = kFrames * 2;
    int16_t* tx = (int16_t*)heap_caps_calloc(kSamples, sizeof(int16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t* rx = (int16_t*)heap_caps_calloc(kSamples, sizeof(int16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (tx == nullptr || rx == nullptr) {
        free(tx);
        free(rx);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < kFrames; ++i) {
        const int16_t v = (int16_t)(i * 0x0101);
        tx[2 * i] = v;
        tx[2 * i + 1] = (int16_t)~v;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = SYNTH_BLOCK_SIZE;
    chan_cfg.auto_clear_after_cb = true;

    i2s_chan_handle_t tx_h = nullptr;
    i2s_chan_handle_t rx_h = nullptr;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_h, &rx_h);
    if (err != ESP_OK) {
        free(tx);
        free(rx);
        return err;
    }

    /* Identical to the real port in every respect that could matter — same
     * clock source, same slot width, same ws_width, same pins — except that DIN
     * is DOUT, which is what arms the loopback. A self-test on a different
     * configuration would prove nothing about this one. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SYNTH_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = OSYNTH_I2S_BCLK,
            .ws = OSYNTH_I2S_WS,
            .dout = OSYNTH_I2S_DOUT,
            .din = OSYNTH_I2S_DOUT, /* the loopback */
            .invert_flags = {},
        },
    };
#if SOC_I2S_SUPPORTS_APLL
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif
#if SYNTH_I2S_MCLK_MODE
    cfg.gpio_cfg.mclk = OSYNTH_I2S_MCLK;
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    cfg.slot_cfg.ws_width = 32;
#endif

    err = i2s_channel_init_std_mode(tx_h, &cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(rx_h, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(rx_h);
    if (err == ESP_OK) err = i2s_channel_enable(tx_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "loopback: port would not open (%s)", esp_err_to_name(err));
        goto done;
    }

    /* Several rounds so the capture is of a settled port rather than of the
     * first frames after enable, which are legitimately ragged. */
    for (int round = 0; round < 8; ++round) {
        size_t n = 0;
        const uint8_t* p = (const uint8_t*)tx;
        size_t remaining = kSamples * sizeof(int16_t);
        while (remaining > 0 &&
               i2s_channel_write(tx_h, p, remaining, &n, 200) == ESP_OK && n > 0) {
            p += n;
            remaining -= n;
        }
        size_t got = 0;
        (void)i2s_channel_read(rx_h, rx, kSamples * sizeof(int16_t), &got, 200);
    }

    {
        /* Raw first, unconditionally. A dump of eight frames is often enough to
         * read the failure directly — a repeated frame, a one-slot shift, a
         * stuck bit — without trusting the analysis below. */
        char line[96];
        size_t n = 0;
        for (size_t i = 0; i < 8; ++i) {
            const int w = snprintf(line + n, sizeof(line) - n, " %04x/%04x",
                                   (unsigned)(uint16_t)rx[2 * i],
                                   (unsigned)(uint16_t)rx[2 * i + 1]);
            if (w < 0 || (size_t)w >= sizeof(line) - n) break;
            n += (size_t)w;
        }
        ESP_LOGI(TAG, "loopback rx L/R:%s", line);

        /* Find where the ramp starts in the capture, then walk it. The offset
         * itself is not a fault: RX begins wherever the enable landed. */
        int offset = -1;
        for (size_t i = 0; i + 1 < kFrames && offset < 0; ++i) {
            const uint16_t a = (uint16_t)rx[2 * i];
            const uint16_t b = (uint16_t)rx[2 * (i + 1)];
            if ((uint16_t)(b - a) == 0x0101 && (uint16_t)~a == (uint16_t)rx[2 * i + 1]) {
                offset = (int)i;
            }
        }
        if (offset < 0) {
            ESP_LOGE(TAG, "loopback FAILED: pattern not found in the capture at "
                          "all — the output path is not carrying these samples");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            size_t bad = 0;
            size_t checked = 0;
            for (size_t i = (size_t)offset; i + 1 < kFrames; ++i, ++checked) {
                const uint16_t l = (uint16_t)rx[2 * i];
                const uint16_t r = (uint16_t)rx[2 * i + 1];
                const uint16_t nl = (uint16_t)rx[2 * (i + 1)];
                if ((uint16_t)~l != r || (uint16_t)(nl - l) != 0x0101) {
                    if (bad < 4) {
                        ESP_LOGE(TAG, "loopback mismatch at frame %u: "
                                      "L %04x R %04x (R should be %04x), next L "
                                      "%04x (should be %04x)",
                                 (unsigned)i, (unsigned)l, (unsigned)r,
                                 (unsigned)(uint16_t)~l, (unsigned)nl,
                                 (unsigned)(uint16_t)(l + 0x0101));
                    }
                    ++bad;
                }
            }
            if (bad == 0) {
                ESP_LOGI(TAG, "loopback PASSED: %u frames exact from offset %d — "
                              "this chip's I2S output is not the fault",
                         (unsigned)checked, offset);
            } else {
                ESP_LOGE(TAG, "loopback FAILED: %u of %u frames corrupt from "
                              "offset %d — the fault is on-chip, before the pin",
                         (unsigned)bad, (unsigned)checked, offset);
                err = ESP_ERR_INVALID_CRC;
            }
        }
    }

done:
    if (tx_h != nullptr) {
        i2s_channel_disable(tx_h);
        i2s_del_channel(tx_h);
    }
    if (rx_h != nullptr) {
        i2s_channel_disable(rx_h);
        i2s_del_channel(rx_h);
    }
    free(tx);
    free(rx);
    return err;
}
#endif /* OSYNTH_I2S_LOOPBACK_TEST */

#if OSYNTH_GPIO_OUTPUT_SCAN
/* Which pins on this board can actually drive?
 *
 * Born on an ESP32-P4 module-on-carrier where GPIO45/46/47 — listed as free
 * header pins, and used for BCLK/WS/MCLK for weeks — turned out to be held at
 * 0.6 V and incapable of driving anything. Two converters failed on that board
 * and a week went into the codec before anyone put a meter on a pin.
 *
 * Each candidate is driven high and left there. A pin that works reads the full
 * rail; a pin held by something on the board reads whatever is holding it. That
 * is the entire test — no toggling, no scope, any multimeter does it.
 *
 * Include pins you already trust. The known-good ones are the control: if they
 * do not read 3.3 V the meter or the method is wrong, not the board.
 *
 * Drive strength is forced to the weakest setting first. If one of these pins is
 * tied to ground in copper, driving it high is a short, and this limits what
 * that costs. Do not run this scan with anything wired to the pins. */
void gpio_output_scan(void) {
    static const int kCandidates[] = {1, 2, 3, 4, 5, 20, 32, 33, 45, 46, 47};
    char line[128];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i) {
        const gpio_num_t pin = (gpio_num_t)kCandidates[i];
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << kCandidates[i];
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cfg) != ESP_OK) continue;
        (void)gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_0);
        (void)gpio_set_level(pin, 1);
        const int w = snprintf(line + n, sizeof(line) - n, " %d",
                               kCandidates[i]);
        if (w < 0 || (size_t)w >= sizeof(line) - n) break;
        n += (size_t)w;
    }
    ESP_LOGW(TAG, "GPIO OUTPUT SCAN — driven high, weakest drive:%s", line);
    ESP_LOGW(TAG, "meter each against GND with nothing connected. ~3.3 V = the "
                  "pin drives; anything else = held by the board. Expect 32 and "
                  "33 to pass (controls) and 45/46/47 to fail.");
}
#endif /* OSYNTH_GPIO_OUTPUT_SCAN */

esp_err_t i2s_start(void) {
#if OSYNTH_GPIO_OUTPUT_SCAN
    /* Takes the pins and keeps them: the port never opens under this switch, so
     * the levels stay put for as long as it takes to walk them with a meter. */
    gpio_output_scan();
    return ESP_OK;
#endif
#if OSYNTH_I2S_LOOPBACK_TEST
    /* Before the real port: it needs the same pins, and it leaves them free. */
    (void)i2s_loopback_selftest();
#endif
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;                 /* ~5.3 ms of buffer at 48 kHz */
    chan_cfg.dma_frame_num = SYNTH_BLOCK_SIZE; /* one render block per buffer */
    chan_cfg.auto_clear_after_cb = true;       /* silence, not stale data, on starve */

#if SYNTH_ENABLE_LINE_IN
    const esp_err_t derr = i2s_open(&chan_cfg, /*with_rx=*/true, /*with_mclk=*/true);
    if (derr == ESP_OK) return ESP_OK;
    /* The input is an accessory; losing it must never cost the output. */
    ESP_LOGW(TAG, "capture bring-up failed (%s); output only",
             esp_err_to_name(derr));
#endif

    /* Output-only, either because this build has no input or because the
     * attempt above failed. The MCLK question is *not* settled by that
     * failure: an ES8388 still needs its system clock to play a note, while
     * a discrete PCM5102A wants the plain 32x fs arrangement back. */
    return i2s_open(&chan_cfg, /*with_rx=*/false,
                    /*with_mclk=*/SYNTH_ENABLE_CODEC_ES8388);
}

esp_err_t i2s_write(const int16_t* interleaved, size_t frames) {
    const uint8_t* p = (const uint8_t*)interleaved;
    size_t remaining = frames * 2 * sizeof(int16_t);
    while (remaining > 0) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, p, remaining, &written, 1000);
        if (err != ESP_OK) return err;
        p += written;
        remaining -= written;
    }
    return ESP_OK;
}

const audio_sink_t s_sink = {"i2s", i2s_start, i2s_write};

} // namespace

const audio_sink_t* audio_sink_i2s(void) { return &s_sink; }

#if SYNTH_ENABLE_LINE_IN

bool audio_source_i2s_ready(void) { return s_rx != nullptr; }

esp_err_t audio_source_i2s_read(int16_t* interleaved, size_t frames,
                                size_t* frames_read) {
    *frames_read = 0;
    if (s_rx == nullptr) return ESP_ERR_INVALID_STATE;
    size_t bytes = 0;
    /* Zero timeout: i2s_channel_read() then takes its semaphore and its queue
     * with a zero tick wait and returns ESP_ERR_TIMEOUT with a partial
     * `bytes`, so this cannot block and cannot pace. The sink's blocking
     * write stays the only clock in the system. */
    const esp_err_t err = i2s_channel_read(s_rx, interleaved,
                                           frames * 2 * sizeof(int16_t), &bytes,
                                           0);
    *frames_read = bytes / (2 * sizeof(int16_t));
    return err;
}

#endif /* SYNTH_ENABLE_LINE_IN */

#endif /* SYNTH_ENABLE_I2S_DAC */
