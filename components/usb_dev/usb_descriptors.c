/*
 * osynth — USB device / configuration / string descriptors (ESP32-S3).
 * Layout and lengths in tusb/usb_descriptors.h; TinyUSB resolves the
 * tud_descriptor_*_cb symbols from here at link time.
 */
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* Espressif VID with a PID from the 0x8xxx test/community range. */
#define OSYNTH_USB_VID 0x303A
#define OSYNTH_USB_PID 0x8000

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO_CONTROL,
    STRID_AUDIO_STREAMING,
    STRID_MIDI,
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    /* Composite device grouped by Interface Association Descriptors. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = OSYNTH_USB_VID,
    .idProduct          = OSYNTH_USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,

    .bNumConfigurations = 0x01,
};

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + OSYNTH_UAC2_DESC_LEN + 8 + TUD_MIDI_DESC_LEN)

static const uint8_t s_config_desc[] = {
    /* Config number, interface count, string index, total length, attribute,
     * power in mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),

    OSYNTH_UAC2_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO_CONTROL,
                           EPNUM_AUDIO_IN),

    /* IAD for the MIDI function (AC + MS interface pair). TinyUSB's
     * TUD_MIDI_DESCRIPTOR carries no IAD, and in a composite that already
     * uses IADs, Windows' usbccgp would otherwise split the pair. Class
     * triple mirrors a legacy MIDI adapter's first interface (audio /
     * control / unused). */
    8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_MIDI, 2, TUSB_CLASS_AUDIO,
    AUDIO_SUBCLASS_CONTROL, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0x00,

    /* Interface number, string index, EP Out & EP In address, EP size */
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, STRID_MIDI, EPNUM_MIDI_OUT,
                        EPNUM_MIDI_IN, 64),
};

/* STRID_SERIAL is filled from the eFuse MAC on first request. */
static const char* s_strings[] = {
    [STRID_LANGID]          = (const char[]){0x09, 0x04}, /* English (0x0409) */
    [STRID_MANUFACTURER]    = "osynth",
    [STRID_PRODUCT]         = "osynth synthesizer",
    [STRID_SERIAL]          = NULL,
    [STRID_AUDIO_CONTROL]   = "osynth audio",
    [STRID_AUDIO_STREAMING] = "osynth audio",
    [STRID_MIDI]            = "osynth MIDI", /* DAWs show this as the port name */
};

const uint8_t* tud_descriptor_device_cb(void) {
    return (const uint8_t*)&s_device_desc;
}

const uint8_t* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_config_desc;
}

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[32];
    static char serial[13];

    uint8_t chr_count;
    if (index == STRID_LANGID) {
        memcpy(&desc_str[1], s_strings[STRID_LANGID], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(s_strings) / sizeof(s_strings[0])) return NULL;

        if (index == STRID_SERIAL && serial[0] == '\0') {
            uint8_t mac[6] = {0};
            esp_efuse_mac_get_default(mac);
            snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        const char* str = (index == STRID_SERIAL) ? serial : s_strings[index];

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    /* First u16: descriptor type in the high byte, byte length in the low. */
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
