/*
 * osynth — USB MIDI host class driver (Session 35).
 *
 * A minimal USB-MIDI 1.0 host on ESP-IDF's usb_host_lib. Deliberately not a
 * third-party component: the only thing a MIDI controller needs from a host
 * is enumeration plus one bulk IN endpoint kept permanently in flight, and
 * the packets that come back are the *same* 4-byte USB-MIDI event packets the
 * device side already receives — so the entire parsing half of the job was
 * already written (midi.c's usb_midi_rx), and this file only has to deliver
 * bytes to it.
 *
 * Class detection is the MIDIStreaming interface: audio class (0x01),
 * subclass 0x03. That interface's bulk IN endpoint is the controller's note
 * stream. Its bulk OUT endpoint is deliberately ignored — osynth sends
 * nothing to a controller (no MIDI out yet, on any transport), so claiming it
 * would only be state to maintain.
 *
 * Hub support is on, so a hub with several controllers works and they all
 * merge into the router. That costs nothing conceptually: the router is omni
 * already, so two keyboards are indistinguishable from one keyboard played
 * with more hands. What it does cost is per-device state, hence the slot
 * table rather than a single device handle.
 *
 * Tasks: the host library wants its own event loop, and the client wants
 * another. Both sit on core 0 with the rest of the control plane. Unlike the
 * device side there is no isochronous audio endpoint to starve here — a late
 * bulk transfer is jitter on a note, not a click in the output — so these run
 * well below the device stack's priority 21.
 */
#include "usb_host_midi.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "synth_config.h"

static const char* TAG = "usb_hmidi";

#if SYNTH_ENABLE_USB_HOST

#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

/* Control plane, core 0. Above the ~20 Hz BLE flush and the preset task,
 * below esp_timer (22) and far below the device stack's iso refill — see the
 * file header for why this one has slack the device side does not. */
#define HOST_TASK_PRIO   12
#define HOST_TASK_CORE   0
#define DAEMON_STACK     3072
#define CLIENT_STACK     4096

/* A USB MIDI device declares itself as the audio class with the
 * MIDIStreaming subclass — "USB Device Class Definition for MIDI Devices 1.0",
 * §3. Both are fixed by the specification.
 *
 * Spelled out here rather than taken from the host library's headers: those
 * moved out of IDF core into the managed `espressif/usb` component in 6.0, and
 * two numbers the USB-IF cannot change are not worth coupling this file to a
 * dependency's header layout. */
#define MIDI_INTF_CLASS    0x01 /* audio */
#define MIDI_INTF_SUBCLASS 0x03 /* MIDIStreaming */

/* How many controllers can be claimed at once. Four is a hub's worth of
 * keyboards and pad grids; past that the router could not tell them apart
 * anyway. Each slot costs a transfer buffer of one endpoint packet. */
#define MAX_MIDI_DEVICES 4

typedef struct {
    bool in_use;
    usb_device_handle_t dev;
    uint8_t addr;
    uint8_t intf_num;
    uint8_t alt;
    uint8_t ep_addr;
    uint16_t mps;
    usb_transfer_t* xfer;
    uint16_t vid;
    uint16_t pid;
    char product[32];
} midi_dev_t;

static midi_dev_t s_devs[MAX_MIDI_DEVICES];
static usb_host_client_handle_t s_client = NULL;
static TaskHandle_t s_daemon_task = NULL;
static TaskHandle_t s_client_task = NULL;
static usb_host_midi_rx_fn s_rx_cb = NULL;
static void* s_rx_ctx = NULL;

/* Guards the slot table against usb_host_midi_get_info() reading a slot the
 * client task is filling in or tearing down. Uncontended in practice — the
 * app asks for status at human speed — and never held across a USB call. */
static portMUX_TYPE s_devs_mux = portMUX_INITIALIZER_UNLOCKED;

void usb_host_midi_set_rx_callback(usb_host_midi_rx_fn fn, void* ctx) {
    s_rx_ctx = ctx;
    s_rx_cb = fn;
}

/* ---- descriptor walk --------------------------------------------------- */

/* Finds the first MIDIStreaming interface with a bulk IN endpoint. Returns
 * the interface descriptor and fills `*ep_out`, or NULL if this device has no
 * MIDI on it — which is the normal answer for the mouse someone plugged into
 * the hub, not an error.
 *
 * A flat walk of the configuration descriptor rather than IDF's parse helpers:
 * a descriptor block is a self-describing chain of {bLength, bDescriptorType,
 * ...} records, so walking it by bLength is both shorter than driving the
 * helpers' in/out offset convention and impossible to get subtly wrong. It
 * also skips the class-specific descriptors (CS_INTERFACE / CS_ENDPOINT, which
 * a MIDI device has plenty of) for free — they are simply types this does not
 * ask about.
 *
 * The audio class's endpoint descriptors are 9 bytes rather than the usual 7,
 * but the standard fields sit at the same offsets, so the usb_ep_desc_t
 * overlay reads them correctly and bLength still advances the walk properly. */
static const usb_intf_desc_t* find_midi_intf(const usb_config_desc_t* cfg,
                                             const usb_ep_desc_t** ep_out) {
    const uint8_t* p = (const uint8_t*)cfg;
    const uint16_t total = cfg->wTotalLength;
    const usb_intf_desc_t* cur = NULL; /* the MIDI interface we are inside */

    for (uint16_t i = 0; i + 2 <= total;) {
        const uint8_t len = p[i];
        const uint8_t type = p[i + 1];
        if (len < 2 || (uint32_t)i + len > total) break; /* malformed */

        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= sizeof(usb_intf_desc_t)) {
            const usb_intf_desc_t* intf = (const usb_intf_desc_t*)(p + i);
            /* Entering an interface ends the previous one, so a non-MIDI
             * interface clears `cur` and its endpoints are ignored. */
            cur = (intf->bInterfaceClass == MIDI_INTF_CLASS &&
                   intf->bInterfaceSubClass == MIDI_INTF_SUBCLASS)
                      ? intf
                      : NULL;
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cur != NULL &&
                   len >= sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(p + i);
            if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK &&
                USB_EP_DESC_GET_EP_DIR(ep) != 0 /* IN */) {
                *ep_out = ep;
                return cur;
            }
        }
        i = (uint16_t)(i + len);
    }
    return NULL;
}

/* Renders a device's product string into `out`, falling back to the ids when
 * the descriptor has no string — plenty of cheap controllers ship without
 * one, and "USB MIDI 1234:5678" still identifies it on the app's page. The
 * string descriptor is UTF-16LE; this takes the low byte of each unit, which
 * is exactly right for the ASCII these names are in practice and produces a
 * readable approximation otherwise. */
static void device_product_name(const usb_device_info_t* info, uint16_t vid,
                                uint16_t pid, char* out, size_t out_len) {
    out[0] = '\0';
    const usb_str_desc_t* s = info->str_desc_product;
    if (s != NULL && s->bLength > 2) {
        const size_t units = (size_t)(s->bLength - 2) / 2;
        size_t n = 0;
        for (size_t i = 0; i < units && n + 1 < out_len; ++i) {
            const uint16_t u = s->wData[i];
            out[n++] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
        }
        out[n] = '\0';
        /* Trailing spaces are common in these descriptors. */
        while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    }
    if (out[0] == '\0') {
        snprintf(out, out_len, "USB MIDI %04x:%04x", vid, pid);
    }
}

/* ---- transfers --------------------------------------------------------- */

static void submit_rx(midi_dev_t* d);

/* Bulk IN completion, on the client task. A USB-MIDI 1.0 bulk packet is a
 * whole number of 4-byte event packets; a device that sends a partial one is
 * out of spec, and the remainder is dropped rather than buffered across
 * transfers, because there is no legitimate sender to accommodate.
 *
 * Zero-length completions are normal and frequent: that is what a controller
 * with nothing to say answers with, and it must be resubmitted, not treated
 * as an end of stream. */
static void rx_done(usb_transfer_t* xfer) {
    midi_dev_t* d = (midi_dev_t*)xfer->context;
    /* The slot was released while this was in flight (an unplug: the flush in
     * release_slot cancels the transfer, and the cancellation is delivered
     * here). There is no device left to resubmit to, and the bytes — if any
     * arrived — belong to a controller that is gone. */
    if (!d->in_use) return;

    switch (xfer->status) {
        case USB_TRANSFER_STATUS_COMPLETED: {
            const usb_host_midi_rx_fn cb = s_rx_cb;
            const int n = xfer->actual_num_bytes & ~3; /* whole packets only */
            if (cb != NULL) {
                for (int i = 0; i < n; i += 4) {
                    cb(xfer->data_buffer + i, s_rx_ctx);
                }
            }
            submit_rx(d);
            break;
        }
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            /* Unplugged or shutting down: DEV_GONE does the teardown, and
             * resubmitting here would only race it. */
            break;
        case USB_TRANSFER_STATUS_STALL:
            /* A halted endpoint stays halted until cleared, so without this a
             * controller that hiccuped once would be mute until replugged.
             * Flush before clear: the host library requires a halted endpoint
             * to be emptied before it will clear the halt. */
            ESP_LOGW(TAG, "ep 0x%02x stalled; clearing", d->ep_addr);
            (void)usb_host_endpoint_flush(d->dev, d->ep_addr);
            if (usb_host_endpoint_clear(d->dev, d->ep_addr) == ESP_OK) {
                submit_rx(d);
            }
            break;
        default:
            ESP_LOGW(TAG, "transfer status %d on ep 0x%02x", (int)xfer->status,
                     d->ep_addr);
            submit_rx(d);
            break;
    }
}

static void submit_rx(midi_dev_t* d) {
    d->xfer->num_bytes = d->mps;
    d->xfer->device_handle = d->dev;
    d->xfer->bEndpointAddress = d->ep_addr;
    d->xfer->callback = rx_done;
    d->xfer->context = d;
    const esp_err_t err = usb_host_transfer_submit(d->xfer);
    if (err != ESP_OK) {
        /* Nothing to retry against: a device that will not take a transfer is
         * on its way to DEV_GONE, which is what cleans the slot up. */
        ESP_LOGW(TAG, "submit failed: %s", esp_err_to_name(err));
    }
}

/* ---- attach / detach --------------------------------------------------- */

static midi_dev_t* free_slot(void) {
    for (int i = 0; i < MAX_MIDI_DEVICES; ++i) {
        if (!s_devs[i].in_use) return &s_devs[i];
    }
    return NULL;
}

static void release_slot(midi_dev_t* d) {
    if (d->xfer != NULL) {
        /* Halting and flushing before releasing the interface is what makes
         * the release legal while a transfer is queued: the endpoint stops and
         * anything outstanding is canceled. Errors are expected and ignored —
         * on an unplug the device is simply gone. */
        (void)usb_host_endpoint_halt(d->dev, d->ep_addr);
        (void)usb_host_endpoint_flush(d->dev, d->ep_addr);
    }
    (void)usb_host_interface_release(s_client, d->dev, d->intf_num);
    (void)usb_host_device_close(s_client, d->dev);

    /* The transfer deliberately outlives the device. A canceled transfer's
     * callback is delivered through usb_host_client_handle_events(), and this
     * function runs *inside* that call — so freeing the transfer here could
     * race the very callback the flush above just queued, and free the buffer
     * rx_done is about to be handed. Instead it stays with the slot and is
     * reused (or resized) by the next device to land in it, by which point any
     * such callback has long since drained. The cost is one endpoint packet of
     * RAM per slot that has ever been used; the alternative is a
     * use-after-free that only shows up when someone unplugs mid-phrase.
     *
     * Clearing in_use is also what tells a late rx_done to do nothing. */
    usb_transfer_t* const keep = d->xfer;
    portENTER_CRITICAL(&s_devs_mux);
    memset(d, 0, sizeof(*d));
    portEXIT_CRITICAL(&s_devs_mux);
    d->xfer = keep;
}

static void device_attached(uint8_t addr) {
    midi_dev_t* d = free_slot();
    if (d == NULL) {
        ESP_LOGW(TAG, "device at %u ignored: all %d slots in use", addr,
                 MAX_MIDI_DEVICES);
        return;
    }

    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open device at %u", addr);
        return;
    }

    const usb_config_desc_t* cfg = NULL;
    const usb_ep_desc_t* ep = NULL;
    const usb_intf_desc_t* intf = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) == ESP_OK) {
        intf = find_midi_intf(cfg, &ep);
    }
    if (intf == NULL) {
        /* Not a MIDI device. Perfectly normal behind a hub — close it and let
         * whatever else is on the bus have it. */
        ESP_LOGI(TAG, "device at %u has no MIDIStreaming interface; ignoring",
                 addr);
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    if (usb_host_interface_claim(s_client, dev, intf->bInterfaceNumber,
                                 intf->bAlternateSetting) != ESP_OK) {
        ESP_LOGW(TAG, "cannot claim interface %u on device %u",
                 intf->bInterfaceNumber, addr);
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    /* Reuse the slot's previous buffer when it is big enough (see
     * release_slot: it is kept precisely so it can be reused here, safely,
     * long after any cancellation callback has drained). Only a device
     * demanding a larger packet than the last one costs an allocation. */
    const uint16_t mps = USB_EP_DESC_GET_MPS(ep);
    usb_transfer_t* xfer = d->xfer;
    if (xfer != NULL && xfer->data_buffer_size < mps) {
        usb_host_transfer_free(xfer);
        xfer = NULL;
        d->xfer = NULL;
    }
    if (xfer == NULL && usb_host_transfer_alloc(mps, 0, &xfer) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for a %u byte transfer", mps);
        (void)usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        (void)usb_host_device_close(s_client, dev);
        return;
    }

    uint16_t vid = 0, pid = 0;
    const usb_device_desc_t* dev_desc = NULL;
    if (usb_host_get_device_descriptor(dev, &dev_desc) == ESP_OK) {
        vid = dev_desc->idVendor;
        pid = dev_desc->idProduct;
    }
    /* Zeroed, not merely NUL-terminated: the whole array is memcpy'd into the
     * slot below, and snprintf leaves everything past its NUL untouched. */
    char product[sizeof(d->product)] = {0};
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        device_product_name(&info, vid, pid, product, sizeof(product));
    } else {
        snprintf(product, sizeof(product), "USB MIDI %04x:%04x", vid, pid);
    }

    portENTER_CRITICAL(&s_devs_mux);
    d->in_use = true;
    d->dev = dev;
    d->addr = addr;
    d->intf_num = intf->bInterfaceNumber;
    d->alt = intf->bAlternateSetting;
    d->ep_addr = ep->bEndpointAddress;
    d->mps = mps;
    d->xfer = xfer;
    d->vid = vid;
    d->pid = pid;
    memcpy(d->product, product, sizeof(d->product));
    portEXIT_CRITICAL(&s_devs_mux);

    ESP_LOGI(TAG, "attached: %s (%04x:%04x) intf %u ep 0x%02x mps %u", product,
             vid, pid, d->intf_num, d->ep_addr, mps);
    submit_rx(d);
}

static void device_gone(usb_device_handle_t dev) {
    for (int i = 0; i < MAX_MIDI_DEVICES; ++i) {
        if (s_devs[i].in_use && s_devs[i].dev == dev) {
            ESP_LOGI(TAG, "detached: %s", s_devs[i].product);
            release_slot(&s_devs[i]);
            return;
        }
    }
    /* A device we never claimed (the mouse behind the hub). It still has to be
     * closed, or the host library will not free its address. */
    (void)usb_host_device_close(s_client, dev);
}

static void client_event_cb(const usb_host_client_event_msg_t* msg, void* arg) {
    (void)arg;
    switch (msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            device_attached(msg->new_dev.address);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            device_gone(msg->dev_gone.dev_hdl);
            break;
        default:
            break;
    }
}

/* ---- tasks ------------------------------------------------------------- */

static void daemon_task(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            /* Only reachable if the client task died; nothing here can
             * recover it, so free what is freeable and keep the loop alive
             * rather than leaving the library wedged. */
            (void)usb_host_device_free_all();
        }
    }
}

static void client_task(void* arg) {
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

esp_err_t usb_host_midi_init(void) {
    if (usb_mode_active() != USB_MODE_HOST) {
        ESP_LOGE(TAG, "not in host mode; refusing to start");
        return ESP_ERR_INVALID_STATE;
    }

    /* Matches the framework's own usb_host_lib example. LOWMED rather than a
     * specific level so the allocator can place the interrupt wherever it has
     * room — this shares a chip with I2S, SDIO to the C6 and the BLE stack.
     *
     * ---------------------------------------------------------------------
     * UNVERIFIED ON THE P4 — which controller the host takes.
     *
     * usb_host_config_t has no controller or root-port field, so unlike the
     * device side (usb_dev.c:78-102, where the PHY target and the TinyUSB port
     * are one decision and picking them apart enumerates nothing while still
     * reporting success) there is nothing here to steer. The host library
     * chooses, and `skip_phy_setup = false` lets it set the PHY up to match.
     *
     * On the S3 that is unambiguous: one OTG controller. On the P4 there are
     * two, and IDF 6.0 documents that only one may host at a time
     * (docs/en/api-reference/peripherals/usb_host.rst:36) without saying which
     * one it picks. This board's OTG socket is on the *high-speed* controller
     * — the full-speed one's GPIO24/25 reach no connector — so if the library
     * defaults to full speed, host mode will come up clean, log this line, and
     * see nothing on the bus. That symptom is the same one the device side
     * hit, and the same place to look: if it appears, the fix belongs here.
     *
     * On the S3 there is a second caveat, from the same docs (:539): USB-OTG
     * and USB-Serial-JTAG share one PHY. osynth's console is UART0 and the
     * devkit is flashed over its UART port, so nothing contends — but a build
     * that moves the console to USB-Serial-JTAG cannot also host.
     * --------------------------------------------------------------------- */
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreatePinnedToCore(daemon_task, "usb_hd", DAEMON_STACK, NULL,
                                HOST_TASK_PRIO, &s_daemon_task,
                                HOST_TASK_CORE) != pdPASS) {
        (void)usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async =
            {
                .client_event_callback = client_event_cb,
                .callback_arg = NULL,
            },
    };
    err = usb_host_client_register(&client_config, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client register failed: %s", esp_err_to_name(err));
        vTaskDelete(s_daemon_task);
        s_daemon_task = NULL;
        (void)usb_host_uninstall();
        return err;
    }

    if (xTaskCreatePinnedToCore(client_task, "usb_hc", CLIENT_STACK, NULL,
                                HOST_TASK_PRIO, &s_client_task,
                                HOST_TASK_CORE) != pdPASS) {
        (void)usb_host_client_deregister(s_client);
        s_client = NULL;
        vTaskDelete(s_daemon_task);
        s_daemon_task = NULL;
        (void)usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "up: MIDI host, up to %d devices, hubs %s", MAX_MIDI_DEVICES,
#if CONFIG_USB_HOST_HUBS_SUPPORTED
             "on"
#else
             "off"
#endif
    );
    return ESP_OK;
}

void usb_host_midi_get_info(usb_host_midi_info_t* out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_devs_mux);
    for (int i = 0; i < MAX_MIDI_DEVICES; ++i) {
        if (!s_devs[i].in_use) continue;
        if (out->attached == 0) {
            out->vid = s_devs[i].vid;
            out->pid = s_devs[i].pid;
            memcpy(out->product, s_devs[i].product, sizeof(out->product));
        }
        out->attached++;
    }
    portEXIT_CRITICAL(&s_devs_mux);
}

#else /* !SYNTH_ENABLE_USB_HOST */

esp_err_t usb_host_midi_init(void) {
    ESP_LOGI(TAG, "USB host not available on this build — compiled out");
    return ESP_ERR_NOT_SUPPORTED;
}

void usb_host_midi_set_rx_callback(usb_host_midi_rx_fn fn, void* ctx) {
    (void)fn;
    (void)ctx;
}

void usb_host_midi_get_info(usb_host_midi_info_t* out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
}

#endif /* SYNTH_ENABLE_USB_HOST */
