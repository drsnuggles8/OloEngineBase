# Every substitution a tenant makes is a seam it stops testing — and they compound

Postmortem of issue **#853**. The filed bug was one defect: `DecalRenderPass`
installs channel-level colour masks per decal mode, and `ApplyPODRenderState`'s
global `SetColorMask` flattens them before the draw. Fixing it turned up **four**
more, in the same feature, all of them live, all of them behind a green suite.

The through-line is not the colour mask. It is that
`VulkanPassSuite.DecalGBufferModeMatrixMasksItsTargetRenderTargets` — a careful,
thorough, 300-line tenant — made **three separate substitutions**, and each one
was hiding a different production bug.

## The tenant, and what each substitution cost

| It substituted | Instead of | What that hid |
|---|---|---|
| `FixtureDecalDispatch` | `CommandDispatch::DrawDecal` | the whole of #853: the fixture never called `SetColorMask` and never ran `ApplyPODRenderState`, so the masks it asserted were the ones the *pass* set, with nothing in between to flatten them |
| a proxy **quad** at NDC z=0.5, in front of the surface | the production decal **cube** | `cullFace = Front` + `depthFunction = LessOrEqual` reject every fragment of a box straddling its receiving surface — **no decal had ever produced a pixel in a real scene, on either rendering path** |
| a hand-built `GBuffer` + a separate depth framebuffer | the real render graph | `DeferredOpaqueDecalPass` declares a `Read` of `SceneDepth`, an attachment view of the *same* framebuffer as the four colour views it writes; the resulting feedback hazards trip an `OLO_CORE_ASSERT`, which is a `__debugbreak` — opening any Deferred scene with an opaque decal **killed the editor outright** |

The tenant's header says the quad is deliberate, and gives a good reason
("coverage exactly once is what the additive emissive mode needs, and it takes
the front-face-winding question off the table"). The reason is sound. The cost
is that the one property a decal cube has and a quad does not — *which of its
faces survive the depth test* — became untested, and stayed untested through the
`GL_LEQUAL`/`GL_FRONT` pair surviving the entire RHI refactor untouched.

**A substitution is a decision about what you are no longer testing.** Write that
down next to it. "One quad, not the production cube" documents the *what*; it
does not say "so nothing here covers the cube's cull/depth pairing", which is
the sentence that would have found this.

## Why nothing else caught it either

The coverage gap was total, and each layer had its own reason:

- **No sandbox scene put a decal on the deferred path.** `TEST_SCENES.md` had no
  decal entry at all. That is also why the render-graph hazard never fired: the
  validation only runs when the graph's *shape* changes, and
  `DeferredOpaqueDecalPass` only declares the offending accesses when it
  actually has decal work.
- **The forward path was equally dead** and equally unnoticed, because
  `Renderer3D::DrawDecal` builds ONE `PODRenderState` for both paths — so the
  cull/depth pairing was wrong in both, and neither had a scene.
- **The one screenshot-shaped gate that exists covers attachment 0.** Same
  coverage condition as
  [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md):
  a defect confined to RT1/RT2 passes every such gate.

## The measurement order that worked

Cheapest decisive step first, and never conclude from silence:

1. **Author the missing scene before touching the fix.** One decal per mode on
   one uniform floor. This is what surfaced the editor-killing hazard within
   seconds — the feature's first real-scene execution is an audit of the feature.
2. **Read the log the user was reading.** `GLStateGuard[DecalRenderPass]: 8 state
   mutation(s) escaped the pass`, listing `ActiveProgram: 0 -> 53`,
   `VAO: 25 -> 28`, `Texture2D[19]: 0 -> 22`, is what turned "the decal draws
   nothing" from a hypothesis into a fact: **the draw is issued, the fragments
   die.** That splits the search space in half and rules out every
   "is it even submitted?" theory at once.
3. **Flip one variable as an experiment, not as a fix.** `LessOrEqual` →
   `GreaterOrEqual`, build, look. `(255, 64, 38)` — the decal's authored
   `[1, 0.25, 0.15]` — appeared in `GBufferAlbedo`. Proven by execution before
   anything was claimed.
4. **Probe channels, do not eyeball composites.** `olo_render_probe_pixel` with
   no `target` returns every G-Buffer RT's raw texel in one call. An
   inside-the-footprint texel against an outside one, per attachment, is the
   whole mode matrix in one table.

## The two green-looking non-results this produced

Both would have read as success to a careless grep, and both came from the test
*harness*, not the code:

- `Running 0 tests from 0 test suites` / `PASSED 0 tests` — the contract tests
  had been added to `Rendering/CommandDispatchTest.cpp`, which is **commented
  out** of `tests/CMakeLists.txt` ("Requires full engine… pulls in OpenGL
  statics"). A filter that matches nothing is not a pass. **Assert the test
  count, not just the absence of failures.**
- `Running 7 tests…` followed by an assertion abort and **no `[ FAILED ]`
  summary at all** — `FrameDataBufferManager not initialized!`. Grepping for
  `FAILED` finds nothing here.

## What the issue got right, and the one thing it could not

#853 read the mechanism end to end and was candid that it had not executed it:
*"I have not captured a decal frame showing the leaked channels. That capture is
the first thing the fix needs."* It also said, correctly, that a capture failing
to show leaked channels would be a real finding.

That is exactly what happened, and the reason is worth keeping: **the leak was
real and simultaneously unobservable.** The masks genuinely were flattened —
provable in the dispatch, and now pinned by seven contract tests — but the draw
they corrupted produced no fragments, so no capture of the shipped build could
ever have shown it. A correct reading of a mechanism does not establish that the
mechanism runs.

## The counter-moves

- When a tenant substitutes the production seam, name the property that
  substitution stops covering, in a comment, next to it.
- A per-draw contract has to be verified through the production dispatch. If the
  full dispatch is unreachable from the tenant's environment (here: Renderer3D's
  statics are live *OpenGL* objects mid-suite, so the Vulkan tenant cannot call
  `CommandDispatch::DrawDecal`), then promote and call the *specific* production
  function that owns the contract — `CommandDispatch::ApplyPODRenderState` — and
  substitute only the parts that are demonstrably orthogonal.
- Build the state the production builder builds. `CreateDecalPODRenderState` is
  shared between `Renderer3D::DrawDecal` and the tenant precisely so the tenant
  cannot pin its own copy of the contract.
- A feature with no scene has no coverage, whatever the suite says. Add the
  scene, then look at the pixels.

## Second instance: a substituted BUFFER CONSTRUCTION, #1052

The archetype repeats with the object under test swapped for a differently-*built*
one, which is harder to see than a substituted call.

`VulkanPassSuite.VirtualGeometryMdiCountDrawsHandAuthoredClusters` pins the
virtual-geometry indirect-draw entry on Vulkan with the real
`VirtualMeshGBuffer.glsl`, and says plainly in its header that the full pass is
"disproportionate headlessly" so it hand-authors the cluster set. Reasonable, and
documented. But it builds the index data as

```cpp
auto indexBuffer = IndexBuffer::Create(indices, 12);   // object-backed
vao->SetIndexBuffer(indexBuffer);
```

while `VirtualMeshRegistry` builds the real one as

```cpp
m_IndexBuffer = RenderCommand::CreateBufferHandle();   // RAW handle
```

and then binds it a second way the test never does — as `SSBO_VIRTUAL_INDICES`.
An object-backed `VulkanIndexBuffer` already carries every usage bit and
registry entry the draw needs; a raw handle is registered in
`RHI::ResourceRegistry` and `VulkanRawBufferRegistry` but *not* in
`VulkanRootObjectRegistry`, the only one `BindStorageBuffer` consulted, and it
lacked the usage bits besides. **That is what made it hard to see: the handle
was valid everywhere anyone thought to look.** It silently bound null, and every
VG scene on Vulkan lost the device. The test could not fail: the buffer it
exercises is not the buffer that breaks.

**The rule this adds.** A substitution is not only "called a different function"
— it is also "built the same object a different way". When a tenant constructs an
input itself, check it against the production *constructor*, not against the
production *type*: same factory, same usage flags, same registry, bound at every
binding point the real path binds it at. `IndexBuffer::Create` and
`CreateBufferHandle` both produce "an index buffer" and share nothing that
mattered here.

**The cheap detector that would have caught it.** `VulkanRendererAPI` already
counts unimplemented-stub hits, and `VulkanDrawPathTest` asserts
`GetUnimplementedStubHitCount() == 0`. No virtual-geometry tenant made that
assertion, so its fall-throughs were counted by nobody. It is one line, and
backend-shaped rather than feature-shaped: it catches the *next* unlowered path
too.

Two aggravating conditions, both worth checking before trusting a Vulkan result:

- **`RendererAttachedTest` creates a GL 4.6 context only.** Every virtual-geometry
  evidence test rides it, so the real pass has never executed on Vulkan in any
  test — a feature with no scene *on that backend* has no coverage there.
- **Check the backend you think you are on.** `driver.ps1 -Action attach` used to
  launch OpenGL unconditionally, so a "Vulkan session" was running GL. It takes
  `-Rhi vulkan` now; either way, `[RHI] Backend:` in `OloEditor/OloEngine.log`
  is the only proof.

Found on #1052 / PR #1054. The *rest* of that issue — why nothing rendered even
after the device loss was fixed — is a second mechanism, in
[no-silent-fallbacks.md](no-silent-fallbacks.md).

## Related

- [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md)
  — issue #823, the archetype #853 is the second instance of, and the document
  that predicted this bug in its closing section.
- [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md) —
  prove a lever acts before concluding anything from its silence.
- [live-verification-noise-floor.md](live-verification-noise-floor.md) — read the
  `liveness` block on every capture.
- [no-silent-fallbacks.md](no-silent-fallbacks.md) — #1052's other half: an
  unlowered entry point whose return value is a legal in-band answer disappears
  into the caller's own degradation branch.
- [testing-architecture.md](testing-architecture.md) — where a tenant belongs and
  what it owes.
