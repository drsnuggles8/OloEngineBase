// =============================================================================
// FoliageSurface.glsl — the vegetation material, in ONE executable home
// (issue #1234).
//
// A leaf is a THIN TWO-SIDED SURFACE: one lamina a fraction of a millimetre
// thick. Light that enters the lit face leaves the other face in essentially
// the same place, so the right model is a transmission LOBE evaluated per
// pixel — not the subsurface diffusion MaterialKind::Skin selects, which
// spreads energy ACROSS the surface and would blur every leaf edge into its
// neighbour. That is why Foliage is its own MaterialKind and not a skin
// profile with a small radius; see Renderer/MaterialKind.h.
//
// WHY EVERYTHING LIVES HERE. #1234's third acceptance criterion is that
// forward, forward+ and deferred preserve the same material MEANING and the
// same NORMAL ORIENTATION. Three shaders agreeing because three authors copied
// the same fifteen lines is a claim a reviewer has to re-check on every later
// edit; three shaders calling the same function is a property of the build.
// So:
//
//   * Foliage_Instance.glsl        (forward / forward+) samples the surface
//     here and evaluates BOTH lobes here.
//   * Foliage_Instance_GBuffer.glsl (deferred) samples the surface here and
//     writes it; DeferredLightingShared.glsl evaluates the transmission lobe
//     here, from the same function the forward path called.
//   * Foliage_Impostor{,_GBuffer}.glsl carry the same kind and the same lobe
//     parameters, with a CONSTANT thickness — an impostor atlas has no
//     thickness channel, and at the distance an impostor is used there is no
//     per-pixel leaf left to vary it. Stated rather than hidden.
//
// The file is split in two halves on purpose. The SURFACE half needs the
// foliage UBO and the leaf samplers, so only the foliage shaders include it
// (OLO_FOLIAGE_SURFACE_SAMPLING). The LOBE half needs neither and is compiled
// into the deferred lighting pass as well.
// =============================================================================

#ifndef OLO_FOLIAGE_SURFACE_GLSL
#define OLO_FOLIAGE_SURFACE_GLSL

// -----------------------------------------------------------------------------
// The two-sided rule.
// -----------------------------------------------------------------------------
// THE single definition of "which way does this leaf face". Every path calls
// it; none of them decides for itself.
//
// A pure function of N and the reference direction — deliberately NOT
// gl_FrontFacing. Foliage draws with three different culling states in the same
// frame (the flat card culls back faces, the authored plant mesh does not, the
// impostor card does not — see Renderer3D::DrawFoliageLayer), and a rule keyed
// on winding would give the same leaf a different normal depending on which of
// those drew it, on which backend resolved the winding, and on nothing the
// author can see. A rule keyed on a direction gives the same answer everywhere
// and can be mirrored on the CPU, which is what FoliageLeafTransmissionTest
// pins.
//
// Called with V (surface -> eye) it returns the SHADING normal: the one facing
// the viewer, which is what both the forward shader and the G-Buffer write.
// Called with L (surface -> light) it returns the LIT-SIDE normal, which is
// what the shadow lookup must be biased along — see oloFoliageShadowNormal.
//
// The dot == 0.0 case (exactly edge-on) keeps N. It is one ray of measure zero
// and both answers are equally wrong there; what matters is that it is the
// SAME answer in every path.
vec3 oloFoliageFaceNormal(vec3 N, vec3 towards)
{
    return (dot(N, towards) < 0.0) ? -N : N;
}

// The normal a foliage pixel's SHADOW lookup must be offset along, for the
// light arriving from direction L.
//
// THIS IS THE ONE THAT BITES. Cascaded-shadow sampling offsets the receiver
// along its normal by ShadowParams.y metres to escape self-shadowing acne. For
// a BACKLIT leaf the shading normal points at the viewer and therefore AWAY
// from the light, so offsetting along it walks the sample point deeper behind
// the leaf's own shadow-map depth — the leaf shadows itself, the shadow factor
// collapses to zero, and the transmission lobe it was supposed to gate goes
// black. The symptom is "backlit leaves do not glow", which reads as the lobe
// being wrong and is actually the bias having the wrong sign.
//
// Offsetting along the LIT-SIDE normal is also the physically honest query:
// the shadow test asks whether the face the light actually hits is occluded,
// and that face's normal points at the light.
//
// It is safe for the reflected lobe too, and that is not a coincidence: where
// the reflected lobe is non-zero, dot(N, L) > 0 and this returns N unchanged,
// so one lookup serves both lobes and the two can never disagree about whether
// this pixel is in shadow.
vec3 oloFoliageShadowNormal(vec3 N, vec3 L)
{
    return oloFoliageFaceNormal(N, L);
}

// -----------------------------------------------------------------------------
// The transmission lobe.
// -----------------------------------------------------------------------------
// `lobe` packs the per-layer authored shape of the term:
//   x — DISTORTION: how far the exit direction is bent back towards the
//       surface normal. 0 transmits only where the eye is exactly in line with
//       the light (a hard specular-looking glint); larger values spread the
//       glow across the whole leaf, which is what real foliage does.
//   y — POWER: the falloff exponent of the forward-scattering lobe.
//   z — WRAP: how much of a plain Lambertian back-face term is mixed in.
//       Without it a leaf lit at ninety degrees to the view transmits nothing
//       and grows a hard terminator down its middle.
//   w — ENVIRONMENT SCALE: how much of the irradiance arriving on the FAR face
//       is transmitted.
//
// THE TERM IS SPLIT IN TWO, and the split is not cosmetic: the DIRECT half is
// per light and the INDIRECT half is per pixel. A single function taking both
// would have to be called inside the light loop, which adds the environment
// term once per light — a two-light scene glowing twice as hard as a one-light
// scene, for no reason an author could see. Both callers (the forward shader
// and the deferred lighting pass) therefore call `Direct` in their loop and
// `Ambient` once, outside it.
//
// `shadow` is the direct-light visibility, and it multiplies the direct half.
// #1234's second criterion names the wrong answer explicitly — leaf
// transmission must not be an unshadowed ambient constant — and this is where
// that is enforced: the direct half is gated by the same shadow factor the
// reflected lobe uses, and the ambient half below is environment irradiance,
// which is neither constant nor authored.
//
// N must already be the shading normal (viewer-facing); V is surface -> eye
// and L is surface -> light, both unit.
vec3 oloFoliageTransmissionDirect(vec3 N, vec3 V, vec3 L, vec3 radiance, float shadow,
                                  float thickness, vec3 tint, vec4 lobe)
{
    if (thickness <= 0.0)
        return vec3(0.0);

    // Forward scattering: light that entered the far face and leaves towards
    // the eye. Brightest when the eye is nearly in line with the light, which
    // is exactly the backlit case this material exists for.
    vec3 Ht = L + N * lobe.x;
    // A zero-length half vector is reachable when distortion is 1 and the light
    // is exactly along -N. normalize() of that is a NaN that would propagate
    // into scene colour, so the degenerate case falls back to the light
    // direction rather than being left to the driver.
    float htLen = length(Ht);
    Ht = (htLen > 1e-5) ? (Ht / htLen) : L;
    float fwd = pow(max(dot(V, -Ht), 0.0), max(lobe.y, 1.0));

    // Lambertian wrap on the far face. Without it a leaf lit at ninety degrees
    // to the view transmits nothing and grows a hard terminator down its
    // middle.
    float back = max(dot(-N, L), 0.0);

    return tint * (thickness * (fwd + back * lobe.z) * shadow) * radiance;
}

// The INDIRECT half: the environment arriving on the FAR face, transmitted.
//
// Not shadowed — the ambient ladder's own occlusion already covers it, and
// multiplying a second approximation of the same visibility darkens twice —
// but not a constant either: `backEnvIrradiance` is the irradiance sampled
// along -N, so it moves with the sky, the time of day and the probe the plant
// stands in. The caller supplies it because the two paths reach the
// environment differently (the forward shader has the irradiance cubemap in
// hand, the deferred pass has it behind its own bindless alias), but both hand
// in the irradiance for the SAME direction, so the term means the same thing.
vec3 oloFoliageTransmissionAmbient(float thickness, vec3 tint, vec3 backEnvIrradiance, vec4 lobe)
{
    if (thickness <= 0.0)
        return vec3(0.0);
    return tint * (thickness * lobe.w) * backEnvIrradiance;
}

#ifdef OLO_FOLIAGE_SURFACE_SAMPLING

// -----------------------------------------------------------------------------
// The surface. Only the foliage shaders compile this half.
// -----------------------------------------------------------------------------
// Requires, from the includer:
//   * the FoliageParams UBO at binding 12, with u_LeafSurface / u_LeafTransmit
//     / u_LeafLobe (ShaderBindingLayout::FoliageUBO)
//   * sampler2D u_LeafNormalMap (TEX_NORMAL, 2)
//   * sampler2D u_LeafRoughnessMap (TEX_ROUGHNESS, 6)
//   * sampler2D u_LeafThicknessMap (TEX_METALLIC, 7 — repurposed; foliage is
//     never metallic, and the engine already repurposes a semantic slot per
//     shader this way, e.g. PBR_MultiLight's u_MetallicRoughnessMap on
//     TEX_SPECULAR)

// Which maps this layer actually bound. A bitfield in u_LeafSurface.w, carried
// as a float because it is a small exact integer — the same trick the G-Buffer
// flags lane uses. A layer with no map must not sample the slot: an unbound
// sampler reads the engine's typed null, whose black would mean "thickness 0"
// (a leaf that never transmits) and "roughness 0" (a mirror leaf), and both
// look like the feature being broken rather than absent.
#define OLO_LEAF_MAP_NORMAL    1
#define OLO_LEAF_MAP_ROUGHNESS 2
#define OLO_LEAF_MAP_THICKNESS 4

bool oloLeafHasMap(int which)
{
    return (int(u_LeafSurface.w + 0.5) & which) != 0;
}

// Is this layer authored as a leaf at all? A layer with zero transmission
// strength keeps the pre-#1234 behaviour exactly: MaterialKind::Generic, no
// lobe, no thickness lane in the G-Buffer. That is what makes a scene saved
// before this material existed load unchanged.
bool oloLeafEnabled()
{
    return u_LeafTransmit.w > 0.0;
}

// Cotangent frame from screen-space derivatives.
//
// The foliage vertex stream is the engine's 32-byte {position, normal, uv}
// (FoliageInstanceVertexStage.glsl) and carries NO tangent — shared by the flat
// card, the authored plant mesh and the impostor. Deriving the frame here keeps
// all three on one definition instead of adding a tangent to one stream and
// leaving the others to disagree, and it costs two derivative pairs on a
// surface that is already alpha-tested.
//
// Degenerate UVs (a card with a collapsed texcoord, a mesh seam) make the
// derivative cross product zero-length; that falls back to the unperturbed
// normal rather than normalizing a zero vector.
mat3 oloFoliageTangentFrame(vec3 worldPos, vec2 uv, vec3 N)
{
    vec3 dp1 = dFdx(worldPos);
    vec3 dp2 = dFdy(worldPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float maxLen = max(dot(T, T), dot(B, B));
    if (maxLen <= 1e-12)
        return mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), N);

    float invMax = inversesqrt(maxLen);
    return mat3(T * invMax, B * invMax, N);
}

struct OloFoliageSurface
{
    vec3 Albedo;
    // World space, ALREADY oriented towards the viewer by oloFoliageFaceNormal
    // and then perturbed by the leaf normal map. This is the normal the forward
    // shader lights with and the normal the G-Buffer stores, which is what makes
    // criterion 3's "same normal orientation" structural.
    vec3 Normal;
    // The un-perturbed, un-flipped interpolated normal. The lobe does not want
    // it, but the G-Buffer velocity/debug paths and the CPU mirror do.
    vec3 GeometricNormal;
    float Roughness;
    // [0, 1]. 0 means "this pixel does not transmit" and is the value a layer
    // with no thickness map and zero scale produces.
    float Thickness;
    float Alpha;
};

// Sample the whole vegetation surface. ONE function, called by the forward
// shader and by the G-Buffer shader, so the two cannot diverge on what the
// maps mean, on the order the normal is flipped and perturbed in, or on what a
// missing map falls back to.
//
// `V` is surface -> eye, unit.
OloFoliageSurface oloFoliageSampleSurface(vec3 worldPos, vec3 geometricNormal, vec2 uv,
                                          vec3 V, vec3 tint, vec4 albedoSample)
{
    OloFoliageSurface s;
    s.GeometricNormal = normalize(geometricNormal);
    s.Albedo = albedoSample.rgb * tint;
    s.Alpha = albedoSample.a;

    // ORDER MATTERS AND IS FIXED HERE. Flip to face the viewer FIRST, then
    // perturb — a tangent frame built on the un-flipped normal would hand the
    // back face a left-handed basis and mirror the leaf's veins, which is a
    // difference the front/back capture pair would show and no CPU test would.
    vec3 N = oloFoliageFaceNormal(s.GeometricNormal, V);

    if (oloLeafHasMap(OLO_LEAF_MAP_NORMAL))
    {
        vec3 tn = texture(u_LeafNormalMap, uv).xyz * 2.0 - 1.0;
        // Strength scales the tangential components only, so 0 is exactly the
        // geometric normal and 1 is exactly the map.
        tn.xy *= u_LeafSurface.y;
        mat3 TBN = oloFoliageTangentFrame(worldPos, uv, N);
        vec3 mapped = TBN * tn;
        float len = length(mapped);
        N = (len > 1e-5) ? (mapped / len) : N;
    }
    s.Normal = N;

    // The layer's roughness is the authored constant; a map MULTIPLIES it, so a
    // white map is a no-op and the constant keeps meaning what the inspector
    // says it means.
    s.Roughness = u_LeafSurface.x;
    if (oloLeafHasMap(OLO_LEAF_MAP_ROUGHNESS))
        s.Roughness *= texture(u_LeafRoughnessMap, uv).r;
    s.Roughness = clamp(s.Roughness, 0.02, 1.0);

    // Thickness: the authored scale, modulated by the map. With no map the
    // whole leaf transmits uniformly at the authored scale, which is the right
    // conservative answer and is also what the impostor gets.
    s.Thickness = u_LeafSurface.z;
    if (oloLeafHasMap(OLO_LEAF_MAP_THICKNESS))
        s.Thickness *= texture(u_LeafThicknessMap, uv).r;
    s.Thickness = clamp(s.Thickness, 0.0, 1.0);

    return s;
}

#endif // OLO_FOLIAGE_SURFACE_SAMPLING

#endif // OLO_FOLIAGE_SURFACE_GLSL
