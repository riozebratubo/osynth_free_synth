# Raising QML AOT coverage in app_osyntho

## The thing that surprised everyone

**The QML compiler was already on.** `qt_add_qml_module` runs `qmlcachegen`
automatically, and it has been compiling this app since the module was created.
What was missing was not a switch — it was *coverage*.

`qmlcachegen` compiles each binding and JS function to C++ **only when it can
resolve every name and type in it**. Anything it cannot resolve silently falls
back to interpreted byte code. The build tree keeps the scorecard
(`--dump-aot-stats` is on by default), and it read:

```
AOT coverage: 1014/2202 bindings compiled to C++ (46.0%), 1188 fell back
```

Top reasons, from `tools/qml_aot_score.py`:

| count | reason |
|------:|--------|
| 297 | `Cannot access value for name Synth` |
| 230 | `Cannot access value for name t` |
| 137 | `Cannot access value for name App` |
| 130 | `QQuickMaterialStyle::foreground` — QVariant conversion |
|  84 | `Functions without type annotations won't be compiled` |
|  30 | `Cannot access value for name root` |
|  24 | untyped JavaScript function call |
|  18 | `Cannot load property exists from ::meta with type QVariant` |
|  16 | storing an array in a non-sequence type |
|  14 | `Cannot access value for name screen` |
|   8 | `Cannot access value for name BluetoothManager` |

**672 of the 1188 fallbacks were the four `setContextProperty` globals.** A
context property does not exist until an engine is running, so no compiler can
see through one. That is what phase 1 fixed.

## Measuring

```
python tools/qml_aot_score.py <build-dir> --top 15 --by-file
python tools/qml_aot_score.py <build-dir> --json tools/out/aot_after.json
python tools/qml_aot_score.py --compare tools/out/aot_baseline.json tools/out/aot_after.json
```

`tools/out/aot_baseline.json` is the pre-change measurement, taken from
`build/STATIC_Desktop_Qt_6_11_0_MSVC2022_64bit-Release`. Compare against the
same build dir or the numbers will not mean anything.

`tools/qml_unqualified.py` groups qmllint's `[unqualified]` warnings by the
identifier that caused them, which separates "fix once in C++" from "qualify at
every site".

---

## Phase 1 — context properties to QML singletons (done)

`App`, `Synth`, `t` and `BluetoothManager` were injected with
`QQmlContext::setContextProperty`. They are declaratively registered types now,
so `qmlcachegen` and `qmllint` both know their full API at compile time.

| was | is |
|---|---|
| `setContextProperty("App", …)` | `App` — `QML_ELEMENT` + `QML_SINGLETON` |
| `setContextProperty("Synth", …)` | `SynthController` — `QML_NAMED_ELEMENT(Synth)` + `QML_SINGLETON` |
| `setContextProperty("t", …)` | `Translator` — `QML_NAMED_ELEMENT(Tr)` + `QML_SINGLETON` |
| `setContextProperty("BluetoothManager", …)` | `IBluetoothManager` via `BluetoothManagerForeign` in `src/qmlforeign.h` |
| `Theme` — an unregistered `Q_GADGET` | `QML_ANONYMOUS`, so `App.theme.primaryColor` resolves |
| `qmlRegisterType<SynthController>("org.osynth.main", …)` | deleted — nothing imported that URI |

Each singleton gets a `static T *create(QQmlEngine *, QJSEngine *)` that returns
the existing process-wide instance and pins ownership to C++ with
`QJSEngine::setObjectOwnership`, so the engine never deletes an object it does
not own.

`IBluetoothManager` is abstract and must not name a backend, so it is registered
from the outside by a `QML_FOREIGN` struct — the platform switch between
`bluetoothmanager.h` and `bluetoothmanager2.h` stays in one `.cpp`.

**The `t` rename.** QML type names must start with an upper-case letter, so the
translator could not stay `t`. It is `Tr` now: 320 `Tr.t(…)`, 14 `Tr.ts(…)`,
1 `Tr.setActiveLanguage(…)` across 24 files. If you would rather it were
something else, it is one regex in `tools/qmlaot_p1_singletons.py`.

Every file that touches a singleton now carries `import org.osynth.osyntho`.
That import is *not* optional here: with `QTP0001 NEW` the QML files sit in a
`qml/` subdirectory of the module, so the implicit directory import does not
reach the module's own C++ types.

Also removed: `SettingsItemNumber.qml` and `SettingsItemToggable.qml` (dead —
referenced only by `CMakeLists.txt`, instantiated nowhere) and an unused
`import QtQuick.Layouts` in `Main.qml`.

Scripts: `tools/qmlaot_p1_singletons.py`, `tools/qmlaot_p1_fixups.py`. Both are
idempotent and take `--check`.

### Expected result

Roughly **46% → low-to-mid 70s**. Not the full +672: some bindings have a second
blocker behind the first, and the ones that call `Synth.paramMeta()` or read
`Synth.graphNodes` will now fail on the `QVariantMap`/`QVariantList` return
instead. Those are phase 2. Measure, do not assume — that is what the scorer is
for.

---

## Phase 2 — value types for the QVariant API (not started)

Every `meta.exists`, `modelData.kind`, `cfg.length` in the QML is a runtime
`QVariant` lookup, because `SynthController` hands QML untyped containers.
Resolving the *name* `Synth` does not help the compiler with what comes back
from it.

The good news is that the real structs already exist in
`src/ble/synthprotocol.h` — `ParamInfo`, `SeqStep`, `SeqTrackCfg`, `GraphKind`,
`GraphNode`, `KitSlot`, `KitEntry`, `PresetEntry`, `LoopDumpInfo`, `UsbStatus`.
The `QVariantMap`/`QVariantList` layer is a conversion built *for* QML. The work
is to delete that layer, not to invent a data model.

Surface to convert in `src/synthcontroller.h`:

| returns today | becomes |
|---|---|
| `QVariantMap paramMeta(int)` | `ParamMeta` gadget |
| `QVariantList paramIds()`, `paramIdsByPrefix()` | `QList<int>` |
| `QVariantList paramPickerList()` | `QList<ParamRef>` |
| `QVariantList steps()`, `QVariantMap step(int)` | `QList<SeqStep>` / `SeqStep` |
| `QVariantMap trackConfig()`, `patternConfig()` | `TrackConfig`, `PatternConfig` |
| `QVariantList plocksForStep(int)` | `QList<Plock>` |
| `QVariantList song()` | `QList<SongEntry>` |
| `QVariantMap graphKind(int)`, `QVariantList graphKinds` | `GraphKind` gadget |
| `QVariantList graphNodes` | `QList<GraphNode>` |
| `QVariantMap loopExportInfo` | `LoopExportInfo` gadget |
| `QVariantList presetsFor(int)`, `patches(int)` | `QList<Preset>`, `QList<Patch>` |
| `QVariantList engineList`, `kitSlots`, `kits` | `QList<EngineRef>`, `QList<KitSlot>`, `QList<Kit>` |

Each gadget needs `Q_GADGET` + `Q_PROPERTY` per field + `QML_VALUE_TYPE(name)`
(lower-case) or `QML_ANONYMOUS` where QML never declares one. `QList<T>` of a
registered gadget is a usable QML sequence type.

Then the ~37 non-`modelData` `property var` declarations in QML can take real
types, which is what removes the `Cannot load property … with type QVariant`
family. Representative consumers: `Knob.qml:14`, `EnumSelector.qml:16`,
`ParamControl.qml:22`, `Toolbar.qml:92`, `PlockDialog.qml:19`,
`TrackSheet.qml:15`, `SequencerScreen.qml:28`, `GraphScreen.qml:295`.

Do this one type at a time and re-score after each. `paramMeta` is the highest
value — `Knob`, `EnumSelector`, `ParamControl`, `Toolbar` and `PlockDialog` all
read it.

---

## Phase 3 — QML cleanup (not started)

**163 unqualified local ids**, in 15 files, listed in
`tools/out/unqualified_local.json`. Worst: `GraphScreen` 27, `Main` 23,
`SequencerScreen` 21, `Keyboard` 20, `Toolbar` 17.

These are references to an `id` from an enclosing scope, reached from inside a
`Repeater`/`Component` delegate — `screen.nodeW`, `root.columns`, `grid.cellW`.
QML resolves them through the dynamic scope chain, which cannot be compiled.

The documented fix is `pragma ComponentBehavior: Bound`. **Read this before
adding it:** under `Bound`, a delegate is created in its own component's
context, so a view can no longer inject `index`, `model` and `modelData` as
context properties. Every delegate in the file must declare them as
`required property` first, or they silently become undefined at runtime — the
UI will build and then misbehave. Convert one file at a time and exercise that
screen in the running app.

**~170 QML functions have no type annotations** (84 AOT fallbacks). Purely
mechanical — `function hit(slot, velocity)` → `function hit(slot: int, velocity: real): void`.
No design decisions, but each signature has to be read against its call sites.

**7 `[missing-property]` warnings**, including two real ones in `Main.qml`:
`Connections { target: Qt.application }` reading `.state` (line 125) and
`target: Qt.inputMethod` reading `.visible` (line 139).

**`ToastManager.qml`** is the only dynamic instantiation left in the codebase
(`Qt.createComponent("Toast.qml")` at :52, `createObject(root)` at :29).
172 lines with `Toast.qml`; a `Repeater` over a `ListModel` would remove it.
Low AOT value, but it is the one thing standing between this app and qmltc.

**The 130 Material fallbacks are not worth chasing.**
`QQuickMaterialStyle::foreground` is typed `QVariant` inside Qt Quick Controls,
and 154 sites read it. The only fix is to stop using the attached property and
take the colour from `App.theme` / `UI` instead — a styling change, not a
compiler one.

---

## What about qmltc?

`ENABLE_TYPE_COMPILER` (the placeholder at `CMakeLists.txt`) is a **different
compiler**, not a bigger version of this one:

- `qmlcachegen` compiles **bindings and functions** — the 60fps hot path.
- `qmltc` compiles **whole documents into C++ classes**, replacing
  `QQmlComponent` object creation — startup and screen-push cost.

It is not an alternative to the work above; it sits on top of it. Qt 6.11's own
docs: *"qmltc can only compile a QML document if it completely understands its
structure. It will fail if an unsupported language feature is encountered."*
The same untyped names that make `qmlcachegen` fall back make `qmltc` **fail the
build** — there is no graceful degradation.

Two blockers remain even after phases 2 and 3:

- *"Imported QML modules that consist of QML-defined types (such as
  QtQuick.Controls) might not get compiled correctly."* 37 of the 41 QML files
  import `QtQuick.Controls.Material`.
- The payoff only lands where **C++** instantiates the tree. `main.cpp` uses
  `engine.loadFromModule(…)`, and the `StackView.push("SettingsScreen.qml")`
  sites, the `Loader` in `ParamControl.qml` and `ToastManager` all go through
  the engine regardless.

qmltc is also still marked Tech Preview in 6.11, and requires linking private Qt
libraries (`Qt::QmlPrivate`, `Qt::QuickPrivate`).

`qmlsc` — the compiler with Direct Mode and Static Mode — is a commercial
add-on. On this open-source Qt, `qmlcachegen` is what runs, and raising its
coverage is the whole of the available win.
