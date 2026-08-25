#!/usr/bin/env python3
"""Applied edits: connect-burst halving + connection-parameter policy.

Kept per the project's intermediary-artifacts policy. Third file of the
Windows disconnect-loop investigation; see att_timeout_disconnect_fix.py for
the root cause and win_ble_gui_stall_fix.py for the app-side UI freeze.
Not idempotent -- the asserts fail against an already-patched tree.

app1  Removes the second full value sweep from SynthController::finishDiscovery()
      and the now-dead engineParamIds() helper. onParamListComplete() already
      fetches every id when the list lands, and ble_ctrl's param_listener +
      flush_events() push any non-BLE-origin change as EVT_PARAMS to an app that
      subscribed before the connection was announced -- so the sweep re-read 365
      values to learn nothing, at ~40 full-MTU notification frames added to the
      burst that exhausted the host's msys pool.

fw4   request_fast_conn() reads the live ble_gap_conn_desc and never offers the
      central worse parameters than the link already has:
        * itvl_max capped to the current interval; no request at all if that
          leaves nothing to gain.
        * supervision_timeout keeps the central's choice unless it is below
          kConnTimeout, which becomes a floor rather than a target.
      Observed on Windows before this: 15 ms / 9600 ms became 30 ms / 4000 ms
      *because* we asked -- interval halved in speed, 5.6 s of tolerance lost.
      Also drops "retrying" from the refusal log, since the retry is now
      conditional and the callee says what it did.

fw5   request_fast_conn() returns whether a request went out, and SUBSCRIBE arms
      s_upd_tries only when one did -- otherwise the next central-driven update
      would be mistaken for our result and spend the one retry.

Verified with tools/c_brace_check.py, tools/check_comment_blocks.py,
tools/check_mojibake.py, and tools/syntax_check.sh ble_ctrl.cpp (-fsyntax-only
against the real include tree and -Wall -Werror set): ok.
"""

import io
p = r"D:\dev\osynth_free_synth\app_osyntho\src\synthcontroller.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(old, new, count=1):
    global s
    o, n = old.replace("\n", nl), new.replace("\n", nl)
    assert s.count(o) == count, (o[:70], s.count(o))
    s = s.replace(o, n)

rep("""  // A second value sweep. onParamListComplete() already fetched them so the UI
  // was never showing defaults; this catches anything the synth changed during
  // the metadata walk (a MIDI CC, a preset load, the sequencer's telemetry),
  // which at ~200 params is a few seconds of opportunity. An engine-switch pass
  // only walks the ~36 ids of the new engine, so its window is a fraction of
  // that and one frame covering that range is enough.
  if (m_discoveryScope == DiscoveryScope::EngineParams) {
    requestParamValues(engineParamIds());
  } else {
    requestAllParamValues();
  }
  // Presets are stored per engine, so this one *is* engine-scoped.""",
    """  // There is deliberately no second value sweep here. onParamListComplete()
  // already fetched every id the moment the list landed, and anything the synth
  // changes during the metadata walk \u2014 a MIDI CC, a preset load, the sequencer's
  // telemetry \u2014 arrives on its own: ble_ctrl's param_listener marks every
  // non-BLE-origin change dirty and flush_events() pushes it as EVT_PARAMS, and
  // the app is subscribed to EVT before the connection is even announced. So a
  // sweep here re-read 365 values to learn nothing.
  //
  // It was not free. GET_PARAM answers 120 ids in ~10 full-MTU frames, so this
  // was ~40 notification frames added to a connect burst that already carries
  // the preset list, the step walk, the p-lock list, the song, two kit reads,
  // the graph and the chord set. That burst is what exhausted the firmware's
  // msys pool, and an mbuf the receive path could not get is what left an ATT
  // request unanswered and had the central close the link on the 30 s
  // transaction timeout. Halving the burst is the point of removing this.
  //
  // Presets are stored per engine, so this one *is* engine-scoped.""")

rep("""
QList<quint16> SynthController::engineParamIds() const {
  QList<quint16> out;
  for (quint16 id : m_paramOrder) {
    if (id >= ID_ENGINE_FIRST && id <= ID_ENGINE_LAST) out.append(id);
  }
  return out;
}
""", "")

io.open(p, "w", encoding="utf-8", newline="").write(s)

h = r"D:\dev\osynth_free_synth\app_osyntho\src\synthcontroller.h"
t = io.open(h, encoding="utf-8", newline="").read()
nlh = "\r\n" if "\r\n" in t else "\n"
oldh = "  QList<quint16> engineParamIds() const;  // the registered 0x02xx ids\n".replace("\n", nlh)
assert t.count(oldh) == 1
t = t.replace(oldh, "")
io.open(h, "w", encoding="utf-8", newline="").write(t)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\components\ble_ctrl\ble_ctrl.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(old, new, count=1):
    global s
    o, n = old.replace("\n", nl), new.replace("\n", nl)
    assert s.count(o) == count, (o[:70], s.count(o))
    s = s.replace(o, n)

rep("""constexpr uint16_t kConnTimeout = 400; /* x10 ms = 4 s supervision timeout */""",
    """/* Floor, not target, for the supervision timeout \u2014 see request_fast_conn. */
constexpr uint16_t kConnTimeout = 400; /* x10 ms = 4 s */""")

rep("""void request_fast_conn(uint16_t conn_handle, bool apple_safe) {
    struct ble_gap_upd_params p = {};
    p.itvl_min = apple_safe ? kSafeItvlMin : kFastItvlMin;
    p.itvl_max = apple_safe ? kSafeItvlMax : kFastItvlMax;
    p.latency = 0;
    p.supervision_timeout = kConnTimeout;
    const int rc = ble_gap_update_params(conn_handle, &p);
    if (rc != 0) {
        /* Local refusal (no such connection, one already in flight): nothing
         * to retry against \u2014 the result path below only sees requests that
         * actually went out. */
        ESP_LOGW(TAG, "conn param request not sent (rc %d)", rc);
        return;
    }
    ESP_LOGI(TAG, "asking for a %u.%02u-%u.%02u ms connection interval",
             (unsigned)(p.itvl_min * 125u) / 100u,
             (unsigned)(p.itvl_min * 125u) % 100u,
             (unsigned)(p.itvl_max * 125u) / 100u,
             (unsigned)(p.itvl_max * 125u) % 100u);
}""",
    """/* A parameter update is a request for the *whole* set, and the central then
 * picks anywhere inside what it is offered \u2014 so a careless request can make
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
 *   voice_manager_all_notes_off(). Tolerance is worth more \u2014 the central picked
 *   that number knowing its own scheduling, and cutting it is how a link that
 *   merely stalled became a link that dropped.
 *
 * latency stays 0 either way: a skipped connection event is a delayed note, and
 * forcing it down is an improvement whatever the central had. */
void request_fast_conn(uint16_t conn_handle, bool apple_safe) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "conn param request not sent (no such connection)");
        return;
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
        return;
    }
    p.latency = 0;
    p.supervision_timeout = desc.supervision_timeout > kConnTimeout
                                ? desc.supervision_timeout
                                : kConnTimeout;

    const int rc = ble_gap_update_params(conn_handle, &p);
    if (rc != 0) {
        /* Local refusal (no such connection, one already in flight): nothing
         * to retry against \u2014 the result path below only sees requests that
         * actually went out. */
        ESP_LOGW(TAG, "conn param request not sent (rc %d)", rc);
        return;
    }
    ESP_LOGI(TAG,
             "asking for a %u.%02u-%u.%02u ms connection interval "
             "(timeout %u ms)",
             (unsigned)(p.itvl_min * 125u) / 100u,
             (unsigned)(p.itvl_min * 125u) % 100u,
             (unsigned)(p.itvl_max * 125u) / 100u,
             (unsigned)(p.itvl_max * 125u) % 100u,
             (unsigned)p.supervision_timeout * 10u);
}""")

# The retry log promised a retry that may now be skipped; let the callee say.
rep("""                    ESP_LOGI(TAG,
                             "conn params refused (status 0x%02x), retrying",
                             (unsigned)ev->conn_update.status);""",
    """                    ESP_LOGI(TAG, "conn params refused (status 0x%02x)",
                             (unsigned)ev->conn_update.status);""")

io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\components\ble_ctrl\ble_ctrl.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(old, new, count=1):
    global s
    o, n = old.replace("\n", nl), new.replace("\n", nl)
    assert s.count(o) == count, (o[:70], s.count(o))
    s = s.replace(o, n)

# Keep the policy blurb honest about the now-conditional retry.
rep(""" * A peripheral is allowed to ask for better, so it does \u2014 7.5-15 ms, the
 * fastest range every central must support, with no slave latency (a note must
 * never wait for a skipped event). Apple's rules are stricter (interval min
 * >= 15 ms), so a refusal retries once at 15-30 ms, still well under the
 * default. A second refusal is left alone: the link works, it is just lazier
 * than we would like, and nagging a central that has said no costs airtime on
 * the very path we are trying to keep clear.""",
    """ * A peripheral is allowed to ask for better, so it does \u2014 7.5-15 ms, the
 * fastest range every central must support, with no slave latency (a note must
 * never wait for a skipped event). Apple's rules are stricter (interval min
 * >= 15 ms), so a refusal retries once at 15-30 ms \u2014 but only for as much of
 * that range as is actually an improvement on what the link already has; see
 * request_fast_conn, which will send nothing rather than offer a central
 * permission to slow down. A second refusal is left alone: the link works, it
 * is just lazier than we would like, and nagging a central that has said no
 * costs airtime on the very path we are trying to keep clear.""")

# Report whether a request actually went out, so s_upd_tries only arms when one did.
rep("""void request_fast_conn(uint16_t conn_handle, bool apple_safe) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "conn param request not sent (no such connection)");
        return;
    }""",
    """/* Returns whether a request actually went out. The caller arms s_upd_tries off
 * that: a CONN_UPDATE is only "our result" if we asked, and marking a request
 * outstanding when none is would hand the next central-driven update to the
 * refusal path, spending the one retry on an answer to nobody's question. */
bool request_fast_conn(uint16_t conn_handle, bool apple_safe) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "conn param request not sent (no such connection)");
        return false;
    }""")

rep("""        ESP_LOGI(TAG, "connection interval already %u.%02u ms; not asking",
                 (unsigned)(desc.conn_itvl * 125u) / 100u,
                 (unsigned)(desc.conn_itvl * 125u) % 100u);
        return;
    }""",
    """        ESP_LOGI(TAG, "connection interval already %u.%02u ms; not asking",
                 (unsigned)(desc.conn_itvl * 125u) / 100u,
                 (unsigned)(desc.conn_itvl * 125u) % 100u);
        return false;
    }""")

rep("""        ESP_LOGW(TAG, "conn param request not sent (rc %d)", rc);
        return;
    }""",
    """        ESP_LOGW(TAG, "conn param request not sent (rc %d)", rc);
        return false;
    }""")

rep("""             (unsigned)(p.itvl_max * 125u) / 100u,
             (unsigned)(p.itvl_max * 125u) % 100u,
             (unsigned)p.supervision_timeout * 10u);
}""",
    """             (unsigned)(p.itvl_max * 125u) / 100u,
             (unsigned)(p.itvl_max * 125u) % 100u,
             (unsigned)p.supervision_timeout * 10u);
    return true;
}""")

rep("""                if (ev->subscribe.cur_notify && s_upd_tries == 0) {
                    s_upd_tries = 1;
                    request_fast_conn(ev->subscribe.conn_handle, false);
                }""",
    """                if (ev->subscribe.cur_notify && s_upd_tries == 0) {
                    if (request_fast_conn(ev->subscribe.conn_handle, false)) {
                        s_upd_tries = 1;
                    }
                }""")

io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
