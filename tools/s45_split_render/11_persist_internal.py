"""S45c: stop persist committing a value the firmware only borrowed.

Reported as "in.route comes back on mon after a reflash, but I had set it to
off". The registered default is 0 (off), so nothing was falling back to a
default -- something was storing `mon`.

sampler.cpp's monitor_hold() borrows in.route: while a sampler pad is armed and
the route is `off`, it moves it to `mon` so the player can hear what they are
about to record, and puts back exactly what was there when the recorder goes
idle. It writes with ParamOrigin::Internal precisely because it is not a player
edit.

persist's listener ignored `origin`, so the borrow marked the setting dirty and
the writer task could commit `mon` to NVS -- and a reset taken while the pad was
still armed, or before the restore's own write window came round, made it
permanent. Reflash, and the box comes up monitoring a route the player had
turned off.

presets.cpp already solved exactly this for its working state, and named the
same class of problem in state_tracks(): ParamOrigin::Internal is the firmware
talking to itself, and almost all of it is transient -- playhead telemetry,
parameter locks, and this borrow. persist simply never got the same treatment.

Checked before excluding it: the only other Internal writes that can land on a
persisted id are usb_mode_resolve()'s corrective clamp -- which is recomputed
from the stored value at every boot, so persisting it only ever destroyed the
player's choice -- and ParamStore::resetRange(), which has no caller outside
synth_params.cpp.

Run from the repo root: python tools/s45_split_render/11_persist_internal.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _edit import Editor

# --------------------------------------------------------------------------
# persist.cpp -- the fix
# --------------------------------------------------------------------------
p = Editor("components/persist/persist.cpp", skip_if="ParamOrigin::Internal")
p.sub(
    """/* Any control task; must stay short. Marking is all it does — the writer task
 * decides when. Preset loads are included deliberately: a preset that changes
 * a persisted setting should survive a reboot like any other edit. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    (void)value;
    (void)origin;
    if (!s_ready || !is_persisted(id)) return;
    s_dirty.store(true, std::memory_order_relaxed);
    s_dirty_seq.fetch_add(1, std::memory_order_relaxed);
}""",
    """/* Any control task; must stay short. Marking is all it does — the writer task
 * decides when. Preset loads are included deliberately: a preset that changes
 * a persisted setting should survive a reboot like any other edit.
 *
 * ParamOrigin::Internal is not, and that exclusion is the whole of S45c. It is
 * the firmware talking to itself, and what it says is usually *temporary* — the
 * case that found this is sampler.cpp's monitor_hold(), which borrows in.route
 * from `off` to `mon` while a pad is armed so the player can hear what they are
 * about to record, and puts back what was there when the recorder goes idle.
 * Storing the borrowed value defeats the borrow: a reset taken while the pad
 * was still armed, or simply before the restore's own write window came round,
 * left `mon` in NVS for good. The player turns the input monitor off, reflashes,
 * and the box comes up monitoring.
 *
 * presets.cpp reached the same conclusion first, for the same reason, and
 * state_tracks() there is worth reading beside this: parameter locks rewrite a
 * patch value on every step and put it back when the track stops, and counting
 * those as edits would store the lock's momentary value rather than the one the
 * player set. Identical shape, different owner.
 *
 * Nothing is lost by it. The two other Internal writers that can reach a
 * persisted id are usb_mode_resolve()'s corrective clamp — recomputed from the
 * stored value at every boot, so persisting it only ever overwrote the choice
 * the player is still entitled to — and ParamStore::resetRange(), which has no
 * caller outside synth_params.cpp. A genuine reset-to-defaults arriving from
 * the app, MIDI or the local UI carries that origin and is stored as always. */
void param_listener(uint16_t id, float value, ParamOrigin origin, void*) {
    (void)value;
    if (origin == ParamOrigin::Internal) return;
    if (!s_ready || !is_persisted(id)) return;
    s_dirty.store(true, std::memory_order_relaxed);
    s_dirty_seq.fetch_add(1, std::memory_order_relaxed);
}""",
)
p.save("Internal-origin writes no longer mark the settings dirty")

# --------------------------------------------------------------------------
# sampler.cpp -- record why the borrow is safe, at the borrow
# --------------------------------------------------------------------------
s = Editor("components/drums/sampler.cpp", skip_if="never reaches NVS")
s.sub(
    """ * `smp.src = bus` never borrows anything — the bus is the thing already coming
 * out of the speakers. */""",
    """ * `smp.src = bus` never borrows anything — the bus is the thing already coming
 * out of the speakers.
 *
 * The origin on both writes is load-bearing, not decoration. in.route is a
 * persisted setting, and ParamOrigin::Internal is what keeps a borrowed value
 * out of NVS (persist.cpp's param_listener). Without that, a reset taken while
 * a pad was still armed stored `mon` permanently, and the player's `off` came
 * back as monitoring on the next boot — which is how S45c found it. Anything
 * added here that moves a persisted parameter temporarily has to carry the same
 * origin for the same reason. */""",
)
s.save("note why the borrow never reaches NVS")
