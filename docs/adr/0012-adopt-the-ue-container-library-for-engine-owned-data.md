# Adopt the UE container library for engine-owned data — the half-adopted state is the defect, and we close it by growing usage, not by deleting the port

An architecture review on 2 Aug 2026 surveyed `Containers/`, `Templates/`,
`Algo/` and `Memory/` — 37,719 lines ported from UE 5.7/5.8 — and recommended
**deleting** most of it. The measured case for deletion is strong and is
reproduced in full below, because this ADR decides the opposite and the next
reviewer will otherwise reach the same recommendation from the same numbers.

The decision is to **adopt the port for engine-owned data**. The reasoning is
that the numbers below do not describe a library nobody wanted; they describe a
migration that stopped one file in, and the residue of that stall — `FString`
existing solely so `TMap` can hold a string — is the actual defect. Deleting the
port removes the symptom and keeps the condition that produced it. Finishing the
migration removes both.

Nothing in this ADR is implemented. It records the decision, the constraint that
makes it safe, and the order the work has to happen in.

---

## 0. The survey the decision is taken against

Measured on `39b7e250`, over `OloEngine/src`, `OloEditor/src`, `OloRuntime` and
`OloServer`, excluding tests and excluding the four ported directories
themselves.

| Directory | Lines | Commit touches / KLOC, 9 months |
| --- | ---: | ---: |
| `Containers/` | 22,213 | **6.0** — the coldest large module in the engine |
| `Memory/` | 8,390 | 20.8 |
| `Templates/` | 5,848 | 14.0 |
| `Algo/` | 1,268 | — |
| **Total** | **37,719** | |

Adoption, in production code:

| | std | UE | ratio |
| --- | ---: | ---: | ---: |
| `std::string` vs `FString` | 7,401 in 651 files | 36 in 4 files | **206 : 1** |
| `std::vector` vs `TArray` | 2,954 in 610 files | 106 in 25 files | **28 : 1** |
| `std::unordered_map`/`set`/`map` vs `TMap` | 755 | 43 in 6 files | **17 : 1** |

The port has essentially two consumers: `Task/` (the UE scheduler port) and the
renderer's mesh/material path. Around 7,000 lines are unreachable — `LinkedList.h`
(784) and `Queue.h` (341) have zero includers anywhere including tests;
`TSparseSet` (1,805) is compiled into every build but never instantiated, because
[`Set.h:42`](../../OloEngine/src/OloEngine/Containers/Set.h) hardcodes
`OLO_USE_COMPACT_SET_AS_DEFAULT 1` so `TSet` *is* `TCompactSet`; `MemoryView.h`
(300) has a 380-line test and no production caller; `Algo/` (1,268) has no tests
and one transitive call site.

**None of that is disputed.** It is the correct description of a half-finished
migration, and `docs/analysis/dead-code.md` already reached part of it by
include-graph reachability — a method that structurally cannot see the
`TSparseSet` case, since the header *is* reachable and only the instantiation is
absent.

## 1. Why adopt rather than delete

The deletion case treats the ratios as a verdict on the library. They are better
read as a verdict on the *mixture*.

`FString` is the evidence. It is not a string type the engine chose for its own
sake — it exists because `TArray` relocates its elements bitwise, and libstdc++'s
`std::string` points into its own SSO buffer and does not survive that. The
failure is recorded at the only place it was hit,
[`MeshSource.h:48-52`](../../OloEngine/src/OloEngine/Renderer/MeshSource.h):

> `FString`, not `std::string`: `Submesh` lives in a `TArray`, which relocates
> its elements bitwise (see `Containers/String.h`). libstdc++'s `std::string`
> points into its own SSO buffer and does not survive that — it aborted with
> `"free(): invalid pointer"` in `~TArray<Submesh>`.

and pinned by a deliberately disabled reproduction,
`ContainerTest.cpp:338 TEST(TMapRelocation, DISABLED_StdStringKeysCorruptAcrossGrowth)`.

So the engine already contains a container hazard that only exists **at the
boundary between the two libraries**. Deleting the port removes today's four
call sites of it. It does not remove the condition, because the renderer's mesh
path still wants relocatable storage and would re-acquire the problem the moment
someone reintroduces a container with the same requirement. Committing to one
library removes the boundary.

The port is also better suited to this codebase than the ratios suggest. It was
prepared for adoption rather than for isolation:

- [`String.h:5-12`](../../OloEngine/src/OloEngine/Containers/String.h) records
  that the port deliberately followed UE's **`FUtf8String`** instantiation — a
  `char` element type, not `TCHAR`/UTF-16 — *"because OloEngine's existing string
  surface (scene YAML, asset paths, ImGui, Lua/C# bindings) is UTF-8
  `std::string` throughout, so a UTF-8 `FString` minimises conversion friction at
  the boundaries."* Every boundary conversion is a `memcpy`, never a transcode.
- `FString` takes **implicit** construction from `std::string` and
  `std::string_view` and offers only **explicit** conversion out
  (`ToStdString()`, `ToView()`) — the asymmetry that makes a mixed call site
  cheap in the safe direction and deliberate in the unsafe one. `Find`,
  `Contains`, `StartsWith` and `EndsWith` all accept `std::string_view`.
- `TArray` supports range-for, `std::initializer_list`, and `Append` from any
  contiguous container.

What the port offers that `std::` cannot: allocator policies at the type level
(`TInlineAllocator` removes the heap allocation for small-N members entirely),
bitwise relocation on growth, and integration with `FArchive`'s
`operator<<` — which `Containers/` already implements and which `SaveGame/` and
`Networking/` already consume.

## 2. The distinction the decision rests on: engine-owned data vs binding surface

Adoption is **not** engine-wide. The line is drawn at a distinction this codebase
has not previously named:

- **Engine-owned data** — data the engine allocates, owns and iterates for its
  own purposes. This adopts the UE containers.
- **Binding surface** — data whose shape is dictated by something outside the
  engine, or which is crossed by a generated binding. This keeps `std::`.

The binding surface is concretely: everything handed to `entt`, `yaml-cpp`,
`sol2`, Mono, ImGui, Jolt or `spdlog`, and every ECS component *field a
generated binding marshals*. `String.h`'s own header names most of this set
already — "scene YAML, asset paths, ImGui, Lua/C# bindings".

The test is not "is this hot?" but "does something outside the engine dictate
this type?". A `TArray<glm::vec3>` of path corners is engine-owned however cold
it is. A `std::string` that becomes a YAML key is a binding surface however hot.

ECS components straddle the line, and are split by field rather than as a whole:
their `std::vector` fields are engine-owned storage and convert; their
`std::string` fields are crossed by five generated consumers (scene YAML, the
binary sidecar, save-games, the MCP field registry, C#/Lua) and stay `std::`.

## 3. The load-bearing constraint: trivial relocatability, and the hole in today's guard

Every UE container in this port relocates its elements bitwise. A type is safe to
store in one only if it holds no pointer into itself.

A trait for this already exists —
[`TIsTriviallyRelocatable`](../../OloEngine/src/OloEngine/Templates/UnrealTypeTraits.h),
`UnrealTypeTraits.h:703` — and is already asserted by all six containers
(`Array.h:586`, `CompactSet.h:327`, `Deque.h:164`, `SparseArray.h:703`, and
transitively by `Map.h`/`Set.h`). `std::basic_string` is correctly specialised
`false` at `UnrealTypeTraits.h:743`; `FString` is specialised `true` at
`String.h:701`.

**It would not have caught the cases this migration creates.** Two holes:

1. **The default is `Value = true` for every type, and it is not recursive over
   members.** `TArray<std::string>` is caught. `TArray<UIDropdownOption>` — a
   struct *containing* a `std::string` — is silently `true`, because C++ cannot
   introspect members without reflection and the specialisation only fires on the
   direct case.
2. **It is `OLO_STATIC_ASSERT_WARN`**, a `[[deprecated]]`-backed *warning*. In a
   build this size, a deprecation warning is not a gate.

Four of the fourteen `std::vector` fields in `Components.h` have element types
that hold a `std::string` and would have hit exactly this hole:

| Field | Element's string members |
| --- | --- |
| `std::vector<UIDropdownOption>` | `std::string m_Label` |
| `std::vector<DialogueChoice>` | `std::string Text`, `Condition` |
| `std::vector<FoliageLayer>` | `std::string Name`, `MeshPath`, `AlbedoPath` |
| `std::vector<Material>` | `std::string m_Name` (`Material.h:478`) |

The failure mode is the worst available: MSVC's `std::string` keeps no
self-referential pointer, so a violation **passes silently on the primary
development toolchain** and aborts only under libstdc++ — on the Linux CI and the
GPU runner. Local green does not imply CI green for any part of this work.

No existing `TArray<T>` instantiation violates the trait today. `Submesh` already
uses `FString`; `BoneInfluence`, `BoneInfo`, `Vertex`, `BVHNode` and `BVHTriangle`
are POD. The guard is therefore free to tighten now and gets more expensive with
every conversion made before it.

## 4. Decision

**Adopt the UE container library for engine-owned data. Do not delete the port.**

1. **Scope.** Engine-owned data only. `std::` stays at every third-party seam —
   `entt`, `yaml-cpp`, `sol2`, Mono, ImGui, Jolt, `spdlog` — and at any public
   surface a generated binding crosses.

2. **ECS components split by field.** `std::vector` fields convert to `TArray`.
   `std::string` fields stay `std::string`. `Components.h` holds 48 `std::string`,
   14 `std::vector` and 15 `std::unordered_*` fields today.

3. **The four blocking element types take `FString`** — `Material::m_Name`,
   `FoliageLayer`'s three paths, `DialogueChoice`'s two, and
   `UIDropdownOption::m_Label`. Three of the four live outside `Components.h` and
   are plain engine-owned data; only `UIDropdownOption` is nested inside a
   component, which the classifier's recursive struct path already handles. This
   is what unblocks all fourteen vectors without converting any component's own
   string field.

4. **`TIsTriviallyRelocatable` flips to opt-in and to a hard error.** Default
   becomes `std::is_trivially_copyable_v<T>`; `FString`, `TArray`, `TMap`, `Ref`
   and the other known-relocatable types keep explicit `true` specialisations
   (`FString` requires one — it owns a heap buffer and is not trivially
   copyable). User aggregates then default to **false** and opt in by one line.
   `OLO_STATIC_ASSERT_WARN` becomes a hard `static_assert`, and the result must
   build clean across all six containers, not just `TArray`. This deliberately
   diverges from UE's assume-true convention.

5. **The OloHeaderTool classifier becomes testable before it learns the new
   types.** It is 4,717 lines of hand-rolled text parsing with zero tests, every
   function `static` inside a `main.cpp` and therefore unlinkable from a test
   binary. Teaching it `TArray` and `FString` touches both its YAML and binary
   emit paths and its recursive nested-struct path. A *missing* emit is loud —
   `ComponentSerializerCoverageTest.cpp:188` fails with the component named — but
   a *wrong* emit is caught only if a round-trip test happens to construct that
   component. Lift parse+classify into a linkable module first.

6. **`TMap`/`TSet` are in scope, gated on a per-site reference-stability audit.**
   `std::unordered_map` guarantees that references to mapped values survive
   insertion; `TMap` does not — `TSparseArray` guarantees stable *indices*, not
   stable *addresses* (`SparseArray.h:14`, `:731`). `&map[key]` held across an
   insert is fine with one and undefined with the other, and unlike relocation
   there is **no compile-time guard available** for it. 727 `std::unordered_map`/
   `set` declarations exist in production.

7. **That audit is mechanised, not manual.** Set `CMAKE_EXPORT_COMPILE_COMMANDS`
   on the `clangcl` preset (a Ninja generator, which supports it natively) and
   write a `clang-query` matcher for references and pointers bound to mapped
   values whose lifetime crosses a mutation. `clang-query` and `clang-tidy` are
   already installed; there is no `.clang-tidy` and no `compile_commands.json`
   today. The existing ASan/UBSan/TSan jobs are the runtime backstop — a
   relocated-element access is a `heap-use-after-free` ASan already detects.

8. **No container is deleted, including the currently-unreachable ones.**
   Committing to the port means committing to its shape; deleting parts now means
   re-porting them later. Accepted cost: roughly 5,000 lines of compile time and
   test surface that may never acquire a user, and `TSparseSet` in particular
   stays unreachable *by construction* until someone flips
   `OLO_USE_COMPACT_SET_AS_DEFAULT`.

9. **Conversion follows the four element types outward**, in this order:
   Renderer (`Material`), Terrain (`FoliageLayer`), Dialogue (`DialogueChoice`),
   Scene/UI (`UIDropdownOption`). Those four subsystems' engine-owned data is
   converted, then the work **stops and is measured** before anything else is
   committed to.

### Execution order

Steps 1 and 2 are independent; everything from 3 onward is a chain.

1. Enable `CMAKE_EXPORT_COMPILE_COMMANDS` on the `clangcl` preset; write the
   `clang-query` matcher.
2. Flip the trait to opt-in and to a hard error; add opt-in specialisations until
   all six containers build clean.
3. Lift OloHeaderTool's parse+classify into a linkable module, with tests.
4. Teach the classifier `TArray` and `FString` — YAML emit, binary emit, and the
   nested-struct path.
5. Convert the four element types' strings to `FString`.
6. Convert the fourteen component vectors to `TArray`.
7. Convert the four subsystems' engine-owned data; run the audit over their maps.
8. Measure cost per KLOC. Decide whether to continue.

## 5. Rejected alternatives

- **Delete the port down to its two consumers** (the architecture review's
  recommendation). Removes ~7,000 unreachable lines with no design work and the
  deletion test answers itself. Rejected because it removes the symptom
  (`FString`'s four call sites) while preserving the condition (the renderer's
  mesh path still wants relocatable storage), and because it discards allocator
  control the engine has no other mechanism for — `Renderer/MeshSource.cpp:33`
  already notes the `FMemory` allocator not being instrumented as a *problem*.

- **Total conversion, no `std::` anywhere.** Maximally consistent and kills the
  mixing hazard outright. Rejected: `entt`'s component storage is not convertible
  at all, and conversions at the yaml-cpp/ImGui/sol2 seams are pure cost — those
  are precisely the boundaries `FString`'s implicit-in/explicit-out asymmetry was
  designed to sit at.

- **Strangler adoption — new code and hot paths only.** Lowest risk and no
  migration. Rejected because it institutionalises the mixed state indefinitely,
  which is the exact condition that produced the `TMap<std::string>` corruption.

- **Keep the opt-out trait default and only upgrade the warning to an error.**
  Far cheaper and stays faithful to UE. Rejected because it fixes the case we do
  not have (`TArray<std::string>` directly) and misses all four cases we do
  (a struct containing one).

- **Convert only the nine safe vectors and leave the four alone.** Honours the
  component split with no reinterpretation and touches no string. Rejected
  because the reason would be invisible at the call site: the next person adding
  a `std::string` to `TerrainLayerRule` or `OffMeshLink` silently arms the same
  landmine on a type that looks safe today.

## 6. Consequences

- **The engine takes on two string types deliberately, for a long time.** Under
  this scope `std::string` remains overwhelmingly dominant and `FString` is
  confined to engine-owned data. Anyone reading the ratios in §0 in isolation
  will conclude the port is unused. It is not; it is scoped.

- **Local green stops implying CI green** for any container work, until step 2
  lands. That is the single largest ongoing hazard in this plan and the reason
  the trait flip is sequenced first.

- **Step 2's blast radius is not knowable statically.** Roughly 15–20 opt-in
  specialisations are expected, concentrated in `Task/` and the async asset
  system — `TArray<FCallback>`, `TArray<Tasks::TTask<Ref<Asset>>>`,
  `TArray<TRefCountPtr<FThread>>`, `TArray<LowLevelTasks::FTask>` — i.e. in
  internal plumbing rather than in the data the migration is actually for.

- **Candidate #05 of the architecture review is promoted from a recommendation to
  a prerequisite.** The OloHeaderTool classifier cannot safely learn two new
  types while untestable.

- **`docs/analysis/dead-code.md` is now partly superseded** for the four ported
  directories: its 🟡 KEPT entries stay kept by decision 8 rather than by
  inertia. Its include-graph method remains valid elsewhere, but note it cannot
  observe compiled-but-never-instantiated templates.

- **ADR 0004 is untouched and constrains this work.**
  `Memory/LockFreeList.cpp`'s allocator is first-touched from a scheduler worker
  freeing a Jolt job, and its magic-static initialisation is a recorded decision
  taken against a TSan report. No step here modifies it, and decision 8 means it
  is not a deletion candidate either.

- **ADR 0009 is unaffected in its target and interacts in its timing.** That ADR
  decides bindings are ultimately generated by C++26 static reflection with
  *derived* exclusion sets, and records its status as not yet implemented —
  a GCC-16 experiment pending P2996 in MSVC/Clang, tracked by the monthly cron on
  issue #688. Step 3 here moves the current classifier in the same direction
  (testable, fewer hand-kept tables) and must not be read as competing with it.
  If reflection productionises mid-migration, steps 3 and 4 are the ones to
  re-plan.

## 7. What would have to be true to reverse this

Reversal means returning to the deletion recommendation in §0. Any one of the
following is sufficient cause to re-open:

1. **Step 8's measurement comes back bad.** If converting four subsystems costs
   materially more per KLOC than projected, or produces defects the trait and the
   audit did not catch, stop and reconsider rather than continue by momentum.
2. **The reference-stability audit proves intractable.** If the `clang-query`
   matcher cannot produce a candidate list with an acceptable false-negative
   rate, decision 6 fails and `TMap` adoption should be abandoned — the vector
   and string work can stand without it.
3. **P2996 ships and reflection makes the classifier work moot** before step 4
   is done, changing the cost basis of decisions 3 and 5.
4. **A second relocation hazard class emerges** that the flipped trait cannot
   express. The trait catches self-referential storage; it says nothing about
   types with registered addresses (an object that hands `this` to an external
   registry survives `is_trivially_copyable_v` and still cannot be relocated).

Absent one of those, the ratios in §0 are not on their own grounds to re-open
this — they are the starting condition this ADR exists to explain.
