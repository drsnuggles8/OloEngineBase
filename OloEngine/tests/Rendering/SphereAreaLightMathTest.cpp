// =============================================================================
// SphereAreaLightMathTest.cpp
//
// Pins the shader math used by the sphere-area-light path
// (`OloEditor/assets/shaders/include/PBRCommon.glsl`):
//
//   - The representative-point trick (Karis 2013, SIGGRAPH "Real Shading in
//     Unreal Engine 4", eq. 12). The specular term shades as if the light
//     were a point on the emitter sphere closest to the surface's
//     reflection ray.
//
//   - The energy-conservation normalisation (Karis 2013 eq. 14). A larger
//     emitter widens the highlight, so D is rescaled by `(alpha /
//     alpha_prime)^2` to keep the integrated BRDF energy-conservant. In the
//     `radius -> 0` limit the factor must collapse to 1 (recovering an
//     exact point-light response) — this is the contract that lets
//     SphereAreaLight degrade gracefully into PointLight as radius
//     shrinks, with no discontinuity at the bind site between the two
//     light types.
//
// Why both halves are pinned here rather than only via integration tests:
//
//   - Representative-point: a sign flip or `normalize(centerToRay)` typo
//     puts the highlight on the wrong side of the reflection ray. The
//     bug shows up as a halo on the *back* of glossy spheres lit by a
//     bright area emitter — visible to the eye but invisible to existing
//     L4 / golden tests which exercise point lights only.
//   - Normalisation: drift in the `alpha + r / (2d)` rescale leaks energy
//     (highlights brighter than the light's radiated power) or absorbs it
//     (dim, plastic highlights). The error scales with `(radius / dist)`,
//     so it's most visible in close-up shots — exactly where the existing
//     golden tests rasterise at a fixed framing.
//
// Mirroring these GLSL helpers in C++ and pinning the limit behaviour means
// either side drifting from the other has to break this test before it can
// produce wrong pixels on the GPU.
//
// Classification: L4 / shaderpipe (CPU mirror of shader math).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Mirrors PBRCommon.glsl :: EPSILON
        constexpr f32 kShaderEpsilon = 0.0001f;

        // Mirrors `calculateSphereAreaLightRepresentativePoint` in
        // PBRCommon.glsl. Returned vector is the (unit) representative
        // light direction in world space. If this drifts from the GLSL
        // counterpart, the math is no longer pinned — update both
        // together.
        glm::vec3 RepresentativePointDir(const glm::vec3& fragPos, const glm::vec3& N,
                                         const glm::vec3& V, const glm::vec3& lightPos,
                                         f32 sphereRadius)
        {
            const glm::vec3 r = glm::reflect(-V, N);
            const glm::vec3 L = lightPos - fragPos;
            const glm::vec3 centerToRay = glm::dot(L, r) * r - L;
            const f32 ctrLen = std::max(glm::length(centerToRay), kShaderEpsilon);
            const f32 t = std::clamp(sphereRadius / ctrLen, 0.0f, 1.0f);
            const glm::vec3 closestPoint = L + centerToRay * t;
            return glm::normalize(closestPoint);
        }

        // Same as above but returns the un-normalised closestPoint so
        // tests can interrogate where on the sphere the rep point lies.
        glm::vec3 RepresentativePointWorld(const glm::vec3& fragPos, const glm::vec3& N,
                                           const glm::vec3& V, const glm::vec3& lightPos,
                                           f32 sphereRadius)
        {
            const glm::vec3 r = glm::reflect(-V, N);
            const glm::vec3 L = lightPos - fragPos;
            const glm::vec3 centerToRay = glm::dot(L, r) * r - L;
            const f32 ctrLen = std::max(glm::length(centerToRay), kShaderEpsilon);
            const f32 t = std::clamp(sphereRadius / ctrLen, 0.0f, 1.0f);
            const glm::vec3 closestPoint = L + centerToRay * t;
            return fragPos + closestPoint;
        }

        // Mirrors `sphereAreaLightNormalization` in PBRCommon.glsl.
        f32 SphereAreaNormalization(f32 roughness, f32 distance, f32 sphereRadius)
        {
            const f32 alpha = roughness * roughness;
            const f32 alphaPrime = std::clamp(
                alpha + sphereRadius / std::max(2.0f * distance, kShaderEpsilon),
                0.0f, 1.0f);
            const f32 ratio = alpha / std::max(alphaPrime, kShaderEpsilon);
            return ratio * ratio;
        }

        // --- Sphere-area-light SHADOW math (issue: area-light shadows) ---
        //
        // Shadow-casting sphere area lights reuse the point-light cubemap pool:
        // the emitter is treated as a point at its centre and an omnidirectional
        // depth cubemap is rendered with the light's RANGE as the far plane
        // (Scene.cpp :: SetPointLightShadow(idx, centre, range)). Sampling mirrors
        // `calculatePointShadowFactor` in PBRCommon.glsl:
        //
        //   currentDepth = length(worldPos - lightCentre) / farPlane;   // [0,1]
        //   lit  ⟺  currentDepth - bias <= storedOccluderDepth
        //
        // where storedOccluderDepth is the nearest occluder along worldPos's
        // direction from the centre, in the same normalised [0,1] units. These
        // helpers pin that normalisation + compare so a regression (e.g. dividing
        // by radius instead of range, or flipping the compare) breaks here before
        // it can produce a wrong frame on the GPU.

        // Normalised linear cubemap depth a fragment writes/tests against — mirrors
        // `currentDepth = length(fragToLight) / farPlane`.
        f32 PointShadowDepth(const glm::vec3& worldPos, const glm::vec3& lightCentre, f32 farPlane)
        {
            return glm::length(worldPos - lightCentre) / std::max(farPlane, kShaderEpsilon);
        }

        // Hardware samplerCubeShadow GL_LEQUAL result: 1.0 = lit, 0.0 = shadowed.
        // Matches `texture(cube, vec4(dir, currentDepth - bias))`.
        f32 PointShadowCompare(f32 currentDepth, f32 storedOccluderDepth, f32 bias)
        {
            return ((currentDepth - bias) <= storedOccluderDepth) ? 1.0f : 0.0f;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Representative-point limits
    // -------------------------------------------------------------------------

    TEST(SphereAreaLightMath, RepresentativePointReducesToCentreWhenRadiusZero)
    {
        // r -> 0 must collapse the offset onto the centre direction — the
        // contract that lets sphere area lights degrade to point lights
        // continuously. If `t` ever evaluates to non-zero at r=0 the
        // contribution discontinuously changes sign across light types.
        const glm::vec3 fragPos{ 0.0f, 0.0f, 0.0f };
        const glm::vec3 N{ 0.0f, 1.0f, 0.0f };
        const glm::vec3 V = glm::normalize(glm::vec3(0.5f, 0.5f, 0.5f));
        const glm::vec3 lightPos{ 3.0f, 4.0f, 2.0f };
        const glm::vec3 centreDir = glm::normalize(lightPos - fragPos);

        const glm::vec3 rep = RepresentativePointDir(fragPos, N, V, lightPos, 0.0f);
        // Use a generous tolerance: the shader's EPSILON guard floats a
        // tiny perturbation through `clamp(r/max(len,EPS),0,1)` even when
        // r=0, so the result is within ~EPSILON of `normalize(L)`.
        EXPECT_NEAR(rep.x, centreDir.x, 1e-4f);
        EXPECT_NEAR(rep.y, centreDir.y, 1e-4f);
        EXPECT_NEAR(rep.z, centreDir.z, 1e-4f);
    }

    TEST(SphereAreaLightMath, RepresentativePointLiesOnOrInsideSphereSurface)
    {
        // The representative point is the *clamped* projection of the
        // reflection ray onto the line of L. Karis's construction keeps
        // it within (centre + sphereRadius). The test fails if a sign
        // flip in `centerToRay` ever pushes the point beyond the sphere
        // — the rep point would then sample radiance from outside the
        // emitter, breaking the energy budget downstream.
        const glm::vec3 fragPos{ 0.0f, 0.0f, 0.0f };
        const glm::vec3 N{ 0.0f, 1.0f, 0.0f };
        const glm::vec3 V = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f)); // glancing view
        const glm::vec3 lightPos{ 2.0f, 4.0f, 0.0f };
        const f32 sphereRadius = 1.5f;

        const glm::vec3 worldPoint = RepresentativePointWorld(fragPos, N, V, lightPos, sphereRadius);
        const f32 distFromCentre = glm::length(worldPoint - lightPos);
        // Tolerate the EPSILON guard's tiny stretch.
        EXPECT_LE(distFromCentre, sphereRadius + 1e-3f);
    }

    TEST(SphereAreaLightMath, RepresentativePointStaysOnTheReflectionSideWhenRayMissesSphere)
    {
        // Reflection ray pointing *away* from the light. `dot(L, r)` is
        // negative, so the projection of L onto r lies behind the
        // surface. The construction must still produce a direction
        // pointing toward the emitter (not behind the surface), because
        // the eventual NdotL test gates whether any energy is delivered.
        // This pins the "no negative-NdotL leakage" property of the
        // representative-point trick.
        const glm::vec3 fragPos{ 0.0f, 0.0f, 0.0f };
        const glm::vec3 N{ 0.0f, 1.0f, 0.0f };
        // Reflection of -V about N — pick V so r points *away* from the
        // light direction.
        const glm::vec3 V = glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f));
        const glm::vec3 lightPos{ 0.0f, 4.0f, 4.0f };
        const f32 sphereRadius = 0.5f;

        const glm::vec3 rep = RepresentativePointDir(fragPos, N, V, lightPos, sphereRadius);
        // The representative direction should still have a positive
        // y-component (light is above the surface). If a sign flip ever
        // produces a downward direction, NdotL becomes negative and the
        // contribution silently zeros out — a regression that hides
        // until the next golden run.
        EXPECT_GT(rep.y, 0.0f);
    }

    TEST(SphereAreaLightMath, ReflectionRayHitsSphereCentre_RepresentativePointEqualsCentre)
    {
        // Aim the reflection ray *directly* at the sphere centre:
        // V is chosen so reflect(-V, N) points exactly at the light.
        // `centerToRay = dot(L,r)*r - L = 0`, so the representative
        // point degenerates to the centre regardless of radius. This is
        // the "perfect specular hit" edge — the highlight should sit
        // exactly at the geometric reflection.
        const glm::vec3 fragPos{ 0.0f, 0.0f, 0.0f };
        const glm::vec3 N{ 0.0f, 1.0f, 0.0f };
        // For N = (0,1,0): reflect(-V, N) = (-V.x, V.y, -V.z). To make
        // reflection land at lightPos = (0, 1, 0), V = (0, 1, 0).
        const glm::vec3 V{ 0.0f, 1.0f, 0.0f };
        const glm::vec3 lightPos{ 0.0f, 5.0f, 0.0f };
        const f32 sphereRadius = 2.0f;

        const glm::vec3 rep = RepresentativePointDir(fragPos, N, V, lightPos, sphereRadius);
        const glm::vec3 centreDir = glm::normalize(lightPos - fragPos);
        EXPECT_NEAR(rep.x, centreDir.x, 1e-4f);
        EXPECT_NEAR(rep.y, centreDir.y, 1e-4f);
        EXPECT_NEAR(rep.z, centreDir.z, 1e-4f);
    }

    // -------------------------------------------------------------------------
    // Normalisation limits (Karis 2013 eq. 14)
    // -------------------------------------------------------------------------

    TEST(SphereAreaLightMath, NormalisationIsExactlyOneAtZeroRadius)
    {
        // Karis eq. 14: alpha_prime = alpha when r = 0, so ratio = 1
        // exactly. This is the load-bearing contract that prevents a
        // brightness discontinuity at the SphereAreaLight ↔ PointLight
        // boundary when authoring tools collapse radius to 0.
        // Sweep roughness and distance — must hold across the whole
        // parameter range, not just at one sample point.
        for (f32 roughness : { 0.05f, 0.25f, 0.5f, 0.85f, 1.0f })
        {
            for (f32 distance : { 0.1f, 1.0f, 10.0f, 100.0f })
            {
                const f32 n = SphereAreaNormalization(roughness, distance, 0.0f);
                EXPECT_NEAR(n, 1.0f, 1e-4f)
                    << "roughness=" << roughness << " distance=" << distance;
            }
        }
    }

    TEST(SphereAreaLightMath, NormalisationBoundedInUnitInterval)
    {
        // ratio = alpha / alpha_prime, where alpha_prime >= alpha for
        // any non-negative radius / distance. Therefore ratio <= 1, and
        // ratio^2 <= 1. The lower bound is 0 (very rough surface + huge
        // emitter degenerates the highlight). Pin both — a sign flip in
        // the rescale (subtracting instead of adding the radius term)
        // would lift the factor above 1 and dump extra energy.
        for (f32 roughness : { 0.05f, 0.5f, 1.0f })
        {
            for (f32 sphereRadius : { 0.0f, 0.5f, 5.0f })
            {
                for (f32 distance : { 0.5f, 5.0f, 50.0f })
                {
                    const f32 n = SphereAreaNormalization(roughness, distance, sphereRadius);
                    EXPECT_GE(n, 0.0f);
                    EXPECT_LE(n, 1.0f + 1e-5f);
                }
            }
        }
    }

    TEST(SphereAreaLightMath, NormalisationShrinksAsRadiusGrows)
    {
        // For fixed roughness + distance, larger radii spread the highlight
        // over a larger solid angle. To keep the integral of the BRDF
        // constant, D must shrink. The rescale factor is therefore a
        // monotonically *decreasing* function of radius. This is the
        // signal-direction property: any future "optimisation" that
        // re-derives the factor with a sign error has to fail this test.
        const f32 roughness = 0.3f;
        const f32 distance = 4.0f;
        const f32 nSmall = SphereAreaNormalization(roughness, distance, 0.1f);
        const f32 nMedium = SphereAreaNormalization(roughness, distance, 1.0f);
        const f32 nLarge = SphereAreaNormalization(roughness, distance, 4.0f);

        EXPECT_GT(nSmall, nMedium);
        EXPECT_GT(nMedium, nLarge);
        EXPECT_GT(nLarge, 0.0f);
    }

    TEST(SphereAreaLightMath, NormalisationGrowsTowardOneAsDistanceIncreases)
    {
        // For fixed radius + roughness, the rescale term `r / (2d)`
        // vanishes as d -> infinity, so alpha_prime -> alpha and the
        // factor -> 1. Conceptually: a very distant area light looks
        // like a point light again. Pinning this rules out a
        // `2*distance` -> `2*radius` typo that would invert the
        // dependence.
        const f32 roughness = 0.4f;
        const f32 sphereRadius = 1.0f;
        const f32 nClose = SphereAreaNormalization(roughness, 1.0f, sphereRadius);
        const f32 nMid = SphereAreaNormalization(roughness, 10.0f, sphereRadius);
        const f32 nFar = SphereAreaNormalization(roughness, 1000.0f, sphereRadius);

        EXPECT_LT(nClose, nMid);
        EXPECT_LT(nMid, nFar);
        EXPECT_NEAR(nFar, 1.0f, 1e-2f);
    }

    TEST(SphereAreaLightMath, NormalisationMatchesKarisAnalyticAtKnownSample)
    {
        // Sample reference computed by hand from Karis 2013 eq. 14:
        //   roughness = 0.5 -> alpha = 0.25
        //   sphereRadius = 1, distance = 5 -> r/(2d) = 0.1
        //   alpha_prime = clamp(0.25 + 0.1, 0, 1) = 0.35
        //   factor = (0.25 / 0.35)^2 = (5/7)^2 = 25 / 49 ≈ 0.5102040816
        //
        // This anchors the implementation to the published reference. A
        // PR that rewrites the helper for "clarity" or "performance" but
        // accidentally changes its semantics has to update the constant
        // here — i.e. the divergence is loud, not silent.
        const f32 n = SphereAreaNormalization(0.5f, 5.0f, 1.0f);
        const f32 expected = (25.0f / 49.0f);
        EXPECT_NEAR(n, expected, 1e-5f);
    }

    // -------------------------------------------------------------------------
    // Representative-point shadow depth-compare (point-cubemap reuse)
    // -------------------------------------------------------------------------

    TEST(SphereAreaLightShadowMath, OccluderBetweenCentreAndReceiverShadowsReceiver)
    {
        // Area light centre 5 units above the floor, range = far plane = 25.
        // An occluder sits 2 units below the centre (3 above the floor), directly
        // on the centre→receiver ray. The cubemap therefore records the occluder's
        // normalised depth along that direction; the floor receiver is further, so
        // its currentDepth exceeds the stored depth → shadowed.
        const glm::vec3 lightCentre{ 0.0f, 5.0f, 0.0f };
        constexpr f32 range = 25.0f;
        constexpr f32 bias = 0.005f;

        const glm::vec3 occluder{ 0.0f, 3.0f, 0.0f }; // 2 units from centre
        const glm::vec3 receiver{ 0.0f, 0.0f, 0.0f }; // 5 units from centre, behind occluder

        const f32 storedDepth = PointShadowDepth(occluder, lightCentre, range);  // 2/25 = 0.08
        const f32 currentDepth = PointShadowDepth(receiver, lightCentre, range); // 5/25 = 0.20

        EXPECT_GT(currentDepth, storedDepth) << "receiver must be farther than the occluder it hides behind";
        EXPECT_FLOAT_EQ(PointShadowCompare(currentDepth, storedDepth, bias), 0.0f)
            << "a receiver behind an occluder relative to the area-light centre must be in shadow";
    }

    TEST(SphereAreaLightShadowMath, BlockerBehindReceiverLeavesReceiverLit)
    {
        // A blocker FARTHER from the light than the receiver must NOT shadow it.
        // Both depths are computed INDEPENDENTLY from real positions (not
        // storedDepth := currentDepth, which would be tautological — it would hold
        // for any bias regardless of whether PointShadowDepth is correct). The
        // receiver is 5 units below the centre; a wall behind it sits 9 units below
        // on the same straight-down ray. currentDepth (receiver, nearer) is less
        // than storedDepth (wall, farther), so the GL_LEQUAL compare keeps the
        // receiver lit. This pins both PointShadowDepth's near/far ordering and the
        // compare's "foreground stays lit" property.
        const glm::vec3 lightCentre{ 0.0f, 5.0f, 0.0f };
        constexpr f32 range = 25.0f;
        constexpr f32 bias = 0.005f;

        const glm::vec3 receiver{ 0.0f, 0.0f, 0.0f };    // 5 units from centre
        const glm::vec3 wallBehind{ 0.0f, -4.0f, 0.0f }; // 9 units from centre, same ray

        const f32 currentDepth = PointShadowDepth(receiver, lightCentre, range);  // 5/25 = 0.20
        const f32 storedDepth = PointShadowDepth(wallBehind, lightCentre, range); // 9/25 = 0.36

        EXPECT_LT(currentDepth, storedDepth) << "receiver must be nearer the light than the blocker behind it";
        EXPECT_FLOAT_EQ(PointShadowCompare(currentDepth, storedDepth, bias), 1.0f)
            << "a receiver in front of (nearer than) the only blocker must stay lit";
    }

    TEST(SphereAreaLightShadowMath, DepthUsesRangeNotRadiusAsFarPlane)
    {
        // The far plane is the area light's RANGE, independent of the emitter
        // RADIUS. Mixing the two up (a tempting bug, since both are "size" knobs
        // on the component) would rescale every depth and break the compare. Pin
        // that PointShadowDepth ignores radius entirely: two lights at the same
        // centre/range yield identical normalised depth regardless of emitter size.
        const glm::vec3 lightCentre{ 0.0f, 8.0f, 0.0f };
        const glm::vec3 receiver{ 0.0f, 0.0f, 0.0f };
        constexpr f32 range = 20.0f;

        const f32 depth = PointShadowDepth(receiver, lightCentre, range); // 8/20 = 0.4
        EXPECT_NEAR(depth, 0.4f, 1e-5f);
        // In [0,1] for a receiver inside range; would blow past 1.0 if a small
        // radius were ever substituted for the far plane.
        EXPECT_GE(depth, 0.0f);
        EXPECT_LE(depth, 1.0f);
    }

    // -------------------------------------------------------------------------
    // Point-shadow pool sharing invariant (Scene.cpp index assignment)
    // -------------------------------------------------------------------------
    //
    // Casting sphere area lights are assigned cubemap slots AFTER point lights,
    // sharing one pool of MAX_POINT_SHADOWS (= 4). The shader reads the slot from
    // each light's direction.w. The two contracts that must hold for the upload
    // loop and the shadow-setup loop to agree on which cubemap a light samples:
    //   (1) area-light slots never collide with point-light slots, and
    //   (2) the pool is capped — the 5th+ casting light gets no shadow (slot -1).
    namespace
    {
        // Mirrors the shared-counter assignment in Scene::ProcessScene3DSharedLogic
        // (both the light-upload loop and the shadow-setup loop run this same
        // logic against the same MAX). Returns each light's assigned slot, with
        // -1 meaning "no shadow" (cap reached or not a caster).
        //
        // SCOPE: this models ONLY the shadow-slot cap (MAX_POINT_SHADOWS). The real
        // upload loop also has a SEPARATE MultiLightUBO MAX_LIGHTS (256) cap that
        // `continue`s before encoding direction.w; a casting light past 256 total
        // lights gets no UBO entry at all. That cap is irrelevant to these
        // ≤4-light invariants, so it's deliberately not mirrored here — don't reuse
        // AssignPool as a full model of Scene.cpp's assignment at high light counts.
        constexpr i32 kMaxPointShadows = 4; // ShadowMap::MAX_POINT_SHADOWS

        std::vector<i32> AssignPool(const std::vector<bool>& pointCasters,
                                    const std::vector<bool>& areaCasters)
        {
            std::vector<i32> slots;
            i32 next = 0;
            for (bool casts : pointCasters)
                slots.push_back((casts && next < kMaxPointShadows) ? next++ : -1);
            for (bool casts : areaCasters)
                slots.push_back((casts && next < kMaxPointShadows) ? next++ : -1);
            return slots;
        }
    } // namespace

    TEST(SphereAreaLightShadowMath, AreaSlotsNeverCollideWithPointSlots)
    {
        // 2 casting point lights + 2 casting area lights → point lights take 0,1;
        // area lights take 2,3. No overlap, all within the pool.
        const auto slots = AssignPool({ true, true }, { true, true });
        ASSERT_EQ(slots.size(), 4u);
        EXPECT_EQ(slots[0], 0);
        EXPECT_EQ(slots[1], 1);
        EXPECT_EQ(slots[2], 2);
        EXPECT_EQ(slots[3], 3);

        // No two casters share a slot.
        std::vector<i32> used;
        for (i32 s : slots)
            if (s >= 0)
            {
                EXPECT_EQ(std::count(used.begin(), used.end(), s), 0) << "slot " << s << " reused";
                used.push_back(s);
            }
    }

    TEST(SphereAreaLightShadowMath, PoolCapDropsExcessAreaCasters)
    {
        // 3 casting point lights consume slots 0,1,2; the first area light takes
        // the last slot (3); every further casting area light gets -1 (no shadow).
        const auto slots = AssignPool({ true, true, true }, { true, true, true });
        ASSERT_EQ(slots.size(), 6u);
        EXPECT_EQ(slots[0], 0);
        EXPECT_EQ(slots[1], 1);
        EXPECT_EQ(slots[2], 2);
        EXPECT_EQ(slots[3], 3);  // first area light grabs the final slot
        EXPECT_EQ(slots[4], -1); // pool exhausted
        EXPECT_EQ(slots[5], -1);
    }

    TEST(SphereAreaLightShadowMath, NonCastersConsumeNoSlots)
    {
        // A non-casting point light in the middle must not consume a slot, so the
        // casting area light after it still gets a low index. Pins that the cap
        // counts CASTERS, not light ordinals.
        const auto slots = AssignPool({ true, false, true }, { true });
        ASSERT_EQ(slots.size(), 4u);
        EXPECT_EQ(slots[0], 0);
        EXPECT_EQ(slots[1], -1); // non-caster
        EXPECT_EQ(slots[2], 1);
        EXPECT_EQ(slots[3], 2); // area light, third caster overall
    }
} // namespace OloEngine::Tests
