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
