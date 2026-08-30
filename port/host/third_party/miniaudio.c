/*
 * miniaudio implementation TU.
 *
 * Upstream ships miniaudio as a header plus this one source file; keeping the
 * split means the 96k-line implementation is compiled exactly once instead of
 * once per translation unit that wants a device.
 *
 * The MA_NO_* switches below are not micro-optimisation. miniaudio carries a
 * whole audio engine -- decoders, encoders, a resource manager, a node graph,
 * 3D spatialisation -- and osynth needs none of it: the synth *is* the engine,
 * and all this port wants is a device that takes interleaved int16 frames at
 * a fixed rate. Cutting the rest keeps the build fast and keeps the surface
 * this port depends on small enough to reason about.
 *
 * What is deliberately kept is the data-conversion layer, because the device
 * may not run natively at SYNTH_SAMPLE_RATE and miniaudio resamples for us.
 */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
