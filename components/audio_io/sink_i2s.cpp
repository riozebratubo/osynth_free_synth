/*
 * osynth — I2S sink for an external DAC (behind SYNTH_ENABLE_I2S_DAC).
 *
 * Standard-mode master, 16-bit stereo Philips frames, no MCLK (a PCM5102A
 * runs from BCK via its internal PLL — tie its SCK pin to GND). The blocking
 * i2s_channel_write() against the DMA queue is what paces the audio task.
 * Pinout per target: docs/HARDWARE.md.
 */
#include "audio_sink.h"

#if SYNTH_ENABLE_I2S_DAC

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "soc/gpio_num.h"

static const char* TAG = "sink_i2s";

#if CONFIG_IDF_TARGET_ESP32S3
#define OSYNTH_I2S_BCLK GPIO_NUM_16
#define OSYNTH_I2S_WS   GPIO_NUM_17
#define OSYNTH_I2S_DOUT GPIO_NUM_18
#else /* classic ESP32 */
#define OSYNTH_I2S_BCLK GPIO_NUM_27
#define OSYNTH_I2S_WS   GPIO_NUM_32
#define OSYNTH_I2S_DOUT GPIO_NUM_33
#endif

namespace {

i2s_chan_handle_t s_tx = nullptr;

esp_err_t i2s_start(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;                 /* ~5.3 ms of buffer at 48 kHz */
    chan_cfg.dma_frame_num = SYNTH_BLOCK_SIZE; /* one render block per buffer */
    chan_cfg.auto_clear_after_cb = true;       /* silence, not stale data, on starve */

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK) return err;

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
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return err;
    }

    ESP_LOGI(TAG, "up: %d Hz 16-bit stereo, bclk %d ws %d dout %d",
             SYNTH_SAMPLE_RATE, OSYNTH_I2S_BCLK, OSYNTH_I2S_WS, OSYNTH_I2S_DOUT);
    return ESP_OK;
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

#endif /* SYNTH_ENABLE_I2S_DAC */
