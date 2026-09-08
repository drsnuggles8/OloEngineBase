// OLO_TEST_LAYER: L1
// =============================================================================
// ReferenceEnvironmentTest.cpp
//
// The directional environment (issue #869, ADR 0022) and the GROUND TRUTH that
// makes shipping it defensible.
//
// #869's second blocker was that a sky bake could not be validated: "with no
// directional environment model there is no ground truth to validate an HDRI
// bake against — you couldn't tell whether the result was right". That is true
// only if the candidate ground truth is the engine's own IBL. It is not. This
// file is the three checks that replace it, none of which involves the raster
// path:
//
//   1. REDUCTION      — a cubemap of constant radiance must behave exactly like
//                       the uniform environment the white-furnace suite already
//                       trusts. The whole existing furnace argument then covers
//                       the new code path as a special case.
//   2. ANALYTIC       — an unoccluded surface under a uniform sky has
//                       E = pi * L, in closed form, with no renderer in it.
//   3. QUADRATURE     — for a NON-uniform sky, the traced estimate must land on
//                       a dense deterministic Riemann sum of the same
//                       Evaluate(). The two share only the environment lookup:
//                       one integrates by cosine-weighted Monte Carlo through
//                       the integrator, the other by a grid over (theta, phi)
//                       with no sampler, no BVH and no MIS. Agreement is
//                       evidence about the TRANSPORT.
//
// Plus the convention itself: which face a direction reads, and how the two
// in-face axes are oriented. That is pinned against the OpenGL cubemap table
// rather than against anything the engine computes — `GetSkyboxSampleDirection`
// (include/SkyboxSampling.glsl) is the identity, so the engine samples with the
// raw world direction and the GL table IS the convention.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <glm/glm.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        constexpr f32 kPi = std::numbers::pi_v<f32>;

        // A cubemap whose texels are `radiance(direction)` evaluated at each
        // texel centre, using the SAME face table Sample() decodes with. Built
        // by inverting the table rather than by re-deriving it, so a fixture
        // bug cannot masquerade as agreement: if Sample()'s table were wrong,
        // this generator's inverse would be wrong the same way and the
        // face-selection tests below would catch it, not this one.
        [[nodiscard]] glm::vec3 FaceTexelDirection(u32 face, f32 s, f32 t)
        {
            // s, t in [0, 1] across the face; sc/tc in [-1, 1].
            const f32 sc = 2.0f * s - 1.0f;
            const f32 tc = 2.0f * t - 1.0f;
            switch (face)
            {
                case 0:
                    return glm::normalize(glm::vec3(1.0f, -tc, -sc)); // +X
                case 1:
                    return glm::normalize(glm::vec3(-1.0f, -tc, sc)); // -X
                case 2:
                    return glm::normalize(glm::vec3(sc, 1.0f, tc)); // +Y
                case 3:
                    return glm::normalize(glm::vec3(sc, -1.0f, -tc)); // -Y
                case 4:
                    return glm::normalize(glm::vec3(sc, -tc, 1.0f)); // +Z
                default:
                    return glm::normalize(glm::vec3(-sc, -tc, -1.0f)); // -Z
            }
        }

        template<typename Fn>
        [[nodiscard]] ReferenceEnvironmentCubemap MakeCubemap(u32 faceSize, Fn&& radiance)
        {
            std::vector<f32> rgba(static_cast<sizet>(ReferenceEnvironmentCubemap::kFaceCount) * faceSize * faceSize * 4u);
            sizet write = 0;
            for (u32 face = 0; face < ReferenceEnvironmentCubemap::kFaceCount; ++face)
            {
                for (u32 y = 0; y < faceSize; ++y)
                {
                    for (u32 x = 0; x < faceSize; ++x)
                    {
                        const f32 s = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(faceSize);
                        const f32 t = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(faceSize);
                        const glm::vec3 value = radiance(FaceTexelDirection(face, s, t));
                        rgba[write++] = value.x;
                        rgba[write++] = value.y;
                        rgba[write++] = value.z;
                        rgba[write++] = 1.0f;
                    }
                }
            }
            return ReferenceEnvironmentCubemap::FromFacesRgba32F(faceSize, std::span<const f32>(rgba));
        }

        // Six distinct constant faces, so a direction's answer names its face.
        [[nodiscard]] ReferenceEnvironmentCubemap MakePerFaceCubemap()
        {
            std::vector<f32> rgba(ReferenceEnvironmentCubemap::kFaceCount * 4u);
            const glm::vec3 colours[6] = {
                { 1.0f, 0.0f, 0.0f },
                { 0.5f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.5f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.0f, 0.0f, 0.5f },
            };
            for (u32 face = 0; face < 6; ++face)
            {
                rgba[face * 4 + 0] = colours[face].x;
                rgba[face * 4 + 1] = colours[face].y;
                rgba[face * 4 + 2] = colours[face].z;
                rgba[face * 4 + 3] = 1.0f;
            }
            return ReferenceEnvironmentCubemap::FromFacesRgba32F(1, std::span<const f32>(rgba));
        }

        // An EMPTY built scene: rays hit nothing, so every one of them escapes
        // and collects the environment. That is what isolates the environment
        // term from every other part of the integrator.
        [[nodiscard]] ReferenceScene MakeSkyOnlyScene(const ReferenceEnvironment& environment)
        {
            ReferenceScene scene;
            scene.SetEnvironment(environment);
            scene.Build();
            return scene;
        }

        // Deterministic quadrature of E(n) = integral of L(w) * max(0, n.w) dw
        // over the hemisphere about `n`, as a midpoint Riemann sum in
        // (theta, phi). No sampler, no random numbers, no scene: this is a
        // different integration RULE over the same integrand, which is what
        // makes it usable as ground truth for the Monte Carlo estimate.
        [[nodiscard]] glm::vec3 QuadratureIrradiance(const ReferenceEnvironment& environment, const glm::vec3& n,
                                                     u32 thetaSteps, u32 phiSteps)
        {
            // Any orthonormal basis about n; the integral is rotation
            // invariant about n, so the choice cannot bias the answer.
            const glm::vec3 up = std::abs(n.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 tangent = glm::normalize(glm::cross(up, n));
            const glm::vec3 bitangent = glm::cross(n, tangent);

            const f32 dTheta = 0.5f * kPi / static_cast<f32>(thetaSteps);
            const f32 dPhi = 2.0f * kPi / static_cast<f32>(phiSteps);
            glm::dvec3 sum(0.0);
            for (u32 i = 0; i < thetaSteps; ++i)
            {
                const f32 theta = (static_cast<f32>(i) + 0.5f) * dTheta;
                const f32 sinTheta = std::sin(theta);
                const f32 cosTheta = std::cos(theta);
                for (u32 j = 0; j < phiSteps; ++j)
                {
                    const f32 phi = (static_cast<f32>(j) + 0.5f) * dPhi;
                    const glm::vec3 direction =
                        tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi)) + n * cosTheta;
                    sum += glm::dvec3(environment.Evaluate(direction)) * static_cast<f64>(cosTheta * sinTheta);
                }
            }
            return glm::vec3(sum * static_cast<f64>(dTheta * dPhi));
        }

        [[nodiscard]] PathTracerSettings SkySettings(u32 samples)
        {
            PathTracerSettings settings;
            settings.SamplesPerPixel = samples;
            // One surface interaction is enough: nothing is hit, so every path
            // is "escape immediately". Higher would cost nothing and prove
            // nothing.
            settings.MaxBounces = 1;
            settings.RussianRouletteStartBounce = 0;
            settings.EnableNextEventEstimation = true;
            return settings;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The convention
    // -------------------------------------------------------------------------

    TEST(ReferenceEnvironment, EachAxisDirectionReadsItsOwnFace)
    {
        const ReferenceEnvironmentCubemap cube = MakePerFaceCubemap();
        ASSERT_TRUE(cube.IsValid());

        EXPECT_EQ(cube.Sample(glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f, 0.0f, 0.0f)) << "+X";
        EXPECT_EQ(cube.Sample(glm::vec3(-1.0f, 0.0f, 0.0f)), glm::vec3(0.5f, 0.0f, 0.0f)) << "-X";
        EXPECT_EQ(cube.Sample(glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f)) << "+Y";
        EXPECT_EQ(cube.Sample(glm::vec3(0.0f, -1.0f, 0.0f)), glm::vec3(0.0f, 0.5f, 0.0f)) << "-Y";
        EXPECT_EQ(cube.Sample(glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(0.0f, 0.0f, 1.0f)) << "+Z";
        EXPECT_EQ(cube.Sample(glm::vec3(0.0f, 0.0f, -1.0f)), glm::vec3(0.0f, 0.0f, 0.5f)) << "-Z";

        // Off-axis, but still in the +Y major-axis cone.
        EXPECT_EQ(cube.Sample(glm::vec3(0.3f, 1.0f, -0.4f)), glm::vec3(0.0f, 1.0f, 0.0f));
        // A direction need not be normalized — only its major axis and ratios
        // matter, which is what a cube lookup means.
        EXPECT_EQ(cube.Sample(glm::vec3(0.0f, 17.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    TEST(ReferenceEnvironment, InFaceAxesFollowTheGlOrientationTable)
    {
        // A 2x2 +Z face with four distinguishable texels; every other face is
        // black. The GL table says the +Z face has sc = +x and tc = -y, so
        // s grows with x and t grows as y DECREASES. Two probes pin both axes
        // independently — a swapped or flipped axis moves exactly one of them.
        constexpr u32 kFaceSize = 2;
        std::vector<f32> rgba(static_cast<sizet>(ReferenceEnvironmentCubemap::kFaceCount) * kFaceSize * kFaceSize * 4u, 0.0f);
        const sizet plusZ = static_cast<sizet>(4) * kFaceSize * kFaceSize * 4u; // face 4
        // Row 0 (t small): (0.1, 0.2); row 1 (t large): (0.3, 0.4).
        const f32 values[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        for (u32 i = 0; i < 4; ++i)
        {
            rgba[plusZ + i * 4 + 0] = values[i];
            rgba[plusZ + i * 4 + 3] = 1.0f;
        }
        const ReferenceEnvironmentCubemap cube =
            ReferenceEnvironmentCubemap::FromFacesRgba32F(kFaceSize, std::span<const f32>(rgba));
        ASSERT_TRUE(cube.IsValid());

        // Deep into each quadrant so bilinear clamping lands on that texel
        // exactly (the texel centres are at s, t = 0.25 and 0.75; anything
        // outside that range clamps).
        const auto probe = [&cube](f32 x, f32 y)
        { return cube.Sample(glm::vec3(x, y, 1.0f)).r; };
        EXPECT_FLOAT_EQ(probe(-0.9f, 0.9f), 0.1f) << "x < 0, y > 0 must be s small, t small";
        EXPECT_FLOAT_EQ(probe(0.9f, 0.9f), 0.2f) << "x > 0, y > 0 must be s large, t small";
        EXPECT_FLOAT_EQ(probe(-0.9f, -0.9f), 0.3f) << "x < 0, y < 0 must be s small, t large";
        EXPECT_FLOAT_EQ(probe(0.9f, -0.9f), 0.4f) << "x > 0, y < 0 must be s large, t large";
    }

    TEST(ReferenceEnvironment, DegenerateDirectionsReadBlackRatherThanIndexingOutOfRange)
    {
        const ReferenceEnvironmentCubemap cube = MakePerFaceCubemap();
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();

        EXPECT_EQ(cube.Sample(glm::vec3(0.0f)), glm::vec3(0.0f));
        EXPECT_EQ(cube.Sample(glm::vec3(nan, 0.0f, 0.0f)), glm::vec3(0.0f));
        EXPECT_EQ(cube.Sample(glm::vec3(0.0f, inf, 0.0f)), glm::vec3(0.0f));

        // An unbuilt cube answers black too, rather than reading Texels[0] of
        // an empty vector.
        const ReferenceEnvironmentCubemap empty;
        EXPECT_FALSE(empty.IsValid());
        EXPECT_EQ(empty.Sample(glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.0f));
    }

    TEST(ReferenceEnvironment, NonFiniteAndNegativeTexelsAreDroppedAtConstruction)
    {
        // One NaN in a sky poisons every baked texel in the scene, and an .hdr
        // with a stray negative is a real thing. Both are neutralised where
        // they enter, not at every escape.
        std::vector<f32> rgba(ReferenceEnvironmentCubemap::kFaceCount * 4u, 1.0f);
        rgba[0] = std::numeric_limits<f32>::quiet_NaN();
        rgba[5] = -3.0f;
        const ReferenceEnvironmentCubemap cube = ReferenceEnvironmentCubemap::FromFacesRgba32F(1, std::span<const f32>(rgba));
        ASSERT_TRUE(cube.IsValid());
        EXPECT_FLOAT_EQ(cube.Sample(glm::vec3(1.0f, 0.0f, 0.0f)).r, 0.0f);
        EXPECT_FLOAT_EQ(cube.Sample(glm::vec3(-1.0f, 0.0f, 0.0f)).g, 0.0f);
    }

    // -------------------------------------------------------------------------
    // Check 1: reduction to the uniform environment
    // -------------------------------------------------------------------------

    TEST(ReferenceEnvironment, ConstantCubemapEvaluatesLikeTheUniformEnvironment)
    {
        const glm::vec3 radiance(0.35f, 0.6f, 0.9f);

        ReferenceEnvironment uniform;
        uniform.Radiance = radiance;

        ReferenceEnvironment directional;
        directional.Cubemap = std::make_shared<const ReferenceEnvironmentCubemap>(
            ReferenceEnvironmentCubemap::Constant(radiance));

        EXPECT_FALSE(uniform.IsDirectional());
        EXPECT_TRUE(directional.IsDirectional());

        // Bit-exact, not approximate: a constant cube's bilinear taps are four
        // copies of one texel, so the filter is an identity. If that ever
        // stops holding, the reduction argument this file rests on has moved
        // and should fail loudly rather than pass within a tolerance.
        const glm::vec3 directions[] = {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.3f, 0.7f, -0.2f },
            { -0.9f, 0.1f, 0.4f },
        };
        for (const glm::vec3& d : directions)
        {
            EXPECT_EQ(directional.Evaluate(d), uniform.Evaluate(d)) << "direction " << d.x << ", " << d.y << ", " << d.z;
        }
    }

    TEST(ReferenceEnvironment, IntensityScalesEitherEnvironmentTheSameWay)
    {
        const glm::vec3 radiance(0.2f, 0.4f, 0.8f);

        ReferenceEnvironment uniform;
        uniform.Radiance = radiance;
        uniform.Intensity = 2.5f;

        ReferenceEnvironment directional;
        directional.Cubemap = std::make_shared<const ReferenceEnvironmentCubemap>(
            ReferenceEnvironmentCubemap::Constant(radiance));
        directional.Intensity = 2.5f;

        const glm::vec3 d(0.3f, 0.7f, -0.2f);
        EXPECT_EQ(uniform.Evaluate(d), radiance * 2.5f);
        EXPECT_EQ(directional.Evaluate(d), uniform.Evaluate(d));
    }

    // -------------------------------------------------------------------------
    // Check 2: the analytic case
    // -------------------------------------------------------------------------

    TEST(ReferenceEnvironment, UnoccludedIrradianceUnderAUniformSkyIsPiTimesRadiance)
    {
        // The closed form, with no renderer in it: for an unoccluded surface
        // under radiance L arriving from every direction,
        //     E = integral of L cos(theta) dw = pi * L.
        // This is the single strongest statement available about the new code
        // path, and it holds for BOTH environment representations — so it also
        // re-states the reduction as a number rather than as an equality of
        // lookups.
        const glm::vec3 radiance(0.4f, 0.6f, 1.0f);
        const glm::vec3 expected = radiance * kPi;

        ReferenceEnvironment uniform;
        uniform.Radiance = radiance;
        ReferenceEnvironment directional;
        directional.Cubemap = std::make_shared<const ReferenceEnvironmentCubemap>(
            ReferenceEnvironmentCubemap::Constant(radiance));

        for (const ReferenceEnvironment& environment : { uniform, directional })
        {
            const ReferenceScene scene = MakeSkyOnlyScene(environment);
            for (const glm::vec3& n : { glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                                        glm::normalize(glm::vec3(0.4f, 0.8f, -0.3f)) })
            {
                const glm::vec3 e =
                    PathTracer::EstimateIrradiance(scene, glm::vec3(0.0f), n, SkySettings(1024), 0x51'0000u);
                // Cosine-weighted sampling of a CONSTANT integrand has zero
                // variance — the estimator is L * pi for every sample — so
                // this is exact up to float summation, not up to noise.
                EXPECT_NEAR(e.x, expected.x, 1e-3f);
                EXPECT_NEAR(e.y, expected.y, 1e-3f);
                EXPECT_NEAR(e.z, expected.z, 1e-3f);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Check 3: the quadrature, for a sky that actually has direction in it
    // -------------------------------------------------------------------------

    TEST(ReferenceEnvironment, TracedSkyIrradianceMatchesAnIndependentQuadrature)
    {
        // A smooth, strongly directional sky: bright and warm overhead, dim
        // and cool below the horizon. Smooth on purpose — a per-face-constant
        // sky is discontinuous, and then the GRID rule's own error, not the
        // tracer's, would set the tolerance.
        const auto skyRadiance = [](const glm::vec3& d) -> glm::vec3
        {
            const f32 up = 0.5f * (d.y + 1.0f); // 0 straight down, 1 straight up
            return glm::mix(glm::vec3(0.05f, 0.06f, 0.08f), glm::vec3(1.2f, 1.0f, 0.7f), up);
        };

        ReferenceEnvironment environment;
        environment.Cubemap =
            std::make_shared<const ReferenceEnvironmentCubemap>(MakeCubemap(32, skyRadiance));
        ASSERT_TRUE(environment.Cubemap->IsValid());

        const ReferenceScene scene = MakeSkyOnlyScene(environment);

        // Three normals, because a magnitude-only agreement would also be
        // produced by an environment that had lost its direction: up sees the
        // bright half, down sees the dim half, sideways sees exactly half of
        // each. The ORDERING between them is the directional claim.
        const glm::vec3 normals[] = { { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
        glm::vec3 traced[3]{};
        glm::vec3 quadrature[3]{};
        for (u32 i = 0; i < 3; ++i)
        {
            traced[i] = PathTracer::EstimateIrradiance(scene, glm::vec3(0.0f), normals[i], SkySettings(4096),
                                                       0x51'0100u + i);
            // 256 x 512 midpoint cells over the hemisphere. The integrand is
            // smooth, so this rule's own error is far below the tolerance.
            quadrature[i] = QuadratureIrradiance(environment, normals[i], 256, 512);

            for (u32 c = 0; c < 3; ++c)
            {
                const f32 reference = quadrature[i][c];
                ASSERT_GT(reference, 0.0f);
                EXPECT_NEAR(traced[i][c] / reference, 1.0f, 0.02f)
                    << "normal " << i << " channel " << c << ": traced " << traced[i][c] << " vs quadrature "
                    << reference;
            }
        }

        // The shape, asserted as an ordering so no tolerance can wash it out:
        // up > sideways > down. A sky that had lost its direction — a bug that
        // averaged the cube, say — passes every magnitude check above at some
        // scale and fails this.
        EXPECT_GT(traced[0].r, traced[2].r);
        EXPECT_GT(traced[2].r, traced[1].r);
    }

    TEST(ReferenceEnvironment, ADirectionalSkyIsWhatTheEscapingRayCollects)
    {
        // The guard for the plumbing itself: with the cubemap detached the
        // same scene must fall back to the uniform radiance, so a future
        // refactor that reads Radiance directly instead of going through
        // Evaluate() fails here rather than silently tracing the wrong sky.
        ReferenceEnvironment environment;
        environment.Radiance = glm::vec3(0.1f);
        environment.Cubemap = std::make_shared<const ReferenceEnvironmentCubemap>(
            ReferenceEnvironmentCubemap::Constant(glm::vec3(1.0f)));

        const ReferenceScene withSky = MakeSkyOnlyScene(environment);
        environment.Cubemap.reset();
        const ReferenceScene withoutSky = MakeSkyOnlyScene(environment);

        const glm::vec3 n(0.0f, 1.0f, 0.0f);
        const glm::vec3 lit = PathTracer::EstimateIrradiance(withSky, glm::vec3(0.0f), n, SkySettings(256), 7u);
        const glm::vec3 dim = PathTracer::EstimateIrradiance(withoutSky, glm::vec3(0.0f), n, SkySettings(256), 7u);

        EXPECT_NEAR(lit.r, kPi, 1e-3f);
        EXPECT_NEAR(dim.r, 0.1f * kPi, 1e-3f);
    }
} // namespace OloEngine::Tests
