/*
 * osynth — BLE control implementation (Session 14).
 *
 * NimBLE peripheral, one connection, service 395f2e00-7c4f-4a48-94e7-
 * dafcf25ef34a with four characteristics (docs/BLE_PROTOCOL.md):
 *   CTRL  write / write-no-response  — command frames app -> synth
 *   EVT   notify                     — responses + unsolicited events
 *   BULK  write / notify             — reserved in v1 (writes ignored)
 *   INFO  read                       — protocol + firmware version, target
 *
 * Threading: the NimBLE host task only flattens incoming CTRL frames into a
 * queue; the `ble_cmd` task (core 0, prio 3 — below the preset task) does
 * all protocol work: parameter access (origin Ble), preset requests (the
 * S13 async API), note routing, and building every outgoing frame. It also
 * owns the event side: a ParamStore listener marks changed ids in a dirty
 * bitmap (non-Ble origins only — echo suppression), and the task flushes
 * the bitmap as batched EVT_PARAMS notifications at most every 50 ms
 * (~20 Hz), reading the latest values at send time so bursts coalesce.
 * Engine switches are detected by polling engines_active_type() on the same
 * cadence and announced as EVT_ENGINE {engine, caps} — caps is the module-
 * gating bitmask the app uses to hide dead controls.
 *
 * Responses that exceed one frame (LIST_PRESETS, PARAM_INFO id lists) are
 * chunked: every non-final frame sets bit 7 of the status byte. Frame
 * payloads are sized to the live ATT MTU, so small-MTU clients stay
 * protocol-correct (just slower); the full metadata responses assume the
 * documented MTU >= 247.
 */
#include "ble_ctrl.h"

#include "esp_log.h"
#include "synth_config.h"

static const char* TAG = "ble_ctrl";

#if SYNTH_ENABLE_BLE && CONFIG_BT_NIMBLE_ENABLED

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#if SYNTH_BLE_VIA_HOSTED
#include "esp_hosted.h"
#endif

#include "drum_kit_fmt.h"
#include "drums.h"
#include "engines.h"
#if SYNTH_ENABLE_MODULAR
#include "graph_model.h"
#include "graph_render.h" /* live_cost() for the app's budget meter */
#endif
#include "looper.h"
#include "midi.h"
#include "persist.h"
#include "presets.h"
#include "seq_model.h"
#include "seq_play.h"
#include "seqarp.h"
#include "synth_params.h"
#include "synth_voice.h"
#include "usb_host_midi.h"

using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;

namespace {

constexpr char kDeviceName[] = "osynth";
constexpr uint8_t kProtoVersion = 1;
constexpr uint8_t kFwVersion[3] = {0, 1, 0}; /* keep in sync with main.cpp */

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

constexpr size_t kMaxFrame = 256; /* rx and tx ceiling, incl. 4 B header */
/* Deep enough to absorb the app's connect burst. Only PARAM_INFO is windowed
 * app-side (three in flight); the GET_PARAM value sweep, LIST_PRESETS and the
 * sequencer and kit probes ride outside that window, so the old depth of 4 was
 * reachable by an ordinary discovery and cost a dropped frame plus a BUSY
 * round trip. At 258 B per CmdBuf this is ~4 KB against ~1 KB. */
constexpr int kCmdQueueDepth = 16;
constexpr int kTaskPrio = 3; /* control plane, below preset (4) */
constexpr int kTaskStack = 4096;
constexpr TickType_t kFlushPeriod = pdMS_TO_TICKS(50); /* ~20 Hz events */

/* 128-bit UUIDs, generated for v1 (little-endian byte order for NimBLE):
 * 395f2eXX-7c4f-4a48-94e7-dafcf25ef34a, XX = 00 svc / 01 ctrl / 02 evt /
 * 03 bulk / 04 info. */
#define OSYNTH_UUID128(idx)                                                  \
    {{BLE_UUID_TYPE_128},                                                    \
     {0x4a, 0xf3, 0x5e, 0xf2, 0xfc, 0xda, 0xe7, 0x94, 0x48, 0x4a, 0x4f,     \
      0x7c, (idx), 0x2e, 0x5f, 0x39}}

const ble_uuid128_t kSvcUuid = OSYNTH_UUID128(0x00);
const ble_uuid128_t kCtrlUuid = OSYNTH_UUID128(0x01);
const ble_uuid128_t kEvtUuid = OSYNTH_UUID128(0x02);
const ble_uuid128_t kBulkUuid = OSYNTH_UUID128(0x03);
const ble_uuid128_t kInfoUuid = OSYNTH_UUID128(0x04);

struct CmdBuf {
    uint16_t len;
    uint8_t data[kMaxFrame];
};

QueueHandle_t s_cmd_queue = nullptr;
TaskHandle_t s_cmd_task = nullptr;
uint8_t s_own_addr_type = 0;
uint16_t s_evt_handle = 0;
uint16_t s_bulk_handle = 0;

std::atomic<uint16_t> s_conn{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> s_evt_sub{false};
std::atomic<const char*> s_state{"unavailable"};
std::atomic<int> s_announced_engine{-1}; /* -1 forces an EVT_ENGINE */

/* Dirty bitmap for param-change events (one bit per possible id). */
std::atomic<uint32_t> s_dirty[osynth::PID_SPACE_END / 32];

uint8_t s_tx[kMaxFrame]; /* ble_cmd task only */

inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline float rdf32(const uint8_t* p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}
inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
inline void wrf32(uint8_t* p, float v) { memcpy(p, &v, 4); }

/* ---- outgoing frames ------------------------------------------------- */

/* All synth -> app traffic is EVT notifications; the app must subscribe
 * before sending commands (documented flow), so unsubscribed = drop. */
bool send_frame(const uint8_t* frame, size_t len) {
    const uint16_t conn = s_conn.load(std::memory_order_relaxed);
    if (conn == BLE_HS_CONN_HANDLE_NONE ||
        !s_evt_sub.load(std::memory_order_relaxed)) {
        return false;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(frame, len);
    if (om == nullptr) return false;
    return ble_gatts_notify_custom(conn, s_evt_handle, om) == 0;
}

/* Small status-only response; safe from the host task (own buffer). */
void send_status(uint8_t op, uint8_t seq, uint8_t status) {
    const uint8_t f[5] = {(uint8_t)(op | 0x80), seq, 1, 0, status};
    send_frame(f, sizeof(f));
}

/* send_frame() with back-pressure, for the loop dump alone.
 *
 * Every other multi-frame response here is a handful of frames of metadata,
 * and a dropped one is the app's to notice and re-request. A track download is
 * thousands of frames of audio in a row, which is the first thing on this link
 * able to outrun the host's mbuf pool — and at that rate "the app re-requests"
 * stops being a rare correction and becomes the throughput. So this waits for
 * a buffer instead of failing, which is exactly the pacing the dump wants: it
 * runs on the ble_cmd task, and the only thing it delays is the rest of the
 * download. Bails at once (rather than after the full wait) if the link is
 * gone, since nothing is coming back then. */
bool send_frame_paced(const uint8_t* frame, size_t len) {
    for (int i = 0; i < 100; ++i) { /* ~500 ms */
        if (s_conn.load(std::memory_order_relaxed) == BLE_HS_CONN_HANDLE_NONE ||
            !s_evt_sub.load(std::memory_order_relaxed)) {
            return false;
        }
        if (send_frame(frame, len)) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* Payload bytes (after the status byte) that fit one frame at the live
 * ATT MTU (a notification carries at most MTU-3 attribute bytes). */
size_t avail_payload() {
    const uint16_t conn = s_conn.load(std::memory_order_relaxed);
    uint16_t mtu = 23;
    if (conn != BLE_HS_CONN_HANDLE_NONE) mtu = ble_att_mtu(conn);
    if (mtu < 23) mtu = 23; /* 0 for a just-dropped handle */
    size_t att = (size_t)mtu - 3; /* notification value size limit */
    if (att > kMaxFrame) att = kMaxFrame;
    return att - 4 /* frame header */ - 1 /* status */;
}

/* Builds record-list frames into s_tx, splitting at the MTU with the
 * ST_MORE continuation bit; `prefix` is repeated in every frame so each
 * one parses standalone. ble_cmd task only. */
class Chunker {
public:
    /* `paced` waits for an mbuf instead of failing (send_frame_paced), for a
     * response whose partial loss the *client* cannot detect. It blocks, so it
     * is only legal from ble_cmd — which is where every command handler runs.
     * Default off: an event must never park the flush task. */
    void begin(uint8_t first_byte, uint8_t seq, const uint8_t* prefix,
               size_t prefix_len, bool suppress_empty, bool paced = false) {
        first_ = first_byte;
        seq_ = seq;
        prefix_len_ = prefix_len;
        suppress_empty_ = suppress_empty;
        paced_ = paced;
        if (prefix_len > 0) memcpy(prefix_, prefix, prefix_len);
        fill_ = 0;
        failed_ = false;
    }

    /* True if any frame of this response could not be sent. The event path acts
     * on it because a dropped *event* is gone for good unless the sender
     * re-arms it, and the PARAM_INFO id list acts on it because a dropped frame
     * there is undetectable by the client (see that call site). For every other
     * response a loss is the app's to notice and re-request. */
    bool failed() const { return failed_; }
    void append(const void* rec, size_t len) {
        if (fill_ + len > cap()) {
            /* Only flush something. With an MTU too small to hold even one
             * record, `fill_` is always 0 here and emitting anyway sent one
             * empty continuation frame *per record* — 112 of them for a
             * preset listing — before the equally empty final frame. */
            if (fill_ > 0) emit(true);
            if (len > cap()) return; /* record can never fit this MTU */
        }
        memcpy(s_tx + 5 + prefix_len_ + fill_, rec, len);
        fill_ += len;
    }
    void finish() {
        if (suppress_empty_ && fill_ == 0) return;
        emit(false);
    }

private:
    size_t cap() const {
        const size_t avail = avail_payload();
        return avail > prefix_len_ ? avail - prefix_len_ : 0;
    }
    void emit(bool more) {
        s_tx[0] = first_;
        s_tx[1] = seq_;
        wr16(s_tx + 2, (uint16_t)(1 + prefix_len_ + fill_));
        s_tx[4] = more ? (uint8_t)(ST_OK | ST_MORE) : (uint8_t)ST_OK;
        if (prefix_len_ > 0) memcpy(s_tx + 5, prefix_, prefix_len_);
        const size_t len = 4 + 1 + prefix_len_ + fill_;
        const bool ok =
            paced_ ? send_frame_paced(s_tx, len) : send_frame(s_tx, len);
        if (!ok) failed_ = true;
        fill_ = 0;
    }

    bool failed_ = false;
    uint8_t first_ = 0, seq_ = 0;
    /* 8 bytes: the S23 sequencer responses carry the widest prefixes
     * (kind + pattern + track + u16 step). */
    uint8_t prefix_[8] = {};
    size_t prefix_len_ = 0, fill_ = 0;
    bool suppress_empty_ = false;
    bool paced_ = false;
};

Chunker s_chunker;

/* ---- command handlers (ble_cmd task) ---------------------------------- */

void handle_set_param(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen == 0 || plen % 6 != 0) {
        send_status(OP_SET_PARAM, seq, ST_MALFORMED);
        return;
    }
    ParamStore& ps = ParamStore::instance();
    for (uint16_t i = 0; i < plen; i += 6) {
        /* unregistered ids fail silently — the inactive-engine convention
         * (a knob sweep racing an engine switch is normal, not an error) */
        ps.set(rd16(p + i), rdf32(p + i + 2), ParamOrigin::Ble);
    }
    /* no response on success (write-no-response fire-and-forget path) */
}

void handle_get_param(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen == 0 || plen % 2 != 0) {
        send_status(OP_GET_PARAM, seq, ST_MALFORMED);
        return;
    }
    ParamStore& ps = ParamStore::instance();
    s_chunker.begin(OP_GET_PARAM | 0x80, seq, nullptr, 0, false);
    for (uint16_t i = 0; i < plen; i += 2) {
        const uint16_t id = rd16(p + i);
        if (ps.describe(id) == nullptr) continue; /* omitted from response */
        uint8_t rec[6];
        wr16(rec, id);
        wrf32(rec + 2, ps.get(id));
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

void handle_param_info(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen != 2) {
        send_status(OP_PARAM_INFO, seq, ST_MALFORMED);
        return;
    }
    ParamStore& ps = ParamStore::instance();
    const uint16_t id = rd16(p);

    if (id == 0xFFFF) { /* id list + active engine and its caps mask */
        /* 640 B at kMaxParams = 320, against a 4 KB task stack — by far the
         * largest frame in this task. Static is safe for exactly the reason
         * s_tx is: every handler here runs on ble_cmd and nowhere else. */
        static uint16_t ids[ParamStore::kMaxParams];
        const size_t n = ps.listIds(ids, ParamStore::kMaxParams);
        const synth_engine_type_t eng = engines_active_type();
        const synth_engine_t* e = engines_get(eng);
        const uint8_t prefix[2] = {(uint8_t)eng,
                                   (uint8_t)(e != nullptr ? e->caps : 0)};
        /* Paced, unlike every other response here, because this is the one the
         * client cannot repair. The others are recoverable by re-asking: a lost
         * preset or sequencer frame leaves a page short and the app re-requests
         * it. The id list is what tells the app *which parameters exist* — and
         * the app ends the list on the frame without the continuation bit, so a
         * lost middle frame is not short, it is invisible. Those ids are then
         * never registered, never requested, and never missed, for the life of
         * the connection: the symptom is a whole feature reported absent (the
         * looper page's "parameters were not received") on a synth that
         * registered it perfectly well. Waiting for an mbuf costs a few ms on a
         * congested link and removes the failure mode. */
        s_chunker.begin(OP_PARAM_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                        /*paced=*/true);
        for (size_t i = 0; i < n; ++i) {
            uint8_t rec[2];
            wr16(rec, ids[i]);
            s_chunker.append(rec, sizeof(rec));
        }
        s_chunker.finish();
        /* Pacing waits out congestion but not a dropped link, so this can still
         * fail. Logged loudly because the app has no way to tell: it will carry
         * on with a short list and quietly hide whatever was in the lost frame. */
        if (s_chunker.failed()) {
            ESP_LOGE(TAG,
                     "id list (%u ids) could not be sent in full — the app's "
                     "parameter list is incomplete and it cannot detect that",
                     (unsigned)n);
        }
        return;
    }

    const ParamDesc* d = ps.describe(id);
    if (d == nullptr) {
        send_status(OP_PARAM_INFO, seq, ST_BAD_ARG);
        return;
    }
    /* [id u16][type u8][curve u8][enum_count u8][min f32][max f32][def f32]
     * [name NUL][enum_count × NUL-terminated names] */
    const size_t limit = 4 + 1 + avail_payload();
    /* The fixed part alone is 22 bytes, and the name/enum loops below already
     * respect `limit` — but nothing checked that the *header* fits. At the
     * 23-byte default ATT MTU a notification carries 20 bytes, so this built
     * a 22-byte frame the stack could only refuse, and the client saw
     * silence. Clients are expected to negotiate MTU >= 247
     * (docs/BLE_PROTOCOL.md); a status is the honest answer for one that has
     * not got there yet. */
    /* header + status + id + type + curve + enum_count + min/max/def */
    constexpr size_t kFixedLen = 5 + 2 + 1 + 1 + 1 + 4 + 4 + 4;
    if (kFixedLen > limit) {
        send_status(OP_PARAM_INFO, seq, ST_UNSUPPORTED);
        return;
    }
    /* The *name* has to fit too, and this is a refusal rather than a partial
     * answer for the same reason the header check above is one.
     *
     * The copy below drops the name when it does not fit and then sends ST_OK
     * regardless, which hands the client a parameter with no name and no way to
     * learn that one was withheld. An app that resolves controls by name — the
     * osynth app does, paramIdForName() — records the metadata as *known*, finds
     * no match, and reports the whole feature absent; the looper page's "not
     * received" is that, and it survives for the life of the connection because
     * nothing looks wrong to either side. Enum labels are different and stay
     * best-effort below: a control with no labels still works, one with no name
     * cannot be found at all.
     *
     * Costs a client on a sub-35-byte MTU the metadata it was already only
     * half-getting, and gives it a status it can act on. */
    const size_t name_len = strlen(d->name) + 1;
    if (kFixedLen + name_len > limit) {
        ESP_LOGW(TAG,
                 "param 0x%04x (%s): MTU too small for the name (need %u, have "
                 "%u) — refusing rather than answering nameless",
                 d->id, d->name, (unsigned)(kFixedLen + name_len),
                 (unsigned)limit);
        send_status(OP_PARAM_INFO, seq, ST_UNSUPPORTED);
        return;
    }
    size_t n = 5; /* payload starts after the 4 B header + status byte */
    wr16(s_tx + n, d->id);
    n += 2;
    s_tx[n++] = (uint8_t)d->type;
    s_tx[n++] = (uint8_t)d->curve;
    s_tx[n++] = d->enum_count;
    wrf32(s_tx + n, d->min);
    n += 4;
    wrf32(s_tx + n, d->max);
    n += 4;
    wrf32(s_tx + n, d->def);
    n += 4;
    size_t len = strlen(d->name) + 1;
    if (n + len <= limit) {
        memcpy(s_tx + n, d->name, len);
        n += len;
    }
    for (uint8_t i = 0; i < d->enum_count && d->enum_names != nullptr; ++i) {
        len = strlen(d->enum_names[i]) + 1;
        if (n + len > limit) break; /* needs the documented MTU >= 247 */
        memcpy(s_tx + n, d->enum_names[i], len);
        n += len;
    }
    s_tx[0] = OP_PARAM_INFO | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = ST_OK;
    send_frame(s_tx, n);
}

uint8_t status_from(esp_err_t err) {
    if (err == ESP_OK) return ST_OK;
    return err == ESP_ERR_INVALID_ARG ? ST_BAD_ARG : ST_BUSY;
}

void handle_list_presets(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen != 1) {
        send_status(OP_LIST_PRESETS, seq, ST_MALFORMED);
        return;
    }
    const int engine = p[0];
    if (engine >= SYNTH_ENGINE_COUNT) {
        send_status(OP_LIST_PRESETS, seq, ST_BAD_ARG);
        return;
    }
    const uint8_t prefix[1] = {(uint8_t)engine};
    s_chunker.begin(OP_LIST_PRESETS | 0x80, seq, prefix, sizeof(prefix),
                    false);
    for (int slot = 0; slot < PRESETS_PER_ENGINE; ++slot) {
        char name[PRESETS_NAME_MAX];
        bool factory = false;
        if (!presets_slot_info(engine, slot, name, &factory)) continue;
        uint8_t rec[2 + PRESETS_NAME_MAX] = {};
        rec[0] = (uint8_t)slot;
        rec[1] = factory ? 0x01 : 0x00;
        strlcpy((char*)rec + 2, name, PRESETS_NAME_MAX);
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

/* USB role and what is plugged into it (S35).
 *
 * `active` is the role the port is actually in; `requested` is what
 * `usb.mode` says it should be. They differ exactly between a write and the
 * restart that applies it, which is what lets the app show "restart to
 * apply" without keeping that state itself — a truth the synth can always
 * answer beats one the app has to remember across a reconnect.
 *
 * `supported` is the build capability. False means the control is not offered
 * at all: either no USB-OTG on this target, or the USB sink is the audio
 * clock and giving up the device role would leave the synth silent. */
void handle_usb_status(uint8_t seq) {
    usb_host_midi_info_t info;
    usb_host_midi_get_info(&info);

    const bool supported = usb_mode_host_supported();
    /* Unregistered on a build without the capability, and ParamStore::get()
     * answers 0.0f for an unknown id — which is device, the right answer. */
    const uint8_t requested =
        ParamStore::instance().get(osynth::PID_USB_MODE) >= 0.5f ? 1 : 0;

    uint8_t f[5 + 8 + sizeof(info.product)] = {(uint8_t)(OP_USB_STATUS | 0x80),
                                               seq, 0, 0, ST_OK};
    size_t n = 5;
    f[n++] = (uint8_t)usb_mode_active();
    f[n++] = requested;
    f[n++] = supported ? 1 : 0;
    f[n++] = info.attached;
    wr16(f + n, info.vid);
    n += 2;
    wr16(f + n, info.pid);
    n += 2;
    const size_t plen = strlen(info.product);
    memcpy(f + n, info.product, plen);
    n += plen;
    f[n++] = 0; /* NUL, so the app's cstr() reader terminates */

    wr16(f + 2, (uint16_t)(n - 4));
    send_frame(f, n);
}

/* Applies a pending role change by restarting.
 *
 * persist_save_now() first, and not optionally: the writer coalesces and then
 * waits for the output to go quiet before touching flash (persist.h), so a
 * `usb.mode` written a second ago is still only marked dirty. Rebooting
 * without flushing would discard exactly the setting this restart exists to
 * apply — and the header names this case as the reason the call is public.
 *
 * The delay is what gets the response out of the door. It runs on the
 * ble_cmd task, so it stalls nothing that matters, and the synth is a
 * quarter-second from gone in any case. */
void handle_reboot(uint8_t seq) {
    send_status(OP_REBOOT, seq, ST_OK);
    const esp_err_t err = persist_save_now();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist flush before reboot failed: %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "restarting on app request");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

void handle_transport(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen != 1 && plen != 5) {
        send_status(OP_TRANSPORT, seq, ST_MALFORMED);
        return;
    }
    if (p[0] > 2) { /* 0 stop, 1 play, 2 rec = the seq.mode enum */
        send_status(OP_TRANSPORT, seq, ST_BAD_ARG);
        return;
    }
    ParamStore& ps = ParamStore::instance();
    if (plen == 5) {
        const float tempo = rdf32(p + 1);
        if (tempo > 0.0f) ps.set(SEQ_PID_TEMPO, tempo, ParamOrigin::Ble);
    }
    ps.set(SEQ_PID_SEQ_MODE, (float)p[0], ParamOrigin::Ble);
    send_status(OP_TRANSPORT, seq, ST_OK);
}

void handle_arp(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen != 4) {
        send_status(OP_ARP, seq, ST_MALFORMED);
        return;
    }
    ParamStore& ps = ParamStore::instance();
    if (p[0] == 0) {
        ps.set(SEQ_PID_ARP_MODE, 0.0f, ParamOrigin::Ble);
    } else {
        int mode = p[1]; /* 1 up … 5 played; 0 defaults to up */
        if (mode < 1) mode = 1;
        if (mode > 5) mode = 5;
        ps.set(SEQ_PID_ARP_MODE, (float)mode, ParamOrigin::Ble);
    }
    /* octaves and division are clamped to their ranges by the store */
    ps.set(SEQ_PID_ARP_OCT, (float)p[2], ParamOrigin::Ble);
    ps.set(SEQ_PID_DIV, (float)p[3], ParamOrigin::Ble);
    send_status(OP_ARP, seq, ST_OK);
}

/* ---- sequencer + drum kit (S23) --------------------------------------- */

/* Both structs go on the wire byte-for-byte. They are all-uint8 apart from
 * one aligned uint16, so there is no padding to leak and no endianness to
 * swap on either target — but assert it rather than trust it, because a
 * field added in the wrong place would silently corrupt every pattern the
 * app sends. */
static_assert(sizeof(seq_step_t) == 8, "wire layout");
static_assert(sizeof(seq_track_cfg_t) == 16, "wire layout");

bool seq_args_ok(int pattern, int track) {
    return pattern >= 0 && pattern < SEQ_PATTERNS && track >= 0 &&
           track < SEQ_TRACKS;
}

void handle_seq_info(uint8_t seq) {
    size_t n = 5;
    s_tx[n++] = SEQ_TRACKS;
    s_tx[n++] = SEQ_PATTERNS;
    wr16(s_tx + n, SEQ_MAX_STEPS);
    n += 2;
    wr16(s_tx + n, SEQ_DEFAULT_STEPS);
    n += 2;
    s_tx[n++] = (uint8_t)seq_song_length();
    wr16(s_tx + n, SEQ_PLOCKS);
    n += 2;
    wr16(s_tx + n, (uint16_t)seq_plock_count());
    n += 2;
    s_tx[n++] = SEQ_SCALE_COUNT;
    s_tx[n++] = SEQ_COND_COUNT;
    s_tx[n++] = SEQ_DIV_COUNT;
    s_tx[n++] = SEQ_TARGET_COUNT;
    s_tx[n++] = SEQ_DIR_COUNT;
    s_tx[n++] = seq_model_ready() ? 1 : 0;
    s_tx[0] = OP_SEQ_INFO | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = ST_OK;
    send_frame(s_tx, n);
}

void handle_seq_steps(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 6) {
        send_status(OP_SEQ_STEPS, seq, ST_MALFORMED);
        return;
    }
    const uint8_t dir = p[0];
    const int pattern = p[1];
    const int track = p[2];
    const int first = rd16(p + 3);
    const int count = p[5];
    if (!seq_args_ok(pattern, track) || first < 0 || first >= SEQ_MAX_STEPS ||
        count <= 0) {
        send_status(OP_SEQ_STEPS, seq, ST_BAD_ARG);
        return;
    }
    int n = count;
    if (first + n > SEQ_MAX_STEPS) n = SEQ_MAX_STEPS - first;

    if (dir == 0) { /* get */
        uint8_t prefix[4];
        prefix[0] = (uint8_t)pattern;
        prefix[1] = (uint8_t)track;
        wr16(prefix + 2, (uint16_t)first);
        s_chunker.begin(OP_SEQ_STEPS | 0x80, seq, prefix, sizeof(prefix), false);
        for (int i = 0; i < n; ++i) {
            seq_step_t st;
            seq_step_get(pattern, track, first + i, &st);
            s_chunker.append(&st, sizeof(st));
        }
        s_chunker.finish();
        return;
    }
    if (plen != 6 + (uint16_t)count * sizeof(seq_step_t)) {
        send_status(OP_SEQ_STEPS, seq, ST_MALFORMED);
        return;
    }
    for (int i = 0; i < n; ++i) {
        seq_step_t st;
        memcpy(&st, p + 6 + (size_t)i * sizeof(st), sizeof(st));
        seq_step_set(pattern, track, first + i, &st);
    }
    send_status(OP_SEQ_STEPS, seq, ST_OK);
}

void handle_seq_track(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 3) {
        send_status(OP_SEQ_TRACK, seq, ST_MALFORMED);
        return;
    }
    const uint8_t dir = p[0];
    const int pattern = p[1];
    const int track = p[2];
    if (!seq_args_ok(pattern, track)) {
        send_status(OP_SEQ_TRACK, seq, ST_BAD_ARG);
        return;
    }
    if (dir != 0) {
        if (plen != 3 + sizeof(seq_track_cfg_t)) {
            send_status(OP_SEQ_TRACK, seq, ST_MALFORMED);
            return;
        }
        seq_track_cfg_t cfg;
        memcpy(&cfg, p + 3, sizeof(cfg));
        /* Mute and solo are not the client's to send here. They are owned by
         * the trk<N>.mute/solo parameters — that is what the playback path
         * reads (seq_play's track_audible()), and seqarp mirrors them into
         * these flags so they travel with a saved pattern.
         *
         * A client edits one field at a time but has to send the whole struct
         * back, so what it sends for the other fields is whatever its last read
         * said. Muting a track moves the flag without going through this
         * opcode, so any track-field edit made before the client re-read —
         * changing the division, dragging the level — carried the pre-mute
         * flags and quietly un-muted the track in the stored pattern. The
         * parameter still said muted, so it still *sounded* muted, and the
         * disagreement only showed up after a save and load.
         *
         * Keeping the stored bits is the conservative half of the fix: a
         * client that wants to change a mute writes the parameter. */
        seq_track_cfg_t prev;
        seq_track_cfg_get(pattern, track, &prev);
        cfg.flags = (uint8_t)((cfg.flags & ~(SEQ_TRACK_F_MUTE |
                                             SEQ_TRACK_F_SOLO)) |
                              (prev.flags & (SEQ_TRACK_F_MUTE |
                                             SEQ_TRACK_F_SOLO)));
        seq_track_cfg_set(pattern, track, &cfg);
    }
    seq_track_cfg_t cfg;
    seq_track_cfg_get(pattern, track, &cfg);
    size_t n = 5;
    s_tx[n++] = (uint8_t)pattern;
    s_tx[n++] = (uint8_t)track;
    memcpy(s_tx + n, &cfg, sizeof(cfg));
    n += sizeof(cfg);
    s_tx[0] = OP_SEQ_TRACK | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = ST_OK;
    send_frame(s_tx, n);
}

void handle_seq_pattern(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 2) {
        send_status(OP_SEQ_PATTERN, seq, ST_MALFORMED);
        return;
    }
    const uint8_t dir = p[0];
    const int pattern = p[1];
    if (pattern < 0 || pattern >= SEQ_PATTERNS) {
        send_status(OP_SEQ_PATTERN, seq, ST_BAD_ARG);
        return;
    }
    seq_pattern_cfg_t cfg;
    seq_pattern_cfg_get(pattern, &cfg);
    if (dir != 0) {
        if (plen < 7) {
            send_status(OP_SEQ_PATTERN, seq, ST_MALFORMED);
            return;
        }
        cfg.length = rd16(p + 2);
        cfg.scale = p[4];
        cfg.root = p[5];
        cfg.swing = p[6];
        size_t name_len = plen - 7;
        if (name_len > sizeof(cfg.name) - 1) name_len = sizeof(cfg.name) - 1;
        memset(cfg.name, 0, sizeof(cfg.name));
        if (name_len > 0) memcpy(cfg.name, p + 7, name_len);
        seq_pattern_cfg_set(pattern, &cfg);
        seq_pattern_cfg_get(pattern, &cfg);
    }
    size_t n = 5;
    s_tx[n++] = (uint8_t)pattern;
    wr16(s_tx + n, cfg.length);
    n += 2;
    s_tx[n++] = cfg.scale;
    s_tx[n++] = cfg.root;
    s_tx[n++] = cfg.swing;
    const size_t len = strnlen(cfg.name, sizeof(cfg.name));
    memcpy(s_tx + n, cfg.name, len);
    n += len;
    s_tx[n++] = 0;
    s_tx[0] = OP_SEQ_PATTERN | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = ST_OK;
    send_frame(s_tx, n);
}

void handle_seq_plock(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_SEQ_PLOCK, seq, ST_MALFORMED);
        return;
    }
    switch (p[0]) {
        case 0: { /* list the locks on one step */
            if (plen != 5) break;
            const int pattern = p[1], track = p[2];
            const int step = rd16(p + 3);
            if (!seq_args_ok(pattern, track) || step >= SEQ_MAX_STEPS) {
                send_status(OP_SEQ_PLOCK, seq, ST_BAD_ARG);
                return;
            }
            seq_plock_t locks[SEQ_TRACK_LOCKS];
            const int n = seq_plocks_for_step(pattern, track, step, locks,
                                              SEQ_TRACK_LOCKS);
            /* The kind byte leads, so a per-step response can never be
             * mistaken for the pattern-wide one (kind 4) when the pattern
             * index happens to be 4. */
            uint8_t prefix[5];
            prefix[0] = 0;
            prefix[1] = (uint8_t)pattern;
            prefix[2] = (uint8_t)track;
            wr16(prefix + 3, (uint16_t)step);
            s_chunker.begin(OP_SEQ_PLOCK | 0x80, seq, prefix, sizeof(prefix),
                            false);
            for (int i = 0; i < n; ++i) {
                uint8_t rec[6];
                wr16(rec, locks[i].pid);
                wrf32(rec + 2, locks[i].value);
                s_chunker.append(rec, sizeof(rec));
            }
            s_chunker.finish();
            return;
        }
        case 1: { /* set (or, with a NaN value, remove) one lock */
            if (plen != 11) break;
            const int pattern = p[1], track = p[2];
            const int step = rd16(p + 3);
            if (!seq_args_ok(pattern, track) || step >= SEQ_MAX_STEPS) {
                send_status(OP_SEQ_PLOCK, seq, ST_BAD_ARG);
                return;
            }
            const bool ok =
                seq_plock_set(pattern, track, step, rd16(p + 5), rdf32(p + 7));
            send_status(OP_SEQ_PLOCK, seq, ok ? ST_OK : ST_BUSY);
            return;
        }
        case 2: { /* clear every lock on a step */
            if (plen != 5) break;
            const int pattern = p[1], track = p[2];
            const int step = rd16(p + 3);
            /* Same range check as sub-ops 0 and 1. seq_plock_clear_step()
             * re-validates before it touches the step, so this was not a
             * memory bug — but an out-of-range step silently answered OK
             * instead of BAD_ARG, which is not what the other sub-ops do. */
            if (!seq_args_ok(pattern, track) || step >= SEQ_MAX_STEPS) {
                send_status(OP_SEQ_PLOCK, seq, ST_BAD_ARG);
                return;
            }
            seq_plock_clear_step(pattern, track, step);
            send_status(OP_SEQ_PLOCK, seq, ST_OK);
            return;
        }
        case 3: { /* clear a whole pattern's locks */
            if (plen != 2) break;
            if (p[1] >= SEQ_PATTERNS) {
                send_status(OP_SEQ_PLOCK, seq, ST_BAD_ARG);
                return;
            }
            seq_plock_clear_pattern(p[1]);
            send_status(OP_SEQ_PLOCK, seq, ST_OK);
            return;
        }
        case 4: { /* every lock in a pattern, for the app's grid shading */
            if (plen != 2) break;
            const int pattern = p[1];
            if (pattern >= SEQ_PATTERNS) {
                send_status(OP_SEQ_PLOCK, seq, ST_BAD_ARG);
                return;
            }
            const uint8_t prefix[2] = {4, (uint8_t)pattern};
            s_chunker.begin(OP_SEQ_PLOCK | 0x80, seq, prefix, sizeof(prefix),
                            false);
            const int total = seq_plock_count();
            for (int i = 0; i < total; ++i) {
                seq_plock_t l;
                if (!seq_plock_get_at(i, &l) || l.pattern != pattern) continue;
                uint8_t rec[9];
                rec[0] = l.track;
                wr16(rec + 1, l.step);
                wr16(rec + 3, l.pid);
                wrf32(rec + 5, l.value);
                s_chunker.append(rec, sizeof(rec));
            }
            s_chunker.finish();
            return;
        }
        default:
            send_status(OP_SEQ_PLOCK, seq, ST_BAD_ARG);
            return;
    }
    send_status(OP_SEQ_PLOCK, seq, ST_MALFORMED);
}

void handle_seq_edit(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_SEQ_EDIT, seq, ST_MALFORMED);
        return;
    }
    switch (p[0]) {
        case 0: /* clear pattern */
            if (plen != 2 || p[1] >= SEQ_PATTERNS) break;
            seq_pattern_clear(p[1]);
            send_status(OP_SEQ_EDIT, seq, ST_OK);
            return;
        case 1: /* clear track */
            if (plen != 3 || !seq_args_ok(p[1], p[2])) break;
            seq_track_clear(p[1], p[2]);
            send_status(OP_SEQ_EDIT, seq, ST_OK);
            return;
        case 2: /* copy pattern */
            if (plen != 3 || p[1] >= SEQ_PATTERNS || p[2] >= SEQ_PATTERNS) break;
            seq_pattern_copy(p[1], p[2]);
            send_status(OP_SEQ_EDIT, seq, ST_OK);
            return;
        case 3: /* rotate track */
            if (plen != 4 || !seq_args_ok(p[1], p[2])) break;
            seq_track_rotate(p[1], p[2], (int8_t)p[3]);
            send_status(OP_SEQ_EDIT, seq, ST_OK);
            return;
        case 4: /* euclidean fill */
            if (plen != 9 || !seq_args_ok(p[1], p[2])) break;
            seq_track_euclid(p[1], p[2], p[3], rd16(p + 4), (int8_t)p[6], p[7],
                             p[8]);
            send_status(OP_SEQ_EDIT, seq, ST_OK);
            return;
        case 5: /* bake humanisation into the stored velocities/micro */
            if (plen != 4 || !seq_args_ok(p[1], p[2])) break;
            seq_track_humanize(p[1], p[2], p[3]);
            send_status(OP_SEQ_EDIT, seq, ST_OK);
            return;
        case 6: { /* toggle a step — the grid's primary gesture */
            if (plen != 6 || !seq_args_ok(p[1], p[2])) break;
            const bool filled = seq_step_toggle(p[1], p[2], rd16(p + 3), p[5]);
            uint8_t f[6] = {OP_SEQ_EDIT | 0x80, seq, 2, 0, ST_OK,
                            (uint8_t)(filled ? 1 : 0)};
            send_frame(f, sizeof(f));
            return;
        }
        default:
            send_status(OP_SEQ_EDIT, seq, ST_BAD_ARG);
            return;
    }
    send_status(OP_SEQ_EDIT, seq, ST_MALFORMED);
}

void handle_seq_song(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_SEQ_SONG, seq, ST_MALFORMED);
        return;
    }
    if (p[0] == 1) { /* set the whole chain in one frame */
        if (plen < 2) {
            send_status(OP_SEQ_SONG, seq, ST_MALFORMED);
            return;
        }
        int len = p[1];
        if (len > SEQ_SONG_MAX) len = SEQ_SONG_MAX;
        if (plen != (uint16_t)(2 + len * 2)) {
            send_status(OP_SEQ_SONG, seq, ST_MALFORMED);
            return;
        }
        for (int i = 0; i < len; ++i) {
            seq_song_entry_t e;
            e.pattern = p[2 + i * 2];
            e.repeats = p[3 + i * 2];
            seq_song_set(i, &e);
        }
        seq_song_set_length(len);
        send_status(OP_SEQ_SONG, seq, ST_OK);
        return;
    }
    const uint8_t prefix[1] = {(uint8_t)seq_song_length()};
    s_chunker.begin(OP_SEQ_SONG | 0x80, seq, prefix, sizeof(prefix), false);
    for (int i = 0; i < seq_song_length(); ++i) {
        seq_song_entry_t e;
        seq_song_get(i, &e);
        const uint8_t rec[2] = {e.pattern, e.repeats};
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

void handle_kit_info(uint8_t seq, const uint8_t* p, uint16_t plen) {
    const uint8_t what = plen >= 1 ? p[0] : 1;
    if (what == 0) { /* the selectable kits */
        const uint8_t prefix[3] = {0, (uint8_t)ParamStore::instance().get(
                                          DRUM_PID_KIT),
                                   (uint8_t)drums_kit_count()};
        s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false);
        for (int i = 0; i < drums_kit_count(); ++i) {
            uint8_t rec[1 + DRUM_KIT_NAME_MAX] = {};
            rec[0] = (uint8_t)i;
            /* rec is zero-initialised, so the name stays NUL-padded to its
             * fixed wire width; strlcpy always terminates. */
            strlcpy((char*)rec + 1, drums_kit_name_at(i), DRUM_KIT_NAME_MAX);
            s_chunker.append(rec, sizeof(rec));
        }
        s_chunker.finish();
        return;
    }
    /* the current kit's slots: what the app labels the drum lanes and the
     * mixer strips with, since the parameters themselves are named
     * generically (drum1.level …) and outlive any one kit */
    const uint8_t prefix[3] = {1, (uint8_t)drums_slot_count(), 0};
    s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false);
    for (int i = 0; i < drums_slot_count() && i < DRUM_SLOTS; ++i) {
        uint8_t rec[2 + DRUM_SLOT_NAME_MAX] = {};
        rec[0] = (uint8_t)i;
        const int note = drums_slot_note(i);
        rec[1] = (uint8_t)(note >= 0 ? note : 0);
        strlcpy((char*)rec + 2, drums_slot_name(i), DRUM_SLOT_NAME_MAX);
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

/* ---- loop track download (S33) ----------------------------------------
 *
 * Two sub-ops behind one opcode, the direction-byte convention the sequencer
 * block already uses:
 *
 *   0 INFO [u8 source][u8 slot]
 *       -> [u8 0][u8 source][u8 slot][u8 filled][u8 codec][u8 tracks]
 *          [u8 rsvd][u32 loop_frames][u32 sample_rate][u32 track_bytes]
 *     `filled` 0 means "nothing to download there", which is an answer and
 *     not an error — an unused slot and an empty live set both give it.
 *
 *   1 READ [u8 source][u8 slot][u8 track][u32 offset][u16 len]
 *       -> one or more frames, ST_MORE on all but the last:
 *          [u8 1][u8 track][u32 offset][u16 n][n bytes]
 *     Each frame carries its own offset, so the app can spot a gap and ask
 *     again from exactly there; a final frame with n == 0 is the end of the
 *     track. The bytes are the stored codec, undecoded (looper.h).
 *
 * Windowed rather than a "start streaming" command on purpose. The synth
 * sends only what the last request asked for, so the app's buffer, the
 * host's mbuf pool and a cancel all have the same natural bound — one
 * window — and there is no transfer state on this side to get out of step
 * with the app's. */
constexpr uint32_t kDumpWindow = 2048;
uint8_t* s_dump = nullptr; /* ble_cmd task only; kept once allocated */

bool ensure_dump_buf() {
    if (s_dump == nullptr) {
        /* Internal RAM: it is small, it is touched on every frame of a long
         * transfer, and a build with no PSRAM has no looper to dump anyway —
         * so it is never allocated there. */
        s_dump = (uint8_t*)heap_caps_malloc(kDumpWindow,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return s_dump != nullptr;
}

uint8_t dump_status(esp_err_t err) {
    switch (err) {
        case ESP_OK: return ST_OK;
        case ESP_ERR_NOT_SUPPORTED: return ST_UNSUPPORTED;
        case ESP_ERR_INVALID_ARG: return ST_BAD_ARG;
        case ESP_ERR_NOT_FOUND: return ST_BAD_ARG; /* no such track/slot */
        /* "Not now": a take is open, or the flash backend wants the transport
         * stopped. Both clear on their own, which is what BUSY means here. */
        case ESP_ERR_INVALID_STATE: return ST_BUSY;
        default: return ST_MALFORMED;
    }
}

void handle_loop_dump(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_LOOP_DUMP, seq, ST_MALFORMED);
        return;
    }
    if (p[0] == 0) { /* INFO */
        if (plen < 3) {
            send_status(OP_LOOP_DUMP, seq, ST_MALFORMED);
            return;
        }
        looper_export_info_t info;
        const esp_err_t err = looper_export_info(p[1], p[2], &info);
        if (err != ESP_OK) {
            send_status(OP_LOOP_DUMP, seq, dump_status(err));
            return;
        }
        size_t n = 5;
        s_tx[n++] = 0; /* sub */
        s_tx[n++] = p[1];
        s_tx[n++] = p[2];
        s_tx[n++] = info.filled;
        s_tx[n++] = info.codec;
        s_tx[n++] = LOOP_TRACKS;
        s_tx[n++] = 0; /* rsvd */
        wr32(s_tx + n, info.loop_frames);
        n += 4;
        wr32(s_tx + n, info.sample_rate);
        n += 4;
        wr32(s_tx + n, info.track_bytes);
        n += 4;
        s_tx[0] = OP_LOOP_DUMP | 0x80;
        s_tx[1] = seq;
        wr16(s_tx + 2, (uint16_t)(n - 4));
        s_tx[4] = ST_OK;
        send_frame(s_tx, n);
        return;
    }
    if (p[0] != 1) {
        send_status(OP_LOOP_DUMP, seq, ST_MALFORMED);
        return;
    }
    if (plen < 10) { /* sub, source, slot, track, u32 offset, u16 len */
        send_status(OP_LOOP_DUMP, seq, ST_MALFORMED);
        return;
    }
    if (!ensure_dump_buf()) {
        send_status(OP_LOOP_DUMP, seq, ST_BUSY);
        return;
    }
    const uint8_t track = p[3];
    const uint32_t offset = rd32(p + 4);
    uint32_t want = rd16(p + 8);
    if (want > kDumpWindow) want = kDumpWindow;
    uint32_t got = 0;
    const esp_err_t err = looper_export_read(p[1], p[2], track, offset, s_dump,
                                             want, &got);
    if (err != ESP_OK) {
        send_status(OP_LOOP_DUMP, seq, dump_status(err));
        return;
    }
    const size_t avail = avail_payload();
    if (avail <= 8) { /* an MTU this small cannot carry a data frame at all */
        send_status(OP_LOOP_DUMP, seq, ST_UNSUPPORTED);
        return;
    }
    const uint32_t cap = (uint32_t)(avail - 8);
    uint32_t sent = 0;
    do { /* do-while: got == 0 still emits the end-of-track frame */
        const uint32_t n = (got - sent) < cap ? (got - sent) : cap;
        s_tx[0] = OP_LOOP_DUMP | 0x80;
        s_tx[1] = seq;
        wr16(s_tx + 2, (uint16_t)(1 + 8 + n));
        s_tx[4] = (sent + n) < got ? (uint8_t)(ST_OK | ST_MORE) : (uint8_t)ST_OK;
        s_tx[5] = 1; /* sub */
        s_tx[6] = track;
        wr32(s_tx + 7, offset + sent);
        wr16(s_tx + 11, (uint16_t)n);
        memcpy(s_tx + 13, s_dump + sent, n);
        if (!send_frame_paced(s_tx, 13 + n)) return; /* link gone */
        sent += n;
    } while (sent < got);
}

/* ---- modular patch graph (S28) ---------------------------------------- */

#if SYNTH_ENABLE_MODULAR

namespace gr = osynth::graph;

/* Sizing and budget, so the app never assumes a capacity this build does
 * not have — the same contract SEQ_INFO provides for the sequencer. */
void handle_graph_info(uint8_t seq) {
    size_t n = 5;
    s_tx[n++] = gr::kMaxNodes;
    s_tx[n++] = gr::kNodeParams;
    s_tx[n++] = gr::kMaxInputs;
    s_tx[n++] = gr::kOutSlot;
    s_tx[n++] = (uint8_t)gr::Kind::Count;
    wr16(s_tx + n, gr::kCostBudget);
    n += 2;
    wr16(s_tx + n, gr::live_cost());
    n += 2;
    wr16(s_tx + n, (uint16_t)(gr::model().revision & 0xFFFF));
    n += 2;
    /* Which engine index the app must select to reach the graph. Sending it
     * rather than letting the app hardcode 4 keeps the two in step if the
     * engine list ever changes again. */
    s_tx[n++] = (uint8_t)SYNTH_ENGINE_MODULAR;
    s_tx[0] = OP_GRAPH_INFO | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = ST_OK;
    send_frame(s_tx, n);
}

/* One node kind's descriptor: everything the app needs to draw that node
 * and label its jacks. Payload: {kind, rate, n_inputs, n_params, cost16,
 * name, NUL, input names each NUL-terminated, parameter name suffixes each
 * NUL-terminated}. Strings rather than indices because the app has no table
 * of its own to look them up in — the firmware is the only definition. */
void handle_graph_kind(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_GRAPH_KIND, seq, ST_MALFORMED);
        return;
    }
    const int k = p[0];
    if (k < 0 || k >= (int)gr::Kind::Count) {
        send_status(OP_GRAPH_KIND, seq, ST_BAD_ARG);
        return;
    }
    const gr::KindDesc& d = gr::kind_desc((gr::Kind)k);
    size_t n = 5;
    s_tx[n++] = (uint8_t)k;
    s_tx[n++] = (uint8_t)d.rate;
    s_tx[n++] = d.n_inputs;
    s_tx[n++] = d.n_params;
    wr16(s_tx + n, d.cost);
    n += 2;
    auto put_str = [&](const char* s) {
        const size_t len = strlen(s);
        if (n + len + 1 > kMaxFrame) return false;
        memcpy(s_tx + n, s, len);
        n += len;
        s_tx[n++] = 0;
        return true;
    };
    bool ok = put_str(d.name);
    for (int i = 0; ok && i < d.n_inputs; ++i) ok = put_str(d.input_names[i]);
    for (int i = 0; ok && i < d.n_params; ++i) ok = put_str(d.params[i].suffix);
    if (!ok) {
        /* Cannot happen with the current table — the widest kind is well
         * under the frame ceiling — but a kind added later must fail loudly
         * rather than send a truncated descriptor the app would misparse. */
        send_status(OP_GRAPH_KIND, seq, ST_UNSUPPORTED);
        return;
    }
    s_tx[0] = OP_GRAPH_KIND | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = ST_OK;
    send_frame(s_tx, n);
}

/* The whole model, in the same v1 byte format presets store — one shape for
 * the wire and the file means the two cannot drift. dir 0 = get, 1 = set. */
void handle_graph_nodes(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_GRAPH_NODES, seq, ST_MALFORMED);
        return;
    }
    if (p[0] == 0) { /* get */
        uint8_t blob[gr::kSerialMaxBytes];
        const size_t len = gr::serialize(gr::model(), blob, sizeof(blob));
        if (len == 0 || 5 + len > kMaxFrame) {
            send_status(OP_GRAPH_NODES, seq, ST_UNSUPPORTED);
            return;
        }
        memcpy(s_tx + 5, blob, len);
        s_tx[0] = OP_GRAPH_NODES | 0x80;
        s_tx[1] = seq;
        wr16(s_tx + 2, (uint16_t)(len + 1));
        s_tx[4] = ST_OK;
        send_frame(s_tx, 5 + len);
        return;
    }
    gr::Model m;
    if (!gr::deserialize(p + 1, plen - 1, m)) {
        send_status(OP_GRAPH_NODES, seq, ST_MALFORMED);
        return;
    }
    const esp_err_t rc = gr::load_model(m);
    send_status(OP_GRAPH_NODES, seq,
                rc == ESP_OK ? ST_OK
                             : (rc == ESP_ERR_NOT_SUPPORTED ? ST_UNSUPPORTED
                                                            : ST_BAD_ARG));
}

/* A single edit: {cmd, a, b, c…}. The reply carries the resulting revision
 * and cost even on failure, because a rejected edit is exactly when the app
 * most needs to know where it actually stands — the model is unchanged, and
 * the app can redraw from the values here instead of guessing. */
void handle_graph_edit(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 2) {
        send_status(OP_GRAPH_EDIT, seq, ST_MALFORMED);
        return;
    }
    esp_err_t rc = ESP_ERR_INVALID_ARG;
    switch (p[0]) {
        case 0: /* set kind: {slot, kind} */
            if (plen >= 3) rc = gr::set_kind(p[1], (gr::Kind)p[2]);
            break;
        case 1: /* connect: {dst, port, src} — src 0xFF disconnects */
            if (plen >= 4) {
                rc = gr::connect(p[1], p[2], (p[3] == 0xFF) ? -1 : (int)p[3]);
            }
            break;
        case 2: /* canvas position: {slot, x16, y16} */
            if (plen >= 6) {
                rc = gr::set_ui_pos(p[1], (int16_t)rd16(p + 2),
                                    (int16_t)rd16(p + 4));
            }
            break;
        default:
            rc = ESP_ERR_INVALID_ARG;
            break;
    }
    uint8_t st = ST_OK;
    if (rc == ESP_ERR_NOT_SUPPORTED) {
        st = ST_UNSUPPORTED; /* over the CPU budget */
    } else if (rc == ESP_ERR_INVALID_STATE) {
        st = ST_BUSY; /* cycle: the one rejection that is about shape */
    } else if (rc != ESP_OK) {
        st = ST_BAD_ARG;
    }
    size_t n = 5;
    wr16(s_tx + n, (uint16_t)(gr::model().revision & 0xFFFF));
    n += 2;
    wr16(s_tx + n, gr::live_cost());
    n += 2;
    s_tx[0] = OP_GRAPH_EDIT | 0x80;
    s_tx[1] = seq;
    wr16(s_tx + 2, (uint16_t)(n - 4));
    s_tx[4] = st;
    send_frame(s_tx, n);
}

#endif /* SYNTH_ENABLE_MODULAR */

void handle_frame(const uint8_t* d, size_t n) {
    if (n < 4) return; /* not even a header — nothing to respond to */
    const uint8_t op = d[0];
    const uint8_t seq = d[1];
    const uint16_t plen = rd16(d + 2);
    if ((size_t)plen + 4 != n) {
        send_status(op, seq, ST_MALFORMED);
        return;
    }
    const uint8_t* p = d + 4;
    ParamStore& ps = ParamStore::instance();

    switch (op) {
        case OP_SET_PARAM:
            handle_set_param(seq, p, plen);
            break;
        case OP_GET_PARAM:
            handle_get_param(seq, p, plen);
            break;
        case OP_PARAM_INFO:
            handle_param_info(seq, p, plen);
            break;
        case OP_SELECT_ENGINE:
            if (plen != 1) {
                send_status(op, seq, ST_MALFORMED);
            } else if (p[0] >= SYNTH_ENGINE_COUNT) {
                send_status(op, seq, ST_BAD_ARG);
            } else {
                /* async: the S6 switch task runs it; completion arrives as
                 * EVT_ENGINE (failure reverts engine.type -> EVT_PARAMS) */
                ps.set(osynth::PID_ENGINE_TYPE, (float)p[0], ParamOrigin::Ble);
                send_status(op, seq, ST_OK);
            }
            break;
        case OP_LOAD_PRESET:
            if (plen != 2) {
                send_status(op, seq, ST_MALFORMED);
            } else {
                /* queued; completion shows up as an EVT_PARAMS for
                 * preset.load (the trigger rests on the loaded slot) */
                send_status(op, seq,
                            status_from(presets_request_load(p[0], p[1])));
            }
            break;
        case OP_SAVE_PRESET:
            if (plen < 2) {
                send_status(op, seq, ST_MALFORMED);
            } else {
                char name[PRESETS_NAME_MAX] = {};
                size_t len = plen - 2;
                if (len > PRESETS_NAME_MAX - 1) len = PRESETS_NAME_MAX - 1;
                memcpy(name, p + 2, len); /* NUL-padded by the init above */
                send_status(op, seq,
                            status_from(presets_request_save(
                                p[0], p[1], name[0] != '\0' ? name : nullptr)));
            }
            break;
        case OP_LIST_PRESETS:
            handle_list_presets(seq, p, plen);
            break;
        case OP_USB_STATUS:
            handle_usb_status(seq);
            break;
        case OP_REBOOT:
            /* No payload, and deliberately no "are you sure" byte: the
             * confirmation belongs in the app, where there is a user to ask.
             * A magic number here would only be ceremony on a link that
             * already needs a paired connection to reach. */
            handle_reboot(seq);
            break;
        case OP_TRANSPORT:
            handle_transport(seq, p, plen);
            break;
        case OP_ARP:
            handle_arp(seq, p, plen);
            break;
        case OP_NOTE_ON: /* velocity 0 = note off, MIDI semantics */
            if (plen != 2) {
                send_status(op, seq, ST_MALFORMED);
            } else {
                midi_route_channel_message(0x90, p[0] & 0x7F, p[1] & 0x7F);
            }
            break;
        case OP_NOTE_OFF:
            if (plen != 1) {
                send_status(op, seq, ST_MALFORMED);
            } else {
                midi_route_channel_message(0x80, p[0] & 0x7F, 0);
            }
            break;
        case OP_PING: {
            const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
            uint8_t f[9] = {OP_PING | 0x80, seq, 5, 0, ST_OK};
            memcpy(f + 5, &up, 4);
            send_frame(f, sizeof(f));
            break;
        }
        case OP_SEQ_INFO:
            handle_seq_info(seq);
            break;
        case OP_SEQ_STEPS:
            handle_seq_steps(seq, p, plen);
            break;
        case OP_SEQ_TRACK:
            handle_seq_track(seq, p, plen);
            break;
        case OP_SEQ_PATTERN:
            handle_seq_pattern(seq, p, plen);
            break;
        case OP_SEQ_PLOCK:
            handle_seq_plock(seq, p, plen);
            break;
        case OP_SEQ_EDIT:
            handle_seq_edit(seq, p, plen);
            break;
        case OP_SEQ_SONG:
            handle_seq_song(seq, p, plen);
            break;
        case OP_KIT_INFO:
            handle_kit_info(seq, p, plen);
            break;
        case OP_LOOP_DUMP:
            /* Answers UNSUPPORTED by itself on a build with no looper (the
             * no-PSRAM stubs in looper.cpp), which is what tells the app to
             * leave the download controls off the page. */
            handle_loop_dump(seq, p, plen);
            break;
#if SYNTH_ENABLE_MODULAR
        case OP_GRAPH_INFO:
            handle_graph_info(seq);
            break;
        case OP_GRAPH_KIND:
            handle_graph_kind(seq, p, plen);
            break;
        case OP_GRAPH_NODES:
            handle_graph_nodes(seq, p, plen);
            break;
        case OP_GRAPH_EDIT:
            handle_graph_edit(seq, p, plen);
            break;
#else
        case OP_GRAPH_INFO:
        case OP_GRAPH_KIND:
        case OP_GRAPH_NODES:
        case OP_GRAPH_EDIT:
            /* Answered rather than left to fall through to UNKNOWN_OP: the
             * app probes GRAPH_INFO to decide whether to offer the patch
             * page at all, and UNSUPPORTED is the honest "this build has no
             * graph" — same shape as the looper probes on a classic ESP32. */
            send_status(op, seq, ST_UNSUPPORTED);
            break;
#endif
        case OP_DRUM_TRIG: /* pads: {slot, velocity}, no response on success */
            if (plen != 2) {
                send_status(op, seq, ST_MALFORMED);
            } else {
                drums_trigger(p[0], p[1], 0);
                /* ...and into the sequencer if it is armed. A pad addresses a
                 * kit slot directly rather than going through the MIDI router,
                 * so it never met the recorder: arming rec and playing the pads
                 * used to record nothing at all. A no-op unless seq.mode is
                 * rec, so the hit costs the same as before while just playing.
                 * Velocity 0 is the capability probe the app fires at discovery
                 * and is dropped inside. */
                (void)seqarp_record_drum(p[0], p[1]);
            }
            break;
        case OP_RENAME_PRESET:
        case OP_BULK:
            send_status(op, seq, ST_UNSUPPORTED); /* reserved in v1 */
            break;
        default:
            send_status(op, seq, ST_UNKNOWN_OP);
            break;
    }
}

/* ---- event side ------------------------------------------------------- */

/* Any control task; short — just marks the id dirty. Origin Ble is the
 * app's own write: suppressed, the app never gets an echo. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    (void)value;
    if (origin == ParamOrigin::Ble || id >= osynth::PID_SPACE_END) return;
    s_dirty[id >> 5].fetch_or(1u << (id & 31), std::memory_order_relaxed);
}

void flush_events() {
    const bool live = s_conn.load(std::memory_order_relaxed) !=
                          BLE_HS_CONN_HANDLE_NONE &&
                      s_evt_sub.load(std::memory_order_relaxed);

    /* engine announcement: covers MIDI program change, preset loads and
     * BLE's own SELECT_ENGINE (the app must re-discover 0x02xx either way) */
    const int eng = (int)engines_active_type();
    if (live && eng != s_announced_engine.load(std::memory_order_relaxed)) {
        const synth_engine_t* e = engines_get((synth_engine_type_t)eng);
        const uint8_t f[7] = {EVT_ENGINE, 0, 3, 0, ST_OK, (uint8_t)eng,
                              (uint8_t)(e != nullptr ? e->caps : 0)};
        if (send_frame(f, sizeof(f))) {
            s_announced_engine.store(eng, std::memory_order_relaxed);
        }
    }

    ParamStore& ps = ParamStore::instance();
    constexpr size_t kDirtyWords = sizeof(s_dirty) / sizeof(s_dirty[0]);

    /* Take the whole dirty set first and keep a copy. A notification can fail
     * — ble_gatts_notify_custom has no mbuf to hand when the host task is
     * backed up — and the bits were already cleared by the exchange, so
     * without this the change is gone for good and the app never learns it.
     * Survivable for a knob (the next turn re-marks it); not at all
     * survivable for something that changes once, like loop.filled after a
     * take, where the clears, the save button and the track lights would
     * stay wrong until the app reconnects.
     *
     * On failure the set is re-armed and the next flush retries. ps.get()
     * reads at send time, so a retry always carries the latest value. */
    uint32_t taken[kDirtyWords];
    for (size_t w = 0; w < kDirtyWords; ++w) {
        taken[w] = s_dirty[w].exchange(0, std::memory_order_relaxed);
    }
    if (!live) return; /* nobody listening: drop, don't accumulate */

    s_chunker.begin(EVT_PARAMS, 0, nullptr, 0, true);
    for (size_t w = 0; w < kDirtyWords; ++w) {
        uint32_t bits = taken[w];
        while (bits != 0) {
            const int b = __builtin_ctz(bits);
            bits &= bits - 1;
            const uint16_t id = (uint16_t)(w * 32 + b);
            if (ps.describe(id) == nullptr) continue; /* unregistered since */
            uint8_t rec[6];
            wr16(rec, id);
            wrf32(rec + 2, ps.get(id)); /* latest value — bursts coalesce */
            s_chunker.append(rec, sizeof(rec));
        }
    }
    s_chunker.finish();

    if (s_chunker.failed()) {
        for (size_t w = 0; w < kDirtyWords; ++w) {
            if (taken[w] != 0) {
                s_dirty[w].fetch_or(taken[w], std::memory_order_relaxed);
            }
        }
        ESP_LOGD(TAG, "event flush failed — re-armed for the next pass");
    }
}

void cmd_task(void*) {
    CmdBuf cmd;
    TickType_t last_flush = xTaskGetTickCount();
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, kFlushPeriod) == pdTRUE) {
            handle_frame(cmd.data, cmd.len);
        }
        const TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_flush) >= kFlushPeriod) {
            flush_events();
            last_flush = now;
        }
    }
}

/* ---- GATT ------------------------------------------------------------- */

int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt* ctxt,
                void*) {
    (void)conn;
    (void)attr;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    CmdBuf cmd;
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, cmd.data, sizeof(cmd.data), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cmd.len = len;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, frame dropped");
        if (len >= 4) send_status(cmd.data[0], cmd.data[1], ST_BUSY);
    }
    return 0;
}

int evt_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt*, void*) {
    return BLE_ATT_ERR_UNLIKELY; /* notify-only; never actually called */
}

int bulk_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGD(TAG, "bulk write ignored (reserved in protocol v1)");
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

int info_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t info[4 + sizeof(CONFIG_IDF_TARGET)] = {
        kProtoVersion, kFwVersion[0], kFwVersion[1], kFwVersion[2]};
    memcpy(info + 4, CONFIG_IDF_TARGET, sizeof(CONFIG_IDF_TARGET));
    return os_mbuf_append(ctxt->om, info, sizeof(info)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Sparse designated initializers keep these tables portable across NimBLE
 * struct revisions (e.g. the cpfd member is IDF 6-only) — the skipped
 * members are zero, which is exactly what the stack expects. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const struct ble_gatt_chr_def kChrs[] = {
    {.uuid = &kCtrlUuid.u,
     .access_cb = ctrl_access,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = &kEvtUuid.u,
     .access_cb = evt_access,
     .flags = BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_evt_handle},
    {.uuid = &kBulkUuid.u,
     .access_cb = bulk_access,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &s_bulk_handle},
    {.uuid = &kInfoUuid.u,
     .access_cb = info_access,
     .flags = BLE_GATT_CHR_F_READ},
    {},
};

const struct ble_gatt_svc_def kSvcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &kSvcUuid.u,
     .characteristics = kChrs},
    {},
};

#pragma GCC diagnostic pop

/* ---- GAP -------------------------------------------------------------- */

void start_advertising();

/* Connection interval policy (S30). A key press is one ATT frame, and a frame
 * can only leave the central on a connection event — so the interval is the
 * floor under the app's key-to-sound latency, and a chord is that floor once
 * per note. Centrals pick a lazy one on their own: Android settles at 30-50 ms
 * once discovery is done, which is why the on-screen keyboard feels sluggish
 * even though the synth answers a frame in microseconds.
 *
 * A peripheral is allowed to ask for better, so it does — 7.5-15 ms, the
 * fastest range every central must support, with no slave latency (a note must
 * never wait for a skipped event). Apple's rules are stricter (interval min
 * >= 15 ms), so a refusal retries once at 15-30 ms, still well under the
 * default. A second refusal is left alone: the link works, it is just lazier
 * than we would like, and nagging a central that has said no costs airtime on
 * the very path we are trying to keep clear.
 *
 * Sent when the app subscribes to EVT rather than on connect: discovery is
 * over by then, so the request is not competing with the burst it would
 * accelerate, and centrals are far more likely to accept an update on an idle
 * link. */
constexpr uint16_t kFastItvlMin = 6;   /* x1.25 ms = 7.5 ms */
constexpr uint16_t kFastItvlMax = 12;  /* 15 ms                */
constexpr uint16_t kSafeItvlMin = 12;  /* 15 ms — Apple's floor */
constexpr uint16_t kSafeItvlMax = 24;  /* 30 ms                 */
constexpr uint16_t kConnTimeout = 400; /* x10 ms = 4 s supervision timeout */

/* Host task only (every touch is inside a GAP callback). */
uint8_t s_upd_tries = 0;

void request_fast_conn(uint16_t conn_handle, bool apple_safe) {
    struct ble_gap_upd_params p = {};
    p.itvl_min = apple_safe ? kSafeItvlMin : kFastItvlMin;
    p.itvl_max = apple_safe ? kSafeItvlMax : kFastItvlMax;
    p.latency = 0;
    p.supervision_timeout = kConnTimeout;
    const int rc = ble_gap_update_params(conn_handle, &p);
    if (rc != 0) {
        /* Local refusal (no such connection, one already in flight): nothing
         * to retry against — the result path below only sees requests that
         * actually went out. */
        ESP_LOGW(TAG, "conn param request not sent (rc %d)", rc);
        return;
    }
    ESP_LOGI(TAG, "asking for a %u.%02u-%u.%02u ms connection interval",
             (unsigned)(p.itvl_min * 125u) / 100u,
             (unsigned)(p.itvl_min * 125u) % 100u,
             (unsigned)(p.itvl_max * 125u) / 100u,
             (unsigned)(p.itvl_max * 125u) % 100u);
}

int gap_event(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status == 0) {
                s_conn.store(ev->connect.conn_handle,
                             std::memory_order_relaxed);
                s_state.store("connected", std::memory_order_relaxed);
                s_upd_tries = 0;
                ESP_LOGI(TAG, "app connected");
            } else {
                start_advertising();
            }
            return 0;
        /* The result of a parameter change — ours (see request_fast_conn) or
         * one the central made on its own. Worth logging either way: the
         * interval is what every request/response on this protocol is really
         * priced in — a discovery pass is made of round trips, and a played
         * note is one frame waiting for the next connection event. */
        case BLE_GAP_EVENT_CONN_UPDATE: {
            /* s_upd_tries == 1 means our request is the one being answered:
             * it is only ever set at SUBSCRIBE, so a central-driven update
             * during discovery (which happens on Android) is still 0 here and
             * falls through to the log. Either outcome settles us at 2 — the
             * link is as good as it is going to get, and a later update the
             * central makes on its own must not be mistaken for our result. */
            if (s_upd_tries == 1) {
                s_upd_tries = 2;
                if (ev->conn_update.status != 0) {
                    /* Refused. One retry inside Apple's window, then we live
                     * with whatever the central chose. */
                    ESP_LOGI(TAG,
                             "conn params refused (status 0x%02x), retrying",
                             (unsigned)ev->conn_update.status);
                    request_fast_conn(ev->conn_update.conn_handle, true);
                    return 0;
                }
            }
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(ev->conn_update.conn_handle, &desc) == 0) {
                /* itvl is in 1.25 ms units; print the ms as x.xx */
                const unsigned q = (unsigned)desc.conn_itvl * 125u;
                ESP_LOGI(TAG,
                         "conn params: itvl %u.%02u ms, latency %u, timeout "
                         "%u ms%s",
                         q / 100u, q % 100u, (unsigned)desc.conn_latency,
                         (unsigned)desc.supervision_timeout * 10u,
                         ev->conn_update.status == 0 ? "" : " (request refused)");
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            s_conn.store(BLE_HS_CONN_HANDLE_NONE, std::memory_order_relaxed);
            s_evt_sub.store(false, std::memory_order_relaxed);
            ESP_LOGI(TAG, "app disconnected (reason 0x%02x)",
                     (unsigned)ev->disconnect.reason);
            /* Release anything the app was holding. OP_NOTE_ON goes into the
             * MIDI router like a played key, so its note-off has to come back
             * over the same link — and a link that just dropped will never
             * deliver it. Without this, walking out of range mid-chord leaves
             * the synth droning until a CC 123 arrives from somewhere else.
             * Safe from the host task: the entry point is the same lock-free
             * ring every other control task pushes through. */
            voice_manager_all_notes_off();
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (ev->subscribe.attr_handle == s_evt_handle) {
                s_evt_sub.store(ev->subscribe.cur_notify,
                                std::memory_order_relaxed);
                /* greet a fresh subscriber with the engine + caps */
                s_announced_engine.store(-1, std::memory_order_relaxed);
                /* The app is up: ask for an interval a keyboard can play on.
                 * Once per subscription — a re-subscribe on the same link is
                 * the app re-reading, not a new central. */
                if (ev->subscribe.cur_notify && s_upd_tries == 0) {
                    s_upd_tries = 1;
                    request_fast_conn(ev->subscribe.conn_handle, false);
                }
            }
            return 0;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu %u", (unsigned)ev->mtu.value);
            return 0;
        default:
            return 0;
    }
}

void start_advertising() {
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&kSvcUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = (uint8_t*)kDeviceName;
    fields.name_len = sizeof(kDeviceName) - 1;
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (rc == 0) {
        rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER,
                               &params, gap_event, nullptr);
    }
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_state.store("advertising", std::memory_order_relaxed);
    } else {
        ESP_LOGW(TAG, "advertising start failed (rc %d)", rc);
    }
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGW(TAG, "no usable BLE address, staying dark");
        return;
    }
    uint8_t addr[6] = {};
    ble_hs_id_copy_addr(s_own_addr_type, addr, nullptr);
    ESP_LOGI(TAG, "advertising as \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x)",
             kDeviceName, addr[5], addr[4], addr[3], addr[2], addr[1],
             addr[0]);
    start_advertising();
}

void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

void host_task(void*) {
    nimble_port_run(); /* returns on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

} // namespace

extern "C" esp_err_t ble_ctrl_init(void) {
    /* NimBLE logs two INFO lines for every notification ("GATT procedure
     * initiated: notify" plus the handle). The synth sends events at ~20 Hz
     * and floods during parameter discovery, so at INFO the host task spends
     * a large part of its time in blocking UART writes — and it is the task
     * that has to find an mbuf for the next notification, so the chatter
     * costs reliability, not just readability. It also buries every log the
     * firmware itself prints.
     *
     * sdkconfig.defaults compiles it out; this covers a tree whose sdkconfig
     * was generated before that, and costs nothing either way. */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

#if SYNTH_BLE_VIA_HOSTED
    /* No controller on this die: bring the companion up first, because with
     * CONFIG_BT_CONTROLLER_DISABLED the bt component compiles the host with no
     * HCI transport at all — ESP-Hosted supplies it, and nimble_port_init()
     * below would otherwise have nothing to talk to.
     *
     * Every failure here is degraded-but-alive, like the NimBLE failure below:
     * a synth whose companion did not answer still has USB audio and USB-MIDI,
     * and bootlooping an instrument over its remote control is the wrong
     * trade. The co-processor firmware version is logged because host and
     * slave are versioned separately and a mismatch shows up here first. */
    esp_err_t herr = esp_hosted_connect_to_slave();
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "hosted co-processor did not answer (%s) — no BLE",
                 esp_err_to_name(herr));
        return ESP_OK;
    }
    esp_hosted_coprocessor_fwver_t fwver;
    if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
        ESP_LOGI(TAG, "hosted co-processor firmware %u.%u.%u",
                 (unsigned)fwver.major1, (unsigned)fwver.minor1,
                 (unsigned)fwver.patch1);
    }
    herr = esp_hosted_bt_controller_init();
    if (herr == ESP_OK) herr = esp_hosted_bt_controller_enable();
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "hosted BT controller bring-up failed (%s) — no BLE",
                 esp_err_to_name(herr));
        return ESP_OK;
    }
#endif

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        /* e.g. controller RAM on a loaded classic ESP32 — degrade, the
         * synth itself keeps running (the sink-fallback philosophy) */
        ESP_LOGW(TAG, "NimBLE bring-up failed (%s) — continuing without BLE",
                 esp_err_to_name(err));
        return ESP_OK;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc == 0) rc = ble_gatts_add_svcs(kSvcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(kDeviceName);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed (rc %d)", rc);
        return ESP_FAIL;
    }

    s_cmd_queue = xQueueCreate(kCmdQueueDepth, sizeof(CmdBuf));
    if (s_cmd_queue == nullptr) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(cmd_task, "ble_cmd", kTaskStack, nullptr,
                                kTaskPrio, &s_cmd_task, 0) != pdPASS) {
        return ESP_FAIL;
    }
    if (ParamStore::instance().addListener(param_listener, nullptr) < 0) {
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG,
             "up: SynthCtl v1 GATT service (ctrl/evt/bulk/info), device "
             "\"%s\", param events ~20 Hz",
             kDeviceName);
    return ESP_OK;
}

extern "C" const char* ble_ctrl_state_name(void) {
    return s_state.load(std::memory_order_relaxed);
}

#else /* !SYNTH_ENABLE_BLE || !CONFIG_BT_NIMBLE_ENABLED */

extern "C" esp_err_t ble_ctrl_init(void) {
    ESP_LOGI(TAG, "disabled via CONFIG_OSYNTH_ENABLE_BLE");
    return ESP_OK;
}

extern "C" const char* ble_ctrl_state_name(void) {
    return "off";
}

#endif
