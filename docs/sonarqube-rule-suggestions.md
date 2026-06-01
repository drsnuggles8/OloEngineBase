# SonarCloud Rule Tuning Suggestions

Recommendations for rules that fire frequently in this codebase but produce **false positives** or **low-signal noise** in a real-time C++ game-engine context. For each rule we include: what it's meant to catch, why it doesn't apply (or doesn't apply well) here, and a concrete action.

> The aim is to keep Sonar's signal-to-noise ratio high so genuine findings don't drown in stylistic flags. None of these rules are *wrong* — they're just poor fits for specific patterns in this codebase.

Project: `drsnuggles8_OloEngineBase` (SonarCloud)
Source of truth for issue analysis: a sample of the first 100 of ~7,462 open HIGH+BLOCKER findings, pulled via the SonarQube MCP server.

---

## How to apply these suggestions

Two channels, in order of preference:

1. **`sonar-project.properties`** (this repo). Path-scoped exclusions go here so the rule is version-controlled and reviewable in PRs. Example pattern:

   ```properties
   sonar.issue.ignore.multicriteria=e1,e2
   sonar.issue.ignore.multicriteria.e1.ruleKey=cpp:S5443
   sonar.issue.ignore.multicriteria.e1.resourceKey=OloEngine/tests/**/*
   sonar.issue.ignore.multicriteria.e2.ruleKey=cpp:S5000
   sonar.issue.ignore.multicriteria.e2.resourceKey=OloEngine/src/OloEngine/Math/Math.h
   ```

2. **SonarCloud UI** — *Administration → Quality Profiles → (your C++ profile) → Activate/Deactivate Rule*. Use this only when a rule is genuinely unhelpful project-wide, since the change isn't visible in PR review.

For one-off suppressions on a specific line, prefer an inline comment with rationale:

```cpp
// NOSONAR cpp:S5000 — bitwise-equality check guarded by `constexpr if (std::is_trivially_copyable_v<T>)`.
```

Always include the rule key and a one-line reason. Bare `// NOSONAR` ages badly.

---

## 1. `cpp:S5000` — "Use `operator==` to check equality of non-trivially-copyable types"

**Hits in sample:** 21 (all at the same line)
**Location:** `OloEngine/src/OloEngine/Math/Math.h:27`
**Status:** False positive — ✅ **APPLIED** in `sonar-project.properties` (`fp_s5000`)

### What the rule catches

Detects `std::memcmp` (or equivalents) used to compare objects whose type is not trivially copyable. The concern is real: `memcmp` on a type with padding bytes, virtual tables, or non-trivial members reads uninitialised memory and can produce non-deterministic results.

### Why it's a false positive here

The 21 hits are *all* at the single helper `Math::BitwiseEqual`, called from the editor's `DrawComponent<T>` undo/redo machinery documented in [CLAUDE.md](../CLAUDE.md#editor-undoredo-for-components). The pattern is:

```cpp
// In Math/Math.h
template<typename T>
constexpr bool BitwiseEqual(const T& a, const T& b) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, ...);
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
```

The `static_assert` makes misuse a **compile error** — `memcmp` cannot be invoked on a non-trivially-copyable type. Sonar's analyzer cannot see through the `static_assert` + template instantiation chain and reports a finding for every callsite as if the constraint didn't exist.

Furthermore, `DrawComponent<T>` selects between three branches at compile time with `constexpr if`:

```cpp
if constexpr (std::is_trivially_copyable_v<T>)         { /* memcmp path */ }
else if constexpr (std::equality_comparable<T>)        { /* operator== path */ }
else                                                   { /* no undo */ }
```

The memcmp branch is never reached for non-trivial types. Sonar reports both branches.

### Recommended action

Path-scope exclude `OloEngine/src/OloEngine/Math/Math.h`:

```properties
sonar.issue.ignore.multicriteria.s5000.ruleKey=cpp:S5000
sonar.issue.ignore.multicriteria.s5000.resourceKey=OloEngine/src/OloEngine/Math/Math.h
```

Alternative: keep the rule active but add a `// NOSONAR cpp:S5000` line comment with the rationale above directly on the `memcmp` call. This preserves the protection if another file ever calls `memcmp` directly.

---

## 2. `cpp:S5443` — "Avoid publicly writable directories"

**Hits in sample:** 9 (all in `tests/**`)
**Status:** Scope mismatch — ✅ **APPLIED** in `sonar-project.properties` (`tst_s5443`)

### What the rule catches

Code that writes to a world-writable path (`/tmp`, `%TEMP%`, `std::filesystem::temp_directory_path()`) without taking precautions against symlink attacks, TOCTOU races, or other unprivileged-user interference. Genuine concern in production binaries — especially anything that runs with elevated privileges or processes untrusted input.

### Why it's a poor fit in `tests/**`

GoogleTest fixtures legitimately use `std::filesystem::temp_directory_path()` to isolate test artefacts. The fixture creates a unique subdirectory (e.g. via UUID), writes test data, and cleans up in teardown. The attacker model the rule guards against — an unprivileged adversary racing the test process on a multi-user machine — doesn't apply: tests run on CI runners and developer workstations the developer already controls.

Forcing every test fixture to mint a hardened path (`mkdtemp`-style + permissions) adds dozens of lines per test for no defensive value.

### Recommended action

Path-scope exclude `OloEngine/tests/**`:

```properties
sonar.issue.ignore.multicriteria.s5443.ruleKey=cpp:S5443
sonar.issue.ignore.multicriteria.s5443.resourceKey=OloEngine/tests/**/*
```

Keep the rule active on production sources (`OloEngine/src/**`, `OloEditor/src/**`, `OloRuntime/src/**`, `OloServer/src/**`) where the concern is legitimate.

---

## 3. `cpp:S1067` — "Reduce conditional operators in expression (max 3)"

**Hits in sample:** 10 (concentrated in `Scene/Components.h` and `Scene/SceneSerializer.cpp`)
**Status:** Threshold mismatch — ✅ **APPLIED** Option A (scope `Scene/**`) in `sonar-project.properties` (`ecs_s1067`)

### What the rule catches

Boolean expressions with more than three `&&` / `||` operators. Intent: such expressions are hard to read and easy to misread at a glance.

### Why this codebase is different

The ECS layer iterates the `AllComponents` tuple (see [CLAUDE.md cross-binding check](../CLAUDE.md#definition-of-done---before-you-hand-back-to-the-user)) and the serializer dispatches per component type. Both inevitably produce expressions like:

```cpp
if (entity.HasComponent<TransformComponent>() &&
    entity.HasComponent<MeshComponent>() &&
    entity.HasComponent<MaterialComponent>() &&
    !entity.HasComponent<HiddenComponent>())
```

Refactoring these into named intermediate `bool`s typically **harms** readability because each name is just a re-statement of the predicate — there's no useful abstraction to extract. The rule punishes a pattern that's intrinsic to ECS code rather than a code smell.

### Recommended action

Option A — **scope-exclude the ECS files** (preferred, narrow):

```properties
sonar.issue.ignore.multicriteria.s1067.ruleKey=cpp:S1067
sonar.issue.ignore.multicriteria.s1067.resourceKey=OloEngine/src/OloEngine/Scene/**/*
```

Option B — **raise the threshold** in the C++ Quality Profile (SonarCloud UI → Quality Profiles → Edit `cpp:S1067` → set max from 3 to 5). Applies project-wide.

Pick A if other parts of the codebase want the stricter rule. Pick B if you'd rather one consistent threshold everywhere and 5 feels more realistic.

---

## 4. `cpp:S963` — "Each instance of a function-like macro parameter shall be enclosed in parentheses"

**Hits in sample:** 2 (`OloEngine/src/OloEngine/Scene/Prefab.cpp:117` and `:121`)
**Status:** Unfixable as written — ✅ **APPLIED** scope-exclude in `sonar-project.properties` (`xmacro_s963`)

### What the rule catches

MISRA C++ 2008 16-0-6 / MISRA C 2004 19.10. In a `#define FOO(X) ...`, every textual reference to `X` in the body must be wrapped in parentheses to prevent operator-precedence surprises when `X` expands to a complex expression. Genuine concern for C macros doing arithmetic on the parameter.

### Why the X-macro pattern in `Prefab.cpp` cannot satisfy it

The file uses the X-macro / token-paster pattern to apply a registration step across every copyable component type:

```cpp
#define COPY_COMPONENT(CompType, Name)                \
    if (sourceEntity.HasComponent<CompType>())        \
    {                                                 \
        targetEntity.AddOrReplaceComponent<CompType>( \
            sourceEntity.GetComponent<CompType>());   \
    }

#define FOR_EACH_COPYABLE_COMPONENT(MACRO)              \
    MACRO(TransformComponent, "TransformComponent")     \
    MACRO(CameraComponent,    "CameraComponent")        \
    ...
```

The `CompType` parameter appears inside template-argument lists (`<CompType>`). The C++ grammar **forbids** parentheses there — `<(CompType)>` does not parse. There is no syntactic way to satisfy the MISRA rule for a parameter that's only used as a template argument.

The companion `MACRO` parameter of `FOR_EACH_COPYABLE_COMPONENT` is invoked as a function-like macro: `MACRO(Type, Name)`. Parenthesising it as `(MACRO)(Type, Name)` would break the invocation — the rule explicitly carves out `#` and `##` operands but not function-like-macro names, and Sonar's analyzer doesn't recognise the pattern.

### Recommended action

Scope-exclude `Prefab.cpp` (and any other file using the same X-macro pattern over template types):

```properties
sonar.issue.ignore.multicriteria.s963.ruleKey=cpp:S963
sonar.issue.ignore.multicriteria.s963.resourceKey=OloEngine/src/OloEngine/Scene/Prefab.cpp
```

If the pattern spreads, broaden to a directory scope (e.g. `OloEngine/src/OloEngine/Scene/**/*.cpp`) — or accept that the rule has fundamental friction with template metaprogramming and scope it out of the C++ codebase entirely while keeping it active on any C sources.

The non-template-arg parameters (e.g. `Name` used as a string in comparisons) *can* be parenthesised, and we could do so for partial compliance — but with the dominant offence still flagging at the same lines, the partial fix buys no signal. Suppression is the honest answer here.

---

## 5. `cpp:S3776` — "Reduce cognitive complexity"

**Hits in sample:** 17 (renderer state machines, serialiser dispatch, asset managers)
**Status:** Threshold mismatch — raise, don't silence

### What the rule catches

Functions whose "cognitive complexity" (nested conditions, loops, recursion) exceeds a threshold (default: 15 on most profiles, **25 in your current profile** based on the sample messages). Designed to flag functions that are hard to hold in your head.

### Why this codebase hits it legitimately

Renderer pipelines, asset hot-reload, scene deserialisation, and shader compilation are inherently branchy because they deal with multiple resource types, fallback paths, error recovery, and format variants. Compare:

- `EditorAssetManager::LoadAssetFromFile` — dispatches by extension, falls back through a chain, handles re-import, hot-reload tracking, dependency invalidation.
- `RenderGraph::Build` — topo-sorts passes, validates resource lifetimes, handles RMW chains.

Splitting these into helper functions can make the control flow harder to follow because the state being threaded through is non-trivial. The function *is* the abstraction.

### Recommended action

**Do NOT deactivate** — cognitive complexity is genuinely useful as a signal. Instead:

1. **Raise the threshold** to 35 or 40 in the project's Quality Profile. Most legitimate complexity peaks fall under 40; anything above is usually a real refactor candidate.
2. **Triage individually** anything that remains: each such function is a candidate to either split or to keep with a `// NOSONAR cpp:S3776 — <why this complexity is fundamental>` comment.

```properties
# No properties change needed if raising in the UI.
# If preferring per-file silencing for a known-complex hotspot:
sonar.issue.ignore.multicriteria.s3776.ruleKey=cpp:S3776
sonar.issue.ignore.multicriteria.s3776.resourceKey=OloEngine/src/OloEngine/Renderer/RenderGraph/**/*
```

---

## 6. Reliability `BLOCKER`s on glm storage / guarded derefs — triaged

A pass over the open `RELIABILITY` `HIGH`+`BLOCKER` findings (≈1,126) showed the
genuine-bug rate is very low; most are the analyzer failing to model glm's
union-based vector storage or a guard that precedes a dereference. Two patterns
were triaged here:

### `cpp:S3519` — "access of field 'x' at index 1, while it holds only a single float" — **fixed in code**

Fired on the instance-placement serializers where 16 mat4 floats / 4 vec4 floats
were (de)serialized via `(&m[0][0])[i]` / `(&v[0])[i]`. Sonar reads `m[0][0]` as a
single-`float` subobject and flags the loop as out-of-bounds. It's *technically
right by the standard*: pointer arithmetic across sub-object boundaries
(`&m[0][0] + 1` reaching `m[0][1]`) is UB even though glm guarantees contiguous
storage in practice. **This is a fix-don't-silence case.** Replaced with glm's
blessed contiguous accessor `glm::value_ptr()` (same bytes, well-defined) at all
four sites in `InstancePlacementSerializer.cpp` and `Scene/SceneSerializer.cpp`,
and added `ComponentRoundTrip.InstancedMeshComponentInstancesSurviveYAMLRoundTrip`
to pin the previously-uncovered per-instance Transform/Color round-trip.

### `cpp:S2259` — "forming reference to null pointer" — confirmed false positive

`SaveGameComponentSerializer.cpp:1786` (`ItemInstance copy = *eqItem;`) is guarded
by `present = (eqItem != nullptr)` on the line above; the deref only runs inside
`if (present)`. Sonar doesn't correlate the `bool` with the pointer. **Action:**
mark as *Won't Fix / False Positive* in the SonarCloud UI (a one-line, value-free
deref guard isn't worth an inline `// NOSONAR` in save-game code). Left untouched.

---

## High-volume rules to deactivate or scope (full-corpus histogram)

A full-corpus facet query (≈31,800 open issues) surfaced four very high-count rules that are **MISRA / stylistic rules fighting idiomatic modern C++**. These dwarf everything else and are the reason the raw issue count looks alarming. "Fixing" them mechanically would be harmful or pointless; the right move is to deactivate (or tightly scope) them in the C++ Quality Profile.

| rule | count | why it's a poor fit |
|---|---|---|
| `cpp:S5536` | 2,981 | "Remove unused functions." The rule text itself exempts *library codebases* — and an engine **is** one. Vast amounts of public API are invoked by games, scripts, reflection, serialization, and tests, none of which the analyzer sees. "Fixing" = deleting live API. |
| `cpp:S1271` | 2,004 | "Use `::` to access globals." Prefixing every free-function/global access with `::` across a 312k-LOC engine is enormous churn for negligible readability gain. |
| `cpp:S1712` | 1,017 | "No default parameters." Default arguments are idiomatic, well-understood C++; the rule wants you to hand-write overload chains instead. High-risk refactor, negative ergonomics. |
| `cpp:S909` | 797 | "No `continue`." MISRA C:2004 14.5. `continue` is a normal, often *more* readable control-flow tool; restructuring 797 loops risks behavior changes. |

### `cpp:S5536` — "remove unused functions"

The single biggest contributor (2,981). Per its own description: *"Unless you are in a library codebase context, functions that are declared but never executed are dead code."* OloEngine ships a static library (`OloEngine`) consumed by `OloEditor`, `OloRuntime`, `OloServer`, the test binary, C#/Lua scripting bindings, and `OLO_PROPERTY` reflection. The analyzer only sees intra-TU call graphs, so it flags huge swaths of legitimately-public API.

**Action:** deactivate `cpp:S5536` in the Quality Profile. (Its subset `cpp:S1144` — unused *private* members — is safer and could stay active; private members genuinely unused within their own class are real dead code.)

### `cpp:S1271`, `cpp:S1712`, `cpp:S909`

**Action:** deactivate all three in the Quality Profile. They encode MISRA safety-critical-C conventions that don't match this codebase's modern-C++23 style. If any are wanted for a *specific* safety-critical subsystem later, scope them narrowly rather than project-wide.

---

## What we are NOT recommending

For completeness, some rules that *look* noisy but are worth keeping at their current threshold:

- **`cpp:S126`** ("End if/else-if construct with else clause") — 7 hits. Defensive coding rule that catches real bugs when a new branch is added later. Cheap fix per site.
- **`cpp:S131`** ("Add default case to switch") — 3 hits. Same rationale.
- **`cpp:S859`** ("`const_cast` removing const may cause UB") — 1 hit but the *exact* kind of bug Sonar should catch. Fix the issue, don't silence the rule.
- **`cpp:M23_235`** ("Capture lambda variables explicitly") — 4 hits, real dangling-reference bug class. Fix the issues.

---

## Open items

- Full-corpus histogram (≈31,800 open issues) obtained via the SonarCloud `facets=rules` API. The four rules in the "High-volume" section above account for ≈6,800 issues on their own; deactivating them would roughly halve the raw count without touching a line of code.
- `cpp:S6004` (if/switch init-statement, 835 hits) was swept and fixed in bulk — it aligns with the project's own [coding standard §1](agent-rules/cpp-coding-quality.md) rather than being a false positive.
- ✅ The recommended path-scopes (`fp_s5000` / `ecs_s1067` / `xmacro_s963`, plus the pre-existing `tst_*`) are now applied in `sonar-project.properties`. Re-run the quality-gate check to confirm the false-positive concentration in `Math.h`, `Scene/**`, `Prefab.cpp`, and `tests/**` has dropped.
- The four high-volume rules in the section above (`cpp:S5536`, `cpp:S1271`, `cpp:S1712`, `cpp:S909`, ≈6,800 issues) are **not** applied here — they remain a Quality-Profile (UI) action as recommended, since project-wide deactivation is better reviewed there than buried in a `**/*` properties scope. Doing so would roughly halve the raw count.
- Coverage is reported as **0%** in SonarCloud despite an extensive GoogleTest suite. The CI scan isn't picking up coverage XML — separate problem from rule tuning, but worth fixing for the maintainability dashboard to make sense.
