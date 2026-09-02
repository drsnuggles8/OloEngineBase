# Every SSBO binding stays below 80 — Mesa exposes 80, not the UBO namespace's 84

**Rule:** an `SSBO_*` constant in `ShaderBindingLayout.h` must be strictly below
`SSBO_BINDING_LIMIT` (80). The UBO limit (84, the GL 4.6 minimum for
`GL_MAX_UNIFORM_BUFFER_BINDINGS`) says nothing about storage buffers. When the namespace is full,
fold the new buffer into an existing block as a fixed header or share a dispatch-local number with
buffers that are rebound before every use; never renumber upward.

Issue #1015. Found in CI run 33561256256 and the AMD nightly 33578381676.

## What the driver said

The GL 4.6 minimum for `GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS` is 8. What a driver actually
exposes is a product of its per-stage limits. Mesa gallium drivers (radeonsi on the self-hosted
box's RX 5600 XT, and every Mesa driver with 16 SSBOs per stage) report 5 graphics stages x 16 =
80; compute is not counted. NVIDIA reports 96, which is why bindings 80..83 compiled and ran on
every dev box for months. On the AMD box the GLSL compiler refused the shader outright:

```
layout(binding = 82) for 1 SSBOs exceeds the maximum number of SSBO binding points (80)
```

and every `glBindBufferBase` on an index >= 80 raised `GL_INVALID_VALUE`.

## How it presented

Four bindings sat above the ceiling: `SSBO_TERRAIN_VT_BAKE` (80), `SSBO_TERRAIN_VT_INDIRECTION`
(81), `SSBO_DDGI_PROBE_AUX` (82), `SSBO_DDGI_STATS` (83). The header's own comments called the
namespace "full at 84", on the premise that the UBO minimum bounded SSBOs too.

- **Debug (sanitizer jobs):** `DDGI_ProbeMaintain.comp` failed to compile inside
  `Renderer3D::Init`, which is an `OLO_CORE_ASSERT` and therefore a `SIGTRAP`. 212 test processes
  died with no sanitizer line in any of them. The failing test was whatever ran first.
- **Release (nightly):** the same compile failure left DDGI without a kernel and the terrain VT
  without a bake or indirection dispatch. 13 DDGI tests and 8 terrain-VT tests red every night,
  each failure describing its own subsystem ("no GI", "the VT loop never converges") and none of
  them naming a binding number.

Nothing in the repo could have caught it. `ShaderReflectionBindingTest` checks that a GLSL literal
is not above the highest C++ constant, and 83 was that constant. `SSBOSlotUniqueness` checks that
two constants do not share a number. Neither knew what a driver exposes.

## The fix, and why it is shaped like this

The namespace had no free numbers below 80 (one dead reservation, `SSBO_FOLIAGE_INSTANCES` at 6,
which nothing had ever bound). Two buffers moved by folding, so that two families became two
bindings:

- **DDGI** — the 32-byte stats block became the fixed header of the probe-aux buffer, the same
  header-then-unsized-tail shape `SSBO_VSM_LOCAL_LIGHTS` uses. One binding, `SSBO_DDGI_PROBE_AUX`
  = 6. `Execute` clears the header only; the tail is the sparsity history and must survive.
- **Terrain VT** — feedback, bake and indirection buffers share `SSBO_TERRAIN_VT` = 79. Every user
  rebinds the buffer it needs immediately before its own draw or dispatch, and no VT shader
  declares two of them. That is the dispatch-local pattern the two-phase instance cull already
  uses at 18/19.

Sharing is expressed as one constant, not two equal ones: two constants at one number is exactly
the collision `SSBOSlotUniqueness` exists to catch.

## The guards that exist now

- `SSBO_BINDING_LIMIT` (80) and `SSBO_HIGHEST_BINDING` (a `constexpr` max over every `SSBO_*`),
  with a `static_assert` that the max is below the limit. The list is the mechanism; a hand-pointed
  "top" constant had to be re-pointed six times before this.
- `ShaderBindingLayout.SSBOSlotsFitTheMesaCeiling` pins the limit at exactly 80, so nobody raises
  it to a vendor value and calls the box broken again.
- `OpenGLRendererAPI::Init` queries `GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS` and logs one
  `OLO_CORE_ERROR` naming the driver value, the limit and the highest slot when the driver is
  below the limit. It does not assert: a Release nightly must finish and report.

## When you need a buffer binding next

Read the `SSBO_GPU_STATS` note in `ShaderBindingLayout.h` first. The options, in order: ride an
existing block as a header (#703, #707, #1015 all did); share a number with buffers that are
rebound before every use and never read together; retire a dead reservation. Say which in the
issue before starting, and diff the slot numbers against `master` when rebasing, not just the
files git reports as conflicting.
