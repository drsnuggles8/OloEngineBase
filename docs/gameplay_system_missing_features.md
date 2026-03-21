# Gameplay System — Missing / Untested Features

Features that were planned or partially implemented but not yet wired up in the test scene.

---

## 1. Click-to-Move (Point & Click Pathfinding)

**Goal:** Diablo / Warcraft 3 style movement — left-click on the ground, player walks there via NavMesh pathfinding.

**What's needed:**
- **Screen-to-world raycast** exposed to C# scripting. The engine has `Physics::Raycast` on the C++ side but no C# binding for it. Need an internal call like:
  ```csharp
  // In Input or Physics class
  static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out Vector3 hitPoint);
  ```
- **Mouse world position helper** — convenience method that takes screen coords (from `Input.GetMousePosition()`) and the camera's view-projection matrix to produce a ray.
- **PlayerController update** — on left-click, raycast against ground, set `NavAgent.TargetPosition = hitPoint`.

**Blocked by:** No `Physics.Raycast` or `Camera.ScreenToWorldRay` C# binding exists yet.

---

## 2. NavMesh Baking at Runtime Start

**Goal:** NPCs use `NavAgentComponent` with proper pathfinding instead of direct translation movement.

**Current state:**
- `NavigationSystem::OnUpdate` correctly moves entities along NavMesh paths every frame.
- `NavMeshBoundsComponent` defines the bake area.
- NavMesh baking works from the editor's NavMeshPanel ("Bake NavMesh" button).
- **Problem:** `Scene::OnRuntimeStart()` does NOT auto-bake or load a NavMesh. If you haven't manually baked in the editor before hitting Play, `m_NavMesh` is null and all NavAgent pathfinding silently does nothing.

**What's needed:**
- Auto-bake NavMesh in `Scene::OnRuntimeStart()` if a `NavMeshBoundsComponent` exists and no NavMesh is loaded, OR
- Serialize the baked NavMesh to disk and reload it on Play, OR
- At minimum, log a warning when NavAgentComponents exist but no NavMesh is available.

**Workaround:** Manually click "Bake NavMesh" in the editor panel before entering Play mode. The NPC scripts (`GoblinAI.cs`, `FireMageAI.cs`) currently use direct translation to avoid this issue.

---

## 3. NavAgent-Driven NPC Movement

**Goal:** NPCs chase/kite using proper NavMesh pathfinding with obstacle avoidance.

**Current state:** GoblinAI and FireMageAI move via direct `TransformComponent.Translation` manipulation. This works but ignores obstacles and has no path smoothing.

**What's needed (once NavMesh baking works):**
- Switch NPC scripts back to `NavAgent.TargetPosition = playerPos` instead of direct translation.
- The `CrowdManager` (Detour Crowd) handles avoidance between multiple agents — already integrated in C++ but needs the NavMesh to be present.
- Test with arena obstacles (pillars) to verify agents path around them.

---

## 4. One-Shot Keyboard Input API

**Goal:** `Input.IsKeyJustPressed(KeyCode)` for single-fire key detection.

**Current state:** `Input.IsKeyDown()` maps to GLFW's `glfwGetKey()` which is continuous (true every frame while held). The test scripts manually track previous-frame state for one-shot behavior.

**What exists already:**
- `Input.IsActionJustPressed(string actionName)` — works for action-mapped input (requires registering action names).
- `Input.IsGamepadButtonJustPressed()` — works for gamepad.
- No raw keyboard one-shot equivalent.

**What's needed:**
- Add `Input_IsKeyJustPressed` / `Input_IsKeyJustReleased` internal calls that track per-key previous-frame state in C++.
- Expose as `Input.IsKeyJustPressed(KeyCode)` in C#.

---

## 5. Console.WriteLine → Editor Console

**Status: PARTIALLY FIXED.**

- `Debug.Log()` / `Debug.LogWarning()` / `Debug.LogError()` now route through spdlog's client logger → editor ConsolePanel.
- `Console.WriteLine()` still goes to system stdout only (not captured by editor). This is because Mono's stdout is not redirected.

**Optional improvement:**
- Call `mono_trace_set_print_handler()` or `mono_set_print_callback()` in `ScriptEngine::InitMono()` to redirect Mono's stdout/stderr into spdlog. Then even `Console.WriteLine` would appear in the editor console.

---

## 6. Damage Application Between Entities

**Goal:** When the Goblin uses Bite or the Fire Mage uses Fire Bolt, the target entity actually takes damage.

**Current state:** `TryActivateAbility` activates the ability on the *caster's own* AbilityComponent. Effects (like BurnDoT) are applied to the caster, not to a target entity. There is no target selection or damage routing yet.

**What's needed:**
- A `TryActivateAbilityOnTarget(string abilityTag, ulong targetEntityID)` C# binding that applies the ability's effects to the target entity's AbilityComponent instead of the caster's.
- Alternatively, a `DamageEvent` dispatch system: caster creates a DamageEvent, damage calculation resolves it (armor, resistance, crit), and the result is applied to the target's Health attribute.
- The C++ `DamageCalculation` and `DamageEvent` classes already exist in the GAS implementation — they just need C# bindings and script-level wiring.

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
