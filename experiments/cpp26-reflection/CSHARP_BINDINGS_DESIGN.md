# C# Scripting Bindings — the one piece reflection can *feed* but not *be*

Every other OloHeaderTool output is now reflection-generated (see `OLOHEADERTOOL_REPLACEMENT.md`),
and the Lua bindings too. The C# bindings are the sole remainder, because they are **C# source
files** (`.cs`) that the Mono/Roslyn compiler must consume — and C++ reflection runs in the *C++*
compiler, so it can produce the source *string* but cannot *compile* it. This document designs how to
close that gap.

## What C# needs (two generated files)

1. **`Components.Generated.cs`** — per component, a `public partial class X : Component` with a typed
   property per OLO_PROPERTY field:
   ```csharp
   public partial class BoxCollider2DComponent : Component {
       public Vector2 Offset {
           get { InternalCalls.BoxCollider2DComponent_GetOffset(Entity.ID, out Vector2 value); return value; }
           set => InternalCalls.BoxCollider2DComponent_SetOffset(Entity.ID, ref value);
       }
       public float Density {
           get => InternalCalls.BoxCollider2DComponent_GetDensity(Entity.ID);
           set => InternalCalls.BoxCollider2DComponent_SetDensity(Entity.ID, value);
       }
   }
   ```
2. **`InternalCalls.Generated.cs`** — the matching `[MethodImpl(InternalCall)] extern` declarations,
   whose names bind to the C++ #6 glue.

Two marshalling shapes, both deterministic from the field type: **scalar** (`float`/`bool`/`ulong`/
`int`) returns by value; **struct** (`Vector2/3/4`) uses `out`/`ref`.

## The C++ half is done — reflection emits this source

`csharp_gen.cpp` enumerates OLO_PROPERTY fields via reflection and emits **exactly** the engine's
`Components.Generated.cs` + `InternalCalls.Generated.cs` shapes (verified against the checked-in files
— identical classes, fields, marshalling, and internal-call names; only whitespace differs). The C#
type map is a small `consteval` table (`f32`→`float`, `AssetHandle`→`ulong`, `glm::vec3`→`Vector3`,
enum→`int`, …). So the *content* generation is fully reflection-driven — no text parser.

**A property that falls out for free: the two sides cannot drift.** The C++ #6 glue *and* the C#
`InternalCalls` externs are generated from the *same* OLO_PROPERTY enumeration, so an internal-call
name like `BoxCollider2DComponent_GetOffset` is produced identically on both sides by construction —
the class of "C# calls a native method the C++ side never exported" bug becomes impossible.

## The gap: who *writes* the `.cs`? Two architectures

C++ reflection has produced the source string; something must land it where Roslyn compiles it.

### A. Direct emit — reflection-driven C++ writer (recommended)

A tiny reflection program (this `csharp_gen`, expanded to all components) writes
`Components.Generated.cs` + `InternalCalls.Generated.cs` into `OloEngine-ScriptCore/`, exactly where
OloHeaderTool writes them today.

- **OloHeaderTool collapses to this** — a ~200-line reflection-driven *C#-only emitter*, versus the
  4,540-line text parser. Discovery, field types, names: all from reflection. The existing
  `GenerateBindings` build wiring is unchanged (it already writes these two files).
- **Pros:** smallest possible change; the `.cs` stay real, inspectable, debuggable files; the build
  flow is identical to today; no new toolchain.
- **Cons:** still a build-time C++ step that writes files into the tree (same as today — not a
  regression, just not "zero-tool").

### B. Metadata + Roslyn incremental source generator (the "no `.cs` in the tree" option)

The reflection program emits a **metadata file** instead of source — the component/field/type/
internal-call list as JSON:
```json
{ "BoxCollider2DComponent": [
    {"name":"Offset","cs":"Vector2","struct":true,"get":"BoxCollider2DComponent_GetOffset","set":"..."},
    {"name":"Density","cs":"float","struct":false,"get":"...","set":"..."} ] }
```
A **Roslyn `IIncrementalGenerator`** in the `OloEngine-ScriptCore` project reads it as an
`AdditionalFiles` input and emits the `partial class` proxies + `InternalCalls` at C# compile time,
in-memory.

- **Pros:** no generated `.cs` on disk; the proxies live only in the compiler output; incremental and
  cached by Roslyn; idiomatic modern C#.
- **Cons:** more moving parts (a generator project + the metadata handshake); the metadata still comes
  from a C++ reflection step, so it does not remove the C++ half — it relocates the C# emission into
  the C# build. Overkill unless "no generated `.cs` files" is an explicit goal.

### Not viable: pure Roslyn with no C++ step

Roslyn cannot see C++ types, so a generator alone has no component schema to work from. Some C++
reflection step must export it either way — which is why both A and B keep a (tiny) C++ reflection
program. The 4,540-line text parser is gone in both.

### Rejected: a generic type-erased interop (no per-component proxy)

One hand-written generic accessor (`entity.Get("Translation")`) eliminates the `.cs` entirely but
throws away *typed* C# scripting (`transform.Translation`) — a real DX regression for script authors.
Not worth it.

## Recommendation

**Adopt A.** It is the minimal, lowest-risk path: OloHeaderTool becomes a small reflection-driven C#
emitter, the build is unchanged, and the `.cs` stay debuggable. Keep **B in reserve** — if the team
later wants zero generated `.cs` in the tree, the same reflection program that writes source in A
instead writes the JSON metadata in B, and only the Roslyn generator is new. The reflection layer is
identical for both; the choice is purely *where the C# text materializes*.

## The bespoke tail (same shape as everywhere else)

- **Custom OLO_PROPERTY `Name`** (script name ≠ field name) — needs the `Name` bridged into the
  reflection annotation (a small extension of the `Property` marker); until then those few properties
  keep the field name.
- **Computed properties** (Camera's nested getters, Transform's `Rotation`) — call component methods;
  handled like the serializer's custom set.

Everything else — the overwhelming majority — is generated from reflection, on both the C++ and C#
sides, from one annotation source.
