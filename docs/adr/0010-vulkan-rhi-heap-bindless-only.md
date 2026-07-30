# Add a Vulkan backend alongside OpenGL 4.6 — latest point release only, heap-bindless binding, no legacy descriptor-set path, no D3D12/Metal for now

> **Renumbered 0009 → 0010 (2026-07-30).** This document was committed as
> `0009-vulkan-rhi-heap-bindless-only.md` on the same day as
> [ADR 0009](0009-scripting-bindings-from-reflection-emitted-schema.md) and lost
> the race by about ten hours, so two ADRs shared number 0009. Old links to
> `docs/adr/0009-vulkan-rhi-heap-bindless-only.md` will 404; the content is
> unchanged.
>
> The engine-side design decided *within* this scope — the neutral
> resource/binding model, backend selection, and the PSO-cache story — lives in
> [ADR 0011](0011-rhi-neutral-resource-and-binding-model.md) (issue #691 Phase 1).

Issue [#691](https://github.com/drsnuggles8/OloEngineBase/issues/691) is the
phased roadmap for adding a second, modern graphics-API backend to the
renderer. This ADR records the scope decision behind that roadmap and the
reasoning for each boundary, so a future session doesn't have to re-litigate
"why not D3D12" or "why not support older GPUs" every time the epic is picked
back up.

## Decision

- **Keep OpenGL 4.6 (DSA) exactly as it is.** It stays the primary,
  fast-iteration backend. Adding Vulkan is additive, not a replacement.
- **Add Vulkan, latest point release only.** No support window, no version
  fallback — the same hard-floor philosophy the project already applies
  elsewhere (CMake 4.2+, the GCC-16-only reflection experiment).
- **Binding model is `VK_EXT_descriptor_heap`-based bindless, exclusively.**
  No classic `VkDescriptorSet`/`VkDescriptorSetLayout`/`VkPipelineLayout` code
  path, no `VK_EXT_descriptor_buffer` fallback, no descriptor-indexing-only
  bindless. One binding model, fully committed.
- **No D3D12, no Metal, for now.** Two backends is the scope of this decision.

## Why Vulkan, not D3D12

The renderer's shader pipeline already compiles every authored GLSL shader to
SPIR-V targeting Vulkan via shaderc (`Platform/OpenGL/OpenGLShader.cpp:765`,
currently `shaderc_target_env_vulkan` / `vulkan_1_2`), then cross-compiles
that SPIR-V *back* to GLSL via SPIRV-Cross purely so it can run on the OpenGL
backend (`OpenGLShader.cpp:838-907`). `docs/agent-rules/glsl-shaders.md`
already enforces Vulkan-GLSL-compatible authoring rules (UBOs instead of bare
uniforms, explicit `layout(location=N)`, no `gl_FragColor`) for exactly this
reason, and there is a fuzz harness
(`OloEngine/tests/Fuzzing/FuzzSpirvCross.cpp`) pinning the GLSL → SPIR-V →
SPIRV-Cross round trip as production behavior. A Vulkan backend consumes that
SPIR-V directly and *deletes* the cross-compile-back-to-GLSL hop — it does not
add a new shader pipeline, it shortens the existing one. D3D12 has no
equivalent head start (HLSL/DXIL is a different toolchain entirely) and is
Windows-only, which cuts against `OloServer`'s existing Linux/WSL2 story.

## Why heap-bindless (`VK_EXT_descriptor_heap`) over classic descriptor sets

Given the project's existing appetite for hard version floors rather than
compatibility shims, building the binding abstraction around classic
`VkDescriptorSet`/`VkDescriptorSetLayout` — and then migrating to a bindless
model later — would mean designing and shipping the abstraction twice. Vulkan
1.4's core spec (ratified December 2024) did not remove descriptor sets;
`VK_EXT_descriptor_heap` is a separate, newer extension that has been landing
in 1.4 point releases through 2026 and genuinely eliminates descriptor sets
*and* pipeline layouts — exactly one sampler heap and one resource heap,
indexed by raw offset, modeled explicitly on D3D12's heap-based binding.
It was developed jointly by NVIDIA, AMD, Arm, Nintendo, Valve, and Google
(Valve's motivation was fixing Proton's DX12-on-Vulkan translation overhead,
which is itself a signal of how directly the model maps to D3D12). Committing
to it now means the binding layer is designed once, around the model that is
actually where the ecosystem is heading, and — as a deliberate side effect —
keeps the door open for a future D3D12 backend to reuse the same heap+offset
abstraction almost unchanged, since both APIs would then share the same
binding concept.

## Driver floor, and the consequence we're accepting

As of mid-2026, `VK_EXT_descriptor_heap` support is broad but young:

- **NVIDIA** — full support, driver 610+, with Nsight Graphics 2026.2
  debug/capture/replay tooling.
- **AMD** — shipped in Adrenalin 25.30.17.02+ on Windows; merged into RADV
  for Mesa 26.1 on Linux.
- **Intel** — ANV (the open-source Linux driver) has it landing
  **experimental only**, gated behind `ANV_DEBUG=experimental`, targeting
  Mesa 26.2 for non-experimental support.

Given the "latest only, no fallback" scope, this is a real, accepted
consequence rather than a hypothetical: **the Vulkan backend will not run on
Intel hardware until that extension graduates out of experimental on ANV.**
There is no compatibility path planned around this — if Intel support matters
before then, that's a reason to revisit this ADR, not a reason to quietly add
a descriptor-set fallback that undoes the point of committing to one binding
model.

### Correction: the floor is narrower than the list above implies (2026-07-30)

The per-vendor list is about *current* driver rollout and reads as though the
gap closes on its own. It does not, entirely. Philip Rebohle (DXVK maintainer),
writing ~January 2026 about having shipped all of these binding models in
production:

> Driver support. There's a decent chance that this will **never** be usable on
> e.g. RDNA2 on Windows, which is still very relevant hardware, so you'll likely
> need fallbacks for years to come if you want to target that kind of hardware.

**Re-verified 2026-07-30, because a six-month-old prediction should not harden
into an ADR fact** — and this one held, while [its sibling did not](#tooling-floor--vendor-sdk-14357-0-or-newer-added-2026-07-30):

- `VK_EXT_descriptor_heap` shipped in Adrenalin 25.30.17.02, but **RDNA1/RDNA2
  did not get it** in that branch. There is an open AMD driver tracker for
  exactly this ([GPUOpen-Drivers/AMD-Gfx-Drivers#93](https://github.com/GPUOpen-Drivers/AMD-Gfx-Drivers/issues/93)).
- AMD's Windows driver also still disables `VK_EXT_descriptor_buffer` on RDNA2
  and older over severe performance regressions — so the gap is not specific to
  the heap extension.
- DXVK 3.0 now advises RDNA1/RDNA2 users on Windows to stay on 2.x or move to
  Linux, which is a stronger signal than a forum prediction.

One nuance that keeps this from being permanent: the extension reportedly
**requires essentially no new hardware capability**, so this is driver policy on
AMD's Windows stack, not a silicon limit — RADV on Linux exposes it. It could
therefore change, which is a reason to **re-check at Phase 4** rather than to
treat RDNA2 as permanently excluded.

RDNA2 is the RX 6000 generation — a far larger installed base than "Intel on
Linux, pre-Mesa-26.2". The honest statement of this ADR's accepted consequence
is therefore stronger than written above: **the Vulkan backend targets recent
hardware only, and a significant amount of still-current desktop hardware will
not run it for the foreseeable future.**

This does not change the decision, because our situation differs from DXVK's in
the one way that matters: DXVK *must* run everything, so it needs in-process
fallbacks and still carries legacy descriptors ("we're likely unable to get rid
of Legacy descriptors in the next 5+ years"). We already have a universal
fallback — OpenGL 4.6 — and this ADR already commits to keeping it. What the
correction does is make two other decisions look less like preference and more
like requirement:

- **Keeping OpenGL as the primary, fast-iteration backend** is not conservatism;
  for years it will be the only backend a large share of users can run.
- **Shipping both backends in one binary with runtime selection**
  ([ADR 0011 §2](0011-rhi-neutral-resource-and-binding-model.md)) is the only
  workable form. A build-time preset would force a backend choice at cook time
  for hardware the cook cannot see, on a floor this narrow.

Rebohle's overall verdict on the model itself is nonetheless an endorsement of
the direction — *"I'd personally just go for a full bindless model in anything
that isn't some trivial side project, and heaps are (or will be, once tooling
improves) the most convenient way to achieve that by far"* — and on the specific
concern this ADR weighed, that classic descriptor sets would mean designing the
abstraction twice, his account is unambiguous: descriptor buffers are
*"fundamentally just VkDescriptorSet with extra steps"*, whereas under heaps
*"full bindless is trivial and barely requires any setup code."*

### Tooling floor — vendor SDK 1.4.357.0 or newer (added 2026-07-30)

Driver support is only half the maturity question; the other half is whether
the binding model can be *validated*. Committing to heap-bindless exclusively
means there is no classic descriptor-set path to bisect against, so a wrong
heap offset does not fail — it silently samples the wrong resource. CPU-side
validation cannot catch that, because the index is computed in the shader.

That gap closed on 2026-07-28. **Vulkan SDK 1.4.357.0 enables GPU-assisted
validation (GPU-AV) for `VK_EXT_descriptor_heap`**, and adds a "GPU Dump"
validation-layer tool the release notes call out as useful for exactly this
extension. The history is worth knowing, because it explains why this took a
while: CPU-based validation for the extension arrived back in SDK 1.4.341.0,
GPU-AV was targeted at 1.4.350.0 but held back after it was found it could hang
the GPU before the error could be printed.

**This retires the loudest objection to adopting the model now.** The DXVK
account quoted in the correction above listed tooling as a real con — *"lack of
(mature) validation, RenderDoc support etc. Of course this will improve over
time"* — writing ~January 2026. It improved, within six months, exactly as
predicted. When weighing that write-up, discount the tooling con accordingly;
the *driver-support* con is the one that survived re-verification.

**Consequence for #691 Phase 4:** when the SDK is vendored, pin **1.4.357.0 as
the floor**, and say so. This is not "use the newest thing" — the repo pins
vendor dependencies to fixed versions for reproducibility (#294), and this is
the first SDK in which the binding model this ADR committed to exclusively can
be validated at all. Anything older ships the commitment without the instrument.

This note does not change any decision in this ADR or in
[ADR 0011](0011-rhi-neutral-resource-and-binding-model.md); it records a
tooling date the driver-floor picture above is meant to track.

## Consequences

- OpenGL 4.6 remains the only backend that runs on every supported
  configuration; Vulkan is an opt-in, narrower-hardware-floor addition, not a
  universal second option.
- The renderer will, for the duration of issue #691's roadmap, carry two
  backends with a shared render-graph/command-recording layer but two
  genuinely different physical resource and binding implementations — the
  abstraction-hardening work in #691 Phase 2 exists specifically to make that
  tractable rather than the ~620 scattered `glXxx()` call sites it starts
  from.
- No D3D12/Metal decision is made here; a future backend would need its own
  ADR, though the heap-bindless choice above is deliberately friendly to a
  future D3D12 backend if one is ever proposed.
- Re-opening "should we support pre-heap Vulkan / older GPUs" requires
  revisiting this ADR explicitly, not adding a silent fallback path during
  implementation.
