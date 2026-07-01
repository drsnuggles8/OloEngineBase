# Input action maps & contexts

Gameplay reads input through **named actions** ("Jump", "Fire") rather than raw
keys, so bindings can be remapped without touching gameplay code. The runtime API
lives in `OloEngine/src/OloEngine/Core/InputActionManager.h`; persistence lives in
`InputActionSerializer.{h,cpp}`. Author maps in the editor's **Input Settings** panel.

## Model

- `InputBinding` — one physical source (keyboard / mouse / gamepad button / gamepad
  axis). Axis bindings carry a `Threshold` + `Positive` direction.
- `InputAction` — a name plus a list of bindings (any of which triggers it).
- `InputActionMap` — a named set of actions.
- **Context** (`InputContextType`: `Gameplay`, `Menu`, `Vehicle`, `Custom`) — each
  context owns its own map. Exactly one context is active at a time (the top of the
  context stack). `SetInputContext` hard-switches; `PushContext` / `PopContext` nest
  (e.g. push `Menu` over `Gameplay`, pop to restore). The first `Update()` after a
  context change suppresses just-pressed/just-released so a key still held from the
  previous context doesn't immediately fire a same-key action in the new one.

`IsActionPressed` / `IsActionJustPressed` / `IsActionJustReleased` /
`GetActionAxisValue` all query the **active context's** map.

## Persistence (`Config/InputActions.yaml`)

Every authored context's map is saved together under an `InputActionContexts`
sequence, so per-context bindings survive a reload. The editor Save button writes
all contexts via `InputActionSerializer::SerializeContexts`; project load restores
them all via `DeserializeContexts` + `InputActionManager::SetActionMap(ctx, map)`.

```yaml
InputActionContexts:
  - Context: Gameplay
    Map:
      Name: Gameplay
      Actions:
        - Name: Fire
          Bindings:
            - Type: Keyboard
              Code: 32
  - Context: Menu
    Map:
      Name: Menu
      Actions: [...]
```

**Back-compat:** a file written in the old single-map format (an `InputActionMap`
root node, no contexts) still loads — `DeserializeContexts` reads that shape and
maps it to the `Gameplay` context. The old single-map `Serialize`/`Deserialize`
public methods were removed; only the legacy *read* branch survives, and every
per-map read/write goes through the shared `EmitActionMapNode` / `ParseActionMapNode`
helpers so the on-disk per-map shape is identical for both paths.

**Loading is a wholesale replace.** Project open and the editor's "Load from Disk"
both call `InputActionManager::ReplaceAllContextMaps`, which drops any context not
in the file — so maps never leak from a previously-open project and "Load from Disk"
is a true revert (a project with no input config resets to empty). Save persists
only non-empty context maps, so a context merely selected in the editor combo (which
lazily creates an empty map) is not written out.

**Editor edit target vs. runtime active context are separate.** The Input Settings
panel authors a chosen context via `InputActionManager::GetActionMapMutable(ctx)`
(tracked in a panel-local `m_EditContext`); it does **not** call `SetInputContext`,
so selecting a context to edit never collapses the runtime context stack or resets
live input state during Play. `SetInputContext` / `PushContext` / `PopContext` remain
purely a runtime/gameplay concern.

All floats read back from YAML are validated with `std::isfinite` (a `NaN`
`Threshold` falls back to `0.5`); unknown context names, malformed entries, and
bad bindings are skipped with a warning rather than aborting the load (see the
fuzz-regression and multi-context tests in `OloEngine/tests/InputActionTest.cpp`).
