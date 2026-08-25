#!/usr/bin/env python3
"""Applied edits: the ATT-transaction-timeout disconnect loop (firmware side).

Kept per the project's intermediary-artifacts policy. Companion to
win_ble_gui_stall_fix.py, which covers the app side of the same investigation.
Not idempotent: the asserts fail against an already-patched tree, deliberately.

The bug, from the paired app/firmware logs:

  1. The app's connect burst is several multi-frame listings back to back.
     GET_PARAM alone answers 120 ids in ~10 full-MTU frames, and the app asks
     for all 365 parameters twice (onParamListComplete + finishDiscovery).
  2. Every one of those responses except the PARAM_INFO id list went out
     unpaced, draining the NimBLE host's msys pool.
  3. The *receive* path allocates from the same pool, so NimBLE could not build
     the response to an incoming ATT request -- nor the error response:
     "E NimBLE: ble_att_svr_pkt rc=6" (BLE_HS_ENOMEM), ~8 s after connect,
     which is where discovery ends.
  4. ATT permits one outstanding request per bearer, so the central sent
     nothing further and closed the link on the 30 s transaction timeout.
     Measured 30078 ms and 30079 ms from the rc=6 to "app disconnected
     (reason 0x213)" -- HCI 0x13, remote user terminated. That pair of numbers
     is what identified this.

fw1: pace every multi-frame command response. flush_events() keeps the unpaced
     default on purpose -- an event must never park the flush task.
fw2: rewrite the Chunker's `paced` doc comment to record the above.
fw3: raise the msys pool in sdkconfig.defaults (12/24 -> 24/48) as headroom
     under the pacing.

After fw3, `sdkconfig` is stale -- it is only generated when absent, so an
existing one wins. tools/check_sdkconfig_drift.py reports the mismatch; delete
sdkconfig and rebuild, or set the two values in menuconfig.

Verified with tools/c_brace_check.py, tools/check_comment_blocks.py,
tools/check_mojibake.py and tools/check_sdkconfig_drift.py.
"""

import io
p = r"D:\dev\osynth_free_synth\components\ble_ctrl\ble_ctrl.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
crlf = "\r\n" in s
nl = "\r\n" if crlf else "\n"

def rep(old, new, count=1):
    global s
    o = old.replace("\n", nl)
    n = new.replace("\n", nl)
    assert s.count(o) == count, (o[:60], s.count(o))
    s = s.replace(o, n)

# GET_PARAM -- the widest response in the connect burst.
rep("""    s_chunker.begin(OP_GET_PARAM | 0x80, seq, nullptr, 0, false);""",
    """    s_chunker.begin(OP_GET_PARAM | 0x80, seq, nullptr, 0, false,
                    /*paced=*/true);""")

rep("""    s_chunker.begin(OP_LIST_PRESETS | 0x80, seq, prefix, sizeof(prefix),
                    false);""",
    """    s_chunker.begin(OP_LIST_PRESETS | 0x80, seq, prefix, sizeof(prefix),
                    false, /*paced=*/true);""")

rep("""        s_chunker.begin(OP_SEQ_STEPS | 0x80, seq, prefix, sizeof(prefix), false);""",
    """        s_chunker.begin(OP_SEQ_STEPS | 0x80, seq, prefix, sizeof(prefix), false,
                        /*paced=*/true);""")

rep("""            s_chunker.begin(OP_SEQ_PLOCK | 0x80, seq, prefix, sizeof(prefix),
                            false);""",
    """            s_chunker.begin(OP_SEQ_PLOCK | 0x80, seq, prefix, sizeof(prefix),
                            false, /*paced=*/true);""", count=2)

rep("""    s_chunker.begin(OP_SEQ_SONG | 0x80, seq, prefix, sizeof(prefix), false);""",
    """    s_chunker.begin(OP_SEQ_SONG | 0x80, seq, prefix, sizeof(prefix), false,
                    /*paced=*/true);""")

rep("""        s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false);""",
    """        s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                        /*paced=*/true);""")

rep("""    s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false);""",
    """    s_chunker.begin(OP_KIT_INFO | 0x80, seq, prefix, sizeof(prefix), false,
                    /*paced=*/true);""")

io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok crlf=", crlf)
import io
p = r"D:\dev\osynth_free_synth\components\ble_ctrl\ble_ctrl.cpp"
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
old = """    /* `paced` waits for an mbuf instead of failing (send_frame_paced), for a
     * response whose partial loss the *client* cannot detect. It blocks, so it
     * is only legal from ble_cmd \u2014 which is where every command handler runs.
     * Default off: an event must never park the flush task. */""".replace("\n", nl)
new = """    /* `paced` waits for an mbuf instead of failing (send_frame_paced). It
     * blocks, so it is only legal from ble_cmd \u2014 which is where every command
     * handler runs. Every command response passes it; the parameter keeps its
     * `false` default for the one caller that must not have it, flush_events(),
     * because an event must never park the flush task (it re-arms its dirty
     * bits instead).
     *
     * It started as "for a response whose partial loss the *client* cannot
     * detect", i.e. the PARAM_INFO id list alone. That was too narrow. The
     * unpaced responses did not merely lose their own frames: a connect burst
     * is several multi-frame listings back to back \u2014 GET_PARAM alone answers
     * 120 ids in ~10 full-MTU frames, and the app asks that four times \u2014 and
     * emitting them as fast as the loop runs drained the host's msys pool that
     * the *receive* path also allocates from. NimBLE then could not build the
     * response to an incoming ATT request, nor even the error response
     * ("ble_att_svr_pkt rc=6", BLE_HS_ENOMEM), so the request went unanswered.
     * ATT permits one outstanding request per bearer, so the central sent
     * nothing further and closed the link on the 30 s transaction timeout \u2014
     * measured at 30078 and 30079 ms from the rc=6 to the disconnect, which is
     * what identified this. Waiting for a buffer costs a few ms per frame and
     * leaves the pool something to answer with. */""".replace("\n", nl)
assert s.count(old) == 1
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
import io
p = r"D:\dev\osynth_free_synth\sdkconfig.defaults"
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
old = """CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y
""".replace("\n", nl)
new = """CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y
# Host buffer pool (msys). IDF's defaults are 12x256 B and 24x320 B, which were
# sized for small ATT payloads \u2014 but BT_NIMBLE_ATT_PREFERRED_MTU defaults to
# 256, and a 253-byte ATT payload plus the os_mbuf and packet headers does not
# fit a 256-byte block, so every full-size frame this protocol sends comes out
# of the 24 msys-2 blocks. A connect burst is several multi-frame listings back
# to back and drained that pool, at which point NimBLE could not allocate for
# the *receive* path either: "E NimBLE: ble_att_svr_pkt rc=6" (BLE_HS_ENOMEM),
# an incoming ATT request left unanswered because even the error response
# needs a buffer, and \u2014 ATT allowing one outstanding request per bearer \u2014 the
# central closing the link on the 30 s transaction timeout (reason 0x213,
# measured at 30078 ms from the rc=6, twice).
#
# The pacing in ble_ctrl's Chunker is the actual fix for that; this is the
# headroom underneath it, so a burst waits a few milliseconds rather than
# spending its whole budget in send_frame_paced. ~11 kB of internal RAM: free
# on the P4/S3, and the one place to trim first if the classic ESP32 gets
# tight.
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=24
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=48
""".replace("\n", nl)
assert s.count(old) == 1
s = s.replace(old, new)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
