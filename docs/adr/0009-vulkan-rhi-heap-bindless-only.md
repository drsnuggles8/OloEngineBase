# Add a Vulkan backend alongside OpenGL 4.6 — latest point release only, heap-bindless binding, no legacy descriptor-set path, no D3D12/Metal for now

Issue [#691](https://github.com/drsnuggles8/OloEngineBase/issues/691) is the
phased roadmap for adding a second, modern graphics-API backend to the
renderer. This ADR records the scope decision behind that roadmap and the
reasoning for each boundary, so a future session doesn't have to re-litigate
"why not D3D12" or "why not support older GPUs" every time the epic is picked
back up.

## Decision

- **Keep OpenGL 4.6 (DSA) exactly as it is.** It stays the primary,
  fast-iteration backend. Adding Vulkan is additive, not a replacement —
  ripping OpenGL out is a separate, much larger decision this ADR does not
  make.
- **Add Vulkan, latest point release only.** No support window, no version
  fallback — the same hard-floor philosophy the project already applies
  elsewhere (CMake 4.2+, the GCC-16-only reflection experiment).
- **Binding model is `VK_EXT_descriptor_heap`-based bindless, exclusively.**
  No classic `VkDescriptorSet`/`VkDescriptorSetLayout`/`VkPipelineLayout` code
  path, no `VK_EXT_descriptor_buffer` fallback, no descriptor-indexing-only
  bindless. One binding model, fully committed.
- **No D3D12, no Metal, for now.** Two backends is the scope of this decision.

## Why keep OpenGL instead of a straight cutover

A straight OpenGL → Vulkan cutover would be a much bigger, much riskier
decision than "add a second backend," and it buys nothing this roadmap needs:
OpenGL is fully built, its state-change cost model is cheap enough to keep it
the fast-iteration backend during the years-long process of porting every
render pass, and every engine subsystem still targets it as ground truth for
golden-image comparisons while Vulkan is brought up pass-by-pass (issue #691,
Phase 7). Dropping it would also remove the one backend that currently runs
everywhere, at the exact moment the Vulkan backend is deliberately choosing a
narrow, bleeding-edge hardware floor (see below) — the two decisions pull in
opposite directions and shouldn't be bundled.

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
Metal is out of scope because the project has no macOS/iOS target today.

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
