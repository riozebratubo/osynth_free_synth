# Host-port session logs

Captured output from `osynth_host_demo` while bringing up the standalone build.
Kept because the numbers in them are the evidence behind several decisions, and
re-running does not reproduce a log from a build that no longer exists.

The naming is the shape of a run rather than a scheme: a bare name is stdout and
the `e` suffix is stderr, because the demo writes its check results to one and
the engine's `ESP_LOGx` boot trace to the other, and reading them apart is the
whole point.

| Pair | Run |
| --- | --- |
| `w` / `we`, `sw_out` / `sw_err` | `--storage-write` — set parameters, then exit |
| `b` / `be`, `c` / `ce`, `sc_out` / `sc_err` | `--storage-check` — the second process, reading back what the first wrote |
| `kw` / `kwe` | `--storage-write` after the sample-kit pool was restored |
| `hbo` / `hb`, `hbo2` / `hb2` | the heartbeat, before and after the per-stage cycle accounting went back into `render_chain()` — `hb` shows `[voi 0.0 fx 0.0 loop 0.0]`, `hb2` shows real numbers |
| `a` / `ae` | an earlier storage write, kept for the boot trace |
