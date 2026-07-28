/*
 * osynth — internal 8-bit DAC sink (classic ESP32 only; default output
 * there when the I2S DAC flag is off).
 *
 * dac_continuous DMA in alternating-channel mode: interleaved L/R bytes go
 * to channel 0 (GPIO25) / channel 1 (GPIO26). int16 samples are TPDF-
 * dithered (±1 LSB of the 8-bit target, S17) before the truncation to
 * offset-binary 8-bit — the coarse quantization error becomes a soft
 * constant hiss instead of correlated crackle. Output is DC-biased around
 * ~1.65 V — AC-couple it (series capacitor) before an amp or line-in, see
 * docs/HARDWARE.md. At start the bias is ramped from 0 V over ~24 ms so
 * power-up doesn't thump the speaker.
 */
#include "audio_sink.h"

#if SYNTH_HAS_INTERNAL_DAC

#include "driver/dac_continuous.h"
#include "esp_log.h"

static const char* TAG = "sink_dac";

namespace {

dac_continuous_handle_t s_dac = nullptr;
uint8_t s_buf[SYNTH_BLOCK_SIZE * 2]; /* one block, interleaved L/R bytes */
uint32_t s_rng = 0x1234abcdu;        /* TPDF dither state */

/* Soften the 0 V -> 1.65 V bias step: feed a linear ramp up to the 8-bit
 * midpoint before the first real block. */
void dac_bias_ramp(void) {
    constexpr int kSteps = 128; /* 0..127, ~9 frames each ≈ 24 ms at 48 kHz */
    const size_t frames_per_step =
        (SYNTH_SAMPLE_RATE / 1000) * 24 / kSteps + 1;
    for (int v = 0; v < kSteps; ++v) {
        size_t n = frames_per_step * 2;
        if (n > sizeof(s_buf)) n = sizeof(s_buf);
        for (size_t i = 0; i < n; ++i) s_buf[i] = (uint8_t)v;
        uint8_t* p = s_buf;
        size_t remaining = n;
        while (remaining > 0) {
            size_t loaded = 0;
            if (dac_continuous_write(s_dac, p, remaining, &loaded, 100) !=
                ESP_OK) {
                return; /* best effort — worst case is the old thump */
            }
            p += loaded;
            remaining -= loaded;
        }
    }
}

esp_err_t dac_start(void) {
    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_ALL,
        .desc_num = 4,
        .buf_size = sizeof(s_buf),
        .freq_hz = SYNTH_SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_ALTER, /* byte 0 -> ch0 (L), byte 1 -> ch1 (R) */
    };
    esp_err_t err = dac_continuous_new_channels(&cfg, &s_dac);
    if (err != ESP_OK) return err;

    err = dac_continuous_enable(s_dac);
    if (err != ESP_OK) {
        dac_continuous_del_channels(s_dac);
        s_dac = nullptr;
        return err;
    }

    dac_bias_ramp();
    ESP_LOGI(TAG, "up: %d Hz, 8-bit + TPDF dither, L=GPIO25 R=GPIO26",
             SYNTH_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t SYNTH_RENDER_IRAM dac_write(const int16_t* interleaved,
                                      size_t frames) {
    const size_t n = frames * 2;
    for (size_t i = 0; i < n; ++i) {
        /* TPDF: two uniform bytes sum to a triangle over ±1 8-bit LSB
         * (= ±256 in the int16 domain) */
        uint32_t x = s_rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s_rng = x;
        const int32_t d = (int32_t)(x & 0xFFu) + (int32_t)((x >> 8) & 0xFFu) - 255;
        int32_t s = (int32_t)interleaved[i] + d;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        s_buf[i] = (uint8_t)((s >> 8) + 128);
    }

    uint8_t* p = s_buf;
    size_t remaining = n;
    while (remaining > 0) {
        size_t loaded = 0;
        esp_err_t err = dac_continuous_write(s_dac, p, remaining, &loaded, 1000);
        if (err != ESP_OK) return err;
        p += loaded;
        remaining -= loaded;
    }
    return ESP_OK;
}

const audio_sink_t s_sink = {"dac", dac_start, dac_write};

} // namespace

const audio_sink_t* audio_sink_dac(void) { return &s_sink; }

#endif /* SYNTH_HAS_INTERNAL_DAC */
