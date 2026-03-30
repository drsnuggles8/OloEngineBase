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

- **InternalCall name consistency**: Verify that every `[MethodImpl(MethodImplOptions.InternalCall)]` declaration in `InternalCalls.cs` has a matching `OLO_ADD_INTERNAL_CALL(Name)` registration in `ScriptGlue.cpp`. Implement as a Python script (`scripts/check_internal_calls.py`) that parses both files with regex — extracts method names from `InternalCalls.cs` and registration names from `ScriptGlue.cpp`, then fails with a diff if the sets don't match. Wire into CI as a lint step alongside pre-commit. Tests: `tests/ScriptBindings/`.
- **Component property round-trips**: For each component property exposed to C# (e.g., `TransformComponent.Translation`, `AudioSourceComponent.Volume`), verify get/set through the binding layer. Use GoogleTest with a Mono domain fixture. Tests: `tests/ScriptBindings/`.
- **Method dispatch**: Verify that script methods (`OnUpdate`, `OnStart`, `OnDestroy`) are actually invoked by the engine at the right lifecycle points.
- **Type marshalling**: Verify `glm::vec3` ↔ `Vector3`, `glm::vec4` ↔ `Vector4`, etc. Use GoogleTest. Tests: `tests/ScriptBindings/`.

### Lua Scripting (sol2)

- **Component accessor coverage**: Every `sol::property` binding should be tested with a get and set. Use GoogleTest with a sol2 state fixture (no rendering needed). Tests: `tests/Lua/`.
- **Live-update bindings**: Properties that call `SetVolume()`, `SetPitch()`, etc. on the underlying object (not just the config struct) need verification.
- **Entity method dispatch**: `entity:GetComponent("TransformComponent")`, `entity:HasComponent(...)`, etc.
- **Error handling**: What happens when a Lua script accesses a nil component, calls a method on a destroyed entity, etc.

### Cross-Cutting

- **Naming convention enforcement**: Automated check via `scripts/check_internal_calls.py` — a Python static analyzer that extracts `MethodImpl InternalCall` names from `OloEngine-ScriptCore/src/OloEngine/InternalCalls.cs` and `OLO_ADD_INTERNAL_CALL(Name)` registrations from `OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp`, then asserts set equality. Run in CI lint/test job. This would have caught Bug #2 at CI time.
- **Scripting ↔ Audio integration**: Posting audio events from scripts, verifying spatial position propagation.

### Test Infrastructure

- **C++ binding tests**: GoogleTest (already in use). Place C# binding tests in `tests/ScriptBindings/`, Lua binding tests in `tests/Lua/`.
- **Lua harness**: Create a GoogleTest fixture that initializes a `sol::state`, registers bindings via the same code path as `LuaScriptGlue`, creates a Scene with entities, and verifies property round-trips. No rendering required.
- **C# harness**: Create a GoogleTest fixture that initializes a Mono domain, loads the ScriptCore assembly, and invokes managed methods. Requires `mono` to be available in the test environment (already a build dependency).
- **CI integration**: Add `scripts/check_internal_calls.py` to the CI lint step. Add `tests/ScriptBindings/` and `tests/Lua/` test executables to the existing test build/run stage (`build-tests-debug` / `run-tests-debug`). Report failures alongside existing GoogleTest results.

## Lua Binding Completion Status

The Lua bindings are **not yet at parity with C#**. The following components/APIs are available in C# but missing from Lua:

### Missing Lua Bindings (vs C# parity)

- **P0** `Rigidbody2DComponent` — all physics properties and methods (critical for any 2D gameplay scripts)
- **P0** `BoxCollider2DComponent` — offset, size, density, friction, restitution (required alongside Rigidbody2D)
- **P0** `CircleCollider2DComponent` — offset, radius, density, friction, restitution (required alongside Rigidbody2D)
- **P0** `CameraComponent` — projection type, size, FOV, clips (scripts frequently adjust cameras)
- **P1** `SpriteRendererComponent` — color, tiling factor, texture (high-use for visual feedback in scripts)
- **P1** `TextComponent` — text string, color, kerning, line spacing (UI/HUD scripts need text manipulation)
- **P2** `MeshComponent` — mesh handle (3D scripts need mesh swapping)
- **P2** `AnimationComponent` — play, pause, state machine control (gameplay animation triggers)
- **P3** `CircleRendererComponent` — color, thickness, fade (niche, rarely scripted directly)
- **P3** `ScriptComponent` — fields access from Lua (cross-script communication, lower priority)

### Partially Present

- `TransformComponent` — position/rotation/scale are bound, but no `GetLocalTransform()` method
- **P0** `AudioSourceComponent` — **newly added** (volume, pitch, playOnAwake, looping, spatialization, Play/Stop/Pause/UnPause, useEventSystem, startEvent). Now event-system aware.
- **P1** `AudioListenerComponent` — **newly added** (active flag only; config properties like cone angles not yet exposed)

### AudioEvents API in Lua

The `AudioPlayback` table is bound with `PostTrigger`, `PostTriggerByName`, `StopEvent`, `PauseEvent`, `ResumeEvent`, `StopAll`, `IsEventActive`. This is at parity with C#.

## Recommended Approach

1. **Static analysis test** (immediate, high-value): Implement `scripts/check_internal_calls.py` — parses `OloEngine-ScriptCore/src/OloEngine/InternalCalls.cs` for `MethodImpl InternalCall` method names and `OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp` for `OLO_ADD_INTERNAL_CALL(Name)` registrations. Fails CI if sets differ. Wire into pre-commit or CI lint job.
2. **Lua unit tests** (medium effort): Create `tests/Lua/LuaBindingTest.cpp` using GoogleTest. Fixture initializes a `sol::state`, registers bindings via the same code path as `LuaScriptGlue`, creates a Scene with entities, and verifies property round-trips. No rendering required. Add to `CMakeLists.txt` alongside existing test targets.
3. **Full integration tests** (higher effort): Create `tests/ScriptBindings/MonoBindingTest.cpp` using GoogleTest. Fixture initializes a Mono domain, loads the ScriptCore assembly, instantiates a test script class, and verifies component property round-trips. Requires Mono to be available in the test environment (already a build dependency). Add to CI test matrix.
