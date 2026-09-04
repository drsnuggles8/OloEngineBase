# A compute pass may update a field in place only while every invocation reads its own texel

Applies to every compute pass that reads and writes the same persistent texture:
`compute/WaterDisturbance_Update.comp`, the snow-depth clipmap, DDGI's probe atlases,
GTAO's denoise ping-pong, and any accumulation buffer added later.

**The rule:** the moment a pass starts reading a NEIGHBOUR — an advection backtrace, a
blur, a gradient, a gather of any width — in-place update becomes a data race between
work groups and the pass must ping-pong: two textures, read the previous, write the
current, swap. Adding a spatial term to an existing in-place pass is therefore a
resource change, not a shader change.

---

## Why it does not announce itself

Work groups within one dispatch have no ordering guarantee and no synchronisation
beyond a work group's own `barrier()`. So when invocation A reads the texel invocation
B is writing, A gets either this frame's value or last frame's, decided by whatever
order the scheduler happened to run them in.

That is not a crash and not a black frame. It is a field in which SOME texels advected
from last frame's data and some from this frame's, in a pattern that follows the
dispatch order — which on a tiled scheduler is bands. It renders. It follows the
camera. It looks like a smearing artefact in the advection maths, and it is
reproducible enough (the scheduler is not random) to look like a bug in the code you
just wrote rather than a hazard in how the pass is wired.

## What this cost in issue #1034

`WaterDisturbance_Update.comp` shipped in #967 updating its field **in place**, and
that was correct: the wake channel decays its own texel and stamps splats onto it, so
no invocation ever looks sideways. Issue #1034 added an advected foam channel to the
same pass, and a semi-Lagrangian step reads at `worldXZ - velocity * dt` — a
neighbour. The field became a ping-pong pair (RG16F → RGBA16F, ×2 = 4 MB) for that one
channel, and the wake rides along at no cost because reading its own texel from the
previous texture is identical to reading it in place.

The hazard was pre-empted by reading the existing pass rather than by observing the
banding, which is the cheap way round: the tell is in the DIFF, not in the frame.
When a compute pass gains a `texelFetch`/`imageLoad` at anything but
`gl_GlobalInvocationID.xy`, check what it writes.

## The two ways to get it wrong while thinking you fixed it

* **A memory barrier does not help.** `MemoryBarrier` orders this dispatch against the
  NEXT one; it says nothing about invocations inside the same dispatch. Reaching for a
  barrier here produces code that looks synchronised and is not.
* **Publishing the wrong half.** With a pair, everything downstream — the sampler
  binding, the handle accessor, any readback — must name the texture the last dispatch
  WROTE. Publishing the other one is a whole frame of stale field, which on a decaying,
  advecting signal reads as a stutter rather than as an error.

## One more constraint, on the bindless route

`BindlessHeap.glsl`'s `OLO_HEAP_IMAGE` **declares and initialises** a local, and
initialising a `readonly` variable is a write (`error C7504`). So the read half of a
ping-pong pair cannot be declared `readonly` on the bindless path, and the slot-based
declaration has to match it — both are plain read-write, and the CPU side binds both
with `RHI::Access::StorageReadWrite`. Four compute shaders silently fell back to the
slot path before that was understood; the note lives at the macro, and this is the
second place it bites.
