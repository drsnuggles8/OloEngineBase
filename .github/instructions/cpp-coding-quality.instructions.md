---
description: "C++ coding quality rules enforced by SonarQube and project convention. Apply when writing, editing, or reviewing C++ code. Covers init-statements, floating-point comparison, type deduction, loop optimization, include hygiene, and defensive patterns."
applyTo: "**/*.cpp, **/*.h, **/*.hpp"
---

# C++ Coding Quality Rules

These rules prevent common SonarQube findings and match the project's C++20/23 style.

---

## 1. If / Switch Init-Statements

When a variable is **only used inside** an `if` or `switch` block, declare it in the init-statement to limit its scope.

```cpp
// BAD — variable leaks into outer scope
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

**Exception:** When the variable is read again *after* the `if` (e.g., to check a final value), keep it in the outer scope.

---

## 2. Floating-Point Comparison

### 2a. Never use `==` / `!=` directly on floats or float-containing types

SonarQube flags `operator!=` and `operator==` on `float`, `double`, `glm::vec*`, `glm::mat*`.

- **For tolerance-based comparison** (physics, rendering thresholds): use an epsilon.

```cpp
// BAD
if (distance != 0.0f) { ... }

// GOOD
constexpr f32 epsilon = 1e-6f;
if (std::abs(distance) > epsilon) { ... }
```

- **For bitwise-exact comparison** (undo/redo change detection, serialization round-trip): use `std::memcmp`.

```cpp
// BAD — SonarQube flags this
if (a.GetBaseColorFactor() != b.GetBaseColorFactor()) return false;

// GOOD — explicit bitwise, documents intent
auto lhs = a.GetBaseColorFactor();
auto rhs = b.GetBaseColorFactor();
if (std::memcmp(&lhs, &rhs, sizeof(lhs)) != 0) return false;
```

### 2b. Validate deserialized / external floats

Any `float` read from YAML, JSON, network, or user input must be checked before use:

```cpp
f32 value = node.as<f32>();
if (!std::isfinite(value) || value <= 0.0f)
    value = defaultValue;
```

---

## 3. Use `auto` to Avoid Redundant Type Names

When the type is obvious from the right-hand side (constructor, `static_cast`, factory), prefer `auto`.

```cpp
// BAD — type repeated
sizet expectedSize = static_cast<sizet>(width) * height;
u8 maxIdx = static_cast<u8>(std::min<sizet>(count - 1, 255));

// GOOD
auto expectedSize = static_cast<sizet>(width) * height;
auto maxIdx = static_cast<u8>(std::min<sizet>(count - 1, 255));
```

**Do NOT use `auto`** when it hides a non-obvious type (e.g., `auto x = GetValue();` where the return type matters for understanding the code).

---

## 4. Cache Loop Stop Conditions

If a loop bound calls a method (`.size()`, `.Num()`, etc.) and the container is **not modified** during iteration, hoist it:

```cpp
// BAD — size() called every iteration (SonarQube: "calculate the stop condition outside the loop")
for (sizet i = 0; i < container.Materials.size(); ++i) { ... }

// GOOD
auto materialCount = container.Materials.size();
for (sizet i = 0; i < materialCount; ++i) { ... }
```

When the container **is** modified in the loop (erase, push_back), keep the call inline.

---

## 5. Include What You Use

Do not rely on transitive includes for standard library headers. If a function or type is used, include its header explicitly.

Common ones to watch for:

| Used symbol | Required header |
|---|---|
| `std::clamp`, `std::min`, `std::max` | `<algorithm>` |
| `std::memcpy`, `std::memcmp`, `std::memset` | `<cstring>` |
| `std::isfinite`, `std::abs` (float) | `<cmath>` |
| `std::numeric_limits` | `<limits>` |
| `std::string` | `<string>` |
| `std::vector` | `<vector>` |
| `std::function` | `<functional>` |

The project PCH (`OloEnginePCH.h`) provides many of these, but **headers included by other targets or public headers** must be self-contained.

---

## 6. Bounds-Check Before Raw Memory Operations

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

## 7. Defaulted `operator==` (MSVC)

For non-trivially-copyable components that need undo tracking, provide `operator==`. Use the trailing return type to avoid MSVC rejection:

```cpp
// GOOD — works on MSVC
auto operator==(const MyComponent&) const -> bool = default;
```

When the component contains types without `operator==` (e.g., `Material`), write a manual implementation using `std::memcmp` for float members (see rule 2b).

---

## 8. Prefer Structured Bindings and Range-For

```cpp
// BAD
auto it = map.find(key);
if (it != map.end()) {
    auto& name = it->second.Name;
    ...
}

// GOOD
if (auto it = map.find(key); it != map.end()) {
    auto& [id, entry] = *it;
    ...
}
```

## 9. UI Widget Budget

When creating ImGui editor panels for grid/array data, **never allocate a widget per cell** for potentially large grids. Use `ImGuiListClipper` for row virtualization and compute a visible column range from `ImGui::GetContentRegionAvail().x` divided by per-widget width. Add paging/scrolling controls when the data exceeds the visible area.
