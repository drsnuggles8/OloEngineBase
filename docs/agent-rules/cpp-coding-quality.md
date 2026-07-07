# C++ Coding Quality Rules

Applies to: `**/*.cpp`, `**/*.h`, `**/*.hpp`

Conventions that prevent common SonarQube findings and match the project's C++23 style. Each rule has a concrete example; when reviewing or writing new code, treat the examples as the spec.

---

## 1. If / switch init-statements

When a variable is **only used inside** an `if` or `switch` block, declare it in the init-statement to limit its scope.

```cpp
// BAD — leaks into outer scope
f32 metallic = mat.GetMetallicFactor();
if (ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
{
    mat.SetMetallicFactor(metallic);
}

// GOOD
if (auto metallic = mat.GetMetallicFactor(); ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
{
    mat.SetMetallicFactor(metallic);
}
```

Exception: if the variable is read again *after* the `if`, keep it in the outer scope.

---

## 2. Floating-point comparison

### 2a. Never use `==` / `!=` directly on floats or float-containing types

SonarQube flags `==` / `!=` on `float`, `double`, `glm::vec*`, `glm::mat*`.

- **Tolerance comparison** (physics, rendering thresholds): use an epsilon.

```cpp
// BAD
if (distance != 0.0f) { ... }

// GOOD
constexpr f32 epsilon = 1e-6f;
if (std::abs(distance) > epsilon) { ... }
```

- **Bitwise-exact comparison** (undo/redo change detection, serialization round-trip): use `Math::BitwiseEqual` from `OloEngine/Math/Math.h`. It wraps `std::memcmp` with a `std::is_trivially_copyable_v` static_assert so misuse is a compile error.

```cpp
// BAD
if (a.GetBaseColorFactor() != b.GetBaseColorFactor()) return false;

// BAD — verbose, easy to get sizeof wrong
auto lhs = a.GetBaseColorFactor();
auto rhs = b.GetBaseColorFactor();
if (std::memcmp(&lhs, &rhs, sizeof(lhs)) != 0) return false;

// GOOD — explicit bitwise, documents intent, size derived from the type
if (!Math::BitwiseEqual(a.GetBaseColorFactor(), b.GetBaseColorFactor()))
    return false;
```

For whole-struct bit-exact comparison (e.g. trivially-copyable POD components), call it with `*this` and `other`:

```cpp
auto operator==(const FogVolumeComponent& other) const -> bool
{
    return Math::BitwiseEqual(*this, other);
}
```

### 2b. Validate deserialized / external floats

Any `float` read from YAML, JSON, network, or user input must be checked before use:

```cpp
f32 value = node.as<f32>();
if (!std::isfinite(value) || value <= 0.0f)
    value = defaultValue;
```

This applies at **every** untrusted-float boundary, not just YAML — network deserialization
(`Networking/Replication/ComponentReplicator.cpp`) and environment-variable config
(`Task/Scheduler.cpp`) included. Guard **before** the value reaches arithmetic that casts to
an integer: a non-finite float must be rejected before `static_cast<u32>` / `static_cast<i32>`,
because the cast is undefined behaviour for `NaN`/`±inf`, and a `NaN` silently slips past sign
checks (`x < 0.0f` is `false` for `NaN`). When validating a setter/parser, prefer a pure helper
that returns the sanitized value (or `std::optional` to signal rejection) so it stays unit-testable.

---

## 3. Use `auto` to avoid redundant type names

When the type is obvious from the right-hand side (constructor, `static_cast`, factory), prefer `auto`.

```cpp
// BAD — type repeated
sizet expectedSize = static_cast<sizet>(width) * height;
u8 maxIdx = static_cast<u8>(std::min<sizet>(count - 1, 255));

// GOOD
auto expectedSize = static_cast<sizet>(width) * height;
auto maxIdx = static_cast<u8>(std::min<sizet>(count - 1, 255));
```

Do **not** use `auto` when it hides a non-obvious type (e.g., `auto x = GetValue();` where the return type matters for understanding the code).

---

## 4. Cache loop stop conditions

If a loop bound calls a method (`.size()`, `.Num()`, etc.) and the container is **not modified** during iteration, hoist it:

```cpp
// BAD — size() called every iteration
for (sizet i = 0; i < container.Materials.size(); ++i) { ... }

// GOOD
auto materialCount = container.Materials.size();
for (sizet i = 0; i < materialCount; ++i) { ... }
```

When the container **is** modified in the loop (erase, push_back), keep the call inline.

---

## 5. Include what you use

Do not rely on transitive includes for standard library headers. If a function or type is used, include its header explicitly.

| Symbol | Header |
|---|---|
| `std::clamp`, `std::min`, `std::max` | `<algorithm>` |
| `std::memcpy`, `std::memcmp`, `std::memset` | `<cstring>` |
| `std::isfinite`, `std::abs` (float) | `<cmath>` |
| `std::numeric_limits` | `<limits>` |
| `std::string` | `<string>` |
| `std::vector` | `<vector>` |
| `std::function` | `<functional>` |

The PCH (`OloEnginePCH.h`) covers many of these inside `OloEngine/src/`, but **public headers and any header included from another target** must be self-contained.

---

## 6. Bounds-check before raw memory operations

When using `std::memcpy` or pointer arithmetic on containers, always validate the source range:

```cpp
// BAD — assumes container has at least (row * stride + count) elements
std::memcpy(dst, &vec[row * stride], count);

// GOOD
sizet srcStart = static_cast<sizet>(row) * stride;
sizet available = (srcStart < vec.size()) ? (vec.size() - srcStart) : 0;
sizet bytesToCopy = std::min<sizet>(count, available);
if (bytesToCopy > 0)
    std::memcpy(dst, &vec[srcStart], bytesToCopy);
```

---

## 7. Defaulted `operator==` (MSVC quirk)

For non-trivially-copyable components that need undo tracking, provide `operator==`. Use the **trailing return type** to avoid MSVC rejection:

```cpp
// GOOD — works on MSVC
auto operator==(const MyComponent&) const -> bool = default;
```

Plain `bool operator==(const MyComponent&) const = default;` works too on MSVC, but `auto operator==(...) const = default;` (no trailing return) does **not**.

When the component contains a type without `operator==` (e.g., `Material`), write a manual implementation using `Math::BitwiseEqual` for float members (rule 2).

`UUID` has implicit `operator u64()`. That causes **C2666 ambiguity** with any member `operator==`. In a manual `operator==`, compare UUIDs via `static_cast<u64>()` to disambiguate.

### Mutable caches must be excluded from `operator==` (and routed off the memcmp undo path)

If a component holds a `mutable` runtime-only cache (e.g. `TransformComponent`'s local-matrix
cache from issue #442), do **not** leave `operator==` as `= default`: the defaulted comparison
includes the cache fields, so two components with identical authored data compare **unequal**
when one has a populated cache and the other doesn't — breaking round-trip tests and undo
change-detection. Write a manual `operator==` (trailing-return form) that compares only the
authored fields via `Math::BitwiseEqual`. Additionally, because the cache is `mutable`, a render
pass can repopulate it between the editor undo snapshot and the compare — so the whole-struct
`memcmp` path in `SceneHierarchyPanel::DrawComponent` would see spurious byte diffs. Opt the
component into the value-comparison path with `PreferValueComparison<T> : std::true_type` so undo
uses the cache-agnostic `operator==` instead.

### Prefer input-snapshot validation over a setter dirty-flag when the inputs are public

A dirty flag only works if **every** mutation goes through a setter. `TransformComponent.Translation`
/`.Scale` are public and mutated in-place in hundreds of call sites, so a setter-invalidated flag
would silently miss direct writes. Instead cache the computed result **plus** a bit-copy of the
inputs it was built from, and on read compare the current inputs (`Math::BitwiseEqual`) against the
cached inputs — any mutation path (setter or raw field write) invalidates correctly, with no
call-site changes. Seed a `bool m_CacheValid = false` so the first read always computes.

---

## 8. Prefer structured bindings and range-for

```cpp
// BAD
auto it = map.find(key);
if (it != map.end())
{
    auto& name = it->second.Name;
    ...
}

// GOOD
if (auto it = map.find(key); it != map.end())
{
    auto& [id, entry] = *it;
    ...
}
```

---

## 9. UI widget budget

When creating ImGui editor panels for grid/array data, **never allocate a widget per cell** for potentially large grids. Use `ImGuiListClipper` for row virtualization and derive a visible column range from `ImGui::GetContentRegionAvail().x / per-widget-width`. Add paging or scrolling when the data exceeds the visible area.

---

## 10. Language-version gates: check `_MSVC_LANG`, not bare `__cplusplus` (MSVC quirk)

MSVC reports `__cplusplus` as `199711L` unless `/Zc:__cplusplus` is set — and this repo does **not** set it. A guard like

```cpp
#if __has_include(<stacktrace>) && __cplusplus >= 202302L   // BAD — always false on MSVC here
```

**silently compiles the feature out** on our primary toolchain: no error, no warning, the code just never runs (this hid the GL-error stack-capture path in `OpenGLDebug.cpp` during the #505 hunt, and the same latent gate sits in `AllocationTracker.cpp`). Gate on both:

```cpp
#if __has_include(<stacktrace>) && (__cplusplus >= 202302L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202302L))
```

If a feature-gate ever seems mysteriously inert on Windows, check for a bare `__cplusplus` comparison first.

---

## 11. Raw GL handles published through global state must be reset per frame

A render pass that publishes a raw GL name (`u32` texture/buffer id) into process-wide renderer state (`Renderer3D::Set*TextureID`) **must not rely on its own `Execute()` to clear it** — the render graph can cull the pass entirely, so its "clear at top of Execute" never runs and last frame's (or last *test's*) id survives while the owning framebuffer is deleted by a resize/rebuild. Any consumer that binds the published id then re-binds a dead name: `GL_INVALID_OPERATION "<texture> is not a valid texture name"`, every frame, with visually-correct output (issue #505 — `WaterSurfaceDepthTextureID` / `PlanarReflectionTextureID`, consumed by `ToneMapRenderPass`).

Rule: a raw-id publication is **per-frame state** — reset it in `RenderPipeline::PrepareFrame` (BeginScene) alongside the other per-frame rotations, and let the executing pass re-publish. Holding a `Ref<Texture>` instead sidesteps the dangling-name problem but extends resource lifetime; the per-frame reset is the default. Debugging technique that pins this class of bug in minutes: the GL debug context is synchronous in Debug builds, so `OpenGLMessageCallback` logs a `std::stacktrace` for every ERROR-type message — the offending `glBindTextureUnit` call site is in the log with file:line.

---

## 12. `ParallelFor` result structs: populate identifying fields *before* the early-exit check, not after

A `ParallelFor` "collect results" pattern — a per-task lambda that writes into `results[index]`, guarded by `if (hasError.load()) return;` at the top to stop new work once any task has failed — must assign the result's identifying field (the enum/key the sequential collect loop uses to label a failure, e.g. a `GLenum Stage`) **before** that early-exit check, not after:

```cpp
// BAD — early-exit skips the Stage assignment; results[index].Stage stays
// default-constructed (0) if this task never runs the real work below.
[&](i32 index)
{
    if (hasError.load(std::memory_order_relaxed))
        return;
    const auto& [stage, source] = pairs[index];
    results[index].Stage = stage;   // never reached on early-exit
    ...
};

// GOOD — the identifying field is set unconditionally, so a task that
// never runs its real work still leaves a labelled (if empty) result.
[&](i32 index)
{
    const auto& [stage, source] = pairs[index];
    results[index].Stage = stage;   // always runs
    if (hasError.load(std::memory_order_relaxed))
        return;
    ...
};
```

Why this matters: once one task fails and sets `hasError`, every other in-flight/not-yet-started task takes the early-exit path and its `results[index]` stays default-constructed. If the sequential collect loop then logs *every* failed result (not just the first) — including a task that never ran — it uses that default field value, e.g. `GLShaderStageToString(0)`. A "stage" switch/lookup helper with an `OLO_CORE_ASSERT(false)`/no default case in its fallback branch then crashes on a value that was never a real shader stage, not because the original failure was mishandled, but because a *second*, unrelated result's bookkeeping field was never populated. This exact bug shipped alongside the fix for issue #568: replacing a fail-fast `OLO_CORE_VERIFY(false, ...)` with a "log every failure, don't stop at the first" collect loop made the sequential loop actually reach the second (never-run) result for the first time — previously the immediate crash on the first failure masked it. Any refactor from "crash/return on the first failure" to "collect and report every failure" in a `ParallelFor`-based aggregation needs the same audit: check every field the collect loop reads is populated on **every** path through the per-task lambda, including the early-exit one.
