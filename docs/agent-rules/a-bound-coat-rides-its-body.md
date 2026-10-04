# A bound coat rides its body

**Parent a bound groom's entity to the entity its binding targets.** A coat whose entity stays put
while its body moves still renders correctly, but every root of it is far from the coat's own
origin, and the shadow caster subset (#1533, `GroomCasterPose`) gives up its saving there.

## Why

The caster subset casts the share of a coat group a shadow view's texels need, from each root
triangle's frame in **the coat's space** (`PoseGroomCasterRunsBySurface`). The GPU evaluates those
frames from positions in that space, and f32 rounding grows with magnitude, so each frame is trusted
only to `kGroomCasterPositionTolerance * (1 + |position|)`. The frame error is charged against the
run's claimed projected length. Far from the coat's origin, every triangle carries a large error and
the views cast nearly the whole coat. The answer is conservative, never wrong, so nothing fails; the
frame just costs more.

## The failure that taught it

The dog's walk gained root motion, which moves the dog's **entity**. `DogCoat` was a root entity at
the world origin, bound to the dog through `GroomBindingComponent::TargetEntity`, so as the dog walked
away its roots moved metres from the coat's origin. The walk framing's shadow views then cast 0.989 of
the coat where the walk on the spot cast 0.655, and the coat's caster GPU time rose from 1.76 to
2.42 ms. With the coat parented to the dog, the walking dog casts 0.753, and with the guide
simulation off it casts the same as on the spot (0.580 and 0.582). The remaining 0.1 is the
simulation's real response to the travel, which the bound is right to pay for.

`DogShowcaseEvidenceTest`'s cost test has the switches that attribute this:
`OLO_DOG_COST_SUB=inplace` (the walk on the spot) and `fixedcamera` (the camera does not ride the dog).
