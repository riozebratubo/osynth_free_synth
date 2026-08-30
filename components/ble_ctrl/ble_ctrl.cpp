/*
 * osynth — BLE transport for SynthCtl v1 (Session 14; split from the protocol
 * in the host-port work).
 *
 * NimBLE peripheral, one connection, service 395f2e00-7c4f-4a48-94e7-
 * dafcf25ef34a with four characteristics (docs/BLE_PROTOCOL.md):
 *   CTRL  write / write-no-response  — command frames app -> synth
 *   EVT   notify                     — responses + unsolicited events
 *   BULK  write / notify             — reserved in v1 (writes ignored)
 *   INFO  read                       — protocol + firmware version, target
 *
 * ---------------------------------------------------------------------------
 * What this file is, and what it is not
 *
 * It is a *transport*. The protocol -- the frame parser, the opcode handlers,
 * the response chunker and the event flush -- lives in components/ctrl_proto,
 * and this file registers itself there as one way of moving its bytes. That
 * component's header explains why the two were separated; the short version is
 * that the standalone app embeds the engine and reaches it in process, and the
 * handlers never referenced a BLE type anyway.
 *
 * So everything below is radio: the GATT tables, advertising, connection
 * parameters, and the four functions in ctrl_transport_t --
 *
 *   tp_send            a notification on EVT, if a client has subscribed
 *   tp_send_paced      the same, waiting for an mbuf instead of failing
 *   tp_avail_payload   what fits at the live ATT MTU
 *   tp_link_up         connected and subscribed
 *
 * -- which is the whole of what the protocol asks of a link.
 *
 * ---------------------------------------------------------------------------
 * Threading
 *
 * The NimBLE host task only flattens incoming CTRL frames into a queue. The
 * `ble_cmd` task (core 0, prio 3 — below the preset task) drains it and is the
 * single task every ctrl_proto call is made from, which is that component's
 * stated requirement: its handlers share one TX buffer and one chunker. The
 * same task drives the ~20 Hz event flush.
 *
 * Frame payloads are sized to the live ATT MTU through tp_avail_payload(), so
 * small-MTU clients stay protocol-correct (just slower); the full metadata
 * responses assume the documented MTU >= 247.
 */
#include "ble_ctrl.h"

#include "esp_log.h"
#include "synth_config.h"

static const char* TAG = "ble_ctrl";

#if SYNTH_ENABLE_BLE && CONFIG_BT_NIMBLE_ENABLED

#include <atomic>
#include <cstring>

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

/* The protocol this component carries. Everything the old handlers reached --
 * the sequencer, the kit, the looper, the graph, the parameter store -- moved
 * with them, which is why this file's include list is now the radio and
 * nothing else. */
#include "ctrl_proto.h"

namespace {

constexpr char kDeviceName[] = "osynth";

/* Frame ceiling, from the protocol -- CmdBuf and the MTU clamp are sized by
 * it. Named locally so the moved code below reads unchanged. */
constexpr size_t kMaxFrame = CTRL_PROTO_MAX_FRAME;
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

/* ---- outgoing frames: the ctrl_transport_t implementation --------------- */

/* All synth -> app traffic is EVT notifications; the app must subscribe
 * before sending commands (documented flow), so unsubscribed = drop. */
bool tp_send(const uint8_t* frame, size_t len) {
    const uint16_t conn = s_conn.load(std::memory_order_relaxed);
    if (conn == BLE_HS_CONN_HANDLE_NONE ||
        !s_evt_sub.load(std::memory_order_relaxed)) {
        return false;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(frame, len);
    if (om == nullptr) return false;
    return ble_gatts_notify_custom(conn, s_evt_handle, om) == 0;
}

/* tp_send() with back-pressure -- waits for an mbuf instead of failing. See
 * the note on ctrl_transport_t::send_paced for which response needs it and
 * why. Bails at once (rather than after the full wait) if the link is gone,
 * since nothing is coming back then. */
bool tp_send_paced(const uint8_t* frame, size_t len) {
    for (int i = 0; i < 100; ++i) { /* ~500 ms */
        if (s_conn.load(std::memory_order_relaxed) == BLE_HS_CONN_HANDLE_NONE ||
            !s_evt_sub.load(std::memory_order_relaxed)) {
            return false;
        }
        if (tp_send(frame, len)) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* Payload bytes (after the status byte) that fit one frame at the live
 * ATT MTU (a notification carries at most MTU-3 attribute bytes). */
size_t tp_avail_payload() {
    const uint16_t conn = s_conn.load(std::memory_order_relaxed);
    uint16_t mtu = 23;
    if (conn != BLE_HS_CONN_HANDLE_NONE) mtu = ble_att_mtu(conn);
    if (mtu < 23) mtu = 23; /* 0 for a just-dropped handle */
    size_t att = (size_t)mtu - 3; /* notification value size limit */
    if (att > kMaxFrame) att = kMaxFrame;
    return att - 4 /* frame header */ - 1 /* status */;
}

bool tp_link_up() {
    return s_conn.load(std::memory_order_relaxed) != BLE_HS_CONN_HANDLE_NONE &&
           s_evt_sub.load(std::memory_order_relaxed);
}

const ctrl_transport_t kBleTransport = {
    tp_send,
    tp_send_paced,
    tp_avail_payload,
    tp_link_up,
};


/* The protocol task. Every ctrl_proto call below happens here and nowhere
 * else, which is the single-task rule ctrl_proto.h states: the handlers share
 * one TX buffer and one chunker, so two callers would interleave two responses
 * into one buffer. Incoming writes are queued by ctrl_access() on the NimBLE
 * host task rather than executed there, both to keep that task free and to
 * funnel everything through this one. */
void cmd_task(void*) {
    CmdBuf cmd;
    TickType_t last_flush = xTaskGetTickCount();
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, kFlushPeriod) == pdTRUE) {
            ctrl_proto_handle_frame(cmd.data, cmd.len);
        }
        const TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_flush) >= kFlushPeriod) {
            ctrl_proto_flush_events();
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
    /* Zeroed, not merely filled to `len`. xQueueSend copies the whole struct
     * whatever the frame's length, so without this every short frame carries
     * ~250 bytes of this task's stack into the queue. handle_frame() never
     * reads past cmd.len, so nothing acts on them — but they are the NimBLE
     * host task's stack, they sit in a queue any command can be dumped from
     * while debugging, and the memset is cheaper than reasoning about that
     * every time this function is read. */
    CmdBuf cmd = {};
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, cmd.data, sizeof(cmd.data), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cmd.len = len;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, frame dropped");
        ctrl_proto_reject_busy(cmd.data, len);
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
    /* Built by the protocol: the same bytes whether they are read from this
     * characteristic or handed over in process. */
    uint8_t info[4 + sizeof(CONFIG_IDF_TARGET)];
    const size_t n = ctrl_proto_info(info, sizeof(info));
    if (n == 0) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, info, n) == 0
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
 * >= 15 ms), so a refusal retries once at 15-30 ms — but only for as much of
 * that range as is actually an improvement on what the link already has; see
 * request_fast_conn, which will send nothing rather than offer a central
 * permission to slow down. A second refusal is left alone: the link works, it
 * is just lazier than we would like, and nagging a central that has said no
 * costs airtime on the very path we are trying to keep clear.
 *
 * Sent when the app subscribes to EVT rather than on connect: discovery is
 * over by then, so the request is not competing with the burst it would
 * accelerate, and centrals are far more likely to accept an update on an idle
 * link. */
constexpr uint16_t kFastItvlMin = 6;   /* x1.25 ms = 7.5 ms */
constexpr uint16_t kFastItvlMax = 12;  /* 15 ms                */
constexpr uint16_t kSafeItvlMin = 12;  /* 15 ms — Apple's floor */
constexpr uint16_t kSafeItvlMax = 24;  /* 30 ms                 */
/* Floor, not target, for the supervision timeout — see request_fast_conn. */
constexpr uint16_t kConnTimeout = 400; /* x10 ms = 4 s */

/* Host task only (every touch is inside a GAP callback). */
uint8_t s_upd_tries = 0;

/* A parameter update is a request for the *whole* set, and the central then
 * picks anywhere inside what it is offered — so a careless request can make
 * the link worse than leaving it alone, and did. Windows opens at 15 ms with a
 * 9.6 s supervision timeout, refused the 7.5-15 ms ask outright (HCI 0x3b,
 * unacceptable connection parameters), and then took the 15-30 ms retry at its
 * *slow* end while adopting our 4 s timeout. Net effect of asking: the interval
 * halved in speed and the link lost 5.6 s of tolerance, on the one path meant
 * to make it quicker.
 *
 * Both halves of that are fixed by reading what the link already has and never
 * offering worse:
 *
 *   itvl_max is capped to the current interval, so the answer can only be the
 *   same or faster; if that leaves nothing to gain, no request goes out at all.
 *
 *   supervision_timeout keeps whatever the central chose unless it is below
 *   kConnTimeout, which is therefore a floor rather than a target. The trade is
 *   real and deliberate: a longer timeout means a genuine walk-out-of-range
 *   takes longer to reach BLE_GAP_EVENT_DISCONNECT and its
 *   voice_manager_all_notes_off(). Tolerance is worth more — the central picked
 *   that number knowing its own scheduling, and cutting it is how a link that
 *   merely stalled became a link that dropped.
 *
 * latency stays 0 either way: a skipped connection event is a delayed note, and
 * forcing it down is an improvement whatever the central had. */
/* Returns whether a request actually went out. The caller arms s_upd_tries off
 * that: a CONN_UPDATE is only "our result" if we asked, and marking a request
 * outstanding when none is would hand the next central-driven update to the
 * refusal path, spending the one retry on an answer to nobody's question. */
bool request_fast_conn(uint16_t conn_handle, bool apple_safe) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "conn param request not sent (no such connection)");
        return false;
    }

    struct ble_gap_upd_params p = {};
    p.itvl_min = apple_safe ? kSafeItvlMin : kFastItvlMin;
    p.itvl_max = apple_safe ? kSafeItvlMax : kFastItvlMax;
    if (p.itvl_max > desc.conn_itvl) p.itvl_max = desc.conn_itvl;
    if (p.itvl_min > p.itvl_max) p.itvl_min = p.itvl_max;
    if (p.itvl_min >= desc.conn_itvl) {
        /* Nothing below the current interval left to ask for. This is the
         * Apple-safe retry against a central already sitting at 15 ms: its
         * floor and ours are the same number, so the old code's only possible
         * outcome was to be allowed to slow down. */
        ESP_LOGI(TAG, "connection interval already %u.%02u ms; not asking",
                 (unsigned)(desc.conn_itvl * 125u) / 100u,
                 (unsigned)(desc.conn_itvl * 125u) % 100u);
        return false;
    }
    p.latency = 0;
    p.supervision_timeout = desc.supervision_timeout > kConnTimeout
                                ? desc.supervision_timeout
                                : kConnTimeout;

    const int rc = ble_gap_update_params(conn_handle, &p);
    if (rc != 0) {
        /* Local refusal (no such connection, one already in flight): nothing
         * to retry against — the result path below only sees requests that
         * actually went out. */
        ESP_LOGW(TAG, "conn param request not sent (rc %d)", rc);
        return false;
    }
    ESP_LOGI(TAG,
             "asking for a %u.%02u-%u.%02u ms connection interval "
             "(timeout %u ms)",
             (unsigned)(p.itvl_min * 125u) / 100u,
             (unsigned)(p.itvl_min * 125u) % 100u,
             (unsigned)(p.itvl_max * 125u) / 100u,
             (unsigned)(p.itvl_max * 125u) % 100u,
             (unsigned)p.supervision_timeout * 10u);
    return true;
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
                    ESP_LOGI(TAG, "conn params refused (status 0x%02x)",
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
            /* Release anything the app was holding -- see the reasoning at
             * ctrl_proto_link_down(). Safe from the host task. */
            ctrl_proto_link_down();
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
                ctrl_proto_link_reset();
                /* The app is up: ask for an interval a keyboard can play on.
                 * Once per subscription — a re-subscribe on the same link is
                 * the app re-reading, not a new central. */
                if (ev->subscribe.cur_notify && s_upd_tries == 0) {
                    if (request_fast_conn(ev->subscribe.conn_handle, false)) {
                        s_upd_tries = 1;
                    }
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
    /* The protocol, and this component as its transport. Installed before
     * the host starts advertising, so the first frame of the first connection
     * already has somewhere to go. */
    ctrl_proto_set_transport(&kBleTransport);
    if (ctrl_proto_init() != ESP_OK) return ESP_FAIL;

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
