/*
 * osynth — USB composite device: UAC2 audio source + MIDI (ESP32-S3).
 *
 * The synth appears to the host as a 2-in audio interface (48 kHz / 16-bit /
 * stereo capture device) plus a MIDI port. The audio task pushes rendered
 * blocks into the TinyUSB EP-IN FIFO through usb_dev_audio_write(); the class
 * driver drains one iso packet per USB frame, so while the host streams, the
 * USB clock paces the audio task (audio_io/sink_usb.cpp).
 *
 * Control-request handling (clock entity) follows esp-iot-solution's
 * usb_device_uac (reference copy: tools/ref/usb_device_uac/).
 *
 * On the classic ESP32 (no USB-OTG hardware) this compiles to a no-op stub.
 */
#include "usb_dev.h"

#include <string.h>

#include "esp_log.h"

#include "synth_config.h"

static const char* TAG = "usb_dev";

#if SYNTH_ENABLE_USB

#include <stdatomic.h>

#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* Core 0, above the control-plane tasks but below esp_timer (22): the iso
 * EP refill happens in tud_task, so it must not starve. */
#define USB_TASK_PRIO  21
#define USB_TASK_CORE  0
#define USB_TASK_STACK 4096

static usb_phy_handle_t s_phy = NULL;
static TaskHandle_t s_task = NULL;
static atomic_bool s_streaming = false;
static usb_dev_midi_rx_fn s_midi_cb = NULL;
static void* s_midi_ctx = NULL;

/* ---- Stream health (see usb_dev_audio_health_t) ---- */

/* Same read-and-reset problem as audio_io's peak meters: fifo_min/fifo_max are
 * read-modify-write on both sides, so a reset landing between the audio task's
 * compare and its store would either drop a block or resurrect a cleared
 * extreme. Uncontended on every block. */
static portMUX_TYPE s_health_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped_blocks = 0;
static uint16_t s_fifo_min = UINT16_MAX; /* sentinel: no sample yet */
static uint16_t s_fifo_max = 0;

/* Records one dropped block and yields the "accepted nothing" result, so the
 * several failure paths in usb_dev_audio_write() stay one-liners. */
static size_t note_drop(void) {
    portENTER_CRITICAL(&s_health_mux);
    s_dropped_blocks++;
    portEXIT_CRITICAL(&s_health_mux);
    return 0;
}

static void usb_task(void* arg) {
    (void)arg;
    for (;;) {
        tud_task();
    }
}

esp_err_t usb_dev_init(void) {
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;

    /* The PHY target and the root-hub port in tusb_config.h are one decision,
     * not two, and getting them out of step fails in two different ways — both
     * of which this target has actually done.
     *
     * Name a target the port does not match and nothing works at all: with the
     * PHY on the UTMI (high-speed) controller and TinyUSB on port 0
     * (full-speed), tusb_init() returned false and app_main aborted on the
     * ESP_ERROR_CHECK around this function. That is the state a zeroed
     * otg_speed produces by itself, because usb_new_phy() carries a
     * backward-compatibility rule for the P4 (usb_phy.c): in DEVICE mode with
     * the speed UNDEFINED or HIGH it discards the requested target and takes
     * UTMI, announcing it only as
     *
     *     W usb_phy: Using UTMI PHY instead of requested internal PHY
     *
     * Match them to the wrong controller and it fails silently instead, which
     * is worse: the internal FSLS PHY on GPIO24/25 paired with port 0 brings
     * the stack up perfectly, logs "UAC2+MIDI composite up", and enumerates
     * nowhere, because on this board those pins reach no USB socket. Only the
     * high-speed controller is wired to one.
     *
     * So both fields are set explicitly on both branches. otg_speed is read
     * nowhere else in usb_phy.c — its only job is to stop that override firing
     * behind your back — and the S3 branch keeps the full-speed PHY it has
     * always used, since the override is inside #if CONFIG_IDF_TARGET_ESP32P4. */
    const usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
#if OSYNTH_USB_HS
        /* UTMI is the high-speed controller's PHY, which the DWC2 port table
         * puts on root-hub port 1 — the port tusb_config.h selects here. The
         * two must name the same controller: a PHY on one and TinyUSB on the
         * other enumerates nothing while still reporting success. */
        .otg_speed = USB_PHY_SPEED_HIGH,
        .target = USB_PHY_TARGET_UTMI,
#else
        .otg_speed = USB_PHY_SPEED_FULL,
        .target = USB_PHY_TARGET_INT,
#endif
    };
    esp_err_t err = usb_new_phy(&phy_conf, &s_phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!tusb_init()) {
        ESP_LOGE(TAG, "tusb_init failed");
        return ESP_FAIL;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(usb_task, "usb", USB_TASK_STACK,
                                            NULL, USB_TASK_PRIO, &s_task,
                                            USB_TASK_CORE);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "UAC2+MIDI composite up: %d Hz / %d-bit / %d ch, FIFO %d ms",
             OSYNTH_USB_SAMPLE_RATE, OSYNTH_USB_BITS_PER_SAMPLE,
             OSYNTH_USB_CHANNELS, OSYNTH_USB_FIFO_MS);
    return ESP_OK;
}

bool usb_dev_mounted(void) { return tud_mounted(); }

bool usb_dev_audio_streaming(void) {
    return atomic_load_explicit(&s_streaming, memory_order_relaxed);
}

size_t usb_dev_audio_write(const int16_t* interleaved, size_t frames,
                           uint32_t max_wait_ms) {
    const size_t want =
        frames * OSYNTH_USB_CHANNELS * OSYNTH_USB_BYTES_PER_SAMPLE;
    if (want > CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ) return 0;

    /* The EP-IN FIFO is single-producer (this call, from the audio task) /
     * single-consumer (the class driver), so polling remaining space and
     * writing the whole block is race-free. A stream teardown can clear the
     * FIFO concurrently; worst case one garbled block, tolerated. */
    tu_fifo_t* ff = tud_audio_n_get_ep_in_ff(0);
    if (ff == NULL) return note_drop(); /* audio function not set up yet */

    /* Occupancy is sampled here, before the wait, because this is the one
     * point that runs at a fixed phase against the audio clock — and where
     * the level parks is the readout of whether the class driver's packet
     * steering is holding (see usb_dev_audio_health_t). */
    const uint16_t level = tu_fifo_count(ff);
    portENTER_CRITICAL(&s_health_mux);
    if (level < s_fifo_min) s_fifo_min = level;
    if (level > s_fifo_max) s_fifo_max = level;
    portEXIT_CRITICAL(&s_health_mux);

    uint32_t waited_ms = 0;
    while (tu_fifo_remaining(ff) < want) {
        if (!usb_dev_audio_streaming()) return note_drop();
        if (waited_ms >= max_wait_ms) return note_drop();
        vTaskDelay(1);
        waited_ms += portTICK_PERIOD_MS;
    }
    if (!usb_dev_audio_streaming()) return note_drop();

    return tud_audio_n_write(0, interleaved, (uint16_t)want) /
           (OSYNTH_USB_CHANNELS * OSYNTH_USB_BYTES_PER_SAMPLE);
}

void usb_dev_audio_get_health(usb_dev_audio_health_t* out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    out->streaming = usb_dev_audio_streaming();
    out->fifo_depth = (uint16_t)CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ;

    portENTER_CRITICAL(&s_health_mux);
    out->dropped_blocks = s_dropped_blocks;
    out->fifo_min = (s_fifo_min == UINT16_MAX) ? 0 : s_fifo_min;
    out->fifo_max = s_fifo_max;
    s_fifo_min = UINT16_MAX;
    s_fifo_max = 0;
    portEXIT_CRITICAL(&s_health_mux);
}

void usb_dev_midi_set_rx_callback(usb_dev_midi_rx_fn fn, void* ctx) {
    s_midi_ctx = ctx;
    s_midi_cb = fn;
}

/* ---- TinyUSB device callbacks (resolved by the linker) ---- */

void tud_mount_cb(void) {
    atomic_store(&s_streaming, false);
    ESP_LOGI(TAG, "mounted by host");
}

void tud_umount_cb(void) {
    atomic_store(&s_streaming, false);
    ESP_LOGI(TAG, "unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    atomic_store(&s_streaming, false);
    ESP_LOGI(TAG, "suspended");
}

void tud_resume_cb(void) { ESP_LOGI(TAG, "resumed"); }

/* Host selected an alternate setting on the audio streaming interface:
 * alt 1 = start streaming. */
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING && alt != 0) {
        atomic_store(&s_streaming, true);
        ESP_LOGI(TAG, "host opened the audio stream");
    }
    return true;
}

/* Streaming interface back to alt 0 = stream closed. */
bool tud_audio_set_itf_close_EP_cb(uint8_t rhport,
                                   tusb_control_request_t const* p_request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING && alt == 0) {
        atomic_store(&s_streaming, false);
        ESP_LOGI(TAG, "host closed the audio stream");
    }
    return true;
}

/* Clock entity GET requests: fixed single sample rate. */
static bool clock_get_request(uint8_t rhport,
                              audio_control_request_t const* request) {
    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t curf = {
                (int32_t)tu_htole32(OSYNTH_USB_SAMPLE_RATE)};
            /* Answering through this helper is load-bearing, not just
             * convenient: it stores the CUR value it carries into TinyUSB's
             * sample_rate_tx regardless of transfer direction, and
             * audiod_calc_tx_packet_sz() needs that non-zero before the host
             * selects alt 1 or the packet steering that absorbs clock drift
             * never arms for the session. Our clock source advertises the
             * frequency read-only, so this read is the only chance it gets —
             * a compliant host never issues the SET below. */
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, (tusb_control_request_t const*)request, &curf,
                sizeof(curf));
        }
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) rangef = {
                .wNumSubRanges = tu_htole16(1),
            };
            rangef.subrange[0].bMin = (int32_t)OSYNTH_USB_SAMPLE_RATE;
            rangef.subrange[0].bMax = (int32_t)OSYNTH_USB_SAMPLE_RATE;
            rangef.subrange[0].bRes = 0;
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, (tusb_control_request_t const*)request, &rangef,
                sizeof(rangef));
        }
    } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
               request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport, (tusb_control_request_t const*)request, &cur_valid,
            sizeof(cur_valid));
    }
    ESP_LOGD(TAG, "clock get not supported: selector %u request %u",
             request->bControlSelector, request->bRequest);
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const* p_request) {
    audio_control_request_t const* request =
        (audio_control_request_t const*)p_request;
    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return clock_get_request(rhport, request);
    }
    ESP_LOGD(TAG, "get request not handled: entity %u", request->bEntityID);
    return false; /* stall */
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const* p_request,
                                 uint8_t* buf) {
    (void)rhport;
    audio_control_request_t const* request =
        (audio_control_request_t const*)p_request;

    /* Accept a SET of the (only) sample rate; reject anything else. */
    if (request->bEntityID == UAC2_ENTITY_CLOCK &&
        request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ &&
        request->bRequest == AUDIO_CS_REQ_CUR &&
        request->wLength == sizeof(audio_control_cur_4_t)) {
        uint32_t rate = (uint32_t)((audio_control_cur_4_t const*)buf)->bCur;
        return rate == OSYNTH_USB_SAMPLE_RATE;
    }
    ESP_LOGD(TAG, "set request not handled: entity %u", request->bEntityID);
    return false; /* stall */
}

/* Incoming MIDI. Until the Session-4 parser registers a callback, packets are
 * decoded to the log so enumeration + traffic can be verified end to end.
 * (The FIFO must always be drained, or the host-side sender blocks.) */
void tud_midi_rx_cb(uint8_t itf) {
    (void)itf;
    uint8_t packet[4];
    while (tud_midi_packet_read(packet)) {
        if (s_midi_cb != NULL) {
            s_midi_cb(packet, s_midi_ctx);
            continue;
        }
        const uint8_t status = packet[1] & 0xF0;
        const uint8_t chan = packet[1] & 0x0F;
        if (status == 0x90 && packet[3] != 0) {
            ESP_LOGI(TAG, "midi in: note on  ch%2u note %3u vel %3u", chan + 1,
                     packet[2], packet[3]);
        } else if (status == 0x80 || (status == 0x90 && packet[3] == 0)) {
            ESP_LOGI(TAG, "midi in: note off ch%2u note %3u", chan + 1,
                     packet[2]);
        } else {
            ESP_LOGI(TAG, "midi in: %02X %02X %02X %02X", packet[0], packet[1],
                     packet[2], packet[3]);
        }
    }
}

#else /* !SYNTH_ENABLE_USB */

esp_err_t usb_dev_init(void) {
    ESP_LOGI(TAG, "no USB-OTG on this target (classic ESP32) — compiled out");
    return ESP_OK;
}

bool usb_dev_mounted(void) { return false; }
bool usb_dev_audio_streaming(void) { return false; }

size_t usb_dev_audio_write(const int16_t* interleaved, size_t frames,
                           uint32_t max_wait_ms) {
    (void)interleaved;
    (void)frames;
    (void)max_wait_ms;
    return 0;
}

void usb_dev_audio_get_health(usb_dev_audio_health_t* out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
}

void usb_dev_midi_set_rx_callback(usb_dev_midi_rx_fn fn, void* ctx) {
    (void)fn;
    (void)ctx;
}

#endif /* SYNTH_ENABLE_USB */
