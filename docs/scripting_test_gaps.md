# Scripting Test Gap Analysis

## Current State

The scripting binding layers have **zero automated test coverage**. This applies to both C# (Mono) and Lua (sol2) bindings.

| Layer | LOC (approx) | Unit Tests | Integration Tests |
|-------|-------------|------------|-------------------|
| ScriptGlue.cpp (C# internal calls) | 3,800+ | 0 | 0 |
| LuaScriptGlue.cpp (Lua bindings) | 2,000+ | 0 | 0 |
| ScriptEngine.cpp (Mono runtime) | — | 0 | 0 |
| LuaScriptEngine.cpp (Lua runtime) | — | 0 | 0 |

### Bugs Found Due to This Gap

1. **SetPitch copy-paste bug** — `AudioSourceComponent_SetPitch` called `SetVolume()` instead of `SetPitch()`. Only discoverable by actually calling the C# property from a running script.
2. **C# InternalCall name mismatch** — C# declared `AudioSource_*` but C++ registered `AudioSourceComponent_*`. Mono silently fails on unresolved internal calls, so **all C# audio bindings were broken at runtime** with no compile-time or test-time signal.

## What Needs Testing

### C# Scripting (Mono)

- **InternalCall name consistency**: Verify that every `[MethodImpl(MethodImplOptions.InternalCall)]` declaration in `InternalCalls.cs` has a matching `OLO_ADD_INTERNAL_CALL(Name)` registration in `ScriptGlue.cpp`. This can be a static analysis test (parse both files, compare name sets).
- **Component property round-trips**: For each component property exposed to C# (e.g., `TransformComponent.Translation`, `AudioSourceComponent.Volume`), verify get/set through the binding layer.
- **Method dispatch**: Verify that script methods (`OnUpdate`, `OnStart`, `OnDestroy`) are actually invoked by the engine at the right lifecycle points.
- **Type marshalling**: Verify `glm::vec3` ↔ `Vector3`, `glm::vec4` ↔ `Vector4`, etc.

### Lua Scripting (sol2)

- **Component accessor coverage**: Every `sol::property` binding should be tested with a get and set.
- **Live-update bindings**: Properties that call `SetVolume()`, `SetPitch()`, etc. on the underlying object (not just the config struct) need verification.
- **Entity method dispatch**: `entity:GetComponent("TransformComponent")`, `entity:HasComponent(...)`, etc.
- **Error handling**: What happens when a Lua script accesses a nil component, calls a method on a destroyed entity, etc.

### Cross-Cutting

- **Naming convention enforcement**: Automated check that C# internal call names match the `OLO_ADD_INTERNAL_CALL` registrations. This would have caught Bug #2 at CI time.
- **Scripting ↔ Audio integration**: Posting audio events from scripts, verifying spatial position propagation.

## Lua Binding Completion Status

The Lua bindings are **not yet at parity with C#**. The following components/APIs are available in C# but missing from Lua:

### Missing Lua Bindings (vs C# parity)

- `Rigidbody2DComponent` — all physics properties and methods
- `BoxCollider2DComponent` — offset, size, density, friction, restitution
- `CircleCollider2DComponent` — offset, radius, density, friction, restitution
- `CameraComponent` — projection type, size, FOV, clips
- `SpriteRendererComponent` — color, tiling factor, texture
- `CircleRendererComponent` — color, thickness, fade
- `TextComponent` — text string, color, kerning, line spacing
- `MeshComponent` — mesh handle
- `AnimationComponent` — play, pause, state machine control
- `ScriptComponent` — fields access from Lua

### Partially Present

- `TransformComponent` — position/rotation/scale are bound, but no `GetLocalTransform()` method
- `AudioSourceComponent` — **newly added** (volume, pitch, playOnAwake, looping, spatialization, Play/Stop/Pause/UnPause)
- `AudioListenerComponent` — **newly added** (active flag only)

### AudioEvents API in Lua

The `AudioPlayback` table is bound with `PostTrigger`, `PostTriggerByName`, `StopEvent`, `PauseEvent`, `ResumeEvent`, `StopAll`, `IsEventActive`. This is at parity with C#.

## Recommended Approach

1. **Static analysis test** (immediate, high-value): Parse `InternalCalls.cs` and `ScriptGlue.cpp` to verify all names match. No Mono runtime needed.
2. **Lua unit tests** (medium effort): Create a test that initializes a sol2 state, registers bindings via `LuaScriptGlue::Register(lua)`, creates entities in a Scene, and verifies property access. No rendering required.
3. **Full integration tests** (higher effort): Spin up a Mono domain, load ScriptCore assembly, instantiate a test script class, and verify component property round-trips. Requires Mono to be available in the test environment.
