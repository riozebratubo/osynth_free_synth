/*
 * osynth — USB composite descriptor layout (ESP32-S3, full speed).
 *
 * Interfaces:
 *   0  UAC2 audio control      \  IAD (audio function)
 *   1  UAC2 audio streaming    /  iso IN EP 0x81 (synth -> host)
 *   2  MIDI audio control      \  IAD (added by us; TinyUSB's MIDI
 *   3  MIDI streaming          /  descriptor has none) bulk 0x02/0x82
 *
 * The UAC2 function is a "microphone"-topology source: input terminal ->
 * output terminal on an internal fixed 48 kHz clock, no feature unit. The
 * host records the synth output; the host's iso IN polling is what paces
 * the audio task while the stream is open (see audio_io/sink_usb.cpp).
 *
 * Derived from esp-iot-solution's usb_device_uac descriptors (reference
 * copy under tools/ref/usb_device_uac/), trimmed to the mic-only path.
 * This header must stay free of TinyUSB includes: it is pulled in by
 * tusb_config.h before the stack headers exist; the TUD_AUDIO_DESC_*
 * macros it references expand later, at the point of use.
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Stream format (matches the audio engine, see synth_config.h) ---- */
#define OSYNTH_USB_SAMPLE_RATE      CONFIG_OSYNTH_SAMPLE_RATE
#if (OSYNTH_USB_SAMPLE_RATE % 1000) != 0
#error "USB audio assumes a sample rate divisible by 1000 (default 48000)"
#endif
#define OSYNTH_USB_CHANNELS         2
#define OSYNTH_USB_BYTES_PER_SAMPLE 2
#define OSYNTH_USB_BITS_PER_SAMPLE  16
#define OSYNTH_USB_BYTES_PER_MS     (OSYNTH_USB_SAMPLE_RATE / 1000 \
                                     * OSYNTH_USB_CHANNELS * OSYNTH_USB_BYTES_PER_SAMPLE)
/* EP-IN software FIFO depth; also the worst-case extra latency the USB
 * stream adds on top of the render block. */
#define OSYNTH_USB_FIFO_MS          8

/* ---- Interface numbers ---- */
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL,
};

/* ---- Endpoint addresses ---- */
#define EPNUM_AUDIO_IN   0x81 /* iso, synth audio to host */
#define EPNUM_MIDI_OUT   0x02
#define EPNUM_MIDI_IN    0x82

/* ---- UAC2 entity IDs (arbitrary but unique) ---- */
#define UAC2_ENTITY_CLOCK           0x04
#define UAC2_ENTITY_INPUT_TERMINAL  0x01
#define UAC2_ENTITY_OUTPUT_TERMINAL 0x03

/* ---- UAC2 function descriptor length ---- */
#define OSYNTH_UAC2_DESC_CS_AC_TOTAL_LEN (TUD_AUDIO_DESC_CLK_SRC_LEN \
    + TUD_AUDIO_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO_DESC_OUTPUT_TERM_LEN)

#define OSYNTH_UAC2_DESC_LEN (TUD_AUDIO_DESC_IAD_LEN \
    + TUD_AUDIO_DESC_STD_AC_LEN \
    + TUD_AUDIO_DESC_CS_AC_LEN \
    + OSYNTH_UAC2_DESC_CS_AC_TOTAL_LEN \
    + TUD_AUDIO_DESC_STD_AS_INT_LEN     /* alt 0: zero bandwidth */ \
    + TUD_AUDIO_DESC_STD_AS_INT_LEN     /* alt 1: streaming */ \
    + TUD_AUDIO_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN \
    + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)

/* UAC2 audio source function: IAD + AC interface + one AS interface with a
 * zero-bandwidth alt 0 and a streaming alt 1. String indices: _stridx names
 * the AC interface, _stridx+1 the AS interface. */
#define OSYNTH_UAC2_DESCRIPTOR(_itfnum, _stridx, _epin) \
    /* Interface Association Descriptor */\
    TUD_AUDIO_DESC_IAD(/*_firstitfs*/ _itfnum, /*_nitfs*/ 2, /*_stridx*/ 0x00),\
    /* Standard AC Interface Descriptor (4.7.1) */\
    TUD_AUDIO_DESC_STD_AC(/*_itfnum*/ _itfnum, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Class-Specific AC Interface Header Descriptor (4.7.2) */\
    TUD_AUDIO_DESC_CS_AC(/*_bcdADC*/ 0x0200, /*_category*/ AUDIO_FUNC_MICROPHONE, \
        /*_totallen*/ OSYNTH_UAC2_DESC_CS_AC_TOTAL_LEN, \
        /*_ctrl*/ AUDIO_CS_AS_INTERFACE_CTRL_LATENCY_POS),\
    /* Clock Source Descriptor (4.7.2.1): internal fixed, freq host-readable */\
    TUD_AUDIO_DESC_CLK_SRC(/*_clkid*/ UAC2_ENTITY_CLOCK, /*_attr*/ 1, /*_ctrl*/ 1, \
        /*_assocTerm*/ UAC2_ENTITY_INPUT_TERMINAL, /*_stridx*/ 0x00),\
    /* Input Terminal Descriptor (4.7.2.4) */\
    TUD_AUDIO_DESC_INPUT_TERM(/*_termid*/ UAC2_ENTITY_INPUT_TERMINAL, \
        /*_termtype*/ AUDIO_TERM_TYPE_IN_GENERIC_MIC, \
        /*_assocTerm*/ UAC2_ENTITY_OUTPUT_TERMINAL, /*_clkid*/ UAC2_ENTITY_CLOCK, \
        /*_nchannelslogical*/ OSYNTH_USB_CHANNELS, \
        /*_channelcfg*/ (AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT), \
        /*_idxchannelnames*/ 0x00, \
        /*_ctrl*/ (AUDIO_CTRL_R << AUDIO_IN_TERM_CTRL_CONNECTOR_POS), /*_stridx*/ 0x00),\
    /* Output Terminal Descriptor (4.7.2.5) */\
    TUD_AUDIO_DESC_OUTPUT_TERM(/*_termid*/ UAC2_ENTITY_OUTPUT_TERMINAL, \
        /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, \
        /*_assocTerm*/ UAC2_ENTITY_INPUT_TERMINAL, \
        /*_srcid*/ UAC2_ENTITY_INPUT_TERMINAL, /*_clkid*/ UAC2_ENTITY_CLOCK, \
        /*_ctrl*/ 0x0000, /*_stridx*/ 0x00),\
    /* Standard AS Interface Descriptor (4.9.1), alt 0: zero bandwidth */\
    TUD_AUDIO_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum) + 1), /*_altset*/ 0x00, \
        /*_nEPs*/ 0x00, /*_stridx*/ (uint8_t)((_stridx) + 1)),\
    /* Standard AS Interface Descriptor (4.9.1), alt 1: streaming */\
    TUD_AUDIO_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum) + 1), /*_altset*/ 0x01, \
        /*_nEPs*/ 0x01, /*_stridx*/ (uint8_t)((_stridx) + 1)),\
    /* Class-Specific AS Interface Descriptor (4.9.2) */\
    TUD_AUDIO_DESC_CS_AS_INT(/*_termid*/ UAC2_ENTITY_OUTPUT_TERMINAL, \
        /*_ctrl*/ AUDIO_CTRL_NONE, /*_formattype*/ AUDIO_FORMAT_TYPE_I, \
        /*_formats*/ AUDIO_DATA_FORMAT_TYPE_I_PCM, \
        /*_nchannelsphysical*/ OSYNTH_USB_CHANNELS, \
        /*_channelcfg*/ (AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT), \
        /*_stridx*/ 0x00),\
    /* Type I Format Type Descriptor (2.3.1.6 - Audio Formats) */\
    TUD_AUDIO_DESC_TYPE_I_FORMAT(OSYNTH_USB_BYTES_PER_SAMPLE, OSYNTH_USB_BITS_PER_SAMPLE),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor (4.10.1.1) */\
    TUD_AUDIO_DESC_STD_AS_ISO_EP(/*_ep*/ _epin, \
        /*_attr*/ (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), \
        /*_maxEPsize*/ CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN, /*_interval*/ 0x01),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor (4.10.1.2) */\
    TUD_AUDIO_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, \
        /*_ctrl*/ AUDIO_CTRL_NONE, \
        /*_lockdelayunit*/ AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, \
        /*_lockdelay*/ 0x0000)

#ifdef __cplusplus
}
#endif
