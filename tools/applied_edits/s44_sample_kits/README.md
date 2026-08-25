# S44 — sample kits: the scripted edits

The five scripts here applied the larger structural edits of Session 44 (the
recordable sample kits, the recorder and the sampler engine). They are kept
because they are already applied and re-running one would fail or duplicate —
each checks for its own marker and exits — but they are the record of *what*
was changed mechanically and where, which a diff alone reads badly for edits
that splice whole functions.

Run from the repository root.

| Script | Target | What it did |
|--------|--------|-------------|
| `patch_ble.py` | `components/ble_ctrl/ble_ctrl.cpp` | added `OP_KIT_EDIT` (0x3F), replaced `handle_kit_info()` whole (the old one carried a U+2026 that made a literal match unreliable, so it splices by line), added `handle_kit_edit()` and its dispatch arm |
| `patch_app_ctl.py` | `app_osyntho/src/synthcontroller.{h,cpp}` | the `kitStorage` property, the widened `handleKitInfo()` parser with its old/new record-shape detection, and the `setPadField` / `renameKit` / `renamePad` / `releaseDrum` invokables |
| `patch_drumpads.py` | `app_osyntho/qml/DrumPads.qml` | the Record / Erase / Undo action row, the arm-then-pad gesture, touch-up release, and `slotFilled()` |
| `patch_tr.py` | `app_osyntho/src/translator.cpp` | 29 pt_BR strings for the new UI |
| `patch_docs.py` | `private_docs/PARAM_MAP.md`, `private_docs/BLE_PROTOCOL.md` | the `0x075x` parameter table, the sampler engine's `0x02xx` table, and the `KIT_EDIT` / widened `KIT_INFO` / `DRUM_TRIG` release sections |

Everything else in S44 was written directly: `components/drums/sampler.cpp`,
`components/drums/drums_priv.h`, `components/drums/include/sampler.h`,
`components/engines/engine_sampler.{h,cpp}` and the rewritten
`app_osyntho/qml/DrumsScreen.qml`.

`tools/check_braces.py`, added in the same session, is the structural check
that was run over every touched C/C++ and QML file afterwards — the build
policy here means edits land unbuilt, and an unbalanced block from a scripted
splice is the one mistake worth catching without a compiler.
