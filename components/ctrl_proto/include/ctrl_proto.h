/*
 * osynth — SynthCtl v1 protocol, independent of how the bytes travel.
 *
 * This is the half of the old ble_ctrl.cpp that never mentioned NimBLE: the
 * frame parser, the ~25 opcode handlers, the response chunker and the
 * coalesced parameter-event flush. Spec: docs/BLE_PROTOCOL.md.
 *
 * ---------------------------------------------------------------------------
 * Why it is its own component
 *
 * The protocol was always transport-agnostic -- 1,389 lines of handlers with
 * not one reference to a BLE type -- but it lived inside the file that owns
 * the GATT tables, so the only way to speak it was over a BLE link. That is
 * fine for an instrument and wrong for two things it now has to do:
 *
 *   1. The standalone app embeds the engine in its own process, and reaches it
 *      through this protocol rather than over the air. The app's whole UI is
 *      already written against SynthCtl v1, so an in-process transport means
 *      no screen, no control and no discovery logic changes.
 *   2. It can be exercised on a host, which BLE could not be.
 *
 * ble_ctrl keeps NimBLE, the GATT tables, advertising and its own task, and
 * registers itself here as one transport.
 *
 * ---------------------------------------------------------------------------
 * Threading
 *
 * Exactly as before: ctrl_proto_handle_frame() and ctrl_proto_flush_events()
 * must be called from ONE task, and the same one. Every handler writes into a
 * single shared TX buffer and a single shared chunker, which is what makes the
 * response builders cheap; two callers would interleave two responses into one
 * buffer. Under BLE that task is `ble_cmd`; an embedded transport supplies its
 * own equivalent.
 *
 * ctrl_proto_notify_param() is the exception and may be called from any task:
 * it only sets a bit.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receive and transmit ceiling, including the 4-byte header. A transport sizes
 * its own buffers from this. */
#define CTRL_PROTO_MAX_FRAME 256

/* What the protocol needs from a transport, and nothing more.
 *
 * The shape is dictated by what the old code actually used: three ways of
 * getting bytes out and one question about the link. Everything else it did
 * with a BLE handle -- connection state, subscription state, the MTU -- is
 * folded into these four, which is why the handlers needed no edits at all.
 */
typedef struct {
    /* Deliver one frame. Returns false if it could not be delivered: no
     * client, not subscribed, or no buffer. Never blocks. */
    bool (*send)(const uint8_t* frame, size_t len);

    /* Deliver with back-pressure -- waits for room rather than failing. May
     * block, and is therefore only ever called from the protocol task. Used by
     * the loop-track download alone, which is the one response long enough to
     * outrun a transport's buffers; see the Chunker's `paced` flag.
     * May be NULL, in which case `send` is used instead. */
    bool (*send_paced)(const uint8_t* frame, size_t len);

    /* Payload bytes that fit in one frame at the transport's current limit,
     * counted after the 4-byte header and the status byte. Under BLE this
     * tracks the negotiated ATT MTU and changes per connection; a transport
     * with no such limit returns the largest value that fits the frame. */
    size_t (*avail_payload)(void);

    /* Is there a client able to receive events? Distinct from `send` failing:
     * this is asked before building an event at all, so that a synth with
     * nobody listening does no work and accumulates no backlog. */
    bool (*link_up)(void);
} ctrl_transport_t;

/* Installs the transport. Call before ctrl_proto_init(); passing NULL detaches
 * (every send then fails and the link reads as down), which is what a
 * transport does when it shuts down. */
void ctrl_proto_set_transport(const ctrl_transport_t* transport);

/* Registers the ParamStore listener that feeds the event flush. Safe to call
 * once; a second call is a no-op. */
esp_err_t ctrl_proto_init(void);

/* Parses and executes one command frame, sending the response through the
 * transport. Protocol task only. */
void ctrl_proto_handle_frame(const uint8_t* frame, size_t len);

/* Emits the coalesced parameter and engine events accumulated since the last
 * call. Protocol task only; the BLE transport calls it at ~20 Hz. */
void ctrl_proto_flush_events(void);

/* Refuses one frame the transport could not accept -- its queue was full, or
 * it was otherwise unable to take the work. The protocol formats the reply,
 * so a transport never has to know the status codes.
 *
 * The one protocol call that is safe from ANY task, not just the protocol
 * task: it builds a five-byte reply in a local and touches neither the shared
 * TX buffer nor the chunker. That matters because the caller is the transport's
 * receive path -- under BLE, the NimBLE host task -- which is precisely the
 * task that could not hand the frame to the protocol task in the first place.
 *
 * `len` is the frame as received; a frame too short to carry an opcode and a
 * sequence number is dropped silently, since there is nothing to address a
 * reply to. */
void ctrl_proto_reject_busy(const uint8_t* frame, size_t len);

/* Drops per-connection state so the next client gets a clean start -- today
 * that is the "which engine has been announced" latch, which must be cleared
 * or a reconnecting app never receives the EVT_ENGINE it is waiting for.
 * Call on connect. */
void ctrl_proto_link_reset(void);

/* The client is gone. Releases anything it was holding: all notes off, and
 * the chord table with them.
 *
 * This is protocol lifecycle, not transport cleanup, which is why it lives
 * here. OP_NOTE_ON enters the MIDI router like a played key, so its note-off
 * has to come back over the same link -- and a link that has just dropped will
 * never deliver it. Any transport that loses its client owes the synth this
 * call, or a client that walks away mid-chord leaves it droning.
 *
 * Safe from any task: both entry points are the same lock-free ring every
 * other control task pushes through. */
void ctrl_proto_link_down(void);

/* The INFO payload: protocol version, firmware version, target string. This is
 * protocol content rather than transport content -- it is the same bytes
 * whether it is read from a GATT characteristic or handed over in process --
 * so it is built here and the transport only carries it. Returns the number of
 * bytes written, or 0 if `max` is too small. */
size_t ctrl_proto_info(uint8_t* out, size_t max);

#ifdef __cplusplus
}
#endif
