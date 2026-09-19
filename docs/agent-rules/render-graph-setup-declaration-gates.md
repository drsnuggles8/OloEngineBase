# A pass whose `Setup()` gates on "do I have work?" must put that gate in the fingerprint

If a render-graph pass returns from `Setup()` without declaring anything when it
has nothing to draw, the emptiness it branched on is a **fingerprint input**. Add
it to `ComputeBlackboardFingerprint`, or the pass renders nothing for as long as
the cached build survives — with no error, no warning and a plausible frame.

This is the other half of
[virtual-shadow-map-page-cache.md §5](virtual-shadow-map-page-cache.md#5-a-render-graph-passs-setup-is-cached--it-must-not-branch-on-a-setting).
§5 says: declare unconditionally, gate the work in `Execute()`. Five passes here
do the opposite. §5 tells you how to write a new pass; this page is what to do
about the ones that already branch, and how to recognise the failure.

Issue #1315 is the worked example.

## The mechanism, in four lines

```cpp
void FoliageRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
{
    if (m_CommandBucket.GetCommandCount() == 0)
        return;                     // <-- no reads, no writes, for this build
```

`RenderGraph::BuildFrameGraph(fingerprint)` returns early on a fingerprint hit
and **does not re-run any pass's `Setup()`**. So a graph compiled during a frame
with no foliage caches a node that declares nothing, and reachability culls it.
Gaining the first plant moves no other fingerprint input, so the cache keeps
hitting, `Setup()` never runs again, and the pass stays culled while its bucket
fills every frame.

## The failure signature

**The subject is absent from an otherwise correct frame.** Not corrupted, not
mis-shaded — absent. In #1315 the foliage test rendered the same terrain, the
same lighting and the same splat pattern as its golden, with not one plant.

Three properties make it expensive to chase:

- **Every producer-side number is right.** The capture that solved it showed
  1310 instances generated, two draws submitted per frame, valid VAOs and valid
  indirect buffers — every frame. Only `FoliageRenderPass::Execute` was missing
  from the log entirely. Instrument the *consumer*, not the producer: this class
  is invisible from the submission side, which is where everyone starts.
- **It passes in isolation.** Alone, the first frame that builds the graph
  already has draws, so the pass declares and is never culled. It needs an
  earlier frame with an empty bucket, which in a test binary means an earlier
  *test*.
- **It heals itself on the next rebuild.** In #1315 the test switched rendering
  path after its first cell; that moved the fingerprint, `Setup()` re-ran, and
  the remaining cells were perfect. So "0% coverage on every path", the symptom
  as originally filed, was actually 0% on the *first* path only — and a bug that
  fixes itself partway through its own reproducer invites the conclusion that
  the state is transient, which it is not.

## Recognising it in one run

Log `Setup()`'s gate value, `Execute()`'s entry, and the fingerprint. The shape
that names this class is a `Setup` that ran with the gate closed, then no
`Execute` at all while work is submitted:

```
OLO1315 Setup:   commands=0            <- graph compiled here, pass declared nothing
OLO1315 submit:  layerInfos=2 ...      <- x28 frames, every one with work
                                        (no Execute line anywhere)
OLO1315 Setup:   commands=2            <- an unrelated input finally moved
```

Do not bisect for a corrupted global first. Four reasoned hypotheses were spent
on #1315's "leaked terrain state" framing — wind accumulation, render origin,
renderer configuration, the GPU-driven LOD toggle — and the issue's own
"tessellation is the common factor" reading was true but incidental. The trigger
is not what corrupted something; it is merely a scene that rendered a frame with
your pass empty.

## The two ways to fix it, and when each is right

**Hash the gate** (what #1315 did) when the pass's declarations are a real
topology change that most frames should not pay for. Foliage, decals and the
deferred forward overlay all declare a SceneColor read-modify-write with a
version rename; declaring that unconditionally would add a graph edge and a
rename to every frame in every scene, including ones with no such geometry at
all. Water was already hashed this way, one line above where the others now sit,
which also makes the file self-consistent.

Hash a **boolean**, never the count. The declaration branches on emptiness; a
count rebuilds the whole frame graph every time one plant comes into view.

**Declare unconditionally** (§5's rule) when the declarations are cheap and
unconditional anyway — a pass that always reads the same input and writes the
same target, and only the *work* is conditional. That is strictly safer and
needs no fingerprint maintenance, so prefer it for anything new.

## The passes that branch today

Seven, and every live gate is hashed in `ComputeBlackboardFingerprint`:

| pass | gate | hashed |
|---|---|---|
| `WaterRenderPass` | command bucket empty | yes, before #1315 |
| `GroomRenderPass` | request list empty | yes, by #1246 |
| `FoliageRenderPass` | command bucket empty | **#1315** |
| `DecalRenderPass` | command bucket empty | **#1315** |
| `ForwardOverlayRenderPass` | command bucket empty (plus the path, hashed already) | **#1315** |
| `FluidIntermediatesPass` | `!m_Enabled \|\| m_FrameDraws.empty()` | the draws half only |
| `FluidCompositePass` | `!m_Enabled \|\| !intermediates->HasPendingDraws()` | the draws half only |

The count is the point: water was hashed, then groom was hashed for its own
issue, and the three in between were left. Each fix saw only its own pass.

**The two fluid rows are a live trap with the fuse pulled.** Their `m_Enabled`
half is *not* in the fingerprint, and it cannot bite today only because
`SetEnabled` has no caller anywhere in the engine — `m_Enabled` is stuck at its
`true` default, so the gate reduces to the draws half, which is hashed. Wire
that setter up and you have this bug again, in a pass whose own comment already
says "the pipeline fingerprint must hash this gate". Hash it in the same commit.

`RenderGraphFingerprint.EveryBucketGatedStreamPassChangesFingerprintOnFirstDraw`
pins the four bucket-gated passes by accounting — a pure-CPU assertion that the
fingerprint moves when a bucket gains its first draw — so the next pass to adopt
this shape fails a named test rather than a golden. Groom and the fluid pair are
gated on state that does not live on the pass (a `Renderer3D` static, a sibling
pass's draw list), so they are covered by the fingerprint lines and not by that
loop. Give one of them a bucket and it belongs in the loop.

**Adding a pass that branches in `Setup()`? Add its gate to both.** A gate in
the fingerprint with no test entry is the state this class keeps coming back
from.
