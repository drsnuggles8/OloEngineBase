# Verify scalp clearance while the groom is simulated

Inspect a bound scalp groom through `Scene::OnUpdateRuntime`, with simulation and
collision enabled, before accepting its static editor appearance. A compatible
binding pins the roots; it does not hold each curved shaft above the scalp.

In #1593, an authored bob covered the scanned crown in static captures and passed
cold/warm binding checks. At runtime, stiffness 90 let the scalp-following arcs sag
into the head and across the face. The guide solver's collision proxy did not
reconstruct the scanned scalp closely enough to preserve those arcs. Disabling
simulation would have hidden the failure.

The asset now has 6 mm of authored crown volume, a group stiffness scale of one,
and scene stiffness 2000, damping 12 and gravity `[0, -4, 0]`, within the existing
solver's range. These values are an authoring choice for this short bob, not a
general solver prescription. Non-root controls are projected against the actual
scan; roots remain on its triangles. Cook and bind the pair together after changing
the rest curves.

Keep independent checks for rest clearance, binding identity, guide displacement
and inspected runtime frames. The native rigged-head test runs the full 7.24-second
LookAround clip on every GL raster path and bounds guide displacement by 15 mm;
the rest tips clear the shoulder plane by 24 mm. Those checks catch collapse but
cannot decide whether the hairline, clumps or highlights look convincing.

Live Play mode advances the animation and simulation. Editor Simulate mode advances
physics without advancing this animation; unchanged clip time is not motion proof.
Retain the active scene's observed animation time when capturing a live sequence.
