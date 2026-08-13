# Subsystem notes — authoring MCP tools for the editor

How to add an `olo_*` tool to the OloEditor MCP diagnostics server, and the traps that cost time.
Reference notes, not failure postmortems — see [README.md](README.md). The server itself (tool
catalogue, attach flow, resources) is documented in
[../guides/mcp-diagnostics-server.md](../guides/mcp-diagnostics-server.md).

Salvaged from worktree-scoped memory (see `docs/process/task-loop.md` Phase 7 for why that is now
the wrong destination).

---

## 1. The three-part split every tool follows

1. **Pure header** `OloEditor/src/MCP/Mcp<Name>.h` — renderer/httplib/`McpServer`-free. Only engine
   Scene/ECS, header-only `UndoRedo/`, `nlohmann::json` and `McpSchemaBuilder.h`. Exposes
   `InputSchema()`, `ParseArgs(...)`, `Apply(...)` (or, for an "explain" tool, plain fact structs
   plus one free `inline` reasoning function).
2. **Handler** in `McpTools.cpp` — validate args, gather live facts inside
   `server.MarshalRead([...]{ … })` on the main thread, call the pure code, serialize to JSON.
   Register in `RegisterBuiltinTools` with `MainMarshaled = true`.
3. **Unit test** `OloEngine/tests/MCP/Mcp<Name>Test.cpp` driving the pure code directly, classified
   `// OLO_TEST_LAYER: unit` and added to the explicit source list in `tests/CMakeLists.txt`.

> **Why the split is mandatory: a handler cannot be *run* from the test binary.** Most MCP tests
> compile the pure headers plus `McpServer.cpp` (the dispatch seam) and register *fake* tools wired
> to the same shared code, because a real handler needs `MarshalRead`, a game thread and usually GL.
> So all real logic must live in the pure header, or it is untested.
>
> **Correction (#777):** an older version of this note said the test binary "deliberately does not
> link `McpTools.cpp`". That is **false** — `McpTools.cpp` and the whole per-domain `McpTools*.cpp`
> family have been in `tests/CMakeLists.txt`'s explicit source list since the `McpHeadlessAttachTest`
> work (#316). What was never safe is *invoking* a handler, not linking one; §2 below already said as
> much about `RegisterBuiltinTools`. Practical consequence: **registering a new built-in resource,
> prompt or tool changes what the headless tests see**, so a `resources/list` count assertion in an
> unrelated test can fail on your change.

## 2. A green test run does not mean your handler *works*

Building `OloEngine-Tests` **does** compile `McpTools.cpp` and every per-domain `McpTools*.cpp`, so
a green run proves they compile and that their `ToolDef`s register. What it does **not** prove:

- that the handler *runs* — nothing drives a real one end-to-end (it would need `MarshalRead`, a game
  thread and usually GL), so the body is unexercised;
- that the **`OloEditor` target** builds — it has its own sources (`McpServerPanel.cpp`, the editor
  layer) and its own link step, which the test target does not cover.

**Build the `OloEditor` target too** before calling a tool done — that is also what live-verify needs.

> **Registration-only testing IS safe headless.** `RegisterBuiltinTools(server)` only builds
> `ToolDef`s (schemas, names, annotations); handlers never run, so no MarshalRead, game thread or GL
> is needed — as long as no `tools/call` is issued. `McpOutputSchemaCoverageTest.cpp` uses this to
> assert schema adoption across the real tool surface. Older comments claiming
> "`RegisterBuiltinTools` is intentionally not linked" are **stale** — the concern was always
> handler *invocation*, not registration.

## 3. Schemas: use the builder DSL, and mind byte-identity

Author `InputSchema`/`OutputSchema` with the fluent DSL in `McpSchemaBuilder.h`
(`OloEngine::MCP::Schema`). Do **not** hand-write raw `nlohmann::json` schema literals.

```cpp
Schema::Object().Prop("count", Schema::Int().Min(1).Max(200).Desc("…")).NoAdditional();
```

Factories: `Object()`, `EmptyObject()`, `Int()/Number()/Bool()/String()/Array()`. Helpers:
`Vec3(desc)`, `EntityId(desc)`, `.Pagination(...)`, `Raw(json)` (escape hatch for odd shapes like a
multi-type `{"type":["integer","string"]}`).

Three byte-identity rules that made the migration provably wire-unchanged:

- The project uses the **default** `nlohmann::json` (a sorted `std::map`), so object-key **insertion
  order is irrelevant** to `==` and `.dump()`. Only **array** element order matters (enum, required,
  items) — preserve that.
- **Numeric bounds must stay JSON integers.** `json(1).dump() == "1"` but
  `json(1.0).dump() == "1.0"`, and `json(1) == json(1.0)` is **true** — so `==` alone will not catch
  a float drift. Assert `.dump()` too. `Min/Max/ExclusiveMin/MinItems/MaxItems` take `i64` for this
  reason.
- An object described only by `.Desc()` (no `.Prop()`) must **not** get a `properties` key —
  `Object()` creates it lazily on first `Prop()`. The screenshot `camera`/`orbit` free-form
  sub-args rely on this.

### Validation edges

Server-side enforcement is `McpServer::ValidateArguments`, called from `HandleToolsCall` before the
handler, returning `kInvalidParams (-32602)` naming the field.

- **Entity-id fields declare `type:"string"` while handlers accept string *or* number.** `ParseUuid`
  takes either, but the declared type is string-only, and where a union was genuinely wanted the
  authors declared one explicitly (`olo_events_tail`'s `sinceId`). So **plain `string` is the
  contract** and strict rejection is correct, not a bug — practical regression is ~zero since UUIDs
  are 64-bit (lossy as JS numbers) and `olo_scene_list_entities` already returns strings.
  `EntityId()` is byte-locked by `McpSchemaBuilderTest.cpp`; don't widen it without updating that
  test.
- **`integer` is strict** (`is_number_integer()` only), so a `5.0` JSON float is rejected. Deliberate
  — it keeps the validator free of any float-equality test (SonarQube S1244). `number` accepts
  either; bound checks use relational operators so they stay clean.
- The 8 MiB payload cap and the error handler are **transport** concerns set in `McpServer::Start()`
  (cpp-httplib's default cap is `SIZE_MAX`), not part of the pure dispatch seam — verify by smoke
  test if changed.

## 4. Tool annotations (MCP 2025-06-18)

Verified against the official schema:

- `Tool` extends `BaseMetadata`, so **`title` is a top-level field**, not only `annotations.title`.
  Precedence is `title` > `annotations.title` > `name`. Emit the top-level one; don't mirror it.
- `readOnlyHint` — default **false**. True is the signal a client uses to auto-approve a read.
- `destructiveHint` — default **true**; meaningful only when `readOnlyHint == false`.
- `idempotentHint` — default **false**; meaningful only when `readOnlyHint == false`.
- `openWorldHint` — default **true**; false = closed domain (the local editor).

The server already negotiates 2025-06-18, so annotations need no protocol bump. Use the
`ReadOnlyAnnotations()` / `MutatingAnnotations()` builders.

## 5. Write tools: consent gate, undo, and the reflection gap

- **Session gate.** Set `ToolDef::ProjectWrite = true`. `HandleToolsCall` refuses it with a clean
  `kInvalidParams` unless `SetAllowWrites` is on — atomic, default OFF, **never persisted**. Pair
  with `MutatingAnnotations(/*idempotent*/ false)`. Read-only and ephemeral-editor-state tools
  (camera, viewport, render overrides) are not `ProjectWrite`, so the gate never touches them.
- **Undo.** Route the mutation through the editor `CommandHistory` as a UUID-keyed
  `ComponentChangeCommand<T>` so an agent's edit is a single Ctrl-Z. Skip the push (report
  `changed=false`) when the value is unchanged.

  **Change detection is not one-size-fits-all.** `std::memcmp` is only valid for a type that is
  *both* padding-free and authored-only. Two ways it goes wrong: indeterminate padding bytes make
  two logically-equal components compare unequal (a spurious undo entry), and a runtime-mutated
  field makes them compare unequal for a reason the user never authored. For those, specialize
  **`PreferValueComparison<T>`** (`OloEditor/src/Panels/SceneHierarchyPanel.cpp`) and give the type
  an `operator==` over its **authored fields only** — `IKTargetComponent`, `PerceptibleComponent`
  and `TransformComponent` already do exactly this. `!=` for `std::string`, and **never float `==`**
  (S1244).
- **Threading.** Resolve scene/history and build the command inside `server.MarshalRead(...)` — the
  EnTT registry is not thread-safe. Return `{"__error", msg}` from the job and convert after.

> **There is no runtime reflection.** `OLO_PROPERTY()` expands to *nothing* — it is a compile-time
> marker OloHeaderTool consumes to emit static glue. A generic field-write tool therefore cannot
> look up a `(component, field)` setter at runtime; it needs its own registry plus templated
> JSON→type coercion. Field **names** must be the `m_`-stripped struct keys the SceneSerializer
> writes, so the write contract matches what an agent reads back.
>
> The registry has **three** accessor kinds, and a new writable field must pick one:
>
> | Builder | For |
> |---|---|
> | `MakeField` / `MakeFieldAccess` | a public data member reachable as `comp.member` — the common case |
> | `MakeSetterField` | a private member behind an `OLO_PROPERTY` Get/Set pair, called on the live component (see [mcp-setter-based-field-registry.md](mcp-setter-based-field-registry.md)) |
> | `MakeMapKeyField` | a **map-backed** field with no compile-time-known keys — addressed by a dotted `field.key` path with custom get/set callbacks |
>
> `MorphTargetComponent::Weights` is the map case: its keyset doesn't exist until a
> `MorphTargetSet` is bound at runtime, so there is no static field name to register per key. One
> `MakeMapKeyField` entry covers every key.
>
> *Since these notes were written the registry became generated* — see `CLAUDE.md` → OloHeaderTool,
> `McpFieldRegistry.Generated.inl` — and setter-based fields exist for private members
> ([mcp-setter-based-field-registry.md](mcp-setter-based-field-registry.md)).

## 6. Consent and cancellation concurrency invariants

- **Two mutexes, never nested.** `m_ConsentMutex` and `m_InFlightMutex` are each acquired at most one
  at a time per path — no ABBA cycle. Keep it that way.
- **Cancellation → consent wake.** The `notifications/cancelled` path finds the `CancelFlag` under
  `m_InFlightMutex` (released), sets the atomic, then does a bare `{ lock_guard(m_ConsentMutex); }`
  **barrier** before `notify_all()`. The barrier serialises the flag store against a parked waiter's
  predicate check, so the wake is never lost. Notify outside the lock — a spurious wake is cheap.
- **Register before the gate.** `HandleToolsCall` registers the in-flight `CancelFlag` *before* the
  consent gate (RAII guard erases on every exit), so a call parked on the Prompt-mode modal is
  reachable by cancellation. Cancellation wins over a racing human Approve.

## 7. Don't scan a narrow consecutive port window on Windows

The socket-binding tests intermittently failed with `port == 0`. `DerivePidPort()` samples an
ASLR-derived base in `[20000, 59999]` and `Start()` tried only **24 consecutive** ports — but Windows
reserves **contiguous** excluded TCP blocks in that range (Hyper-V / WSL / Docker; inspect with
`netsh interface ipv4 show excludedportrange protocol=tcp`), observed up to ~480–700 ports wide. When
ASLR landed inside a block, all 24 attempts were excluded.

Fix: stride the derived path by a **prime (769) wider than the widest exclusion block**, so a run of
failures leaps clear; `gcd(stride, span) == 1` keeps candidates distinct. The explicit-`basePort`
path is unchanged — the agent's client expects that exact port.

## 8. Frame capture is graph-wide, and names come from the live graph

- **Live node names are not the class `SetName()` values.** `SceneRenderPass` → **`ScenePass`**,
  `FoliageRenderPass` → **`FoliagePass`**, `WaterRenderPass` → **`WaterPass`**, `DecalRenderPass` →
  **`DecalPass`**. Always tag with `GetName()` so the capture side and the graph-attribution side
  produce identical names — a hard-coded `"SceneRenderPass"` silently fails the join and the counts
  drift.
- **The commit point is central.** `Renderer3D::EndScene` calls `FrameCaptureManager::CommitFrame()`
  once *after* `RGraph->Execute()`. It used to commit mid-graph inside `SceneRenderPass::OnFrameEnd`,
  which is exactly why only the scene pass was ever captured — Water/Foliage/Decal/ForwardOverlay run
  later and reset their own buckets at the end of their Execute. Each bucket pass now calls
  `BeginPass(GetName())` + `OnPreSort` at the **top** of Execute, before any early-return on an empty
  bucket, so even an empty pass registers.
- A pass owns a command bucket iff it is a `CommandBufferRenderPass` (`Ref::As` is a `dynamic_cast`).
- **`capturedPassCount == commandBucketPassCount` excludes culled passes** — the latter counts passes
  that actually ran. A culled bucket pass is listed with `captured=false, culled=true` and not
  counted.

> **`CapturedCommandData::IsDrawCommand()` is an explicit whitelist** and omits `DrawTerrainPatch`,
> `DrawVoxelMesh`, `DrawDecal` and `DrawFoliageLayer`, even though all carry a `renderStateIndex`.
> Any tool filtering on it **under-counts on terrain/voxel/decal/foliage scenes** — a verified frame
> of 16× `DrawTerrainPatch` reported `isDraw:false` for every patch. `FrameCaptureStats.DrawCalls`
> comes from a different source, so a report's "Draw Calls: 7" can disagree with the `isDraw:true`
> row count. If you need an accurate count, widen the whitelist rather than reimplementing per-tool.

## 9. What the live frame can and cannot honestly answer

Established building `olo_render_why_not_visible`:

- **View frustum IS queryable** — `Renderer3D::GetViewFrustum()` returns the actual frustum used for
  culling. Renderer3D expands the sphere radius by **1.3×** before testing; match that. It reflects
  whatever camera last rendered, so compute "behind camera" independently from the editor camera
  pose.
- **HZB occlusion result and LOD selection are NOT queryable** — private per-frame renderer state. An
  editor-side tool cannot honestly claim a verdict; report them as not-observable and fall back to a
  screenshot or capture target.
- **No world-transform flattening** — the scene path submits each entity's **local**
  `TransformComponent::GetTransform()`, so use the local transform for visibility math.
- **A scene mesh's shader is the shared deferred PBR shader**, not `material.GetShader()` (usually
  null). Per-entity compile-error mapping only works for custom-shader materials; otherwise use the
  global "any shader failing" signal.
- **A missing `MaterialComponent` is not an invisibility cause** — the mesh path falls back to
  `GetDefaultMaterial()`. Entities are skipped on a null mesh asset, `m_Visible == false` or
  `m_Enabled == false`.
- There is **no per-entity render layer**; collision layers are physics-only.

> **Honesty rule:** if a signal isn't queryable, report it as unknown rather than fabricating a
> verdict. An "explain" tool's reasoning must be a deterministic root-cause cascade — most
> fundamental precondition first, stopping at the first blocker so `reasonCode` is the root cause,
> not a symptom.

## 10. Two known tool limitations

- **`olo_shader_list` over-reports what is reloadable.** It reads `ShaderDebugger::GetAllShaders()` —
  *every* GL program including post-process and compute — but only shaders owned by the two
  `ShaderLibrary` instances can be reloaded **by name**. The rest are created directly by their pass
  and there is no name→`Ref<Shader>` registry; the ShaderDebugger stores only id/name/path. The
  tool's not-found error lists the reloadable names to bridge the gap.
- **A GLSL syntax error crashes a Debug editor.** Reloading a shader that fails to compile trips
  `OLO_CORE_VERIFY(false, …)` → `__debugbreak()`, and `catch(...)` does **not** catch the SEH
  breakpoint. The MarshalRead job never completes, the tool times out after ~5 s, and the editor
  crashes. This is pre-existing engine behaviour identical to the editor's own Recompile button — so
  reserve reload for edits you expect to compile, and use `olo_shader_errors`/`olo_shader_get` to
  inspect a known-broken shader.

## 11. Why `olo_render_toggle_pass` is still deferred

It must be an **ephemeral session override** — not persisted, reset on play-stop. The naive
implementation (flip a `*Enabled` on the active `PostProcessSettings`) is unsafe:
`Renderer3D::GetPostProcessSettings()` returns `s_Data.PostProcess`, which is *both* what the
pipeline reads each frame **and** the storage the editor persists — `SaveScene`, `SaveSceneAs` and
**`AutoSaveScene`** all write it back. AutoSave runs on a timer, so a direct mutation auto-persists
the override into the project.

A correct version needs an MCP-owned override layer applied around `UploadExecutionState` and
stripped on save, mirroring the existing tiering-overlay pattern — which touches the per-frame hot
path and therefore also needs multi-angle visual verification.

## 12. Misc traps

- **The redaction drive-path regex eats URIs.** `[A-Za-z]:[\\/]…` matches the trailing `o://…` of
  `olo://capture/...` (drive "o:" plus path), corrupting URIs to `ol<path>`. `std::regex` has no
  lookbehind — the fix is a captured non-scheme-char prefix restored via `$1`. Any new URI-bearing
  output must survive redaction; there is a pinned test.
- **`FindToolEntry(server.HandleMessage(...), name)` dangles** — the returned pointer points into the
  temporary. Bind the response to a local first.
- **`u8`/`u32`/`u64` are global typedefs**, not `OloEngine::u8` — see
  [notes-core-and-threading.md](notes-core-and-threading.md) §9.
- **Output-schema sweep contract** (58 tools, zero rework): already-JSON tools convert
  `Text(j.dump(2))` → `Structured(j)`; image tools keep their Content array and set
  `StructuredContent = meta`; dual-format tools convert only the JSON path; genuinely free-form tools
  stay text-only with an exempt-list entry. Required = unconditionally-present fields only; **never**
  `.NoAdditional()` on outputs.
