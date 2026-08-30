/*
 * osynth host port — a SynthCtl v1 round trip against the embedded engine.
 *
 * Drives ctrl_proto through a transport that collects frames into a vector
 * instead of sending them anywhere, and checks the answers. It exists because
 * every other test so far proves the engine *runs*; this is the one that
 * proves it can be *talked to* -- which is what the standalone app does and
 * nothing else here exercises.
 *
 * The opcodes and frame layout are hardcoded rather than shared with
 * components/ctrl_proto, on purpose: this stands in for the app, and the app
 * has its own copy of them (app_osyntho/src/ble/synthprotocol.h). A test that
 * imported the implementation's own constants would agree with it by
 * construction and could not catch the two drifting apart -- which is exactly
 * the failure worth catching, since those two files are maintained separately.
 *
 * Frames, little-endian:
 *   request   [u8 op][u8 seq][u16 len][payload]
 *   response  [u8 op|0x80][u8 seq][u16 len][u8 status][payload]
 * Status bit 7 is the continuation flag.
 */
#include "host_prototest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include "ctrl_proto.h"
#include "engines.h"
#include "synth_config.h"
#include "synth_params.h"

using osynth::ParamOrigin;

namespace {

/* The app's view of the protocol. See the header comment for why these are
 * spelled out here rather than included. */
constexpr uint8_t OP_SET_PARAM = 0x01;
constexpr uint8_t OP_GET_PARAM = 0x02;
constexpr uint8_t OP_PARAM_INFO = 0x03;
constexpr uint8_t OP_LIST_PRESETS = 0x07;
constexpr uint8_t OP_PING = 0x7F;
constexpr uint8_t ST_OK = 0;
constexpr uint8_t ST_MORE = 0x80;

std::vector<std::vector<uint8_t>> g_rx;

bool tp_send(const uint8_t* frame, size_t len) {
    g_rx.emplace_back(frame, frame + len);
    return true;
}
size_t tp_avail() { return CTRL_PROTO_MAX_FRAME - 4 - 1; }
bool tp_up() { return true; }

const ctrl_transport_t kTestTransport = {tp_send, nullptr, tp_avail, tp_up};

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
}

/* Sends one request and returns the frames it produced. */
const std::vector<std::vector<uint8_t>>& request(uint8_t op, uint8_t seq,
                                                 const uint8_t* payload,
                                                 uint16_t plen) {
    std::vector<uint8_t> f;
    f.push_back(op);
    f.push_back(seq);
    f.push_back((uint8_t)(plen & 0xFF));
    f.push_back((uint8_t)(plen >> 8));
    if (plen > 0) f.insert(f.end(), payload, payload + plen);

    g_rx.clear();
    ctrl_proto_handle_frame(f.data(), f.size());
    return g_rx;
}

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
float rdf32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

int osynth_host_prototest(void) {
    std::printf("\n-- SynthCtl v1 round trip --\n");

    /* Replaces whatever transport is installed. Restored at the end, since a
     * caller may have had one. */
    ctrl_proto_set_transport(&kTestTransport);

    /* --- PING: the simplest possible exchange -------------------------- */
    {
        const auto& r = request(OP_PING, 0x11, nullptr, 0);
        const bool shape = r.size() == 1 && r[0].size() >= 5;
        check(shape, "PING answered with one frame");
        if (shape) {
            check(r[0][0] == (uint8_t)(OP_PING | 0x80), "PING response opcode is 0xFF");
            check(r[0][1] == 0x11, "PING echoed the sequence number");
            check((r[0][4] & ~ST_MORE) == ST_OK, "PING status OK");
        }
    }

    /* --- GET_PARAM: read master.volume --------------------------------- */
    {
        uint8_t p[2] = {0x00, 0x00}; /* id 0x0000 */
        const auto& r = request(OP_GET_PARAM, 0x22, p, sizeof(p));
        const bool shape = !r.empty() && r[0].size() >= 5 + 6;
        check(shape, "GET_PARAM answered");
        if (shape) {
            const uint16_t id = rd16(&r[0][5]);
            const float v = rdf32(&r[0][7]);
            check(id == 0x0000, "GET_PARAM returned the id asked for");
            check(v >= 0.0f && v <= 1.0f,
                  "master.volume within its declared range");
        }
    }

    /* --- SET_PARAM, then read it back ---------------------------------- */
    {
        const float want = 0.33f;
        uint8_t p[6] = {0x00, 0x00};
        std::memcpy(p + 2, &want, 4);
        (void)request(OP_SET_PARAM, 0x33, p, sizeof(p));

        const float got = osynth::ParamStore::instance().get(0x0000);
        check(got > want - 0.001f && got < want + 0.001f,
              "SET_PARAM reached the ParamStore");

        uint8_t q[2] = {0x00, 0x00};
        const auto& r = request(OP_GET_PARAM, 0x34, q, sizeof(q));
        const bool shape = !r.empty() && r[0].size() >= 5 + 6;
        check(shape && rdf32(&r[0][7]) > want - 0.001f,
              "GET_PARAM reads back what SET_PARAM wrote");
    }

    /* --- a malformed request is refused ---------------------------------
     *
     * PARAM_INFO wants a 2-byte id. An empty payload is not "ask me
     * everything", it is a short frame, and the handler must say so rather
     * than guess -- which is what it did when this test was first written
     * against the wrong assumption. */
    {
        const auto& r = request(OP_PARAM_INFO, 0x43, nullptr, 0);
        const bool shape = r.size() == 1 && r[0].size() >= 5;
        check(shape && (r[0][4] & ~ST_MORE) != ST_OK,
              "PARAM_INFO with no id refused as malformed");
    }

    /* --- PARAM_INFO: the discovery the app does on every connect -------
     *
     * id 0xFFFF asks for the id list -- the response that spans several frames
     * and exercises the chunker's continuation bit. Every frame carries a
     * 2-byte prefix (the active engine and its caps mask) so that each one
     * parses standalone, which the id counting below has to skip.
     *
     * The count is compared against the ParamStore itself: the protocol and
     * the registry must agree on how many parameters exist, and a chunker that
     * dropped a frame would show up here as a short list. That is not a
     * hypothetical -- the handler's own comment records it happening, and
     * notes the app cannot detect it, which is what makes checking it here
     * worth the lines. */
    {
        constexpr size_t kPrefix = 2; /* engine + caps */
        uint8_t p[2] = {0xFF, 0xFF};
        const auto& r = request(OP_PARAM_INFO, 0x44, p, sizeof(p));
        check(!r.empty(), "PARAM_INFO answered");

        size_t ids = 0;
        size_t continuation = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            const auto& f = r[i];
            if (f.size() < 5 + kPrefix) continue;
            if (f[4] & ST_MORE) ++continuation;
            ids += (f.size() - 5 - kPrefix) / 2; /* u16 per id */
        }
        const size_t registered = osynth::ParamStore::instance().count();
        std::printf("     %zu frame(s), %zu continuation, %zu ids vs %zu registered\n",
                    r.size(), continuation, ids, registered);
        check(r.size() > 1, "PARAM_INFO spanned several frames");
        check(continuation == r.size() - 1,
              "every frame but the last set the continuation bit");
        check(ids == registered,
              "PARAM_INFO listed exactly the registered parameters");
    }

    /* --- the id list after an engine switch -----------------------------
     *
     * The app's flow on every engine change: SELECT_ENGINE, wait for the
     * EVT_ENGINE greeting, then re-request the whole id list because the
     * 0x02xx range has been unregistered and repopulated. If that second list
     * never arrives the app retries and then gives up, leaving the picker on
     * an engine whose parameters it does not have.
     *
     * Worth testing per engine rather than once, because the switch is what
     * moves the id count -- and the sampler is the engine that moves it most.
     */
    for (int eng = 0; eng < SYNTH_ENGINE_COUNT; ++eng) {
        osynth::ParamStore& ps = osynth::ParamStore::instance();
        ps.set(0x0001 /* engine.type */, (float)eng, ParamOrigin::Ble);
        /* The switch runs on its own task and fades the voice bus first. */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const int active = (int)engines_active_type();
        uint8_t p[2] = {0xFF, 0xFF};
        const auto& r = request(OP_PARAM_INFO, (uint8_t)(0x60 + eng), p,
                                sizeof(p));
        size_t ids = 0;
        for (const auto& f : r) {
            if (f.size() >= 5 + 2) ids += (f.size() - 5 - 2) / 2;
        }
        const size_t registered = ps.count();
        char label[96];
        std::snprintf(label, sizeof(label),
                      "engine %d (bound %d): id list %zu vs %zu", eng, active,
                      ids, registered);
        check(!r.empty() && ids == registered && ids > 0, label);
    }

    /* --- every parameter's metadata, for every engine --------------------
     *
     * This is the discovery walk itself: after the id list the app asks for
     * PARAM_INFO on each id in turn. One id that answers with nothing, or with
     * a frame the app cannot parse, stalls that walk -- and the app's only
     * recovery is a watchdog that resends and eventually gives up.
     *
     * Checked per engine because the 0x02xx range is unregistered and
     * repopulated on every switch, so each engine presents a different set. */
    {
        osynth::ParamStore& ps = osynth::ParamStore::instance();
        for (int eng = 0; eng < SYNTH_ENGINE_COUNT; ++eng) {
            ps.set(0x0001, (float)eng, ParamOrigin::Ble);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            uint16_t ids[512];
            const size_t n = ps.listIds(ids, 512);
            size_t answered = 0, unnamed = 0, short_enum = 0;
            uint16_t first_bad = 0;
            for (size_t i = 0; i < n; ++i) {
                uint8_t q[2];
                q[0] = (uint8_t)(ids[i] & 0xFF);
                q[1] = (uint8_t)(ids[i] >> 8);
                const auto& r = request(OP_PARAM_INFO, 0x70, q, sizeof(q));
                /* 22 fixed bytes, then a NUL-terminated name, then the enum
                 * labels. Anything shorter is a status, i.e. a refusal. */
                if (r.size() != 1 || r[0].size() < 22 ||
                    (r[0][4] & ~ST_MORE) != ST_OK) {
                    if (first_bad == 0) first_bad = ids[i];
                    continue;
                }
                ++answered;
                const uint8_t enum_count = r[0][9];
                /* name starts at 22 */
                const char* nm = (const char*)&r[0][22];
                const size_t nlen = strnlen(nm, r[0].size() - 22);
                if (nlen == 0 || 22 + nlen >= r[0].size()) ++unnamed;
                /* Count the labels that actually arrived after the name. */
                size_t at = 22 + nlen + 1, got = 0;
                while (at < r[0].size() && got < enum_count) {
                    const size_t l = strnlen((const char*)&r[0][at],
                                             r[0].size() - at);
                    if (l == 0 && got == 0) break;
                    at += l + 1;
                    ++got;
                }
                if (got < enum_count) {
                    ++short_enum;
                    if (first_bad == 0) first_bad = ids[i];
                }
            }
            char label[110];
            std::snprintf(label, sizeof(label),
                          "engine %d: %zu/%zu answered, %zu unnamed, %zu short "
                          "enum (first 0x%04x)",
                          eng, answered, n, unnamed, short_enum,
                          (unsigned)first_bad);
            check(answered == n && unnamed == 0 && short_enum == 0, label);
        }
    }

    /* --- LIST_PRESETS for every engine -----------------------------------
     *
     * The app sends this on every engine switch, and it is what crashed the
     * synth: a factory bank shorter than PRESETS_FACTORY_SLOTS leaves NULL
     * names in the tail, and the walk dereferenced one. The sampler's bank is
     * deliberately short (7 of 48), so selecting that engine was enough.
     *
     * Reaching the end of this loop at all is most of the check -- a null
     * dereference here takes the process with it. The counts are the rest:
     * a short bank must report its real rows, not 48 and not 0. */
    {
        for (int eng = 0; eng < SYNTH_ENGINE_COUNT; ++eng) {
            uint8_t p[1] = {(uint8_t)eng};
            const auto& r = request(OP_LIST_PRESETS, (uint8_t)(0x80 + eng), p,
                                    sizeof(p));
            /* records are [slot u8][factory u8][name PRESETS_NAME_MAX] after
             * the 5-byte header + 1-byte engine prefix. */
            size_t rows = 0;
            for (const auto& f : r) {
                if (f.size() > 5 + 1) rows += (f.size() - 5 - 1) / (2 + 24);
            }
            char label[80];
            std::snprintf(label, sizeof(label),
                          "engine %d: LIST_PRESETS answered, %zu slot(s)", eng,
                          rows);
            check(!r.empty() && rows > 0, label);
        }
    }

    /* --- an unknown opcode is refused, not ignored ---------------------- */
    {
        const auto& r = request(0x6E, 0x55, nullptr, 0);
        const bool shape = r.size() == 1 && r[0].size() >= 5;
        check(shape && r[0][4] != ST_OK, "unknown opcode answered with an error");
    }

    ctrl_proto_set_transport(nullptr);

    std::printf("\n  %d/%d checks passed%s\n", g_checks - g_failed, g_checks,
                g_failed == 0 ? "" : "  <-- FAULT");
    return g_failed == 0 ? 0 : 1;
}
