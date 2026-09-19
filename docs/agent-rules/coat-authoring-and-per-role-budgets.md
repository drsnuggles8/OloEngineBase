# Coat authoring: roles, per-role budgets, and measuring "not uniform"

Applies to: `OloEngine/src/OloEngine/Groom/**`, `OloEngine/tests/Rendering/PropertyTests/FurCoat*`

A groom's strand budget must be spent **per role**, not as one global stride, or the first thing a
budget cut removes is the animal's outline. And the claim "this coat is not uniformly fuzzy" must be
measured with a statistic the mechanism actually predicts — two plausible ones move the wrong way.

Written after #1251. The rules are first; the measurements that taught them follow.

---

## 1. A coat group needs a ROLE, and the role belongs in the asset

`GroomCoat.h` defines `GroomCoatRole` (Unassigned, Undercoat, GuardHair, Whisker, LongHair) and a
per-group `GroomCoatGroupDesc` carrying it plus density, length, width, clump and tint. The table is
**cooked** into the `.ologroom` as section 9, parallel to the group-name table.

Why the asset and not the component: a coat is a property of how the groom was groomed, and the
component is per-entity. The component carries **bounded overrides** on top — and only for Undercoat
and GuardHair, which is what the issue's "adjusted independently" names. Whiskers and mane are
authored in the asset, where a twelve-strand group has no useful density slider.

**Adding an array to `GroomAsset` means editing `GroomCooker::CookToBytes`.** It copies field by
field rather than cloning, so an array it does not name is dropped by the cook alone: the loose asset
keeps it, the cooked one does not, and the difference is a coat that is subtly too uniform.

---

## 2. Spend the strand budget per role, weighted

`GroomCoatBudgetWeight` gives Whisker 64, GuardHair 8, LongHair 4, and 1 to the rest. `SelectCurves`
solves, by bisection, for the `k` with `sum_r count_r * min(1, k * w_r) == budget`, and turns each
role's retained fraction into its own stride.

On a groom with **no** roles every curve is Unassigned, every weight is 1, and the solution is the
single stride the build used before — which is why `AGroomWithNoRolesBehavesExactlyAsItDidBefore`
asserts on the exact stride, not just on "something sensible".

Measured on the long-coated reference at 12000 → 1500 strands, the fraction of the animal's **outline**
that survives:

| budget allocation | outline retained |
|---|---|
| single global stride | 0.320 |
| per-role weighted | 0.595 |

Same groom, same geometry, same budget. The only difference is which strands the budget buys.

---

## 3. Key everything on the root UV and the curve index — nothing else

`EvaluateGroomCoatStrand` takes a curve index, a root UV and a group id. It cannot see a pose, a
world position, a frame counter or a camera. That is the whole of "regional maps remain attached
under body deformation": the coat's shape changes are applied in **rest space**, and the binding's
root transform is applied to the result. A regional map cannot slide off a running animal because it
was never consulted in a space the body moves.

The strand build must apply them in that order — `ApplyGroomCoatShape` first, `ApplyGroomRootTransform`
second. Reversing them is not a compile error and the bind pose looks identical.

`ApplyGroomCoatShape` returns the root **exactly** at `t == 0`, bit for bit, for both the length
scale and the clump pull. A root that drifted by an ulp would drift differently in every pose.

---

## 4. A per-vertex payload bit-cast to a float must not be a denormal

The per-strand tint rides in the strand vertex's one spare float lane as 8:8:8. A bare 24-bit payload
bit-cast to a float **is a denormal** for every tint whose blue channel is below 0x80 — and a vertex
pipeline may flush denormals to zero. A saturated red tint would arrive as 0.0 and the strand would
render black, on some drivers and not others, with nothing in any log.

`PackGroomCoatTint` forces the top byte to `0x3F`, which puts every payload in `[0.5, 2)`: always
normal, never NaN. The shader masks the exponent off again (`floatBitsToUint(x) & 0x00FFFFFFu`).

The range is `[0.5, 2)` and **not** `[0.5, 1)`: the payload is 24 bits and the mantissa is 23, so the
blue channel's top bit lands in the exponent's low bit. Both exponents are normal. A test asserting
`< 1.0f` fails on every tint with blue ≥ 0x80.

---

## 5. Measuring "not uniformly fuzzy": two statistics that move the wrong way

This is the part that cost the most time, because both wrong measurements produce a plausible number.

### 5a. Absolute variance falls when a multiplicative tint is applied

The obvious test for "the coat has more variation" is the variance of coat luminance. It **fell**
when shade jitter went from 0.0 to 0.6, on a frame where 27 000 pixels visibly changed.

A per-strand tint is a *multiplicative* perturbation, and a tint cannot push an albedo above the
authored one — so the upper half of a symmetric jitter clamps at white and only the lower half bites.
The coat darkens. Absolute variance scales with the square of the mean, so a 25% darkening costs it
44% while the relative variation grew by a few per cent.

**Use the relative spread — standard deviation over mean — for a multiplicative effect.** Never a
ratio of two tone-mapped means across frames (`groom-fibre-scattering.md` rule 13); a spread *within*
one frame is fine.

### 5b. Comparing two frames' coats selects two different pixel sets

"The authored coat has more structure than the flat one" measures over the pixels each frame's *own*
coat occupies — and an authored coat with longer, thicker guard hairs occupies a different set,
covering more dark background. Across six capture cells the statistic went both ways for that reason
alone.

**When an A/B changes the geometry, the pixel set is part of the result, not a fixed frame of
reference.** Either hold the geometry fixed (shade jitter changes no geometry at all, so its two arms
draw the same strands and the comparison is controlled) or assert on something the selection cannot
move.

### 5c. Interior pixel counts are saturated; outline counts are not

The first silhouette test compared the *outline's* retained fraction against the *whole coat's* under
one budget cut, expecting the outline to survive better. It does not — and not because the weighting
is broken. Thousands of undercoat strands land on the same pixels, so removing half of them barely
moves the interior count; the sparse, non-overlapping outline responds almost linearly. The test was
measuring that saturation.

**Compare like with like: two budget allocations, one metric.** With the coat disabled every curve is
Unassigned, so the build falls back to the single global stride — which is exactly the control arm
the per-role claim needs. That comparison gave the 0.320 vs 0.595 in §2.

---

## 6. Checklist for the next coat change

- New `GroomAsset` array → add it to `GroomCooker::CookToBytes`, to the serializer's section list,
  to `GroomAsset::Validate`, and bump `OloGroomFormat::CurrentVersion` **and** `MinSupportedVersion`
  (a `.ologroom` is a derived artifact; old files are rejected by version, not migrated).
- New `GroomStrandBuildSettings` field → add it to `GroomRenderPass::CacheKey`, which hashes field by
  field on purpose. `GroomStrandMeshTest.TheCacheKeySeparatesSettingsThatProduceDifferentMeshes`
  catches forgetting.
- New `GroomCoatSettings` field → add it to `GroomCoatDigest`, or the slider does nothing until
  something else invalidates the geometry.
- New section in the binary format → `GroomCookDeterminism.EveryEnumeratedSectionIsWrittenExactlyOnce`
  pins the section count deliberately. Bumping it is the moment to check the encoder, the decoder's
  fixed order, the payload-size arithmetic and both version constants.
