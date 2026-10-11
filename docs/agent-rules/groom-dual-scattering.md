# Dual scattering in a dense coat (#1533)

Read before touching `GroomFibreComputeDualScattering` / `GroomFibreBackScatterProjected`
(`Groom/GroomFibreScattering.{h,cpp}`), `GroomCoatShadow::CoatForwardTransmittance`, the
`oloGroomShadeFibre` / `oloGroomComposite` half of `GroomStrand.glsl`, or a coat that looks grey,
flat or plush.

## The rules

1. **A crossing forwards light; it does not destroy it.** The #1248 volume counts the fibres
   between a strand and a light, and on its own it treats each one as an opaque, colourless
   occluder. A pale fibre forwards most of what it intercepts, through TT, in its own colour. So the
   light that reaches a strand is two parts: the part no fibre intercepted
   (`oloGroomCoatTransmittance`) and the part other fibres forwarded
   (`oloGroomCoatForwardTransmittance` minus that). Shade both. Treating crossings as opaque turns a
   golden coat grey and its depths charcoal. On the dog showcase that read as plush.

2. **`tau` stays colourless. Colour enters once per crossed fibre, never the shaded one.** The
   per-channel survival of a crossing is `1 - p (1 - a_f)` with `p = 1 - exp(-kappa)`, and its
   Poisson mean is the #1360 generating function at that survival. `a_f` is the pigment of the fibres
   the light CROSSED. `exp(-sigma_a * chord)` in the BCSDF is the shaded fibre's own pigment. They are
   different fibres on one path, so this is not the double count
   [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md) rule 1 forbids. With `a_f = 0` the
   formula is exactly that rule's term, and a test pins the reduction.

3. **In a coat, the sky's TT comes from behind, and only its unscattered part counts.** The
   uniform-environment approximation hands every lobe its full albedo, TT included. TT is light from
   behind the fibre, which in a coat is more coat and then the body. With a volume bound, R, TRT and
   half the residual see the sky on the viewer's side; TT and the other half see it along the
   opposite direction, marched through the coat. Behind, use the colourless transmittance only.
   The volume holds strands, not the body, so pale fibres' forwarding along that ray would light the
   front of the animal with the sky behind it. Before this, that uniform TT term was most of the
   dog's brightness, and it lit the coat evenly from everywhere, which is the flat look.

   **The light the coat behind returns is A_b, and only as much as there is coat behind** (#1558):
   the sky's back-scatter is scaled by `1 - T` of that same march. Where the march leaves the coat
   into the sky (a fringe, an ear's edge, the tail), TT already counts the sky it meets, and A_b at
   full strength counted the same light twice: a coat of clear fibres read 1.33 of the sky lighting
   it.

4. **Dual scattering runs only where a volume counts the neighbours, and it has its own switch.**
   The `.w` density factors in `FibreForwardScatter` / `FibreBackScatter` go up zero. Only the
   coat-volume success path in `GroomRenderPass` writes them (`d_f` Zinke's 0.7, `d_b` 1, rule 8),
   and only when
   `GroomCoatShadowComponent::m_MultipleScattering` is on (the default). A coat without a volume is a
   lone-fibre picture and stays #1247's exactly, the same structural fallback as `CoatModes.x`.
   #1248's occlusion evidence, and the horse fixture graded before this, run with the switch off.
   Their claims ("the volume darkens a dense coat") are about crossings as occluders, and the
   transport brightens a pale coat on top. `MultipleScatteringWarmsAndBrightensAPaleCoat` is the
   transport's own evidence.

5. **A pale coat is authored with PALE fibres.** `BaseColor` still inverts against the single
   fibre's albedo ([groom-fibre-scattering.md](groom-fibre-scattering.md) rule 12). Dual scattering
   then compounds that colour with every crossing, so the coat renders more saturated than the fibre.
   That is right: a golden coat is made of nearly clear fibres. The dog uses `(1.0, 0.93, 0.80)` at
   intensity 1. Authoring the coat's final gold as the fibre colour renders orange. Nearly clear is
   not clear, though: at `(1.0, 0.97, 0.90)` the forwarded light came back almost white, the lit side
   washed out to beige in the tonemapper's shoulder, and the only gold left was in the self-shadowed
   depths, which read as brown blotches. An intensity above 1 was compensation for the missing
   transport; do not bring it back.

   **Near 1, a hundredth is a colour.** With A_b summed (rule 7) a clear channel returns all of its
   light from the coat's depths, and one at 0.97 returns half of it: a fibre colour of `(1.0, 0.99,
   0.97)` turned a white coat pink in the shade, and the golden's `(1.0, 0.93, 0.80)` went orange.
   A white coat's fibres are neutral `(1, 1, 1)`, its roots near clear, and its creams the map's; the
   golden's red is 0.985, which returns what its graded coat did. Check a fibre colour's A_b with the
   constants before grading by eye.

6. **The coat tint is pigment and never colours R.** `oloGroomComposite` tints TT, TRT, the
   residual and the multiple back-scatter, and leaves R white. R is the cuticle's surface
   reflection. #1251's first form tinted the whole sum, which coloured the sheen with the coat and
   read as matte ([groom-fibre-scattering.md](groom-fibre-scattering.md) rule 9 already said so).

7. **The constants are derived per groom from the SAME quadrature the shader renders.** `a_f` / `a_b`
   use the shipped h nodes, the attenuations and the node-widened azimuthal lobes, with the backward
   mass from the trimmed logistic's closed-form CDF. They are averaged over the sphere (4-node
   Gauss-Legendre in `sin theta`). `GroomFibreDualScatteringTests` pins that the split partitions
   the albedo exactly and matches the far field integrated over each half-space. `A_b` is Zinke's
   series summed to every order: the reflectance of a stack of layers that each reflect `a_b` and
   transmit `a_f`, less the first layer's, whose expansion is exactly `A1 + A3 + O(a_b^5)`. The paper
   cut it after `A3`; for a clear fibre that kept 0.51 of the 0.85 the stack returns. The back lobe
   integrates to it. `GroomCoatTransmittanceParityTest` pins both GLSL twins against the compiled
   shader.

8. **A coat that absorbs nothing returns the sky, and never more.**
   `GroomEnvironmentFurnaceEvidenceTest.ACoatOfClearFibresReturnsTheSkyThatLightsIt` holds a clear
   coat under a uniform sky between 0.84 and 1 of it at its bright end, and above 0.65 at its median.
   That is why `d_b` is 1: the paper's 0.7, fitted to its cut series on heads of hair, lost a further
   30 % in that furnace. Each model this replaced fails a bound there, and the test lists the numbers.

## Declared approximations

- The forwarded light is shaded with the unwidened single-scatter lobes. Zinke widens them by the
  spread it picked up, so highlights in the coat's depths are a little crisper than they should be.
- The back lobe is one Gaussian in `theta_h` for all three channels, with a cosine over the backward
  azimuths instead of a constant.
- The coat behind a strand is marched through the coat only, so on a strand's near side it counts
  the far side's coat beyond the body too: A_b is at full strength over any body, thin coat or not.
- The direct lights' back-scatter is not scaled by the coat behind them; that would cost a march per
  light.

## Diagnosing

Fibre debug mode 6 (`MultipleScattering`, the editor's "Multiple scattering") shows the local
back-scatter alone. On a pale coat it should be a large, warm share of the frame. It is zero
without a volume.
