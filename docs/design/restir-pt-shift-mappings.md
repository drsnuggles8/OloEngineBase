# ReSTIR PT: suffix measures, reversible shifts, and resampling

Design contract for #1211, written before production code. This extends
[GI reconnection](restir-gi-reconnection-shift.md), following the
[measure rule](../agent-rules/resampled-estimator-measure-convention.md).
Section numbers are stable. Implementation and measured validation are separate
deliverables; this note does not establish that an estimator has passed them.

## 1. Integral and restricted tier

Estimate indirect surface transport with up to three secondary surface vertices,
including the full supported reflection closure at every vertex. Keep primary
emission and direct NEE in the direct tier. An emitter reached after a secondary
reflection is indirect; an emitter reached immediately from the primary is direct.
Account for NEE versus BSDF sampling with the oracle's light PDFs and MIS.
Primary BSDF escape to the environment belongs to PT's indirect/ambient output,
replacing diffuse sky and specular IBL. It must not disappear with those fallbacks.

Each candidate represents one terminal contribution, not the oracle's sum over a
path tree. Select secondary count n uniformly from 0..3 and endpoint strategy
(NEE or BSDF) with probability 1/2, retaining both labels. Include these selection
probabilities in the initial proposal density. At n=0 only BSDF environment escape
belongs to this tier; the other direct strata contribute zero. For n>0, NEE and
BSDF emitter hits use complementary MIS weights with their actual strategy PDFs;
BSDF escape has no NEE competitor. Punctual endpoints use discrete probability
mass, area emitters use the shared area density. Preserve the oracle's contribution
support rather than subtracting a depth-one image from a deeper image: depth-one
termination also removes the BSDF complement of primary NEE.
Initially support punctual lights and emissive triangles; finite-radius analytic
lights require their own endpoint chart and cause counted stand-down until added.

The prototype is explicit and off by default. Its finite path-length truncation is
reported. It supports opaque triangle geometry and finite-width reflective lobes;
it does not approximate unsupported transmission, participating media, or singular
delta closures by a diffuse lobe. Such scenes stand down with a named reason and a
counter. Near-specular reflection remains supported and must appear in evidence.
True delta reflection requires a separate lower-dimensional chart before enabling
it; the existing GPU oracle also samples finite-width lobes.

## 2. Sample, coordinates, and storage

The sample is an entire suffix, not cached outgoing radiance at one vertex. Store
its sampled directions, endpoint kind, hit identities/barycentrics, geometric
normals, chart labels and random coordinates, and the receiver from which it was
generated. Reevaluate direction-dependent BSDF factors after a shift. Storing GI's
direction-independent radiance and multiplying it by a new primary BSDF is invalid.

Use a disjoint union over path lengths, endpoint strategies and sampler charts.
Within a chart, use the product of solid angles for non-delta scattering directions,
with counting measure for discrete labels and uniform measure for residual random
coordinates. A held surface point is a representation of a direction; it does not
silently change the density to area measure.
An NEE terminal triangle is the explicit exception: its endpoint uses area measure,
so its geometry factor belongs to the terminal integrand. Punctual NEE endpoints
use counting measure. A BSDF-sampled endpoint retains its scattering solid angle.

A mixture sampler's PDF is not generally its mapping determinant. If branch `l`
has probability `q_l` and conditional directional density `p_l`, keep its branch
label and normalized residual inside the branch-selection interval. The local
chart density is `c_l = q_l p_l`. The extended integrand is the physical integrand
times `c_l / p_mix`; summing labels and integrating residuals recovers the physical
integrand because `sum_l c_l = p_mix`. Thus a fresh sample still contributes the
oracle's `F / p_mix`. Apply this construction at every sampled mixture vertex.
Inactive coordinates are retained, not discarded when a path terminates.

This padding makes replay and inverse replay well-defined even though the closure
draw consumes three random numbers to produce a two-dimensional direction. A
seed alone is insufficient after reconnection changes a sampled direction.

Proposed version-1 record uses the following aligned blocks:

| Block | Contents |
|---|---|
| Header | layout version, length/endpoint/chart flags, origin pixel/frame, age, scene epoch |
| Receiver | world-space identity, barycentrics, position/normal, outgoing direction |
| Three vertices | hit identity/barycentrics, position/geometric normal, outgoing direction, chart and residual coordinates |
| Terminal endpoint | NEE/BSDF label, emitter identity/kind, selection and point/cone uniforms, density or discrete mass, endpoint position/normal or environment direction |
| Reservoir | selected extended target, contribution weight W, confidence M, lineage, diagnostics |

Use typed device-address storage buffers for variable records, with an explicit
CPU/GLSL stride assertion and GPU round-trip test. Do not reinterpret integer bits
through floating-point MRT lanes. Separate read and write buffers per draw; declare
their graph accesses and barriers. Size allocations with checked arithmetic.
The record describes the selected suffix only. M counts represented path proposals,
not vertices, rays, or accepted connections. RGB transport is reevaluated, while
W is transported; neither is an accumulated radiance history.

## 3. Shift interface and domain

`Shift(sourceReceiver, destinationReceiver, suffix, mapping)` returns either a
rejection reason or `(destinationSuffix, logJacobian, inverseMetadata)`. A separate
domain result includes visibility, topology/chart compatibility and conditioning.
Never turn rejection into J=1 or clamp a failed connection into a valid one.

For all mappings require finite state, matching scene epoch, supported closures,
valid paths at both endpoints, and an inverse that reconstructs the source chart
and suffix. Validate geometry identity and chart decisions, not a radiance epsilon.
Sampling boundaries of measure zero are excluded consistently in both directions.

Reconnection holds a suffix vertex and everything beyond it fixed. Both the source
and destination segment must exceed the minimum distance, lie above their receiver
hemisphere, and arrive on the stored vertex's geometric front side. Require both
endpoint cosines above the minimum. Absolute cosines in the determinant cannot
replace these signed tests. Test visibility before assigning proposal support.

Replay holds the original raw uniforms fixed, including the lobe-selection
uniform; it does NOT hold the normalized lobe residual fixed when branch
probabilities change. It uses those coordinates to trace a new prefix. It may change hits,
but must remain in a compatible, invertible chart with the same supported path
dimension and endpoint strategy. Trace the inverse and reject mismatches. It does
not claim support across a hit/miss, branch or termination change unless the padded
mapping explicitly defines that change and its inverse.

Hybrid replays a prefix and reconnects at the first admissible rough connection.
The connection predicate examines source and destination receivers and the held
vertex, including sampled-lobe roughness, both distances and visibility. Forward
and reverse traversal must choose the same connection index. Reconnection at index
zero reduces to reconnection; absence of a connection reduces to full replay.

## 4. Jacobians derived in the stored measure

Write `J = |d destination / d source|`. Then `p_dst = p_src / J`, hence
`W_dst = W_src J`. The destination target is never divided by J.

For a held vertex z, let `g(x,z)=abs(n_z dot normalize(x-z))/|x-z|^2`.
The projected-area identity is `d omega_x = g(x,z) dA_z`. Dividing the two
differentials gives the reconnection factor

```
J_connect = g(destination,z) / g(source,z).
```

For replay on one invertible sampler chart, `du = c_src d omega_src dr_src`
and `du = c_dst d omega_dst dr_dst`, where r denotes retained residuals. Thus

```
J_replay,k = c_src,k / c_dst,k.
```

Use chart density here, not the full mixture PDF merely because the oracle divides
throughput by that PDF. The extended target in section 2 supplies the complementary
mixture factor. The distinction disappears for a single-lobe sampler.

Vertex k depends only on earlier vertices and its own random coordinates. The
derivative matrix is block triangular, so its determinant is the product of its
diagonal determinants:

```
J_hybrid = product(k before connection, J_replay,k) * J_connect
J_fullReplay = product(all replayed vertices, J_replay,k).
```

After the held connection, directions are unchanged and remaining geometric
factors are one. Nevertheless reevaluate the BSDF and chart-mixture integrand at
the held vertex: its direction toward the preceding vertex changed.
NEE preserves emitter identity and uniform triangle-area coordinates, whose
distribution is receiver-independent: its endpoint determinant is one in area
measure. Reevaluate the terminal geometry and visibility. Discrete light-selection
mass is in the proposal, not a solid-angle PDF. Reject changes in endpoint kind or
light-table epoch; do not replay a receiver-dependent endpoint sampler without
including its own density ratio and inverse chart.

In primary-sample coordinates the equivalent determinant is
`J_u = J * product(c_dst / c_src)`, including density changes at a held vertex.
Consequently full replay has `J_u=1`. This is a coordinate conversion, not permission
to put one into an implementation whose W and target use solid-angle charts.

## 5. Limits and reciprocal checks

Moving source toward destination gives every factor approaching one. An empty
replayed prefix has empty product one and gives ordinary reconnection. Extending
the replayed prefix to the whole suffix removes the connection factor and gives
full replay. For a held endpoint `z=t*w`, as t tends to infinity the distance and
cosine ratios tend to one: a fixed environment direction has connection J=1.
An environment direction reached through replay still carries prefix factors.
A punctual endpoint is the limit of a shrinking emitter with retained normalized
area coordinates and fixed integrated power: the endpoint becomes an atom, while
the identity transformation on emitter coordinates remains one. Its discrete mass
does not acquire an inverse-square Jacobian; inverse-square falloff is in F.

At grazing incidence or zero distance, a finite invertible chart is lost: reject
the boundary rather than assigning its divergent expression a finite value.
Taking roughness toward zero collapses the continuous lobe measure to a delta;
there is no ordinary solid-angle Jacobian at the limit. The singular arm therefore
stands down until a delta chart exists. No epsilon-density substitution is valid.

Swapping endpoints inverts each factor and reverses the same chart decisions, so
`J_forward J_reverse=1` on the shared domain, for all three mappings. A reciprocal
test alone does not establish correct factors or their placement in the estimator.

## 6. Conditioning a product

Accumulate `s=sum log(J_k)` and `a=sum abs(log(J_k))` in finite arithmetic. Admit
reuse only when `a <= log(8)`; compute J with `exp(s)` after this gate. This is a
fixed budget per entire shift, independent of suffix length. It implies
`1/8 <= J <= 8` and also rejects ill-conditioned intermediate factors that cancel
in the product. Under reversal every logarithm changes sign, so admission is
reciprocal-symmetric. It is stricter for long prefixes by construction.

This is a numerical reuse policy, not a theorem that variance cannot grow. In
general `Var(sum log J_k)` contains covariance terms as well as individual
variances; neither independence nor white-out prevention follows from a bound.
Keep a fresh canonical proposal, incorporate rejected domains into MIS, cap
confidence, and validate temporal-plus-spatial feedback during motion. Count the
rejected factors and show the log-product and conditioning state. Do not present
the gate as radiance clamping or omit its domain from the MIS denominator.

## 7. MIS over sources AND mappings

One strategy is a pair `(source reservoir, mapping)`. Include the current pixel's
fresh canonical reservoir with identity mapping. Enumerate available mappings;
using the same reservoir for several maps introduces correlation, which GRIS
permits, but does not permit counting their full contributions multiple times.

For candidate y at the destination, invert every strategy on y. For strategy i,
define `h_i(y)=pHat_i(inverse_i(y))/J_i(inverse_i(y))` on its valid image domain,
zero otherwise. Set `a_i(y)=M_i h_i(y)` and `m_i(y)=a_i(y)/sum_j a_j(y)`.
The canonical strategy covers the destination target's support. All target
functions must cover the support of the estimator they represent. In particular,
an old reservoir with zero source target cannot be claimed to cover a newly lit
path after a scene change; invalidate it via the epoch contract.

For every proposed mapped survivor, stream

```
w_i = m_i(y_i) * pHat_destination(y_i) * W_i * J_i.
W_selected = sum_i w_i / pHat_destination(y_selected).
```

There is no additional M multiplier or division by strategy count. The partition
of unity gives the identity
`sum_i integral m_i(y) F(y) dy = integral F(y) dy`; conditional reservoir selection
then preserves that weighted sum in expectation. This derives both placement of
J and the normalizer. Fresh initial RIS retains the core's separate 1/M normalizer.
Enumerate maps deterministically; random map selection would need its selection
probability and is outside this first contract.

Every inverse-domain test includes conditioning and visibility. Skipping those
tests in other strategies' denominator loses the partition of unity on the actual
proposal supports. The biased diagnostic mode may use the conventional M-only
normalizer, but it is labeled biased and never used as the correctness reference.
Confidence budgets are deterministic functions of stage, frame and source slots;
count failed proposals as well as successful ones, and cap by the scheduled budget.
Do not derive M from which sampled survivor passes a shift: a survivor-dependent
M makes the partition random and correlated with the very estimator it weights.
The expectation argument conditions on fixed strategy schedules and scene state;
it relies on properly weighted input reservoirs on their stated support, not just
on a numerical partition summing to one at a selected sample.

## 8. Ownership, staleness, and motion

Use one pure ownership function for all DI/GI/PT/DDGI/SSGI engagement combinations.
An active PT indirect estimate owns both diffuse and specular indirect transport:
GI, SSGI, primary diffuse probes/lightmaps/sky, and specular IBL/SSR/RT-reflection
estimates of the same term must stand down. DI keeps primary direct transport.
When PT is unavailable, restore the existing GI/probe ladder and specular fallbacks.
Derive actual engagement from current output availability, including graph culling.

The probe-cache rule becomes: if a cache tail is enabled, read it at exactly one
terminal diffuse vertex of the suffix, replacing the untraced tail. Never read it
at primary and again at a secondary vertex, or add it to an explicitly traced tail.
The first finite-depth prototype has no cache tail, so its read count is zero;
DDGI availability cannot turn on a second estimator implicitly. Report truncation.

M is confidence, not freshness. Cap it independently of selected-path age and keep
finalized W invariant when scaling M and the unnormalized weight sum together.
Combined M is the capped sum of scheduled source-reservoir confidence, counted
once per source rather than once per mapping. Preserve previous-frame INITIAL
reservoirs as the only temporal input. Their age is deterministically one for
every proposal; spatial/temporal mixed survivors are never persisted as temporal
inputs. Keep original lineage for diagnostics. Expiring a mixed survivor according
to its randomly selected birth date would instead require age-stratified support
and another proper-weight proof. Recursive feedback is outside this restricted
prototype; compare combined reuse to the separate modes without claiming that
this validates unrestricted temporal-plus-spatial recursion.

Any geometry, transform, material, lighting, environment, closure/sampler-layout,
or path-settings change invalidates the entire suffix history by epoch. Receiver
depth/normal/motion validity alone is insufficient. Camera motion reprojects the
receiver, then validates and shifts the entire suffix. Retained positions must
share the current TLAS origin; convert every position or invalidate on origin shift.
Retrace affected segments and reevaluate radiance before accepting reuse.

## 9. Evidence required before claiming completion

- Per-map randomized non-axis-aligned reciprocal and change-of-variables tests;
  independent quadrature and negative controls for connection, each replay factor,
  the full product, mapping application and MIS. Prove each control changes the
  answer; pure replay in PSS needs a changed-mapping control because its J is one.
- Reject backside connections at either end, zero/near distances, non-finite
  densities, chart/index mismatches, visibility failures, epoch/age mismatches and
  factors whose reciprocal product conceals excessive intermediate conditioning.
- CPU/GLSL packing and arithmetic parity plus real Vulkan pass/graph integration;
  the latter cannot be replaced by a shader-only fixture.
- Deterministic #974 benchmark captures against #1055 at matched depth, materials,
  lighting, seed policy, and clamp state. Inspect raw linear HDR, variance, validity,
  lineage and biased-mode/clamp state. Check nonempty TLAS before collecting images.
- Several camera angles and a moving camera/object sequence; compare initial-only,
  temporal-only, spatial-only, and combined modes over time, including near-specular
  transport. Quantify drift and error against the oracle, not just still-frame noise.
- Measured GPU timestamps, instrumented traced-ray counts, AS build/refit cost,
  acceptance/rejection and reservoir statistics. Existing upper-bound ray budgets
  are not measured counts. Show unsupported-scene/backend stand-down explicitly.

The mathematical reference is Lin et al., *Generalized Resampled Importance
Sampling* (2022), sections 4–8, particularly chart/padded-space discussion in 8.1:
<https://graphics.cs.utah.edu/research/projects/gris/>. The concrete conditioning,
history and ownership policies above are this prototype's decisions, not measured
results or claims made by that paper.
