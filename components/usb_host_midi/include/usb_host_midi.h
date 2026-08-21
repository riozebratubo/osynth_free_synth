/*
 * osynth — USB host: MIDI class driver + the boot-time USB role decision
 * (Session 35). The other half of usb_dev: that one makes the synth a device
 * on someone's computer, this one makes it the host a MIDI controller plugs
 * into.
 *
 * The two cannot coexist. There is one OTG controller reaching one socket on
 * every board osynth runs on (on the P4 the full-speed controller's pins go to
 * no connector at all — see usb_dev/tusb/usb_descriptors.h), and a controller
 * is either a host or a device, never both. So the role is chosen once at
 * boot from the persisted `usb.mode` parameter and changing it costs a
 * restart. main.cpp calls usb_mode_resolve() before either stack starts and
 * brings up exactly one of them.
 *
 * Incoming MIDI lands in the same router as everything else: the USB-MIDI 1.0
 * wire format is the same 4-byte event packet the device side already
 * receives, so midi.c parses host and device packets with one function.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- USB role ---------------------------------------------------------- */

typedef enum {
    USB_MODE_DEVICE = 0, /* UAC2 audio + MIDI, enumerated by a computer */
    USB_MODE_HOST = 1,   /* drives a USB MIDI controller */
} usb_mode_t;

/* Whether this build can take the host role at all. False on a target with no
 * USB-OTG, and false on a build where the USB sink is the audio clock — there
 * the device role is load-bearing and giving it up would leave the synth
 * silent (SYNTH_USB_IS_AUDIO_CLOCK in synth_config.h explains why). */
bool usb_mode_host_supported(void);

/* Reads `usb.mode` from the ParamStore (restored from NVS by persist_init)
 * and settles the role for this boot, clamping to device where host is not
 * supported. Call once, before either USB stack starts; the answer is latched
 * and every later call returns it unchanged.
 *
 * A clamp also writes the parameter back, so the app's control reflects what
 * actually happened rather than what was asked for — a stored `host` that
 * this build cannot honour would otherwise read as host forever while the
 * port sat in device mode. */
usb_mode_t usb_mode_resolve(void);

/* The role this boot came up in. USB_MODE_DEVICE before usb_mode_resolve(). */
usb_mode_t usb_mode_active(void);

/* ---- host-side MIDI ---------------------------------------------------- */

/* Starts the USB host library and the MIDI class driver. Only legal when
 * usb_mode_active() == USB_MODE_HOST; returns ESP_ERR_INVALID_STATE
 * otherwise. Registers no RX callback of its own — midi_init() does that. */
esp_err_t usb_host_midi_init(void);

/* Incoming MIDI, one 4-byte USB-MIDI event packet per call, invoked from the
 * host driver's task. Identical contract to usb_dev_midi_set_rx_callback():
 * same wire format, same parsing, so midi.c hands both the same function. */
typedef void (*usb_host_midi_rx_fn)(const uint8_t packet[4], void* ctx);
void usb_host_midi_set_rx_callback(usb_host_midi_rx_fn fn, void* ctx);

/* What is plugged in, for the app's status panel and the heartbeat. With hub
 * support on, several controllers can be attached at once; they are merged
 * into the router (which is omni anyway), and this reports the count plus the
 * first one's identity — enough to answer "did it see my keyboard".
 *
 * `product` is the device's product string where it has one, and a
 * "USB MIDI xxxx:xxxx" rendering of the ids where it does not. Safe to call
 * from any task. */
typedef struct {
    uint8_t attached;     /* MIDI devices currently claimed (0 = nothing) */
    uint16_t vid;         /* first device's ids, 0 when nothing is attached */
    uint16_t pid;
    char product[32];     /* NUL-terminated; empty when nothing is attached */
} usb_host_midi_info_t;

void usb_host_midi_get_info(usb_host_midi_info_t* out);

#ifdef __cplusplus
}
#endif
