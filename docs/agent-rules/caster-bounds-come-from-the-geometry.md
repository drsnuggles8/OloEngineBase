# A conservative bound is computed from the geometry it bounds

**Rule.** A quantity used as a one-sided bound (a projected-length lower bound, a box that must hold
every point) is computed from the actual geometry or from a derivation that covers it. Never from an
estimate, and never from a related object's bound. A scaled sample is an estimate. A bone's image of a
strand bounds what that bone does, not what the strand's root frame does. Aggregate moments bound
projection along one direction, not along a fan of rays. Where the geometry cannot be seen, pay the
uncertainty as lost credit or a wider box, and test the bound against the geometry, not against its
own arithmetic.

## What happened (#1533 review)

A shadow view thins each run of a coat by `L - d^T M d`, a lower bound on its projected length that
holds when `M` is the run's actual second moments and every segment is seen along `d`. Three inputs
broke that:

- **Sampled moments.** A bound coat's moments were the rest moments plus a 128-strand sample's
  change, scaled to the run's length. It was described as unbiased, which a length-weighted ratio
  over unequal strands is not, and it is not one-sided in any case. 128 unit strands turned from x to y
  beside one unsampled strand of length 9 posed to `xx = -9` and claimed 146 across x where the run
  projects to 137.
- **Bone images plus 15%.** A strand rides its root triangle's orthonormal frame. A triangle blended
  between bones turned +60 and -60 degrees shrinks without turning, so its strand keeps its full
  reach while each bone's image of it reaches half.
- **Corner rays over aggregate moments.** Strands on a lamp's own rays project to points, but every
  single direction sees most of them across. The minimum over the box's corner rays credited a fan
  with 19.8 NDC of a true 0.0002.

The fix poses each run exactly, root triangle by root triangle. It pays the GPU's frame uncertainty
(measured on the real kernel: within 0.004 of the tolerance), ill-conditioned frames, held strands
and the simulation's displacement as credit or box. It reads the moments along the centre's ray and
charges the box's angular spread. Each counterexample is now a regression test checked against the
deformed or projected geometry. Each was reproduced on the old functions before the fix, and two of
the scenarios needed reshaping before they actually failed the old rule.
