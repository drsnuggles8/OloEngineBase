#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/SphereProxyAO.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

// =============================================================================
// Analytic sphere-proxy AO — CPU contract tests (issue #710).
//
// OLO_TEST_LAYER: shaderpipe
//
// Pins the closed-form sphere-occlusion integral and the proxy fit WITHOUT a GL
// context, so they run in headless CI. Per the CLAUDE.md rendering rule these
// prove the FORMULA; SphereProxyAOVisualEvidenceTest proves the frame.
//
// The integral is the part that most needs pinning: a mis-typed constant in it
// produces a frame that still looks like a frame. Every case below is one whose
// answer is known independently of the implementation — a limit, a symmetry, or
// a value the geometry forces.
// =============================================================================

using namespace OloEngine;

namespace
{
    constexpr f32 kTol = 1e-4f;

    // The four configurations the acceptance criteria name, expressed against a
    // receiver at the origin with an up normal.
    const glm::vec3 kOrigin{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 kUp{ 0.0f, 1.0f, 0.0f };

    // Wide enough that InfluenceWindow is 1 everywhere these near-field cases
    // reach, so they measure the integral and the self-occlusion fade only.
    constexpr f32 kWideInfluence = 64.0f;
} // namespace

// -----------------------------------------------------------------------------
// The integral
// -----------------------------------------------------------------------------

TEST(SphereProxyAOMathTest, SphereOnTheNormalAxisMatchesTheFormFactor)
{
    // A sphere directly overhead and entirely above the horizon is the classic
    // sphere form factor cos(gamma) * sin^2(alpha) = nl * (r/l)^2. With nl = 1
    // that is exactly (r/l)^2, independently of the closed form's second branch.
    for (const f32 distanceInRadii : { 2.0f, 5.0f, 10.0f, 50.0f })
    {
        const f32 radius = 1.0f;
        const glm::vec4 sphere{ 0.0f, distanceInRadii * radius, 0.0f, radius };
        const f32 expected = 1.0f / (distanceInRadii * distanceInRadii);
        EXPECT_NEAR(SphereProxyAO::SphereOcclusion(kOrigin, kUp, sphere), expected, kTol)
            << "distanceInRadii = " << distanceInRadii;
    }
}

TEST(SphereProxyAOMathTest, TangentSphereOverheadOccludesTheWholeHemisphere)
{
    // A sphere of radius r centred at distance r along the normal touches the
    // receiver and subtends the entire hemisphere above it.
    const glm::vec4 sphere{ 0.0f, 2.0f, 0.0f, 2.0f };
    EXPECT_NEAR(SphereProxyAO::SphereOcclusion(kOrigin, kUp, sphere), 1.0f, 1e-3f);
}

TEST(SphereProxyAOMathTest, TangentSphereCentredInTheTangentPlaneOccludesHalf)
{
    // Centre in the tangent plane, touching the receiver: the sphere's cone has
    // angular radius pi/2 about an axis lying IN the plane, so it covers exactly
    // half the hemisphere, and the cosine weighting is symmetric about that
    // split. Half is forced by geometry, so this pins the horizon-crossing
    // branch against something the branch itself cannot influence.
    // The residual is the tangent clamp alone (kMinTangentMargin), which biases
    // this exact case low by ~sqrt(margin)/pi and nothing else does.
    const glm::vec4 sphere{ 3.0f, 0.0f, 0.0f, 3.0f };
    EXPECT_NEAR(SphereProxyAO::SphereOcclusion(kOrigin, kUp, sphere), 0.5f, 2e-3f);
}

TEST(SphereProxyAOMathTest, SphereBelowTheHorizonContributesNothing)
{
    const glm::vec4 sphere{ 0.0f, -6.0f, 0.0f, 1.0f };
    EXPECT_FLOAT_EQ(SphereProxyAO::SphereOcclusion(kOrigin, kUp, sphere), 0.0f);

    // Just below, still clear of the horizon by more than its angular radius.
    const glm::vec4 grazing{ 8.0f, -2.0f, 0.0f, 1.0f };
    EXPECT_FLOAT_EQ(SphereProxyAO::SphereOcclusion(kOrigin, kUp, grazing), 0.0f);
}

TEST(SphereProxyAOMathTest, SphereBehindTheCameraStillOccludes)
{
    // THE POINT OF THE FEATURE. The integral has no camera term, so a proxy the
    // camera cannot see occludes exactly as much as one it can. Modelled here as
    // a receiver looking down -Z from the origin with a camera at +Z: the sphere
    // sits at +Z, i.e. behind the camera, well outside any view frustum.
    const glm::vec4 behindCamera{ 0.0f, 5.0f, 20.0f, 4.0f };
    const f32 occlusion = SphereProxyAO::SphereOcclusion(kOrigin, kUp, behindCamera);
    EXPECT_GT(occlusion, 0.0f);

    // And it is the SAME value as the mirrored configuration in front of the
    // camera — the property a screen-space term structurally cannot have.
    const glm::vec4 inFront{ 0.0f, 5.0f, -20.0f, 4.0f };
    EXPECT_NEAR(occlusion, SphereProxyAO::SphereOcclusion(kOrigin, kUp, inFront), kTol);
}

TEST(SphereProxyAOMathTest, IsContinuousAcrossTheHorizonBranchBoundary)
{
    // The branch switches at h * nl == 1. Sweeping the sphere across that
    // boundary must not step: a discontinuity here shows up in a frame as a hard
    // arc across a wall, which is exactly the kind of artefact that survives
    // "looks plausible".
    const f32 radius = 2.0f;
    const f32 distance = 6.0f;
    // nl = 1/h occurs at elevation asin(r/l) above the tangent plane.
    const f32 boundary = std::asin(radius / distance);

    f32 previous = SphereProxyAO::SphereOcclusion(
        kOrigin, kUp, glm::vec4(distance * std::cos(boundary - 0.05f), distance * std::sin(boundary - 0.05f), 0.0f, radius));
    for (i32 step = -49; step <= 50; ++step)
    {
        const f32 elevation = boundary + static_cast<f32>(step) * 0.001f;
        const glm::vec4 sphere{ distance * std::cos(elevation), distance * std::sin(elevation), 0.0f, radius };
        const f32 value = SphereProxyAO::SphereOcclusion(kOrigin, kUp, sphere);
        EXPECT_LT(std::abs(value - previous), 5e-3f) << "elevation = " << elevation;
        previous = value;
    }
}

TEST(SphereProxyAOMathTest, IsMonotonicInDistanceAndBoundedToUnitRange)
{
    const f32 radius = 1.5f;
    f32 previous = 2.0f;
    for (i32 step = 1; step <= 60; ++step)
    {
        const f32 distance = radius * (1.0f + 0.25f * static_cast<f32>(step));
        const f32 value = SphereProxyAO::SphereOcclusion(kOrigin, kUp, glm::vec4(0.0f, distance, 0.0f, radius));
        EXPECT_GE(value, 0.0f);
        EXPECT_LE(value, 1.0f);
        EXPECT_LT(value, previous) << "distance = " << distance;
        previous = value;
    }
}

TEST(SphereProxyAOMathTest, ReceiverOnTheProxySurfaceStaysFinite)
{
    // The occluder's own surface sits at h == 1, where the unclamped closed form
    // divides by sqrt(h^2 - 1) == 0. This is not a corner case for this feature,
    // it is where every proxy's own object lives.
    for (const f32 ratio : { 0.0f, 0.25f, 0.5f, 0.9f, 1.0f, 1.0001f })
    {
        const f32 radius = 3.0f;
        const glm::vec4 sphere{ 0.0f, ratio * radius, 0.0f, radius };
        const f32 value = SphereProxyAO::SphereOcclusion(kOrigin, kUp, sphere);
        EXPECT_TRUE(std::isfinite(value)) << "ratio = " << ratio;
        EXPECT_GE(value, 0.0f);
        EXPECT_LE(value, 1.0f);
    }
}

TEST(SphereProxyAOMathTest, DegenerateProxyContributesNothing)
{
    EXPECT_FLOAT_EQ(SphereProxyAO::SphereOcclusion(kOrigin, kUp, glm::vec4(0.0f, 4.0f, 0.0f, 0.0f)), 0.0f);
    EXPECT_FLOAT_EQ(SphereProxyAO::SphereOcclusion(kOrigin, kUp, glm::vec4(0.0f, 4.0f, 0.0f, -1.0f)), 0.0f);
}

TEST(SphereProxyAOMathTest, IsInvariantUnderRigidTransforms)
{
    // The pass evaluates in VIEW space from world-space inputs, so the integral
    // must not depend on which rigid frame it is expressed in.
    const glm::vec3 position{ 1.0f, 2.0f, -3.0f };
    const glm::vec3 normal = glm::normalize(glm::vec3(0.3f, 0.9f, -0.2f));
    const glm::vec4 sphere{ 4.0f, 7.0f, -1.0f, 2.5f };
    const f32 reference = SphereProxyAO::SphereOcclusion(position, normal, sphere);

    const glm::mat4 rigid = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(11.0f, -4.0f, 6.0f)),
                                        0.7f, glm::normalize(glm::vec3(0.2f, 0.5f, 0.8f)));
    const glm::vec3 movedPosition = glm::vec3(rigid * glm::vec4(position, 1.0f));
    const glm::vec3 movedNormal = glm::normalize(glm::mat3(rigid) * normal);
    const glm::vec4 movedSphere{ glm::vec3(rigid * glm::vec4(glm::vec3(sphere), 1.0f)), sphere.w };

    EXPECT_NEAR(SphereProxyAO::SphereOcclusion(movedPosition, movedNormal, movedSphere), reference, 1e-3f);
}

// -----------------------------------------------------------------------------
// The self-occlusion fade — what the shader actually accumulates
// -----------------------------------------------------------------------------

TEST(SphereProxyAOMathTest, SelfOcclusionFadeRampsFromTheProxySurface)
{
    constexpr f32 radius = 4.0f;

    // Inside and exactly on the surface: nothing.
    EXPECT_FLOAT_EQ(SphereProxyAO::SelfOcclusionFade(0.0f, radius), 0.0f);
    EXPECT_FLOAT_EQ(SphereProxyAO::SelfOcclusionFade(radius, radius), 0.0f);

    // Beyond the ramp: unchanged.
    const f32 rampEnd = radius * (1.0f + SphereProxyAO::kSelfOcclusionFadeWidth);
    EXPECT_FLOAT_EQ(SphereProxyAO::SelfOcclusionFade(rampEnd, radius), 1.0f);
    EXPECT_FLOAT_EQ(SphereProxyAO::SelfOcclusionFade(rampEnd * 4.0f, radius), 1.0f);

    // Monotone in between, so no seam appears where a receiver crosses.
    f32 previous = -1.0f;
    for (i32 step = 0; step <= 20; ++step)
    {
        const f32 distance = radius + (rampEnd - radius) * (static_cast<f32>(step) / 20.0f);
        const f32 value = SphereProxyAO::SelfOcclusionFade(distance, radius);
        EXPECT_GE(value, previous);
        EXPECT_GE(value, 0.0f);
        EXPECT_LE(value, 1.0f);
        previous = value;
    }

    EXPECT_FLOAT_EQ(SphereProxyAO::SelfOcclusionFade(10.0f, 0.0f), 0.0f);
}

TEST(SphereProxyAOMathTest, ProxyDoesNotOccludeItsOwnObjectSurface)
{
    // The artefact this fade exists for: a volume-matched proxy pokes through
    // the object's own faces, so the face is INSIDE the sphere. Before the fade
    // the tangent clamp reported ~0.25 there and painted a soft dark disc across
    // the occluder's own front face.
    //
    // Modelled with an OVERSIZED proxy on purpose: 3.72 is the volume-matched
    // radius a 6-unit cube would get without containment, so its front face at
    // z = +3 is 0.72 inside the sphere. FitProxySpheres no longer produces such
    // a fit (it caps at the inscribed 3.0), but the fade is what makes the
    // integral safe if one ever arrives — from an authored proxy, say — and this
    // is the configuration that produced the visible disc.
    constexpr f32 proxyRadius = 3.72f;
    const glm::vec4 proxy{ 0.0f, 0.0f, 0.0f, proxyRadius };
    const glm::vec3 faceNormal{ 0.0f, 0.0f, 1.0f };

    // Near the face centre the point is inside the sphere and the fade is
    // exactly 0. Toward the face's outer corners it passes outside (a corner at
    // lateral 2.9 sits 4.17 from the centre, past the 3.72 radius), so the fade
    // is only partly on — but the raw integral is near zero there anyway,
    // because the face normal points away from the centre. Both regimes have to
    // stay far below anything an eye can see as a disc.
    for (const f32 lateral : { 0.0f, 1.0f, 2.0f })
    {
        const glm::vec3 facePoint{ lateral, 0.0f, 3.0f };
        EXPECT_FLOAT_EQ(SphereProxyAO::ProxyOcclusion(facePoint, faceNormal, proxy, kWideInfluence), 0.0f)
            << "lateral = " << lateral;
    }
    // ~0.012 at the extreme corner, where the surface genuinely IS outside this
    // oversized sphere and the raw integral genuinely is small but non-zero. The
    // bar is "far below anything visible as a disc", not zero; the disc that
    // motivated the fade read ~0.25.
    EXPECT_LT(SphereProxyAO::ProxyOcclusion(glm::vec3(2.9f, 0.0f, 3.0f), faceNormal, proxy, kWideInfluence), 0.02f);

    // Far enough out, both the fade and the window are inert and the term is
    // the raw integral again.
    const glm::vec3 farReceiver{ 0.0f, 8.0f, 0.0f };
    EXPECT_FLOAT_EQ(SphereProxyAO::ProxyOcclusion(farReceiver, kUp, proxy, kWideInfluence),
                    SphereProxyAO::SphereOcclusion(farReceiver, kUp, proxy));
}

TEST(SphereProxyAOMathTest, ProxyOcclusionIsContinuousAcrossTheProxySurface)
{
    // The ramp's whole job: crossing the proxy's surface must not step.
    constexpr f32 radius = 3.0f;
    const glm::vec4 proxy{ 0.0f, 0.0f, 0.0f, radius };
    f32 previous = SphereProxyAO::ProxyOcclusion(glm::vec3(0.0f, radius * 0.9f, 0.0f), kUp, proxy, kWideInfluence);
    for (i32 step = 0; step <= 60; ++step)
    {
        const f32 distance = radius * (0.9f + 0.02f * static_cast<f32>(step));
        const f32 value = SphereProxyAO::ProxyOcclusion(glm::vec3(0.0f, distance, 0.0f), kUp, proxy, kWideInfluence);
        EXPECT_LT(std::abs(value - previous), 0.06f) << "distance = " << distance;
        previous = value;
    }
}

TEST(SphereProxyAOMathTest, InfluenceWindowReachesZeroAtTheBinningCutoff)
{
    // The property that makes the tile seam impossible: at the influence radius
    // — exactly where the binning stops including the proxy — the contribution
    // is already zero, so dropping it changes nothing.
    constexpr f32 radius = 3.0f;
    constexpr f32 scale = 4.0f;
    const f32 influence = radius * scale;

    EXPECT_FLOAT_EQ(SphereProxyAO::InfluenceWindow(influence, radius, scale), 0.0f);
    EXPECT_FLOAT_EQ(SphereProxyAO::InfluenceWindow(influence * 1.5f, radius, scale), 0.0f);

    // Untouched across the near field this feature exists for.
    EXPECT_FLOAT_EQ(SphereProxyAO::InfluenceWindow(0.0f, radius, scale), 1.0f);
    EXPECT_FLOAT_EQ(SphereProxyAO::InfluenceWindow(influence * 0.5f, radius, scale), 1.0f);

    // Monotone and bounded in between, so it cannot introduce a band of its own.
    f32 previous = 2.0f;
    for (i32 step = 0; step <= 40; ++step)
    {
        const f32 distance = influence * (0.5f + 0.0125f * static_cast<f32>(step));
        const f32 value = SphereProxyAO::InfluenceWindow(distance, radius, scale);
        EXPECT_GE(value, 0.0f);
        EXPECT_LE(value, 1.0f);
        EXPECT_LE(value, previous + 1e-6f) << "distance = " << distance;
        previous = value;
    }

    EXPECT_FLOAT_EQ(SphereProxyAO::InfluenceWindow(1.0f, 0.0f, scale), 0.0f);
}

TEST(SphereProxyAOMathTest, ProxyContributionVanishesAtTheCutoffForEveryScale)
{
    // Asserted through ProxyOcclusion, not just the window, because it is the
    // ACCUMULATED term the binning drops — and it must vanish for whatever
    // influence scale the settings carry, not only the default.
    const glm::vec4 proxy{ 0.0f, 0.0f, 0.0f, 2.0f };
    for (const f32 scale : { 1.5f, 4.0f, 12.0f, 64.0f })
    {
        const f32 cutoff = proxy.w * scale;
        const glm::vec3 receiver{ 0.0f, cutoff, 0.0f };
        EXPECT_FLOAT_EQ(SphereProxyAO::ProxyOcclusion(receiver, kUp, proxy, scale), 0.0f)
            << "scale = " << scale;
    }
}

// -----------------------------------------------------------------------------
// Proxy fitting
// -----------------------------------------------------------------------------

TEST(SphereProxyAOMathTest, CubeFitsOneContainedSphere)
{
    std::array<SphereProxyAO::Proxy, SphereProxyAO::kMaxSpheresPerBounds> out{};
    const BoundingBox cube{ glm::vec3(-1.0f), glm::vec3(1.0f) };
    const u32 count = SphereProxyAO::FitProxySpheres(cube, 100.0f, out.data(), static_cast<u32>(out.size()));

    ASSERT_EQ(count, 1u);
    EXPECT_NEAR(out[0].Center.x, 0.0f, kTol);
    EXPECT_NEAR(out[0].Center.y, 0.0f, kTol);
    EXPECT_NEAR(out[0].Center.z, 0.0f, kTol);

    // A cube's volume-matched radius is cbrt(6/pi) = 1.24, which pokes out
    // through every face; containment caps it at the inscribed 1.0. Which of the
    // two wins is the whole point of the cap, so assert both halves.
    EXPECT_GT(std::cbrt(6.0f / std::numbers::pi_v<f32>), 1.0f);
    EXPECT_NEAR(out[0].Radius, 1.0f, kTol);
}

TEST(SphereProxyAOMathTest, EveryFittedSphereIsContainedInItsBox)
{
    // The invariant SelfOcclusionFade's narrow ramp depends on: no sphere pokes
    // out through a face, so the object's own surface is at h >= 1 and only a
    // thin guard band around the tangent point has to be faded away.
    const std::array<BoundingBox, 5> boxes = { {
        { glm::vec3(-1.0f), glm::vec3(1.0f) },                             // cube
        { glm::vec3(-8.0f, -1.0f, -1.0f), glm::vec3(8.0f, 1.0f, 1.0f) },   // corridor
        { glm::vec3(-5.0f, -0.25f, -5.0f), glm::vec3(5.0f, 0.25f, 5.0f) }, // slab
        { glm::vec3(-2.0f, -7.0f, -3.0f), glm::vec3(2.0f, 7.0f, 3.0f) },   // pillar
        { glm::vec3(3.0f, 1.0f, -4.0f), glm::vec3(9.0f, 6.0f, 2.0f) },     // off-origin
    } };

    std::array<SphereProxyAO::Proxy, SphereProxyAO::kMaxSpheresPerBounds> out{};
    for (const BoundingBox& box : boxes)
    {
        const u32 count = SphereProxyAO::FitProxySpheres(box, 1000.0f, out.data(), static_cast<u32>(out.size()));
        ASSERT_GT(count, 0u);
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3 lo = out[i].Center - glm::vec3(out[i].Radius);
            const glm::vec3 hi = out[i].Center + glm::vec3(out[i].Radius);
            EXPECT_GE(lo.x, box.Min.x - kTol);
            EXPECT_GE(lo.y, box.Min.y - kTol);
            EXPECT_GE(lo.z, box.Min.z - kTol);
            EXPECT_LE(hi.x, box.Max.x + kTol);
            EXPECT_LE(hi.y, box.Max.y + kTol);
            EXPECT_LE(hi.z, box.Max.z + kTol);
        }
    }
}

TEST(SphereProxyAOMathTest, ElongatedBoxSplitsAlongItsLongAxis)
{
    std::array<SphereProxyAO::Proxy, SphereProxyAO::kMaxSpheresPerBounds> out{};
    const BoundingBox corridor{ glm::vec3(-8.0f, -1.0f, -1.0f), glm::vec3(8.0f, 1.0f, 1.0f) };
    const u32 count = SphereProxyAO::FitProxySpheres(corridor, 100.0f, out.data(), static_cast<u32>(out.size()));

    ASSERT_GT(count, 1u);
    EXPECT_LE(count, SphereProxyAO::kMaxSpheresPerBounds);

    for (u32 i = 0; i < count; ++i)
    {
        // Spread along X, centred on the other two axes, in ascending order.
        EXPECT_NEAR(out[i].Center.y, 0.0f, kTol);
        EXPECT_NEAR(out[i].Center.z, 0.0f, kTol);
        EXPECT_LE(std::abs(out[i].Center.x), 8.0f);
        if (i > 0)
            EXPECT_GT(out[i].Center.x, out[i - 1].Center.x);
    }

    // The cross-section is what bounds the radius here (containment caps at the
    // smallest half-extent, which is 1), and the split is what stops a single
    // sphere of that radius from representing a 16-unit-long box.
    EXPECT_NEAR(out[0].Radius, 1.0f, kTol);
}

TEST(SphereProxyAOMathTest, RejectsDegenerateAndOversizedBounds)
{
    std::array<SphereProxyAO::Proxy, SphereProxyAO::kMaxSpheresPerBounds> out{};
    const u32 capacity = static_cast<u32>(out.size());

    // The NoBounds sentinel — what every caster with no tight bounds carries.
    EXPECT_EQ(SphereProxyAO::FitProxySpheres(NoBounds, 100.0f, out.data(), capacity), 0u);

    // Zero-thickness (a ground quad).
    const BoundingBox flat{ glm::vec3(-10.0f, 0.0f, -10.0f), glm::vec3(10.0f, 0.0f, 10.0f) };
    EXPECT_EQ(SphereProxyAO::FitProxySpheres(flat, 100.0f, out.data(), capacity), 0u);

    // Non-finite.
    const BoundingBox nan{ glm::vec3(std::nanf("")), glm::vec3(1.0f) };
    EXPECT_EQ(SphereProxyAO::FitProxySpheres(nan, 100.0f, out.data(), capacity), 0u);

    // Too big to be an object: the terrain/ground filter.
    const BoundingBox terrain{ glm::vec3(-500.0f), glm::vec3(500.0f) };
    EXPECT_EQ(SphereProxyAO::FitProxySpheres(terrain, 25.0f, out.data(), capacity), 0u);
    EXPECT_GT(SphereProxyAO::FitProxySpheres(terrain, 10000.0f, out.data(), capacity), 0u);
}

TEST(SphereProxyAOMathTest, SelectionKeepsTheNearestAndLargestProxies)
{
    // Three cubes at increasing distance. With a budget of one, the ranking must
    // keep the nearest — it is the one that can darken the frame most.
    std::vector<BoundingBox> bounds;
    for (const f32 distance : { 5.0f, 40.0f, 200.0f })
        bounds.push_back(BoundingBox{ glm::vec3(distance - 1.0f, -1.0f, -1.0f), glm::vec3(distance + 1.0f, 1.0f, 1.0f) });

    const auto picked = SphereProxyAO::SelectProxies(bounds, glm::vec3(0.0f), 1u, 100.0f);
    ASSERT_EQ(picked.size(), 1u);
    EXPECT_NEAR(picked[0].Center.x, 5.0f, kTol);

    // An unconstrained budget keeps all three.
    EXPECT_EQ(SphereProxyAO::SelectProxies(bounds, glm::vec3(0.0f), 64u, 100.0f).size(), 3u);

    // A zero budget, an empty source and an all-rejected source all yield none.
    EXPECT_TRUE(SphereProxyAO::SelectProxies(bounds, glm::vec3(0.0f), 0u, 100.0f).empty());
    EXPECT_TRUE(SphereProxyAO::SelectProxies({}, glm::vec3(0.0f), 64u, 100.0f).empty());
    EXPECT_TRUE(SphereProxyAO::SelectProxies(bounds, glm::vec3(0.0f), 64u, 0.1f).empty());
}

// -----------------------------------------------------------------------------
// Binding + GLSL contract
// -----------------------------------------------------------------------------

TEST(SphereProxyAOMathTest, UBOBindingIsRegisteredWithTheValidator)
{
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_SPHERE_PROXY_AO,
                                                       "SphereProxyAOParams"));
    EXPECT_LT(ShaderBindingLayout::UBO_SPHERE_PROXY_AO, ShaderBindingLayout::UBO_BINDING_LIMIT);
}

TEST(SphereProxyAOMathTest, ShaderAgreesWithTheCPUOnTheProxyArrayLengthAndBinding)
{
    // The proxy array is part of the uniform block, so its length is a
    // compile-time constant on BOTH sides and a drift is a silent std140
    // mismatch rather than a build error.
    // Several fixtures in this binary chdir the process into OloEditor/ and do
    // not chdir back (RenderPropertyTest, VulkanPassSuiteTest), so the repo-root
    // path this test documents is not the only working directory it can run
    // under. A single hard-coded path plus GTEST_SKIP would make this guard
    // quietly stop guarding depending on test ORDER, which is the worst failure
    // mode a mirror check can have.
    static constexpr std::array<const char*, 3> kCandidatePaths = {
        "OloEditor/assets/shaders/compute/SphereProxyAO.comp", // repo root
        "assets/shaders/compute/SphereProxyAO.comp",           // cwd == OloEditor/
        "../OloEditor/assets/shaders/compute/SphereProxyAO.comp",
    };

    std::filesystem::path shaderPath;
    std::ifstream file;
    for (const char* candidate : kCandidatePaths)
    {
        file.open(candidate);
        if (file)
        {
            shaderPath = candidate;
            break;
        }
        file.clear();
    }
    if (!file)
    {
        GTEST_SKIP() << "Shader source not reachable from the test working directory ("
                     << std::filesystem::current_path().generic_string() << ")";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string source = buffer.str();

    EXPECT_NE(source.find("#define OLO_SPA_MAX_PROXIES " + std::to_string(SphereProxyAO::kMaxProxies)),
              std::string::npos)
        << "SphereProxyAO.comp's OLO_SPA_MAX_PROXIES must equal SphereProxyAO::kMaxProxies";
    EXPECT_NE(source.find("#define OLO_SPA_WINDOW_START 0.6"), std::string::npos)
        << "SphereProxyAO.comp's OLO_SPA_WINDOW_START must equal InfluenceWindow's kRampStart";
    EXPECT_NE(source.find("#define OLO_SPA_SELF_FADE   0.15"), std::string::npos)
        << "SphereProxyAO.comp's OLO_SPA_SELF_FADE must equal SphereProxyAO::kSelfOcclusionFadeWidth "
        << "(currently " << SphereProxyAO::kSelfOcclusionFadeWidth << ")";
    EXPECT_NE(source.find("binding = " + std::to_string(ShaderBindingLayout::UBO_SPHERE_PROXY_AO) +
                          ") uniform SphereProxyAOParams"),
              std::string::npos)
        << "SphereProxyAO.comp must declare its params block at UBO_SPHERE_PROXY_AO";

    // The within-shader namespace rule for binding 64 (see ShaderBindingLayout).
    // Matched against #include DIRECTIVES only: the shader's own comments name
    // both files precisely to say it must not include them, and a bare substring
    // search fails on that prose rather than on the thing it is guarding.
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line))
    {
        const auto firstNonSpace = line.find_first_not_of(" 	");
        if (firstNonSpace == std::string::npos || line.compare(firstNonSpace, 8, "#include") != 0)
            continue;
        EXPECT_EQ(line.find("DDGICommon.glsl"), std::string::npos)
            << "SphereProxyAO.comp must not sample TEX_DDGI_VISIBILITY — same number as its UBO binding";
        EXPECT_EQ(line.find("GPUReadbackStats.glsl"), std::string::npos)
            << "SphereProxyAO.comp must not bind SSBO_GPU_STATS — same number as its UBO binding";
    }
}

TEST(SphereProxyAOMathTest, UBOMirrorsTheShaderBlockLayout)
{
    using UBO = UBOStructures::SphereProxyAOUBO;

    // std140 offsets the shader block relies on. Named explicitly because a
    // member inserted above any of them relayouts everything after it with no
    // build error at all.
    EXPECT_EQ(offsetof(UBO, NDCToViewMul), 0u);
    EXPECT_EQ(offsetof(UBO, NDCToViewAdd), 8u);
    EXPECT_EQ(offsetof(UBO, ScreenWidth), 16u);
    EXPECT_EQ(offsetof(UBO, Strength), 32u);
    EXPECT_EQ(offsetof(UBO, ViewMatrix), 48u);
    EXPECT_EQ(offsetof(UBO, Proxies), 112u);
    EXPECT_EQ(offsetof(UBO, CombineParams), 112u + 16u * SphereProxyAO::kMaxProxies);
    EXPECT_EQ(UBO::GetSize(), 128u + 16u * SphereProxyAO::kMaxProxies);
    EXPECT_EQ(std::tuple_size_v<decltype(UBO::Proxies)>, SphereProxyAO::kMaxProxies);
}
