# Groom LOD: the cross-fade across a hand-over, and the global detail bias (#1612, #1616, #1628)

The rules for changing how a groom moves between its tiers. The ladder itself, the budgets and the
width compensation are in [groom-representation-lod.md](groom-representation-lod.md). The stories
and measurements are in [the details file](groom-lod-cross-fade-and-detail-bias-details.md).

## The rules

1. **Hide a hand-over by drawing both tiers through complementary masks from one per-pixel hash.**
   Scene publishes a fading coat twice, the primary on the incoming tier (keeps `h < t`) and a copy
   on the outgoing one (keeps `h >= t`), with `t = frame / (frames + 1)` and `h` independent of the
   segment. [Details](groom-lod-cross-fade-and-detail-bias-details.md#1-complementary-masks-from-one-per-pixel-hash).

2. **Every pass that rasterises a tier applies the mask, from the same two numbers.** The strand
   shader discards before its coverage test; the casters discard per shadow texel in a
   `*CrossFade.glsl` variant bound only for a fading caster; the lane comes from
   `MakeGroomCrossFadeLane`. [Details](groom-lod-cross-fade-and-detail-bias-details.md#2-every-pass-that-rasterises-a-tier-applies-the-mask).

3. **The outgoing copy is the same coat, not a second one.** Everything counted per groom skips it
   (`GroomStrandRequest::IsCrossFadeSecondary`), and so does the ray-traced proxy; strands, segments
   and bytes count it, because they are cost. Its VSM caster key is its own.
   [Details](groom-lod-cross-fade-and-detail-bias-details.md#3-the-outgoing-copy-is-the-same-coat).

4. **Run the per-entity Scene work once, then copy the whole primary and restore the partner's own
   fields** (`AdoptGroomCrossFadePrimary`): its tier, which includes its visibility step, and its
   streaming. Restore that short list; never mirror what deformation writes. Read roots through
   `DrawnRoots()`. [Details](groom-lod-cross-fade-and-detail-bias-details.md#4-run-the-per-entity-scene-work-once).

5. **A hand-over the budget cannot fade is a hard switch, reported by name.**
   `RendererSettings::GroomLodMaxCrossFades` (4) bounds a transition frame. Halvings and
   representation hand-overs share it, and a coat holds at most one window; a change mid-window
   restarts it against the tier that showed more of the coat. A refusal sets
   `CrossFadeRefused` (and `CrossFadeBudgetSpent`, not a fallback). A switch back mid-window
   reverses the window. [Details](groom-lod-cross-fade-and-detail-bias-details.md#5-the-budget-and-what-a-refused-hand-over-does).

6. **A test that forces a tier holds the cross-fade off; one that walks to it waits the window out**
   (`6 + GroomLodCrossFadeFrames` frames). [Details](groom-lod-cross-fade-and-detail-bias-details.md#6-tests-that-force-or-walk-to-a-tier).

7. **The detail bias divides every pixel threshold once, where Scene assembles the policy.** Not
   the hysteresis, the width cap or the cooked `SourcePixelSize`; bias 1 is bit-identical to none.
   [Details](groom-lod-cross-fade-and-detail-bias-details.md#7-the-detail-bias).

8. **Judge a fading shadow by its step, not its depth**, against a floor the series itself shows.
   A doubled shadow on a dense coat is no darker than one tier's.
   [Details](groom-lod-cross-fade-and-detail-bias-details.md#8-judge-a-fading-shadow-by-its-step).

9. **Draw a fading coat's pair prepass, prepass, shade, shade.** A tier whose prepass runs after the
   other tier's shading loses its early depth rejection (2.2 ms against 0.62 ms on the dog).
   [Details](groom-lod-cross-fade-and-detail-bias-details.md#9-draw-a-fading-pair-prepass-prepass-shade-shade).

10. **Advance the LOD ladder and the fade window once per frame, not once per publish**
    (`Scene::BeginGroomFrame`); a later publish in the frame reuses the frame's decisions.
    [Details](groom-lod-cross-fade-and-detail-bias-details.md#10-once-per-frame-not-once-per-publish).

11. **A VSM caster that stops fading counts as moved for one frame.**
    [Details](groom-lod-cross-fade-and-detail-bias-details.md#11-a-vsm-caster-that-stops-fading).

12. **Only a coat that can reach the view opens a fade window**; a coat out of every view drops its
    window and comes back settled on the tier it then draws. [Details](groom-lod-cross-fade-and-detail-bias-details.md#12-only-a-coat-that-can-reach-the-view-opens-a-window).

13. **A halving is a hand-over: a tier is a representation and a visibility step.** A step change
    opens the same window, with the outgoing copy on the step the coat was last built on, so it is
    the geometry cache's entry. Two steps that both build every curve of the tier
    (`GroomCrossFadeStrands`) are one tier and open nothing. A coat seen for the first time starts
    settled on the tier it draws (`SettledGroomCrossFade`).
    [Details](groom-lod-cross-fade-and-detail-bias-details.md#13-a-halving-is-a-hand-over).
