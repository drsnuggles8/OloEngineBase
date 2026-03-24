# Gameplay System — Missing / Untested Features

Features that were planned or partially implemented but not yet wired up in the test scene.

---

## ~~1. Click-to-Move (Point-and-Click Pathfinding)~~ — **DONE**

**Goal:** Diablo / Warcraft 3 style movement — left-click on the ground, player walks there via NavMesh pathfinding.

**Resolved.** Both `Physics.Raycast` and `Camera.ScreenToWorldRay` are now exposed to C# and Lua scripting. `PlayerController.cs` uses click-to-move via `Camera.ScreenToWorldRay` -> `Physics.Raycast` -> `NavAgent.TargetPosition`.

---

## ~~2. NavMesh Baking at Runtime Start~~ — **DONE**

**Goal:** NPCs use `NavAgentComponent` with proper pathfinding instead of direct translation movement.

**Resolved.** `Scene::OnRuntimeStart()` now auto-bakes the NavMesh when `NavAgentComponent` entities exist but no NavMesh is loaded. It collects bounds from `NavMeshBoundsComponent` entities (defaulting to ±100 if none exist) and calls `NavMeshGenerator::Generate()` with default `NavMeshSettings`. The baked NavMesh is set via `SetNavMesh()` and immediately available for pathfinding.

---

## 3. NavAgent-Driven NPC Movement

**Goal:** NPCs chase/kite using proper NavMesh pathfinding with obstacle avoidance.

**Current state:** GoblinAI and FireMageAI move via direct `TransformComponent.Translation` manipulation. This works but ignores obstacles and has no path smoothing.

**What's needed (once NavMesh baking works):**
- Switch NPC scripts back to `NavAgent.TargetPosition = playerPos` instead of direct translation.
- The `CrowdManager` (Detour Crowd) handles avoidance between multiple agents — already integrated in C++ but needs the NavMesh to be present.
- Test with arena obstacles (pillars) to verify agents path around them.

---

## ~~4. One-Shot Keyboard Input API~~ — **DONE**

**Goal:** `Input.IsKeyJustPressed(KeyCode)` for single-fire key detection.

**Resolved.** Per-key previous/current frame state tracking added to `WindowsInput.cpp` (via `s_CurrentKeys[]` / `s_PreviousKeys[]` arrays, updated each frame by `Input::Update()`). The following APIs are now available:

```csharp
// C# API
bool justPressed = Input.IsKeyJustPressed(KeyCode.E);
bool justReleased = Input.IsKeyJustReleased(KeyCode.E);
```

```lua
-- Lua API
local justPressed = Input.IsKeyJustPressed(KeyCode.E)
local justReleased = Input.IsKeyJustReleased(KeyCode.E)
```

Exposed through `ScriptGlue.cpp` (C# InternalCalls) and `LuaScriptGlue.cpp` (Sol2 bindings).

---

## ~~5. Console.WriteLine → Editor Console~~ — **DONE**

**Resolved.** `mono_trace_set_print_handler()` and `mono_trace_set_printerr_handler()` are now called in `ScriptEngine::InitMono()`, routing Mono's stdout and stderr through `OLO_CLIENT_INFO` / `OLO_CLIENT_ERROR` respectively. Both `Console.WriteLine()` and `Debug.Log()` output now appears in the editor's ConsolePanel (prefixed with `[C#]`).

---

## ~~6. Damage Application Between Entities~~ — **DONE**

**Goal:** When the Goblin uses Bite or the Fire Mage uses Fire Bolt, the target entity actually takes damage.

**Resolved.** Two new C# and Lua bindings enable cross-entity damage:

```csharp
// C# API
AbilityComponent.ApplyDamageToTarget(targetEntityID, baseDamage);        // raw damage
AbilityComponent.TryActivateAbilityOnTarget(abilityTag, targetEntityID); // ability + effects on target
```

```lua
-- Lua API
Damage.ApplyToTarget(casterEntityID, targetEntityID, baseDamage)
Damage.TryActivateAbilityOnTarget(casterEntityID, abilityTag, targetEntityID)
```

The C++ `DamageCalculation` and `DamageEvent` are now wired through `ScriptGlue.cpp` and `LuaScriptGlue.cpp`. NPC scripts (`GoblinAI.cs`, `FireMageAI.cs`) can use these to apply damage/effects to the player or other entities.

---

## 7. Visual Feedback for Abilities

**Goal:** Visible effects when abilities are used (particles, color flash, floating damage numbers).

**Ideas:**
- Flash the caster's `MaterialComponent.AlbedoColor` briefly on ability activation.
- Spawn a particle effect at the target location (engine has `ParticleSystemComponent`).
- Floating damage text using `TextComponent` or UI system.
- Projectile entity for Fireball / Fire Bolt (spawn, move toward target, apply effect on arrival).

---

## 8. Health Bars / UI Overlay

**Goal:** Floating health bars above characters, HUD showing player stats.

**What exists:** The engine has a full UI system (`UICanvasComponent`, `UIProgressBarComponent`, `UITextComponent`, etc.) with C# bindings. These could be used to build:
- World-space health bars parented to characters.
- A HUD canvas with HP/Mana bars, ability cooldown indicators, and buff/debuff icons.

---

## 9. Death / Respawn

**Goal:** When Health reaches 0, the entity "dies" (remove `State.Alive` tag, disable AI, play death animation, maybe despawn after delay).

**What's needed:**
- Health-clamping logic: clamp Health to [0, MaxHealth] after modifier application.
- On Health reaching 0: remove `State.Alive` tag, which already gates AI behavior in the scripts.
- Optional: change material color to gray, disable mesh, or trigger a death animation.
- Respawn timer or permanent death depending on game design.
