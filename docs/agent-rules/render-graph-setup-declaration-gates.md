# A pass whose `Setup()` gates on "do I have work?" must put that gate in the declaration key

If a render-graph pass returns from `Setup()` without declaring anything when it
has nothing to draw, the emptiness it branched on is a **declaration input**.
Report it from the pass's `AppendDeclarationInputs`, through the same accessor
`Setup()` gates on, or the pass renders nothing for as long as the cached build
survives — with no error, no warning and a plausible frame. Since #1333 the
mechanism is [render-graph-declaration-config.md](render-graph-declaration-config.md);
this page is the worked example that motivated it.

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

**Key the gate** (what #1315 did, and since #1333 through `AppendDeclarationInputs`) when the pass's declarations are a real
topology change that most frames should not pay for. Foliage, decals and the
deferred forward overlay all declare a SceneColor read-modify-write with a
version rename; declaring that unconditionally would add a graph edge and a
rename to every frame in every scene, including ones with no such geometry at
all. 

Report a **boolean**, never the count. The declaration branches on emptiness; a
count rebuilds the whole frame graph every time one plant comes into view.

**Declare unconditionally** (§5's rule) when the declarations are cheap and
unconditional anyway — a pass that always reads the same input and writes the
same target, and only the *work* is conditional. That is strictly safer and
needs no declaration-input maintenance, so prefer it for anything new.

## The passes that branch today

Every gate below reaches the key through the pass itself, so nothing depends on
a central list remembering it (#1333):

| pass | gate | reported by |
|---|---|---|
| `WaterRenderPass` | command bucket empty | `HasSubmittedCommands()` in `AppendDeclarationInputs` |
| `FoliageRenderPass` | command bucket empty | same |
| `DecalRenderPass` | command bucket empty, OIT on | same, plus `m_OITEnabled` |
| `ForwardOverlayRenderPass` | command bucket empty (the path is a config field) | same |
| `GroomRenderPass` | request list empty | `m_Requests.Num() != 0` |
| `FluidIntermediatesPass` | `!m_Enabled \|\| !HasPendingDraws()` | `IsEnabled()` + `HasPendingDraws()` |
| `FluidCompositePass` | `!m_Enabled \|\| !intermediates->HasPendingDraws()` | `IsEnabled()` + the sibling's draws |
| `ParticleRenderPass` | no render callback | `HasRenderCallback()` |
| `OITPrepare/ResolveRenderPass` | no contributor | `m_HasContributors` |

The count is why the mechanism changed: water was hashed, then groom was hashed
for its own issue, and the three in between were left, because each fix saw only
its own pass. The particle callback and the OIT contributors were still missing
when #1333 took the inventory. The fluid pair's `m_Enabled` half was an unhashed
trap waiting for a caller of `SetEnabled`; it is keyed through `IsEnabled()` now.

`RenderGraphFingerprint.EveryBucketGatedStreamPassChangesFingerprintOnFirstDraw`
pins the four bucket-gated passes by accounting, and
`RenderGraphFingerprint.ParticleCallbackAndOITContributorsMoveTheKey` the rest.
For a gate nobody wrote a test for, the verifier
(`OLO_RG_VERIFY_DECLARATION_CACHE`) reports the pass by name.

**Adding a pass that branches in `Setup()`? Report the gate from its
`AppendDeclarationInputs`, and read it through the same accessor in both.**
