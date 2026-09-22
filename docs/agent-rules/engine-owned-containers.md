# Engine-owned data uses the ported UE containers

**New engine-owned sequence and string data is `TArray<T>` / `FString`, not `std::vector` /
`std::string`.** Data whose shape is dictated from outside the engine keeps `std::`. Associative
containers stay `std::unordered_map` / `std::unordered_set` everywhere for now.

That is the whole rule. The rest of this file is how to tell the two apart and what goes wrong when
you get it backwards.

Decided in [ADR 0012](../adr/0012-adopt-the-ue-container-library-for-engine-owned-data.md),
carried out in [#738](https://github.com/drsnuggles8/OloEngineBase/issues/738).

## The test

> **Does something outside the engine dictate this type?**

Not *"is this hot?"* — performance is not the criterion and never was.

| | Use |
|---|---|
| The engine allocates it, owns it, iterates it | `TArray<T>`, `FString` |
| `entt`, `yaml-cpp`, `sol2`, Mono, ImGui, Jolt, `spdlog` sit on the other side of it | `std::` |
| An ECS component **string** field a generated binding marshals | `std::string` — five generated consumers cross it |
| An ECS component **sequence** field | `TArray<T>` — the carve-out is for strings, not their containers |
| A map or a set, anywhere | `std::unordered_map` / `std::unordered_set` |

Borrowing across the seam is free and does not allocate: `FString::ToView()` hands out a
`std::string_view`. `ToStdString()` **allocates** — reach for it only when a callee genuinely needs
an owning `std::string`.

## Why maps are excluded

`std::unordered_map` is node-based, so a reference to a mapped value survives insertion and rehash.
`TMap` stores elements in a relocatable array — `TSparseArray` gives stable *indices*, not stable
*addresses*. `&map[key]` held across an insert is fine with one and undefined with the other, and
**unlike relocation there is no compile-time guard available**.

The audit built to make this decidable ([`tools/container-audit/`](../../tools/container-audit/))
is a lexical candidate finder, not a reference-lifetime proof: it misses escaped borrows through
return values and out-parameters, misses interprocedural mutation, and flags two patterns that are
safe. **Zero candidates is not evidence of safety.** The gate is closed; reopening it is
[#1411](https://github.com/drsnuggles8/OloEngineBase/issues/1411).

## What goes wrong

**`TArray` relocates its elements bitwise.** A type that cannot survive a `memcpy` of its bytes
corrupts the heap when the array grows. libstdc++'s `std::string` keeps an SSO self-pointer and
does not survive it; MSVC's does not keep one and does. So the abort is
**green on this box and red on Linux CI** — see `Containers/String.h:26-33`, and
[`build-trees-and-windows-asan.md`](build-trees-and-windows-asan.md) for why local green proves
nothing here. `FString` exists for exactly this reason and for no other.

`TIsTriviallyRelocatable` is the guard. It defaults to `std::is_trivially_copyable_v<T>` and is a
**hard** `static_assert` in `Array`, `CompactSet`, `Deque` and `SparseArray`. It fails closed: a
type it cannot prove safe is rejected at compile time.

## Adding a `TArray<T>` whose `T` is not trivially copyable

The build will stop you. To opt in, write the specialisation **member by member** — never a bare
`true`:

```cpp
template <>
struct TIsTriviallyRelocatable<FGroomStrandRequest>
{
    static constexpr bool Value =
        TIsTriviallyRelocatable_V<decltype(FGroomStrandRequest::Handle)> &&
        TIsTriviallyRelocatable_V<decltype(FGroomStrandRequest::Name)> &&
        /* … every member, so adding an unrelocatable one re-closes the gate … */;
};
```

Deriving it from the members is the point: a hand-written `true` is a promise that rots the moment
someone adds a field. There are 184 of these in the tree; copy the shape of the nearest one.

Two things the trait cannot see, which are yours to check:

- **A type that registers `this` with something external** passes `is_trivially_copyable_v` and
  still cannot be relocated. ADR 0012 reversal condition 4.
- **A `std::string_view` member** is trivially copyable and therefore accepted — but if it points
  into a buffer the same struct owns, relocation leaves it dangling.

## Two traps this migration actually hit

**A view into a temporary.** `set.insert(name.ToStdString())` on a `std::unordered_set<std::string_view>`
compiles, and every entry dangles the moment the temporary dies. It cost a frame-graph mis-ordering
that no test caught. Use `.ToView()` when the container borrows and the owner outlives it.

**`SetNum` default-initialises; `resize` value-initialises.** Converting
`vector.resize(n)` to `array.SetNum(n)` used to leave aggregates of scalars — `glm::vec3`, POD
structs — holding garbage, because `MemoryOps.h`'s zeroing whitelist covered bare scalars only.
Fixed engine-side by value-initialising, but the asymmetry is worth knowing when reading UE code.

## The trait answers differently on clang-cl and MSVC

`std::is_trivially_copyable_v<T>` is **not** portable for a class whose copy *and* move
operations are all deleted. clang-cl calls it vacuously trivial; MSVC does not:

| | `std::is_trivially_copyable_v<std::atomic<int*>>` |
|---|---|
| clang-cl (`dev-cached`, what you build locally) | `true` |
| MSVC (`msvc` preset, and every Windows CI job) | `false` |

So `TArray<T>` where `T` holds a `std::atomic`, `std::mutex` or any all-deleted-copy member
**compiles here and fails the hard `static_assert` on CI**, with the same shape as the
libstdc++ traps — except this one is the *compiler*, not the standard library, so a Linux
job will not catch it either. It cost a CI round on `TaskConcurrencyLimiter.h`'s
`FPaddedSharedTask` (#738).

`std::atomic<T>` itself now carries a specialisation derived from `T`, so the direct case is
portable. A **wrapper struct** holding one still needs its own, derived the usual way:

```cpp
template<>
struct TIsTriviallyRelocatable<Tasks::Private::FPaddedSharedTask>
{
    static constexpr bool Value =
        TIsTriviallyRelocatable_V<decltype(Tasks::Private::FPaddedSharedTask::Task)>;
};
```

Two mechanics that are easy to get wrong:

- The specialisation must be **visible before the container is instantiated**. Declaring it
  after the class that holds the `TArray` member is too late and is IFNDR.
- A specialisation **cannot name a private nested type**, which is why `FPaddedSharedTask`
  sits at namespace scope rather than inside its class.

To check without a CI round, compile one TU with real `cl.exe`: take the flags from
`build-cached/compile_commands.json`, drop the clang-cl-only spellings (`-clang:`, `-Xclang`,
`-Wno-`), turn `-imsvc` into `/external:I`, add `/std:c++latest`, and run it under
`vcvars64.bat`. Assert the *opposite* of what you expect so the diagnostic prints the real
value instead of you guessing it.

## Heterogeneous lookup, if you keep a `std::` map keyed by string

C++20 gives transparent comparators `find` / `count` / `contains` / `equal_range` — and **not**
`insert`, `at`, `operator[]` or `erase`. `erase` with a heterogeneous key is an MSVC extension that
does not compile under libstdc++; use `find()` then `erase(iterator)`.

`RGBuilder.h`'s `RGStringTransparentHash` / `RGStringTransparentEqual` accept `FString`,
`std::string` and `std::string_view` against one map, so a lookup keyed by an `FString` costs no
allocation. Reuse them rather than writing a fourth pair.

## What is pinned, and what is not

[`OwnedContainerInvariantsTest.cpp`](../../OloEngine/tests/Containers/OwnedContainerInvariantsTest.cpp)
fails if a component gains a `std::vector` member, if one of the four unblocking element types
(`Material::m_Name`, `FoliageLayer::Name`, `DialogueChoice::Text`, `UIDropdownOption::m_Label`)
reverts to `std::string`, or if the trait stops failing closed.

Everything else is judgement. There is deliberately **no** engine-wide ratchet on `std::vector`
members: master added 269 of them across the migrated subsystems in the 30 days to 2026-09-22, and
a guard that fires on most pull requests is one that gets worked around. Roughly 860 `std::vector`
members remain engine-wide, most in subsystems ADR 0012 never scoped. Converting them is not an
obligation you have inherited — but new engine-owned data should land on the right side to begin
with.
