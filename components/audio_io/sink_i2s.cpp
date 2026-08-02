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

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "soc/gpio_num.h"

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
            .invert_flags = {},
        },
    };

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
        std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        std_cfg.slot_cfg.ws_width = 32;
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

    if (with_mclk) {
        ESP_LOGI(TAG,
                 "up: %d Hz, 16-bit data in 32-bit slots (bclk %u Hz = 64fs, "
                 "mclk %u Hz = 256fs), %s",
                 SYNTH_SAMPLE_RATE, (unsigned)(SYNTH_SAMPLE_RATE * 64u),
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
    return ESP_OK;
}

esp_err_t i2s_start(void) {
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
