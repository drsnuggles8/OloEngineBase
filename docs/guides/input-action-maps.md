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
root node, no contexts) still loads — `DeserializeContexts` maps it to the
`Gameplay` context. Legacy `Serialize` / `Deserialize` (single map) remain for that
on-disk shape; the contexts always emit the same per-map shape via the shared
`EmitActionMapNode` / `ParseActionMapNode` helpers.

All floats read back from YAML are validated with `std::isfinite` (a `NaN`
`Threshold` falls back to `0.5`); unknown context names, malformed entries, and
bad bindings are skipped with a warning rather than aborting the load (see the
fuzz-regression and multi-context tests in `OloEngine/tests/InputActionTest.cpp`).
