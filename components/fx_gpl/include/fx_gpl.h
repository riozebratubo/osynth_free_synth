/*
 * osynth — GPL-3 reverb algorithms (S36).
 *
 * ============================== LICENCE ==============================
 * Everything in components/fx_gpl is GPL-3, NOT MIT like the rest of
 * osynth. It is a separate component for exactly that reason, and it is
 * compiled only when CONFIG_OSYNTH_FX_GPL is set (off by default).
 *
 * Building with that option ON produces a firmware image that is a GPL-3
 * combined work: you may distribute it, but only under GPL-3, and only
 * with the complete corresponding source of what you flashed. Building
 * with it OFF leaves this component out of the link entirely and the
 * image stays MIT. See components/fx_gpl/LICENSE.
 * =====================================================================
 *
 * Two algorithms, both cousins of the same 1997 Dattorro paper and both
 * audibly different from it and from each other:
 *
 *   MVerb    — Martin Eastwood, https://github.com/martineastwood/mverb
 *              The textbook figure-of-eight: two cross-coupled tanks fed
 *              through four smearing allpasses, with an 8-tap early field
 *              per side. The reference implementation of the structure.
 *
 *   DuskVerb — Dusk Audio, the "Plate" engine of their DuskVerb.
 *              The same figure-of-eight with a twelve-deep allpass density
 *              cascade inside each loop, which multiplies echo density per
 *              pass instead of relying on loop time to build it up. That is
 *              what separates it from MVerb on the same knob settings: it
 *              arrives dense rather than becoming dense.
 *
 * The bus owns one instance of each for the life of the process; these
 * return the singletons. Null is never returned — an algorithm that could
 * not allocate reports it from init() instead, and the bus then refuses to
 * select it.
 */
#pragma once

#include "synth_reverb_algo.h"

namespace osynth {
namespace fx {

RevAlgorithm* mverb_instance();
RevAlgorithm* duskverb_instance();

}  // namespace fx
}  // namespace osynth
