# Groom coats in the ray-traced scene

Issue #1253. Read before touching `Groom/GroomRayTracingProxy.*`,
`Renderer/RayTracing/GroomSurfaceCache.*`, or anything that decides what a strand is lit by.

## 1. A groom proxy occludes other receivers. It never occludes its own coat.

The coat's own self-occlusion is `tau`, the expected number of fibre crossings, marched through
#1248's density volume in `GroomStrand.glsl`. The proxy in the TLAS is a member of a *different*
attenuation — "everything else in the scene occluding the coat", the third of the three
`GroomCoatShadow.h` names. Keep them disjoint.

Concretely: **do not make a groom a ray-traced-shadow receiver.** `GroomRenderPass` does not write
the G-Buffer, so no screen-space shadow term reaches a strand today, and that is what keeps the
invariant true by construction rather than by care. If you ever give grooms a G-Buffer contribution,
the coat's own proxy instance has to be excluded from its own visibility rays first — an instance
mask lane, not a distance heuristic.

The failure this prevents is already on record in a different door: a groom that both **casts and
receives** goes from 44.98 to 0.22 luma (`shadow-map-is-binary-a-coat-is-not`). A coat is not a
binary occluder, and shadowing it by a second copy of itself is how it goes black.

**The A/B that catches a violation**, and the one the PR has to run: turn groom proxies on and off
and diff the frame. The coat's own pixels must be **identical**; the body and the ground under it
must darken. A coat that dimmed is a double count.

## 2. The proxy's radius carries the coverage, so both width factors belong to it.

The representation is the coat's own strands thinned by a stride and widened by `1/k`. The quantity
preserved is the directional projected area `sum 2*rbar*L*sin(theta)`, which is **linear** in the
radius — so the compensation is `1/k`, never `1/sqrt(k)` (that one is foliage's, whose instances are
sprites). Same arithmetic, same reason, as `GroomLod.h`.

Two consequences that are easy to get wrong:

- **Compensate on the ACHIEVED fraction**, `StrandsSelected / StrandsAvailable`, never the requested
  one. The budget is spent as an integer stride, so a request for 0.4 retains 1/3.
- **`BuildGroomStrandMesh` applies neither width factor.** It emits the cooked radii; the raster
  path multiplies by `request.WidthScale * widthCompensation` *in the shader*. A proxy that applied
  only its own compensation is thinner in ray space than on screen for every groom exported at
  another unit scale, and nothing says so.

## 3. A proxy refill that does not reallocate freezes an undeformed coat forever.

`GroomSurfaceCache` keys its GPU Scene geometry record on the buffer handles. An **undeformed** coat
classifies `Static`: its BLAS is built once, compacted, and never rebuilt. So refilling its vertex
buffer in place leaves the geometry record identical, nothing asks for a rebuild, and the coat casts
the shadow of the shape it had when it was first seen — for the rest of the session, with every
counter reading healthy.

That is why the cache has **one** hash, not a shape hash and a content hash: every input that
changes an undeformed coat's bytes also changes how many there are, and a change reallocates. A
deformed coat is exempt because it carries `GPUSceneInstanceFlagAnimated` and a
`DeformedContentRevision`, which is exactly how `RayTracingScene` knows to refit it.

If you add an input that scales the geometry without resizing it, put it in that hash.

## 4. Crossed ribbons, because a shadow ray's direction is the light's.

One flat ribbon per segment is half the triangles and vanishes edge-on, and the geometry cannot be
turned to face a ray without becoming per-light. Two ribbons in perpendicular planes through the
centreline keep a silhouette from every direction. The single-ribbon arm is kept behind
`GroomProxyConversionSettings::CrossedRibbons` so the analysis document can measure what the second
one buys, not as a shipping configuration.

The perpendicular is chosen from the tangent's **smallest** component. A fixed axis produces a
zero-length cross product for every strand that happens to run along it — one flank of an animal,
silently missing from every shadow.

## 5. A degenerate segment must be dropped, not normalised.

A zero-length tangent normalises to NaN, and a NaN vertex in a BLAS build is undefined at the device
**with no validation message**. The structure is simply wrong, for every ray, for the rest of the
session. `ConvertGroomStrandMeshToProxy` drops and counts; keep it that way, and keep the counter.

## 6. The proxy is built from `BuildGroomStrandMesh`'s output, not from the cooked curves.

The binding (#1249), the guide simulation (#1250), the per-strand coat width (#1251) and the LOD
level (#1252) have all been applied to those vertices already. Rebuilding from the curves is a
second evaluation of four features that then have to agree with the first, and the frame where they
disagree is a coat whose shadow is at last frame's pose.

The conversion reads vertex `4s+0` as P0 and `4s+2` as P1. That is a claim about the emitter's
corner order, and `GroomRayTracingProxyTest` asserts it against a real build — change the emitter
and the test says so, rather than the renderer quietly building bowties.

## 7. Where the cost is bounded, and where it is not.

The per-frame conversion budget is spent at the **producer**, which is the only place the real
triangle count is known before a buffer exists. The **device** AS-byte budget is spent in
`VulkanRayTracingBackend`, under `GroomProxyPolicy::AccelerationStructureBytes` — its own budget,
not vegetation's, because a shared one would let a forest evict an animal's coat from the ray-traced
world with no counter able to say which.

Every refusal is fail-closed: the coat leaves the ray-traced scene with a counted reason and the
raster tier is untouched.

## 8. Reachability

Ray tracing is **Vulkan-only** here, and both hybrid consumers (the RT shadow tier and RT
reflections) run only on the **Deferred** path — `Renderer3D::WantsRayTracingGrooms` forwards to
`WantsRayTracingVegetation` rather than copying the predicate, so the two cannot drift. On Forward,
Forward+, OpenGL, or a device without ray tracing, every coat is `NotRequested` and nothing is
built. That is not a fallback; it is the absence of a consumer.

The headless evidence fixtures need a real GL 4.6 context, so **the suite can never cover a Vulkan
cell of this feature**. Its captures are live-editor work.
