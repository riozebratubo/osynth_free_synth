/*
 * osynth — SynthCtl v1 wire constants.
 *
 * The opcodes and status codes, and nothing else. Split out of ctrl_proto.cpp
 * so that they have one owner rather than two.
 *
 * The second owner was app_osyntho/src/ble/synthprotocol.h: the app has always
 * carried its own copy, because it was a separate program and had no other
 * way to know them. That copy is still what the BLE build compiles -- an app
 * talking to an instrument over the air cannot include a firmware header.
 *
 * What changed is that the standalone build has both in one binary, so the two
 * can be *checked* against each other. app_osyntho/src/ble/protocolparity.cpp
 * does exactly that, with a static_assert per constant, and it is compiled only
 * when OSYNTHO_EMBEDDED is on. Drift that used to be found by a user is now
 * found by the compiler -- which is worth having, since the app's engine enum
 * silently lagged the firmware's by one entry for a whole release and cost a
 * crash.
 *
 * So: change a value here, and the embedded build stops until the app agrees.
 */
#pragma once

#include <stdint.h>

/* Opcodes (SynthCtl v1).
 *
 * A response is `request_op | 0x80`; the two event opcodes are the literal
 * values 0xC0 and 0xC1. So the only reserved commands are 0x40 and 0x41 —
 * those, and only those, would produce a response byte a client could not
 * tell from an event. (An earlier comment here claimed the whole 0x40-0x5F
 * block was reserved and that responses "cannot collide" with events at all;
 * neither is true — OP_PING's response really is 0xFF, which is why the app
 * matches events by exact opcode rather than by `op >= 0xC0`.) */
enum : uint8_t {
    OP_SET_PARAM = 0x01,
    OP_GET_PARAM = 0x02,
    OP_PARAM_INFO = 0x03,
    OP_SELECT_ENGINE = 0x04,
    OP_LOAD_PRESET = 0x05,
    OP_SAVE_PRESET = 0x06,
    OP_LIST_PRESETS = 0x07,
    OP_RENAME_PRESET = 0x08, /* reserved in v1 */
    OP_BULK = 0x09,          /* reserved in v1 */
    /* USB role (S35). Two opcodes, not a parameter listener, on purpose: the
     * role *is* an ordinary parameter (`usb.mode`, persisted like any other),
     * but restarting the synth must never be a side effect of writing one.
     * SET_PARAM is the path a preset load, a bulk value sweep and a knob drag
     * all take, and any of those touching a self-rebooting parameter would
     * restart the instrument mid-performance. So the write only persists the
     * choice, and this asks for it to be applied. */
    OP_USB_STATUS = 0x0A,
    OP_REBOOT = 0x0B,
    OP_TRANSPORT = 0x10,
    OP_ARP = 0x11,
    OP_NOTE_ON = 0x20,
    OP_NOTE_OFF = 0x21,
    /* Sequencer + drum kit (S23). Pattern data is not parameters — 16384
     * steps could never be — so it travels over these instead. Each op that
     * can both read and write starts its payload with a direction byte
     * (0 = get, 1 = set) rather than burning two opcodes apiece. */
    OP_SEQ_INFO = 0x30,
    OP_SEQ_STEPS = 0x31,
    OP_SEQ_TRACK = 0x32,
    OP_SEQ_PATTERN = 0x33,
    OP_SEQ_PLOCK = 0x34,
    OP_SEQ_EDIT = 0x35,
    OP_SEQ_SONG = 0x36,
    OP_KIT_INFO = 0x37,
    /* Velocity-carrying drum hit. The `drums.trig` parameter can only say
     * *which* slot (a parameter is one float), so a velocity-sensitive pad
     * needs its own opcode; it also avoids depending on drums.midich being
     * set, which a NOTE_ON-with-channel scheme would. */
    OP_DRUM_TRIG = 0x38,
    /* Modular patch graph (S28). Same reasoning as the sequencer block: a
     * graph's *structure* is not parameter space, so it travels here while
     * its node values stay ordinary parameters. GRAPH_KIND is separate from
     * GRAPH_INFO because the kind table is the one part the app cannot
     * guess — it needs each kind's port names and parameter layout to draw
     * a node at all, and that is far too much for one frame.
     *
     * There is deliberately no graph event opcode: `graph.rev` is a
     * read-only parameter, so the existing ~20 Hz parameter batch already
     * tells the app the model changed, including when a preset load was
     * what changed it. */
    OP_GRAPH_INFO = 0x39,
    OP_GRAPH_KIND = 0x3A,
    OP_GRAPH_NODES = 0x3B,
    OP_GRAPH_EDIT = 0x3C,
    /* Reading a recorded loop track back out (S33), so the app can write it
     * to a WAV. Audio, not control data — but it goes here rather than over
     * the reserved BULK characteristic because there is nothing about it that
     * BULK would do better: it is a windowed request/response like every
     * other read on this link, and putting it on the one transport the app
     * already has flow control, retries and a sequence number for was worth
     * more than a second channel. The window is what keeps it honest — the
     * synth only ever sends what was just asked for. */
    OP_LOOP_DUMP = 0x3D,
    /* The user chord set (S41). Twelve slots of eight bytes is not
     * parameter space — a parameter is one float — and it is far too
     * small to deserve the chunking the sequencer opcodes need, so it is
     * one opcode that carries the whole set in a single frame either way.
     * Direction byte first, like every other read-and-write op here. */
    OP_CHORD_SET = 0x3E,
    /* Sample-kit editing (S44). Everything the *recorder* does rides on
     * ordinary parameters (smp.arm, smp.rec, smp.erase, smp.undo), because a
     * float is all any of it needs and the app already has plumbing for those.
     * What could not go there is the per-pad performance data - play mode,
     * reverse, start offset, choke group, note, name - which is kit data
     * rather than patch data: it has to follow a kit switch, and a parameter
     * does not. There is also no room for it, with 448 parameter slots and
     * ~418 in use. So it travels here, the same reasoning the sequencer's
     * pattern data and the graph's structure both took. */
    OP_KIT_EDIT = 0x3F,
    OP_PING = 0x7F,
    EVT_PARAMS = 0xC0,
    EVT_ENGINE = 0xC1,
};

enum : uint8_t {
    ST_OK = 0,
    ST_MALFORMED = 1,
    ST_UNKNOWN_OP = 2,
    ST_BAD_ARG = 3,
    ST_UNSUPPORTED = 4,
    ST_BUSY = 5,
    ST_MORE = 0x80, /* continuation bit: more frames of this response follow */
};
