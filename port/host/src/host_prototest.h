/* osynth host port — the SynthCtl round-trip test. See the .cpp. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 if every check passed. Installs its own transport for the
 * duration and detaches it afterwards, so the engine must be up but nothing
 * else may be driving the protocol at the same time. */
int osynth_host_prototest(void);

#ifdef __cplusplus
}
#endif
