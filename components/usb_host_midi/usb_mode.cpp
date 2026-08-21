/*
 * osynth — the boot-time USB role decision (Session 35).
 *
 * C++ rather than C only so it can read the ParamStore directly; the driver
 * beside it stays C. Splitting it out also keeps the decision in one place:
 * main.cpp asks once, before either stack starts, and everything downstream
 * (the heartbeat, the BLE status opcode, the app's page) reads the answer
 * instead of re-deriving it.
 */
#include "usb_host_midi.h"

#include "esp_log.h"

#include "synth_config.h"
#include "synth_params.h"

static const char* TAG = "usb_mode";

namespace {
usb_mode_t s_active = USB_MODE_DEVICE;
bool s_resolved = false;
} // namespace

bool usb_mode_host_supported(void) { return SYNTH_ENABLE_USB_HOST != 0; }

usb_mode_t usb_mode_resolve(void) {
    if (s_resolved) return s_active;
    s_resolved = true;

#if SYNTH_ENABLE_USB_HOST
    osynth::ParamStore& ps = osynth::ParamStore::instance();
    const usb_mode_t want =
        ps.get(osynth::PID_USB_MODE) >= 0.5f ? USB_MODE_HOST : USB_MODE_DEVICE;
    s_active = want;
    ESP_LOGI(TAG, "role: %s (from usb.mode)",
             want == USB_MODE_HOST ? "host" : "device");
#else
    /* Either no USB-OTG on this target, or the USB sink is the audio clock
     * and the device role cannot be given up. The parameter is not registered
     * on such a build, so there is nothing to write back and nothing the app
     * can have asked for — the clamp below exists for the case where a build
     * *did* register it and the capability later went away (a config change
     * between flashes, with the old value still in NVS). */
    s_active = USB_MODE_DEVICE;
    if (osynth::ParamStore::instance().get(osynth::PID_USB_MODE) >= 0.5f) {
        ESP_LOGW(TAG,
                 "stored usb.mode = host, but this build cannot host "
                 "(usb_audio_clock:%d usb:%d) — staying a device",
                 SYNTH_USB_IS_AUDIO_CLOCK, SYNTH_ENABLE_USB);
        /* Write the truth back so the app's control shows the role the port
         * is actually in. A stored value nothing ever corrects would read as
         * host forever while the hardware sat in device mode. */
        osynth::ParamStore::instance().set(osynth::PID_USB_MODE, 0.0f,
                                           osynth::ParamOrigin::Internal);
    }
#endif
    return s_active;
}

usb_mode_t usb_mode_active(void) { return s_active; }
