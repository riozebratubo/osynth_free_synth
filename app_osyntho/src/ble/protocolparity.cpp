// Compile-time proof that the app's copy of SynthCtl v1 matches the synth's.
//
// The app has always carried its own opcode and status constants
// (synthprotocol.h), and it has to: a BLE build talks to an instrument over
// the air and cannot include a firmware header. So the two are, and remain,
// separate declarations of one wire format -- kept in agreement until now by
// nothing but care.
//
// Care is not enough, and there is a scar to prove it. The app's engine enum
// stopped at ENG_GRANULAR = 5 while the firmware had added the sampler at 6.
// The app drew a seventh engine it could not name ("?"), and selecting it
// asked the synth for a preset list it then crashed serving. Nothing in either
// build said a word until a user clicked it.
//
// The standalone build is what makes a check possible: both declarations are
// in one binary, so they can be compared. Every constant below is asserted
// against components/ctrl_proto/include/ctrl_proto_wire.h, which is now the
// single owner. Change a value on either side and this file stops the build.
//
// Compiled only when OSYNTHO_EMBEDDED is on (see CMakeLists.txt), which is the
// only configuration where the firmware headers are reachable. It emits no
// code -- it is entirely static_assert -- so it costs the binary nothing.

#include "src/ble/synthprotocol.h"

extern "C" {
#include "ctrl_proto.h"      /* CTRL_PROTO_MAX_FRAME */
#include "ctrl_proto_wire.h" /* the firmware's opcodes and status codes */
#include "engines.h"         /* SYNTH_ENGINE_COUNT */
}

namespace {

// The wire header's enums are unnamed and at global scope; the app's live in
// SynthProto. Naming both sides on every line is what makes a failure readable
// -- the message names the constant that disagreed.
#define OSYNTH_SAME_OP(name) \
  static_assert(static_cast<quint8>(SynthProto::name) == static_cast<quint8>(name), \
                "SynthCtl drift: " #name " differs between app and firmware")

// ---- commands ------------------------------------------------------------
OSYNTH_SAME_OP(OP_SET_PARAM);
OSYNTH_SAME_OP(OP_GET_PARAM);
OSYNTH_SAME_OP(OP_PARAM_INFO);
OSYNTH_SAME_OP(OP_SELECT_ENGINE);
OSYNTH_SAME_OP(OP_LOAD_PRESET);
OSYNTH_SAME_OP(OP_SAVE_PRESET);
OSYNTH_SAME_OP(OP_LIST_PRESETS);
OSYNTH_SAME_OP(OP_USB_STATUS);
OSYNTH_SAME_OP(OP_REBOOT);
OSYNTH_SAME_OP(OP_TRANSPORT);
OSYNTH_SAME_OP(OP_ARP);
OSYNTH_SAME_OP(OP_NOTE_ON);
OSYNTH_SAME_OP(OP_NOTE_OFF);
OSYNTH_SAME_OP(OP_SEQ_INFO);
OSYNTH_SAME_OP(OP_SEQ_STEPS);
OSYNTH_SAME_OP(OP_SEQ_TRACK);
OSYNTH_SAME_OP(OP_SEQ_PATTERN);
OSYNTH_SAME_OP(OP_SEQ_PLOCK);
OSYNTH_SAME_OP(OP_SEQ_EDIT);
OSYNTH_SAME_OP(OP_SEQ_SONG);
OSYNTH_SAME_OP(OP_KIT_INFO);
OSYNTH_SAME_OP(OP_DRUM_TRIG);
OSYNTH_SAME_OP(OP_GRAPH_INFO);
OSYNTH_SAME_OP(OP_GRAPH_KIND);
OSYNTH_SAME_OP(OP_GRAPH_NODES);
OSYNTH_SAME_OP(OP_GRAPH_EDIT);
OSYNTH_SAME_OP(OP_LOOP_DUMP);
OSYNTH_SAME_OP(OP_CHORD_SET);
OSYNTH_SAME_OP(OP_KIT_EDIT);
OSYNTH_SAME_OP(OP_PING);

// ---- events --------------------------------------------------------------
OSYNTH_SAME_OP(EVT_PARAMS);
OSYNTH_SAME_OP(EVT_ENGINE);

// ---- status codes --------------------------------------------------------
OSYNTH_SAME_OP(ST_OK);
OSYNTH_SAME_OP(ST_MALFORMED);
OSYNTH_SAME_OP(ST_UNKNOWN_OP);
OSYNTH_SAME_OP(ST_BAD_ARG);
OSYNTH_SAME_OP(ST_UNSUPPORTED);
OSYNTH_SAME_OP(ST_BUSY);

#undef OSYNTH_SAME_OP

// ---- the two flags, which the app spells differently ---------------------
//
// Not renamed on either side: RESPONSE_FLAG and CONTINUATION_FLAG read better
// at the app's call sites, and ST_MORE reads better inside the status byte the
// firmware is writing. They are the same two bits, and that is what matters.
static_assert(static_cast<quint8>(SynthProto::CONTINUATION_FLAG) ==
                  static_cast<quint8>(ST_MORE),
              "SynthCtl drift: the continuation bit differs");
static_assert(static_cast<quint8>(SynthProto::RESPONSE_FLAG) == 0x80,
              "SynthCtl drift: a response is request_op | 0x80");

// ---- frame ceiling -------------------------------------------------------
//
// The app sizes its batches from the negotiated MTU rather than from a
// constant, so this checks the only fixed relationship: its preferred MTU has
// to leave room for a full frame, or every batch it packs is refused.
static_assert(SynthProto::kPreferredMtu >= CTRL_PROTO_MAX_FRAME - 9,
              "kPreferredMtu is too small for the frames the synth builds");

// ---- engines -------------------------------------------------------------
//
// The count, not the individual values: the app's Engine enum exists to name
// indices the firmware assigns, and the firmware's own list is a table of
// strings in main.cpp rather than an enum this can compare against. What can
// be checked is that the app knows about as many engines as exist -- which is
// exactly what was wrong when the sampler was added.
static_assert(static_cast<int>(SynthProto::ENG_SAMPLER) + 1 ==
                  SYNTH_ENGINE_COUNT,
              "the app's Engine enum does not cover every engine the firmware "
              "has; add the new one and give it a name in engineNameFor()");

}  // namespace
