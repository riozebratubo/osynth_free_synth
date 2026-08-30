/*
 * osynth — SynthCtl v1 protocol. See include/ctrl_proto.h for the contract and
 * for why this is a component of its own.
 *
 * Lifted out of components/ble_ctrl/ble_ctrl.cpp unchanged. The handlers, the
 * chunker and the frame dispatch are the same code that shipped over BLE --
 * they never referenced a NimBLE type, which is what made the split a move
 * rather than a rewrite. Four things did reference the transport, and those
 * four are now the ctrl_transport_t vtable:
 *
 *     send_frame()        -> t->send
 *     send_frame_paced()  -> t->send_paced
 *     avail_payload()     -> t->avail_payload
 *     the link test in flush_events() -> t->link_up
 *
 * Everything else below this line is byte-identical to what it replaced.
 */
#include "ctrl_proto.h"
#include "ctrl_proto_wire.h" /* the opcodes and status codes */

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h" /* the loop-dump staging buffer */
#include "esp_log.h"
#include "esp_system.h" /* esp_restart(), for OP_REBOOT */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "chord.h"
#include "drum_kit_fmt.h"
#include "drums.h"
#include "engines.h"
#include "looper.h"
#include "midi.h"
#include "persist.h"
#include "presets.h"
#include "seq_model.h"
#include "seq_play.h"
#include "seqarp.h"
#include "synth_config.h"
#include "synth_params.h"
#include "synth_voice.h"
#include "usb_host_midi.h"

#if SYNTH_ENABLE_MODULAR
#include "graph_model.h"
#include "graph_render.h" /* live_cost() for the app's budget meter */
#endif

static const char* TAG = "ctrl_proto";

using osynth::ParamDesc;
using osynth::ParamOrigin;
using osynth::ParamStore;

namespace {

/* kMaxFrame kept as a local name so the moved code reads unchanged; the value
 * is the one the header publishes for transports to size their buffers by. */
constexpr size_t kMaxFrame = CTRL_PROTO_MAX_FRAME;

constexpr uint8_t kProtoVersion = 1;
constexpr uint8_t kFwVersion[3] = {0, 1, 0}; /* keep in sync with main.cpp */


/* ---- the transport ----------------------------------------------------- */

/* Set by ctrl_proto_set_transport(). Read on the protocol task for the send
 * paths, and on any task for link_up(); a plain pointer is enough because a
 * transport is installed at init and detached at shutdown, never swapped
 * while frames are in flight. */
const ctrl_transport_t* s_tp = nullptr;

/* All synth -> app traffic is EVT notifications; the app must subscribe
 * before sending commands (documented flow), so unsubscribed = drop. Under
 * BLE that condition lives in the transport, which reports it by failing. */
bool send_frame(const uint8_t* frame, size_t len) {
    const ctrl_transport_t* t = s_tp;
    return t != nullptr && t->send != nullptr && t->send(frame, len);
}

/* send_frame() with back-pressure, for the loop dump alone.
 *
 * Every other multi-frame response here is a handful of frames of metadata,
 * and a dropped one is the app's to notice and re-request. A track download is
 * thousands of frames of audio in a row, which is the first thing on a link
 * able to outrun the transport's buffers -- and at that rate "the app
 * re-requests" stops being a rare correction and becomes the throughput. So
 * this waits for a buffer instead of failing, which is exactly the pacing the
 * dump wants: it runs on the protocol task, and the only thing it delays is
 * the rest of the download.
 *
 * A transport with no back-pressure to offer leaves send_paced NULL and gets
 * the plain send, which is the old behaviour for every response but this one.
 */
bool send_frame_paced(const uint8_t* frame, size_t len) {
    const ctrl_transport_t* t = s_tp;
    if (t == nullptr) return false;
    if (t->send_paced != nullptr) return t->send_paced(frame, len);
    return send_frame(frame, len);
}

/* Payload bytes (after the status byte) that fit one frame at the transport's
 * current limit. */
size_t avail_payload() {
    const ctrl_transport_t* t = s_tp;
    if (t == nullptr || t->avail_payload == nullptr) return 0;
    return t->avail_payload();
}

bool link_up() {
    const ctrl_transport_t* t = s_tp;
    return t != nullptr && t->link_up != nullptr && t->link_up();
}

/* Small status-only response. */
void send_status(uint8_t op, uint8_t seq, uint8_t status) {
    const uint8_t f[5] = {(uint8_t)(op | 0x80), seq, 1, 0, status};
    send_frame(f, sizeof(f));
}

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

/* Builds record-list frames into s_tx, splitting at the MTU with the
 * ST_MORE continuation bit; `prefix` is repeated in every frame so each
 * one parses standalone. ble_cmd task only. */
class Chunker {
public:
    /* `paced` waits for an mbuf instead of failing (send_frame_paced). It
     * blocks, so it is only legal from ble_cmd — which is where every command
     * handler runs. Every command response passes it; the parameter keeps its
     * `false` default for the one caller that must not have it, flush_events(),
     * because an event must never park the flush task (it re-arms its dirty
     * bits instead).
     *
     * It started as "for a response whose partial loss the *client* cannot
     * detect", i.e. the PARAM_INFO id list alone. That was too narrow. The
     * unpaced responses did not merely lose their own frames: a connect burst
     * is several multi-frame listings back to back — GET_PARAM alone answers
     * 120 ids in ~10 full-MTU frames, and the app asks that four times — and
     * emitting them as fast as the loop runs drained the host's msys pool that
     * the *receive* path also allocates from. NimBLE then could not build the
     * response to an incoming ATT request, nor even the error response
     * ("ble_att_svr_pkt rc=6", BLE_HS_ENOMEM), so the request went unanswered.
     * ATT permits one outstanding request per bearer, so the central sent
     * nothing further and closed the link on the 30 s transaction timeout —
     * measured at 30078 and 30079 ms from the rc=6 to the disconnect, which is
     * what identified this. Waiting for a buffer costs a few ms per frame and
     * leaves the pool something to answer with. */
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
    s_chunker.begin(OP_GET_PARAM | 0x80, seq, nullptr, 0, false,
                    /*paced=*/true);
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
                    false, /*paced=*/true);
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
    /* And the working state (S40), for exactly the same reason one line up:
     * it is written when the synth has been left alone and has gone quiet, so
     * a restart requested a second after an edit would otherwise come back
     * missing it — and this restart is the one case where the instrument is
     * definitely about to stop being able to save. */
    const esp_err_t serr = presets_state_save_now();
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "state flush before reboot failed: %s",
                 esp_err_to_name(serr));
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
        s_chunker.begin(OP_SEQ_STEPS | 0x80, seq, prefix, sizeof(prefix), false,
                        /*paced=*/true);
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

/* The user chord set (S41): twelve eight-byte slots, addressed by
 * (played note - chord.root) mod 12.
 *
 *   get  [0]                       -> [u8 slots][slots x chord_user_slot_t]
 *   set  [1][u8 slot][8 B slot]    -> the same full listing
 *
 * A set answers with the whole set rather than an ack because the app's
 * editor draws all twelve at once, and 96 bytes fits one frame with room to
 * spare — cheaper than a re-read and impossible to get out of step with.
 * `chord.rev` moves on every write, so a second app on the link learns of the
 * change through the ordinary parameter batch. */
void handle_chord_set(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_CHORD_SET, seq, ST_MALFORMED);
        return;
    }
    if (p[0] != 0) {
        if (plen != 2 + sizeof(chord_user_slot_t)) {
            send_status(OP_CHORD_SET, seq, ST_MALFORMED);
            return;
        }
        if (p[1] >= CHORD_USER_SLOTS) {
            send_status(OP_CHORD_SET, seq, ST_BAD_ARG);
            return;
        }
        chord_user_slot_t u;
        memcpy(&u, p + 2, sizeof(u));
        chord_user_set(p[1], &u); /* clamps count and transpose itself */
    }
    size_t n = 5;
    s_tx[n++] = (uint8_t)CHORD_USER_SLOTS;
    for (int i = 0; i < CHORD_USER_SLOTS; ++i) {
        chord_user_slot_t u;
        chord_user_get(i, &u);
        memcpy(s_tx + n, &u, sizeof(u));
        n += sizeof(u);
    }
    s_tx[0] = OP_CHORD_SET | 0x80;
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
                            false, /*paced=*/true);
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
                            false, /*paced=*/true);
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
    s_chunker.begin(OP_SEQ_SONG | 0x80, seq, prefix, sizeof(prefix), false,
                    /*paced=*/true);
    for (int i = 0; i < seq_song_length(); ++i) {
        seq_song_entry_t e;
        seq_song_get(i, &e);
        const uint8_t rec[2] = {e.pattern, e.repeats};
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

/* Wire width of one `what = 1` slot record. Sent in the prefix rather than
 * assumed by the app, which is the lesson S44 taught this opcode: the record
 * grew from 14 bytes to 22, and an app with the old width hard-coded would
 * have read the whole listing shifted rather than noticing. A reader that
 * takes the width from the prefix survives the next addition too. */
constexpr uint8_t kKitSlotRecBytes = 6 + 4 + DRUM_SLOT_NAME_MAX;

void handle_kit_info(uint8_t seq, const uint8_t* p, uint16_t plen) {
    const uint8_t what = plen >= 1 ? p[0] : 1;
    if (what == 0) { /* the selectable kits */
        /* The fourth byte is where user kits persist: 0 nowhere, 1 SD card,
         * 2 the LittleFS partition. The app needs it to decide whether to
         * offer a Save control at all, and "nowhere" is a real answer on a
         * board with no card - one worth showing rather than discovering when
         * a kit does not come back after a power cycle. */
        const char* store = drums_storage_name();
        const uint8_t backend = (store[0] == 's') ? 1u
                                                  : (store[0] == 'l' ? 2u : 0u);
        const uint8_t prefix[4] = {0, (uint8_t)ParamStore::instance().get(
                                          DRUM_PID_KIT),
                                   (uint8_t)drums_kit_count(), backend};
        s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                        /*paced=*/true);
        for (int i = 0; i < drums_kit_count(); ++i) {
            uint8_t rec[2 + DRUM_KIT_NAME_MAX] = {};
            rec[0] = (uint8_t)i;
            /* Bit 0 says the kit can be recorded into and saved; the app draws
             * a very different page for one that cannot. */
            rec[1] = (uint8_t)(drums_kit_is_user(i) ? 0x01u : 0x00u);
            /* rec is zero-initialised, so the name stays NUL-padded to its
             * fixed wire width; strlcpy always terminates. */
            strlcpy((char*)rec + 2, drums_kit_name_at(i), DRUM_KIT_NAME_MAX);
            s_chunker.append(rec, sizeof(rec));
        }
        s_chunker.finish();
        return;
    }
    /* The current kit's slots: what the app labels the drum lanes and the
     * mixer strips with, since the parameters themselves are named
     * generically (drum1.level and friends) and outlive any one kit.
     *
     * Since S44 it also carries what a *pad editor* needs - whether the pad
     * has anything in it, how long that is, and the four performance settings
     * that live in the kit rather than in parameter space. All of it in the
     * listing the app already fetches on every kit change, rather than a
     * per-pad round trip sixteen times over. */
    const uint8_t prefix[3] = {1, (uint8_t)drums_slot_count(),
                               kKitSlotRecBytes};
    s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                    /*paced=*/true);
    for (int i = 0; i < drums_slot_count() && i < DRUM_SLOTS; ++i) {
        uint8_t rec[kKitSlotRecBytes] = {};
        drums_pad_t pad;
        const bool filled = drums_pad_get(i, &pad);
        rec[0] = (uint8_t)i;
        const int note = drums_slot_note(i);
        rec[1] = (uint8_t)(note >= 0 ? note : (36 + i));
        rec[2] = (uint8_t)((filled ? 0x01u : 0x00u) |
                           (filled && pad.reverse ? 0x02u : 0x00u));
        rec[3] = filled ? pad.play_mode : (uint8_t)DRUM_PLAY_ONESHOT;
        rec[4] = filled ? pad.choke_group : 0u;
        /* start_ofs as a byte: the app draws it on a slider a couple of
         * hundred pixels long, so 1/255 is already finer than anyone can aim
         * at, and it keeps the record byte-aligned. */
        rec[5] = filled ? (uint8_t)(pad.start_ofs * 255.0f + 0.5f) : 0u;
        const uint32_t frames = filled ? pad.frames : 0u;
        memcpy(rec + 6, &frames, 4);
        strlcpy((char*)rec + 10, drums_slot_name(i), DRUM_SLOT_NAME_MAX);
        s_chunker.append(rec, sizeof(rec));
    }
    s_chunker.finish();
}

/* ---- sample-kit editing (S44) ------------------------------------------
 *
 * One opcode, a sub-op byte, and a status reply. Deliberately not chunked:
 * each of these is a single small write, and the app re-reads KIT_INFO
 * afterwards to see the result rather than being told it twice.
 *
 *   0 PAD FIELD  [u8 kit][u8 slot][u8 field][f32 value]
 *       `kit` 0xFF means the bound one; `field` is drums_pad_field_t.
 *   1 RENAME KIT [u8 kit][char name[DRUM_KIT_NAME_MAX]]
 *   2 RENAME PAD [u8 kit][u8 slot][char name[DRUM_SLOT_NAME_MAX]]
 */
void handle_kit_edit(uint8_t seq, const uint8_t* p, uint16_t plen) {
    if (plen < 1) {
        send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
        return;
    }
    const uint8_t sub = p[0];
    if (sub == 0) {
        if (plen < 8) {
            send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
            return;
        }
        const int kit = (p[1] == 0xFF) ? -1 : (int)p[1];
        float value;
        memcpy(&value, p + 4, 4);
        const esp_err_t err = drums_pad_set_field(
            kit, (int)p[2], (drums_pad_field_t)p[3], value);
        send_status(OP_KIT_EDIT, seq,
                    err == ESP_OK
                        ? ST_OK
                        : (err == ESP_ERR_NOT_SUPPORTED ? ST_UNSUPPORTED
                                                        : ST_BAD_ARG));
        return;
    }
    if (sub == 1) {
        if (plen < 3) {
            send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
            return;
        }
        char name[DRUM_KIT_NAME_MAX];
        const uint16_t avail = (uint16_t)(plen - 2);
        const uint16_t n =
            avail < DRUM_KIT_NAME_MAX - 1 ? avail : DRUM_KIT_NAME_MAX - 1;
        memcpy(name, p + 2, n);
        name[n] = '\0';
        const esp_err_t err = drums_kit_rename((int)p[1], name);
        send_status(OP_KIT_EDIT, seq, err == ESP_OK ? ST_OK : ST_UNSUPPORTED);
        return;
    }
    if (sub == 2) {
        if (plen < 4) {
            send_status(OP_KIT_EDIT, seq, ST_MALFORMED);
            return;
        }
        char name[DRUM_SLOT_NAME_MAX];
        const uint16_t avail = (uint16_t)(plen - 3);
        const uint16_t n =
            avail < DRUM_SLOT_NAME_MAX - 1 ? avail : DRUM_SLOT_NAME_MAX - 1;
        memcpy(name, p + 3, n);
        name[n] = '\0';
        const esp_err_t err =
            drums_pad_rename((p[1] == 0xFF) ? -1 : (int)p[1], (int)p[2], name);
        send_status(OP_KIT_EDIT, seq, err == ESP_OK ? ST_OK : ST_BAD_ARG);
        return;
    }
    send_status(OP_KIT_EDIT, seq, ST_BAD_ARG);
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
        /* Also "not now", and the same retry: loop_ctl could not reach the
         * request inside its window — almost always a card that has stopped
         * answering mid-pass (looper.h). MALFORMED, which this used to fall
         * through to, would tell the app its own frame was wrong. */
        case ESP_ERR_TIMEOUT: return ST_BUSY;
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
        case 3: {
            /* Whole model in one edit: the v1 'OGR1' blob, byte for byte what
             * a version-4 preset stores and what GRAPH_NODES hands out — so
             * the app pushes back exactly the shape it read, with no third
             * encoding to keep in step.
             *
             * It exists because replaying a patch node by node does not
             * work in practice: every set_kind is its own recompile and its
             * own audio duck, a twelve-node patch is a dozen of them, and the
             * intermediate graphs are real patches the synth renders on the
             * way past. This is compiled and cost-checked as a unit, so a
             * patch that does not fit is refused whole rather than half
             * applied. Older firmware answers sub-op 3 with ST_BAD_ARG, which
             * is the app's cue to fall back to the per-node path. */
            gr::Model m;
            rc = gr::deserialize(p + 1, (size_t)plen - 1, m)
                     ? gr::load_model(m)
                     : ESP_ERR_INVALID_ARG;
            break;
        }
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
        case OP_KIT_EDIT:
            handle_kit_edit(seq, p, plen);
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
        case OP_CHORD_SET:
            handle_chord_set(seq, p, plen);
            break;
        /* pads: {slot, velocity} or, since S44, {slot, velocity, release}.
         *
         * The third byte rather than an overloaded velocity of 0: that value
         * is already the capability probe the app fires at discovery, and
         * making it mean "let go of this pad" would have had the probe
         * silencing a held gate pad on slot 0. A longer payload is free here
         * and an old app keeps working unchanged. */
        case OP_DRUM_TRIG:
            if (plen != 2 && plen != 3) {
                send_status(op, seq, ST_MALFORMED);
            } else if (plen == 3 && p[2] != 0) {
                /* Release: only gate and loop pads hold anything, so this is a
                 * no-op on every kit that predates them and the app can send
                 * it on every touch-up without asking what the pad is. */
                drums_release(p[0]);
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
    const bool live = link_up();

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

}  // namespace

void ctrl_proto_set_transport(const ctrl_transport_t* transport) {
    s_tp = transport;
}

esp_err_t ctrl_proto_init(void) {
    static bool done = false;
    if (done) return ESP_OK;
    if (ParamStore::instance().addListener(param_listener, nullptr) < 0) {
        ESP_LOGW(TAG, "no listener slot: the app will not see external changes");
    }
    done = true;
    return ESP_OK;
}

void ctrl_proto_handle_frame(const uint8_t* frame, size_t len) {
    handle_frame(frame, len);
}

void ctrl_proto_flush_events(void) { flush_events(); }

void ctrl_proto_link_down(void) {
    /* Release anything the app was holding. OP_NOTE_ON goes into the MIDI
     * router like a played key, so its note-off has to come back over the same
     * link -- and a link that just dropped will never deliver it. Without
     * this, walking out of range mid-chord leaves the synth droning until a
     * CC 123 arrives from somewhere else. */
    voice_manager_all_notes_off();
    /* And the chord table with it (S41). The voices are already gone, but the
     * table would still be holding those keys -- and their per-tone reference
     * counts -- so the next press of a key it still lists would find its tones
     * "already sounding" and play nothing. The note-offs this sends also clear
     * the arpeggiator's held list, which the call above does not reach. */
    chord_all_off();
}

void ctrl_proto_reject_busy(const uint8_t* frame, size_t len) {
    /* Needs the opcode and the sequence number to address the reply; a frame
     * shorter than a header has neither. */
    if (frame == nullptr || len < 4) return;
    send_status(frame[0], frame[1], ST_BUSY);
}

void ctrl_proto_link_reset(void) {
    /* -1 forces the next flush to announce the engine. A reconnecting app is
     * waiting for that EVT_ENGINE to know which 0x02xx set to discover, and
     * without this reset the latch still holds the value from the previous
     * connection and the announcement never comes. */
    s_announced_engine.store(-1, std::memory_order_relaxed);
}

size_t ctrl_proto_info(uint8_t* out, size_t max) {
    const size_t need = 4 + sizeof(CONFIG_IDF_TARGET);
    if (out == nullptr || max < need) return 0;
    out[0] = kProtoVersion;
    out[1] = kFwVersion[0];
    out[2] = kFwVersion[1];
    out[3] = kFwVersion[2];
    memcpy(out + 4, CONFIG_IDF_TARGET, sizeof(CONFIG_IDF_TARGET));
    return need;
}
