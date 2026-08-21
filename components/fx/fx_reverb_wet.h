/*
 * osynth — WetReverb, algorithm 1 of the master reverb (S36).
 *
 * Private to the `fx` component; fx.cpp is the only caller. Returns the
 * singleton rather than a constructible type because the bus owns exactly one
 * of each algorithm for the life of the process and their buffers are
 * allocated once, at boot, from fx_init().
 */
#pragma once

#include "synth_reverb_algo.h"

namespace osynth {
namespace fx {

RevAlgorithm* wetreverb_instance();

}  // namespace fx
}  // namespace osynth
