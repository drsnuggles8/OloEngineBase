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

While in that code, the same load path was found to be **missing the engine's
mandatory `std::isfinite` validation** ([cpp-coding-quality §2](agent-rules/cpp-coding-quality.md)):
every other deserializer sanitizes floats via `SanitizeFloat`, but the instance
Transform/Color/Custom reads did not — so a NaN/Inf in a corrupt or hand-edited
scene would flow straight into the instance SSBO (and through
`transpose(inverse(Transform))`). Added a reusable `Math::IsFinite(vec2/3/4, mat4,
scalar)` helper and wired it into both instance read paths (non-finite transform →
identity, color → white, custom → 0). Covered by `MathIsFiniteTest` and
`ComponentRoundTrip.InstancedMeshComponentNonFiniteInstanceDataIsSanitizedOnLoad`.

### `cpp:S2259` — "forming reference to null pointer" — confirmed false positive

`SaveGameComponentSerializer.cpp:1786` (`ItemInstance copy = *eqItem;`) is guarded
by `present = (eqItem != nullptr)` on the line above; the deref only runs inside
`if (present)`. Sonar doesn't correlate the `bool` with the pointer. **Action:**
mark as *Won't Fix / False Positive* in the SonarCloud UI (a one-line, value-free
deref guard isn't worth an inline `// NOSONAR` in save-game code). Left untouched.

---

## 7. `cpp:S1244` — "floating-point numbers should not be tested for equality"

**Hits triaged:** ~18 (RELIABILITY). Split between intentional exact-comparison and one genuine fix.

This rule restates the project's own [cpp-coding-quality §2](agent-rules/cpp-coding-quality.md) (no `==`/`!=` on floats). Most hits, though, are the *bit-exact* case §2a explicitly carves out — change detection, where any difference (including a one-ULP edit) must register:

- **`Scene/Components.h` (9 hits)** — each is a `operator==(const T&) const -> bool = default;`, the documented undo/redo change-detection hook (CLAUDE.md "Editor undo/redo for components"). Defaulted member-wise float equality *is* the intended bit-exact semantics; hand-rolling epsilon comparisons would defeat the `= default` pattern. **Scoped out** in `sonar-project.properties` (`cmp_s1244_components`), narrowed to `Components.h` so real float `==` bugs elsewhere in `Scene/**` stay flagged.
- **`Renderer/Commands/RenderCommand.h:155` (7 hits)** — `PODMaterialData::operator==`, a deliberate field-wise exact comparison for render-command change detection / dedup (it intentionally avoids `memcmp` due to padding — see the comment). Exact equality is correct; epsilon would wrongly merge distinct materials. **Scoped out** (`cmp_s1244_rendercmd`).
- **`OloEditor/.../SoundGraphEditorPanel.cpp:961,965` — fixed in code.** A mouse-wheel "did it move" sentinel and a post-clamp zoom change-detection, both written as raw `!= ` on floats. Replaced with `Math::BitwiseEqual` (the §2a-blessed bit-exact form) — same behavior, rule satisfied, no suppression needed.

---

## 8. Reliability cleanup batch — `S867` / `S853` / `S2193` (fixed) and `S3584` / `S6232` (triaged)

A sweep of the remaining `RELIABILITY` findings (excluding the intentional `S8417`
atomics and the `S5000`/`S3519` analyzer FPs) surfaced a set of small, genuine,
mechanical fixes — all applied in code:

- **`cpp:S867`** ("operand should have type `bool`") — implicit conversions in `&&`/`!`.
  Fixed to explicit comparisons: `GetExitCodeThread(...) != 0` ([RunnableThread.cpp](../OloEngine/src/OloEngine/HAL/RunnableThread.cpp), Windows `BOOL` is `int`), `ref == 0` ([NavMeshQuery.cpp](../OloEngine/src/OloEngine/Navigation/NavMeshQuery.cpp), `dtPolyRef` is an integer handle), `(mask & bit) != 0u` ([JoltCharacterController.cpp](../OloEngine/src/OloEngine/Physics3D/JoltCharacterController.cpp)), and `source != nullptr && *source != '\0'` ([RendererProfiler.cpp](../OloEngine/src/OloEngine/Renderer/Debug/RendererProfiler.cpp), a `const char*`).
- **`cpp:S853`** ("explicit cast on the result of `~`") — the `AssetFlag` bit ops in
  [AssetTypes.h](../OloEngine/src/OloEngine/Asset/AssetTypes.h) / [Asset.h](../OloEngine/src/OloEngine/Asset/Asset.h): `~` integer-promotes its `u16` operand to `int`, so the high bits it sets were being narrowed implicitly. Added an explicit cast back to the underlying type.
- **`cpp:S2193`** ("float loop counter") — the SoundGraph editor grid-line loops accumulated `x += gridStep` in a `f32` counter, drifting over many lines. Rewrote with an `int` index and `pos = start + i*step`.

Triaged as **not-a-fix** (left in place, reasoning recorded):

- **`cpp:S3584`** ("potential leak of `New`", `ClosableMpscQueue.h`) — **false positive**. On the success path `New` is published to the queue via `Prev->Next.store(New)` (ownership transfers to the queue, freed at `Close()`); only the closed-queue path owns and deletes it. Adding a `delete` would double-free. Annotated with an inline `// NOSONAR cpp:S3584` + ownership comment.
- **`cpp:S6232` / `cpp:M23_360`** ("type-punning via union → use `std::bit_cast`", `GenericPlatformMemory.cpp` `Memswap`) — left untouched. It's the Unreal-ported aligned word-swap hot path where the pointer union is read for alignment *and* advanced for the swap; a `bit_cast` rewrite is a non-trivial restructure of intentional, perf-critical code with no functional defect. Better handled deliberately, not as a drive-by.

---

## 9. Analysis-setup hygiene — `sonar.tests` / coverage / Python version (#411, slice 1)

These are **analysis-pipeline configuration** fixes, not rule tuning — they change how
SonarCloud is *told to analyse* the project, not which rules fire. Applied in
`sonar-project.properties`:

- **`sonar.tests=OloEngine/tests`** (#411 item 2) — silences the warning that files under
  `OloEngine/tests` "look like test code but `sonar.tests` is not configured; rules targeting
  production code were not executed on these files." Declaring the test root makes the
  classification explicit.
- **`sonar.exclusions=OloEngine/tests/**/*`** — **mandatory companion** to the above.
  `sonar.sources` defaults to `.` (the whole tree), so without this carve-out the test files
  match **both** the source set and the test set and the scan **fails hard** with
  `File ... can't be indexed twice`. The exclusion removes them from the production-source set,
  leaving them indexed only as tests. (This is the documented SonarQube remedy for a
  `sonar.sources=.` + `sonar.tests` overlap — keep them disjoint.)
- **`sonar.coverage.exclusions=OloEngine/tests/**/*`** (#411 item 4) — test files must never
  count toward production-code coverage. No coverage report is imported in CI yet (see the
  0%-coverage note under Open items), so this is a no-op today — kept as correct hygiene.
- **`sonar.python.version=3.10`** — kills the "analysed as compatible with all Python 3
  versions" warning. All repo Python lives under `OloEngine/tests`; CI installs 3.10.

Note: SCM-ignored paths (`build/`, `OloEngine/vendor/*`, the generated `compile_commands.json`)
are dropped from indexing automatically by SonarQube's default SCM-exclusion, so no
`**/vendor/**` / `**/build/**` `sonar.exclusions` were needed.

### Deferred: build-wrapper → `sonar.cfamily.compile-commands` migration (#411 item B) — **do NOT do the naive swap**

The scan warns that `sonar.cfamily.build-wrapper-output` is deprecated in favour of
`sonar.cfamily.compile-commands`. **The naive migration is broken** and was deliberately *not*
shipped in this slice:

- `SonarCloud.yml`'s build step runs `cmake -S . -B build` with **no `-G`** → the default
  **Visual Studio (MSBuild) generator**.
- **`CMAKE_EXPORT_COMPILE_COMMANDS` is implemented only by the Makefile and Ninja generators;
  the Visual Studio generator silently ignores it** (confirmed against the latest CMake docs:
  *"This option is implemented only by Makefile Generators and Ninja Generators. It is ignored
  on other generators."*). So the `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` already passed in the
  workflow is a **no-op today — `build/compile_commands.json` is never produced.**
- Pointing `sonar.cfamily.compile-commands` at that non-existent file → the C-family analyzer
  sees **zero translation units** → the C++ issue count **craters**. That is exactly the
  "near-empty C++ analysis" outcome the migration is meant to avoid.

A *correct* migration requires switching the SonarCloud build to **Ninja** (install Ninja + set
up the MSVC dev-environment so `cl.exe` is on PATH, as `Windows.yml` already does) and
configuring with `-G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. That is a
substantial, fragile rewrite of a ~2 h CI job that cannot be validated locally and must be
proven via a `workflow_dispatch` run before merge. Until then, **keep the working `build-wrapper`
path** — the deprecation is only a warning and the current analysis is correct. This remains an
open part of #411.

---

## High-volume rules — deactivated in the C++ Extended profile ✅

A full-corpus facet query (≈31,800 open issues) surfaced four MISRA / stylistic rules fighting idiomatic modern C++ that dwarfed every other finding (≈6,800 issues between them — about half the raw count). All four are now **deactivated** in the C++ Extended Quality Profile (SonarCloud UI, 2026-06-27), so they no longer fire:

| rule | was (count) | why it was a poor fit |
|---|---|---|
| `cpp:S5536` | "remove unused functions" (2,981) | An engine is a *library codebase* (the rule's own exemption): public API is invoked by games, scripts, reflection, serialization and tests the analyzer can't see, so it flagged live API as dead. **`cpp:S1144`** (unused *private* members) is kept **active** — those are genuinely dead code. |
| `cpp:S1271` | "use `::` to access globals" (2,004) | Prefixing every free-function/global access with `::` across a 312k-LOC engine is enormous churn for negligible readability gain. |
| `cpp:S1712` | "no default parameters" (1,017) | Default arguments are idiomatic, well-understood C++; the rule wants hand-written overload chains instead. |
| `cpp:S909` | "no `continue`", MISRA C:2004 14.5 (797) | `continue` is a normal, often *more* readable control-flow tool. |

If any are wanted for a *specific* safety-critical subsystem later, scope them narrowly there rather than re-enabling project-wide.

---

## What we are NOT recommending

For completeness, some rules that *look* noisy but are worth keeping at their current threshold:

- **`cpp:S126`** ("End if/else-if construct with else clause") — 7 hits. Defensive coding rule that catches real bugs when a new branch is added later. Cheap fix per site.
- **`cpp:S131`** ("Add default case to switch") — 3 hits. Same rationale.
- **`cpp:S859`** ("`const_cast` removing const may cause UB") — 1 hit but the *exact* kind of bug Sonar should catch. Fix the issue, don't silence the rule.
- **`cpp:M23_235`** ("Capture lambda variables explicitly") — 4 hits, real dangling-reference bug class. Fix the issues.

---

## Open items

- Full-corpus histogram (≈31,800 open issues) obtained via the SonarCloud `facets=rules` API. The four high-volume rules above (≈6,800 issues, ~half the raw count) are now **deactivated** in the C++ Extended profile — done in the SonarCloud UI (not as a `**/*` properties scope), where project-wide deactivation stays reviewable.
- `cpp:S6004` (if/switch init-statement, 835 hits) was swept and fixed in bulk — it aligns with the project's own [coding standard §1](agent-rules/cpp-coding-quality.md) rather than being a false positive.
- ✅ The recommended path-scopes (`fp_s5000` / `ecs_s1067` / `xmacro_s963`, plus the pre-existing `tst_*`) are now applied in `sonar-project.properties`. Re-run the quality-gate check to confirm the false-positive concentration in `Math.h`, `Scene/**`, `Prefab.cpp`, and `tests/**` has dropped.
- Coverage is reported as **0%** in SonarCloud despite an extensive GoogleTest suite. The CI scan isn't picking up coverage XML — separate problem from rule tuning, but worth fixing for the maintainability dashboard to make sense.
