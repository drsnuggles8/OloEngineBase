# Scripting bindings come from a reflection-emitted, language-neutral schema — foreign languages generate from the schema, C++-hosted languages use reflection directly

The C++26-reflection experiment (`experiments/cpp26-reflection/`, GCC-16-only for
now) proved that **every C++-side output of OloHeaderTool** — the `AllComponents`
tuple, `OnComponentAdded/Removed` no-ops, the scene serializer, the SaveGame
capture/restore lists, the MCP field registry, and the C++ scripting glue — can
be generated natively by static reflection over the annotated component structs,
with several outputs coming out *more* correct than the hand-written tool
(derived exclusion sets instead of hand-kept ones; no stale-file drift). The Lua
(sol2) bindings, which were never a tool output but hand-written in
`LuaScriptGlue.cpp`, were shown to be reflection-generable too (61 components /
343 fields, driven from real Lua scripts).

That leaves exactly one thing static C++ reflection cannot *do*: emit **C#**. The
C# proxy classes are `.cs` source that the Mono/Roslyn compiler must consume, and
C++ reflection runs in the C++ compiler — it can produce the source *string* but
cannot *compile* it. This ADR records how the foreign-language boundary is
crossed, and — because the team intends to keep extending scripting and to move
off Mono onto a modern .NET host such as [Coral](https://github.com/StudioCherno/Coral)
— the design commitments that keep that boundary from being rewritten every time
scripting grows.

**Status:** decision recorded; not yet implemented on the shipping toolchain.
The reflection layer is a GCC-16 experiment until MSVC/Clang ship P2996; this ADR
is the target architecture for when it is productionised, and the constraint that
new scripting work should not contradict in the meantime.

## Decision

**Scripting bindings are derived from a single, annotation-defined scripting
surface on the C++ components, via reflection. Consumers split by host:**

- **C++-hosted binding languages (Lua/sol2) generate directly from reflection**
  at compile time — no intermediate file, no build step. They are in the C++
  compilation already; a serialized schema would only re-introduce a codegen step
  they don't need.
- **Foreign-language bindings (C#/Coral, and any future non-C++ language) are
  generated from a language-neutral schema** that reflection emits. For C# that
  schema is consumed by a Roslyn incremental source generator living in the
  `OloEngine-ScriptCore` (or its Coral successor) project.

Both paths read the *same* reflection over the *same* annotations, so they cannot
drift. The schema is simply "the reflection truth, serialised for consumers that
cannot run C++ reflection."

```text
C++ components + annotations   ← the ONE definition of the scripting surface
        │ reflection
        ├─ C++-hosted (Lua/sol2)         → generated in-compile, no schema, no step
        └─ neutral schema (versioned)    → the contract for everything foreign
               ├─ C# (Coral) Roslyn generator → typed proxies + host glue
               ├─ (future language) generator
               └─ docs / editor tooling / validation
```

This is "Option B" — the neutral-schema approach — promoted from a C# tactic to
the interop architecture. Option A (a C++ program that writes `.cs` directly) was
rejected: it hard-codes C# *and* the Mono interop ABI into the C++ build at
precisely the moment we intend to change both, and it puts C#-specific knowledge
(syntax, marshalling, host ABI) in the wrong language.

## Rejected alternatives

- **A — C++ program emits `.cs` directly.** Simpler, but couples the C++ build to
  the host's evolving interop ABI and embeds C# codegen in C++. A Mono→Coral move
  becomes a rewrite *inside a C++ program* re-tested against a moving target.
- **Pure Roslyn with no C++ step.** Impossible: Roslyn cannot see C++ types, so a
  reflection step must export the schema regardless. Any "Roslyn reads the C++
  headers" design is just relocating OloHeaderTool's text-parsing into C#.
- **A generic type-erased interop** (`entity.Get("Translation")`, no per-component
  proxy) eliminates generated code but throws away *typed* scripting — a real
  authoring regression. Not acceptable as the primary surface.

## The four design commitments (bake in now; implement as scripting grows)

These shape the annotation vocabulary and schema format that everything else is
built on, so they are decided here even though most are populated later.

1. **The schema is interop-mechanism-agnostic.** It describes *what* is bindable
   (component, member, type, access, marshalling category) and never *how* (Mono
   internal-calls vs Coral function pointers). Consequence: Mono→Coral changes
   only the C# generator; the C++ side and the schema are untouched. This is the
   single strongest reason B beats A once Coral is planned.

2. **The schema is member-kinded and versioned from day one.** Members carry a
   kind (`field` today; `method`, `event` reserved) and a full signature, and the
   schema carries a format version. Fields ship first, but adding methods/events
   later is *additive population*, not a format break that ripples through every
   generator.

3. **The full binding metadata lives in the annotation, not in the generators.**
   The script-exposure annotation carries the script-facing name, read-only vs
   read-write, and any marshalling hint — so the schema is self-sufficient and
   every generator reads one source. This also folds the current "bespoke tail"
   (custom property names, computed getters) into the normal path.

4. **A field-level sync-hook removes the hand-written tail from *both* backends.**
   The remaining hand-written bindings (e.g. Rigidbody2D's setter syncing to
   Box2D) are all "on set, run this side effect." An `OnSet` hook on the field
   lets reflection generate the getter/setter *and* the side-effect call once,
   feeding both the Lua registration and the C# schema. Without it, every synced
   field stays hand-written in Lua *and* C#; with it, the bespoke set nearly
   vanishes.

## Consequences

- **OloHeaderTool (the 4,540-line text parser) is deleted.** C++ outputs become
  in-compile reflection with no generation step; C# is served by a ~100-line
  reflection schema-emitter plus a C# generator. The irreducible minimum for
  *typed* foreign scripting is that one tiny emitter — Roslyn cannot read C++
  types and compile-time reflection cannot write files, so some C++ step must
  export the schema.
- **The schema is a reusable IDL.** Beyond C#, it can feed documentation, editor
  tooling, binding validation, and future language backends from one source.
- **Coral migration is contained.** Because of commitment #1, moving off Mono is
  a C#-generator change, not an engine-wide one.
- **Nothing is required on the shipping toolchain yet.** Until P2996 lands on
  MSVC/Clang, the hand-maintained touch-points in `CLAUDE.md` remain the source
  of truth; this ADR governs the *direction*, so new scripting features should be
  expressible as annotations on components rather than as new hand-written
  per-language glue.
