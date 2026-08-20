// OLO_TEST_LAYER: L1
// =============================================================================
// ReferenceSceneBuilderTest.cpp — the ECS -> ReferenceScene adapter, validated
// against a hand-built reference twin (issue #439).
//
// WHAT IS UNDER TEST
// ------------------
// ReferenceSceneBuilder promises that walking a live ECS Scene produces the
// SAME world the reference path tracer would have been handed by hand — same
// surfaces, same materials, same light model. The test therefore builds one
// room TWICE from a single table of boxes (the §4 "one description" rule from
// docs/agent-rules/reference-path-tracer.md):
//
//   * through the ECS: cube MeshComponents + a PointLightComponent, walked by
//     ReferenceSceneBuilder::AddScene, and
//   * by hand: ReferenceSceneFixtures::AddBox at the same coordinates with the
//     same albedos, plus a ReferenceLight packed the way Scene.cpp packs the
//     MultiLight UBO (AttenuationParams = (1, 0, m_Attenuation, m_Range)).
//
// Both worlds render LambertianDiffuseOnly so the comparison isolates the
// builder's geometry/material/light mirroring from any BRDF question; the
// physical quantity compared is PathTracer::EstimateIrradiance, not pixels.
//
// Also pinned: the builder's determinism contract (two walks of the same scene
// produce BIT-IDENTICAL traces — the path tracer's exact-hash gate depends on
// stable geometry and light ordering), the non-uniform-scale pre-transform
// path (an entity scaled (1,2,1) must exist in the built world at its scaled
// coordinates, since ReferenceScene::AddInstance would loudly reject that
// transform as an instance), and geometry sharing for uniform-scale instances.
//
// Headless — no GL. MeshPrimitives::CreateCube works with no graphics device
// (MeshSource::Build defers GPU buffers and keeps the CPU arrays, which is all
// the builder reads).
//
// Classification: L1 (pure CPU, headless).
// =============================================================================

#include "OloEnginePCH.h"

#include "PathTracing/ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PathTracing/PathSampler.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;
    namespace Fixtures = OloEngine::Tests::PathTracingFixtures;

    namespace
    {
        // ---------------------------------------------------------------------
        // The room. ONE description, consumed by the ECS scene and the
        // hand-built reference twin alike — a hand-mirrored pair is how this
        // class of comparison goes quietly wrong (a wall half a unit off reads
        // as a stable, fictitious "builder bug").
        //
        // A PARTIAL room on purpose: floor + two walls, open elsewhere, so the
        // light field has real spatial structure (an enclosed symmetric room
        // makes every probe nearly equal and the ordering assertion vacuous).
        // Environment radiance is zero in BOTH worlds, so the openings agree.
        //
        // `Scale` is the full extent — a MeshComponent cube spans -0.5..0.5,
        // so world extent == scale (same convention as DDGIReferenceParityTest).
        // ---------------------------------------------------------------------
        struct BoxDesc
        {
            const char* Name;
            glm::vec3 Position;
            glm::vec3 Scale;
            glm::vec3 Albedo;
        };

        [[nodiscard]] const std::vector<BoxDesc>& RoomBoxes()
        {
            static const std::vector<BoxDesc> s_Boxes = {
                { "Floor", { 0.0f, 0.0f, 0.0f }, { 8.0f, 0.2f, 8.0f }, { 0.55f, 0.55f, 0.55f } },
                { "Left Wall", { -4.0f, 2.0f, 0.0f }, { 0.2f, 4.0f, 8.0f }, { 0.6f, 0.1f, 0.1f } },
                { "Back Wall", { 0.0f, 2.0f, -4.0f }, { 8.0f, 4.0f, 0.2f }, { 0.55f, 0.55f, 0.55f } },
            };
            return s_Boxes;
        }

        // The single point light, described once for both worlds. Off-centre so
        // the three probe points below receive clearly different irradiance.
        constexpr glm::vec3 kLightPosition{ 1.5f, 3.0f, 1.0f };
        constexpr glm::vec3 kLightColor{ 1.0f, 0.95f, 0.9f };
        constexpr f32 kLightIntensity = 12.0f;
        constexpr f32 kLightRange = 14.0f;
        constexpr f32 kLightAttenuation = 1.0f;

        constexpr f32 kRoughness = 0.9f; // mirrored into the MaterialComponents below

        // Interior probe points (all with a +Y normal), chosen well clear of
        // every surface. Light distances: A = |(1.5,1.8,1)| = 2.55, B =
        // |(4.5,1.8,1)| = 4.95, C = |(1,2.4,1.5)| = 3.00 — all inside the
        // 14-unit range, and far enough apart that the reference ranks them
        // decisively (the ordering assertion skips <15% ties anyway).
        [[nodiscard]] const std::vector<glm::vec3>& ProbePoints()
        {
            static const std::vector<glm::vec3> s_Points = {
                { 0.0f, 1.2f, 0.0f },
                { -3.0f, 1.2f, 0.0f },
                { 2.5f, 0.6f, 2.5f },
            };
            return s_Points;
        }

        // ---------------------------------------------------------------------
        // World builders.
        // ---------------------------------------------------------------------

        [[nodiscard]] Ref<Scene> BuildEcsRoom()
        {
            auto scene = Ref<Scene>::Create();

            for (const BoxDesc& box : RoomBoxes())
            {
                Entity entity = scene->CreateEntity(box.Name);
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = box.Position;
                transform.Scale = box.Scale;

                auto& mesh = entity.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                    mesh.m_MeshSource = cube->GetMeshSource();

                auto& material = entity.AddComponent<MaterialComponent>();
                material.m_Material.SetBaseColorFactor(glm::vec4(box.Albedo, 1.0f));
                material.m_Material.SetMetallicFactor(0.0f);
                material.m_Material.SetRoughnessFactor(kRoughness);
            }

            {
                Entity lightEntity = scene->CreateEntity("Point Light");
                lightEntity.GetComponent<TransformComponent>().Translation = kLightPosition;
                auto& pointLight = lightEntity.AddComponent<PointLightComponent>();
                pointLight.m_Color = kLightColor;
                pointLight.m_Intensity = kLightIntensity;
                pointLight.m_Range = kLightRange;
                pointLight.m_Attenuation = kLightAttenuation;
            }

            return scene;
        }

        // The builder-made twin. Lambertian on this side AND the hand side, so
        // the comparison isolates the builder's TRANSPORT inputs (geometry,
        // materials, lights) rather than folding in a BRDF question.
        [[nodiscard]] ReferenceScene BuildViaBuilder(Scene& scene)
        {
            ReferenceSceneBuilder builder;
            builder.AddScene(scene, {});

            ReferenceSceneBuildOptions options;
            options.EnvironmentRadiance = glm::vec3(0.0f);
            options.LambertianDiffuseOnly = true;
            return builder.Build(options);
        }

        // The hand-built twin, ReferenceSceneFixtures-style, from the SAME
        // table of boxes.
        [[nodiscard]] ReferenceScene BuildByHand()
        {
            ReferenceScene scene;

            for (const BoxDesc& box : RoomBoxes())
            {
                ReferenceMaterial material;
                material.BaseColor = box.Albedo;
                material.Metallic = 0.0f;
                material.Roughness = kRoughness;
                material.LambertianDiffuseOnly = true;
                const u32 materialIndex = scene.AddMaterial(material);

                Fixtures::AddBox(scene, box.Position - box.Scale * 0.5f, box.Position + box.Scale * 0.5f,
                                 materialIndex);
            }

            ReferenceLight light;
            light.Type = ReferenceLightType::Point;
            light.Position = kLightPosition;
            light.Color = kLightColor;
            light.Intensity = kLightIntensity;
            // Scene.cpp packs a point light as (1, 0, m_Attenuation, m_Range);
            // ReferenceBRDF::CalculateAttenuation is the port of the shader
            // function that consumes it. The builder must produce exactly this
            // packing, or the comparison would measure a falloff-convention
            // difference and call it a builder bug.
            light.AttenuationParams = glm::vec4(1.0f, 0.0f, kLightAttenuation, kLightRange);
            scene.AddLight(light);

            scene.Build();
            return scene;
        }

        [[nodiscard]] f32 MeanChannel(const glm::vec3& v)
        {
            return (v.x + v.y + v.z) / 3.0f;
        }

        // Shared trace settings — small on purpose (this whole file's tracing
        // budget is under ~1k paths of <= 3 surface interactions; ~65 s is the
        // cost of the whole PathTracing set under MSVC Debug, and this file
        // must stay a rounding error inside it).
        [[nodiscard]] PathTracerSettings ParitySettings()
        {
            PathTracerSettings settings;
            settings.SamplesPerPixel = 96;
            // Direct + two bounces: with 0.55-0.6 albedos the remaining series
            // tail is ~0.55^3 = 17% of the bounce term, identical in both
            // worlds — the comparison needs the two worlds to agree, not the
            // series to converge.
            settings.MaxBounces = 3;
            settings.RussianRouletteStartBounce = 0; // 0 disables RR: less noise at these depths
            settings.EnableNextEventEstimation = true;
            return settings;
        }
    } // namespace

    // =========================================================================
    // 1. The builder twin transports like the hand twin.
    // =========================================================================
    TEST(ReferenceSceneBuilderTest, BuilderTwinMatchesHandBuiltTwinTransport)
    {
        auto ecsScene = BuildEcsRoom();
        ReferenceScene built = BuildViaBuilder(*ecsScene);
        ReferenceScene handmade = BuildByHand();

        ASSERT_TRUE(built.IsBuilt());
        ASSERT_TRUE(handmade.IsBuilt());
        // 3 boxes and 1 light survived the walk (structure first, so a failure
        // below is attributable).
        ASSERT_EQ(built.GetLights().size(), 1u);
        ASSERT_FALSE(built.GetInstances().empty());

        const PathTracerSettings settings = ParitySettings();

        std::vector<f32> builtMeans;
        std::vector<f32> handMeans;
        for (sizet i = 0; i < ProbePoints().size(); ++i)
        {
            const glm::vec3& point = ProbePoints()[i];
            // Same seed on both sides: PathSampler is a pure function of
            // (seed, sample index, dimension), so both worlds integrate the
            // SAME hemisphere directions and the estimates are strongly
            // correlated — which is what makes a +-10% band meaningful at
            // only 96 samples.
            const u32 seed = MakePixelSeed(static_cast<u32>(i), 0u, 0x439u);

            const glm::vec3 builtE =
                PathTracer::EstimateIrradiance(built, point, glm::vec3(0.0f, 1.0f, 0.0f), settings, seed);
            const glm::vec3 handE =
                PathTracer::EstimateIrradiance(handmade, point, glm::vec3(0.0f, 1.0f, 0.0f), settings, seed);

            const f32 builtMean = MeanChannel(builtE);
            const f32 handMean = MeanChannel(handE);

            // The hand twin must actually light this point, or the ratio below
            // is 0/0 and the test measures nothing.
            ASSERT_GT(handMean, 1e-4f) << "hand-built twin is dark at probe " << i
                                       << " — the fixture, not the builder, is broken";
            ASSERT_GT(builtMean, 1e-4f)
                << "builder twin is dark at probe " << i << " where the hand twin is lit — geometry or "
                << "light was dropped by the walk";

            // MAGNITUDE. The two worlds hold geometrically identical surfaces
            // (12-triangle cube slabs vs 6-quad boxes on the same planes),
            // identical factor materials and an identically-packed light, so
            // the residual is Monte Carlo noise plus float-order differences
            // from the differing instance transforms (the builder's boxes go
            // through a TRS instance transform; the hand twin bakes world
            // coordinates under identity instances). Band per the task spec.
            const f32 ratio = builtMean / handMean;
            EXPECT_GT(ratio, 0.9f) << "probe " << i << ": builder twin is " << ratio
                                   << "x the hand-built twin — too dark (built " << builtMean << ", hand "
                                   << handMean << ")";
            EXPECT_LT(ratio, 1.11f) << "probe " << i << ": builder twin is " << ratio
                                    << "x the hand-built twin — too bright (built " << builtMean << ", hand "
                                    << handMean << ")";

            builtMeans.push_back(builtMean);
            handMeans.push_back(handMean);
        }

        // FIELD SHAPE. Magnitude can be off by a constant and still be useful;
        // the SPATIAL ORDERING cannot — a global scale error passes the band
        // above but cannot pass this (the field-shape assertion from
        // docs/agent-rules/reference-path-tracer.md §4). Ties (<15%) are
        // skipped: asserting an ordering the reference itself calls a tie is a
        // coin flip.
        for (sizet a = 0; a < handMeans.size(); ++a)
        {
            for (sizet b = a + 1; b < handMeans.size(); ++b)
            {
                const f32 relativeGap =
                    std::abs(handMeans[a] - handMeans[b]) / std::max(handMeans[a], handMeans[b]);
                if (relativeGap < 0.15f)
                    continue;

                const bool handOrder = handMeans[a] > handMeans[b];
                const bool builtOrder = builtMeans[a] > builtMeans[b];
                EXPECT_EQ(handOrder, builtOrder)
                    << "probes " << a << " and " << b << " rank differently: hand " << handMeans[a] << " vs "
                    << handMeans[b] << ", built " << builtMeans[a] << " vs " << builtMeans[b]
                    << " — the builder twin's light field has a different spatial structure";
            }
        }
    }

    // =========================================================================
    // 2. Two builds of the same scene are BIT-IDENTICAL.
    //
    // The path tracer's determinism gate asserts exact hashes, which requires
    // the scene description itself to be byte-stable: geometry order, light
    // order, instance order all change the floating-point accumulation order.
    // AddScene sorts every gathered entity by UUID precisely so EnTT pool
    // packing order cannot leak into the result.
    // =========================================================================
    TEST(ReferenceSceneBuilderTest, TwoBuildsOfTheSameSceneTraceBitIdentically)
    {
        auto ecsScene = BuildEcsRoom();

        ReferenceScene first = BuildViaBuilder(*ecsScene);
        ReferenceScene second = BuildViaBuilder(*ecsScene);

        ASSERT_EQ(first.GetGeometryCount(), second.GetGeometryCount());
        ASSERT_EQ(first.GetInstances().size(), second.GetInstances().size());
        ASSERT_EQ(first.GetLights().size(), second.GetLights().size());

        PathTracerSettings settings;
        settings.SamplesPerPixel = 32;
        settings.MaxBounces = 2;
        settings.RussianRouletteStartBounce = 0;
        settings.EnableNextEventEstimation = true;

        for (sizet i = 0; i < 2; ++i)
        {
            const glm::vec3& point = ProbePoints()[i];
            const u32 seed = MakePixelSeed(static_cast<u32>(i), 1u, 0x439u);

            const glm::vec3 a =
                PathTracer::EstimateIrradiance(first, point, glm::vec3(0.0f, 1.0f, 0.0f), settings, seed);
            const glm::vec3 b =
                PathTracer::EstimateIrradiance(second, point, glm::vec3(0.0f, 1.0f, 0.0f), settings, seed);

            // EXPECT_EQ on floats is CORRECT here, deliberately violating the
            // usual "never == on floats" rule: the contract under test is
            // bit-identity (same scene + same seed => the same bits), not
            // numerical closeness. A tolerance would silently accept the
            // ordering instability this test exists to catch.
            EXPECT_EQ(a.x, b.x) << "probe " << i << ": two builds of the same scene diverge — ordering is unstable";
            EXPECT_EQ(a.y, b.y) << "probe " << i << ": two builds of the same scene diverge — ordering is unstable";
            EXPECT_EQ(a.z, b.z) << "probe " << i << ": two builds of the same scene diverge — ordering is unstable";
        }
    }

    // =========================================================================
    // 3. Non-uniform scale is PRE-TRANSFORMED, not dropped.
    //
    // ReferenceScene::AddInstance loudly rejects a (1, 2, 1)-scaled transform
    // (its TMax conversion needs a single scalar). The builder must therefore
    // bake such an entity's geometry to world space under an identity
    // instance — silently dropping it would leave a hole in the world that
    // reads as a transport bug in whatever comparison runs next.
    // =========================================================================
    TEST(ReferenceSceneBuilderTest, NonUniformScaleIsPreTransformedNotDropped)
    {
        auto scene = Ref<Scene>::Create();
        {
            Entity entity = scene->CreateEntity("Stretched");
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = { 0.0f, 1.0f, 0.0f };
            transform.Scale = { 1.0f, 2.0f, 1.0f };

            auto& mesh = entity.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                mesh.m_MeshSource = cube->GetMeshSource();
        }

        ReferenceSceneBuilder builder;
        builder.AddScene(*scene, {});
        ReferenceSceneBuildOptions options;
        ReferenceScene built = builder.Build(options);

        ASSERT_TRUE(built.IsBuilt());
        ASSERT_EQ(built.GetInstances().size(), 1u) << "the non-uniformly scaled entity was dropped";
        ASSERT_EQ(built.GetGeometryCount(), 1u);
        // Pre-transformed geometry rides an IDENTITY instance; UniformScale is
        // derived from the accepted transform, so 1.0 here is the signature of
        // the pre-transform path (a smuggled non-uniform transform could not
        // produce it).
        EXPECT_NEAR(built.GetInstances()[0].UniformScale, 1.0f, 1e-6f);

        // The unit cube spans -0.5..0.5 locally; scale (1, 2, 1) + translation
        // (0, 1, 0) => world box x,z in [-0.5, 0.5], y in [0, 2]. A ray from
        // (5, 1.8, 0) along -X meets the +X face plane x = 0.5 at t = 4.5,
        // hitting (0.5, 1.8, 0) — a point INSIDE the scaled face (y <= 2) but
        // OUTSIDE the unscaled cube (whose top would sit at y = 1.5), so this
        // single ray distinguishes "pre-transformed correctly" from both
        // "dropped" and "added without the scale".
        const Ray ray(glm::vec3(5.0f, 1.8f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f));
        SurfaceInteraction hit;
        ASSERT_TRUE(built.Intersect(ray, hit))
            << "no surface where the scaled +X face should be — the scale was not baked in";
        EXPECT_NEAR(hit.Distance, 4.5f, 1e-3f);
        EXPECT_NEAR(hit.Position.x, 0.5f, 1e-3f);
        EXPECT_NEAR(hit.Position.y, 1.8f, 1e-3f);
        // +X face normal under the inverse-transpose of diag(1, 2, 1) is
        // still (1, 0, 0) after renormalization.
        EXPECT_GT(hit.GeometricNormal.x, 0.99f);
        EXPECT_GT(hit.ShadingNormal.x, 0.99f);

        // And the stretched TOP face: from (0, 5, 0) straight down, the first
        // surface is y = 2 (t = 3), not the unscaled cube's y = 1.5 (t = 3.5).
        const Ray downRay(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
        SurfaceInteraction topHit;
        ASSERT_TRUE(built.Intersect(downRay, topHit));
        EXPECT_NEAR(topHit.Distance, 3.0f, 1e-3f);
        EXPECT_GT(topHit.GeometricNormal.y, 0.99f);
    }

    // =========================================================================
    // 4. Uniform-scale entities SHARE geometry; the predicate and the
    //    zero-intensity light rule are honoured.
    // =========================================================================
    TEST(ReferenceSceneBuilderTest, UniformScaleInstancesShareGeometry)
    {
        auto scene = Ref<Scene>::Create();

        // ONE MeshSource, three entities — the cache key is (MeshSource,
        // submesh), so the two included entities must produce ONE geometry and
        // TWO instances.
        Ref<Mesh> cube = MeshPrimitives::CreateCube();
        ASSERT_TRUE(cube);
        Ref<MeshSource> sharedSource = cube->GetMeshSource();
        ASSERT_TRUE(sharedSource);

        const auto addCubeEntity = [&](const char* name, const glm::vec3& translation, f32 scale)
        {
            Entity entity = scene->CreateEntity(name);
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.Scale = glm::vec3(scale);
            auto& mesh = entity.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = sharedSource;
        };
        addCubeEntity("Big", { -2.0f, 0.0f, 0.0f }, 2.0f);
        addCubeEntity("Small", { 2.0f, 0.0f, 0.0f }, 0.5f);
        addCubeEntity("Excluded", { 0.0f, 10.0f, 0.0f }, 1.0f);

        // A zero-intensity light contributes nothing in either world — the
        // builder skips it (the raster path would multiply everything it
        // touches by 0).
        {
            Entity lightEntity = scene->CreateEntity("Dead Light");
            lightEntity.GetComponent<TransformComponent>().Translation = { 0.0f, 3.0f, 0.0f };
            auto& pointLight = lightEntity.AddComponent<PointLightComponent>();
            pointLight.m_Intensity = 0.0f;
        }

        ReferenceSceneBuilder builder;
        builder.AddScene(*scene, [](Entity entity)
                         { return entity.GetName() != "Excluded"; });

        // Pending-state introspection: one shared geometry, two instances, no
        // lights.
        EXPECT_EQ(builder.GetPendingGeometryCount(), 1u)
            << "uniform-scale instances of one MeshSource must share ONE geometry";
        EXPECT_EQ(builder.GetPendingInstanceCount(), 2u) << "the predicate-excluded entity leaked in, or an "
                                                            "included entity was dropped";
        EXPECT_EQ(builder.GetPendingLightCount(), 0u) << "a zero-intensity light was not skipped";

        ReferenceSceneBuildOptions options;
        ReferenceScene built = builder.Build(options);
        EXPECT_TRUE(builder.IsConsumed());

        ASSERT_TRUE(built.IsBuilt());
        EXPECT_EQ(built.GetGeometryCount(), 1u);
        ASSERT_EQ(built.GetInstances().size(), 2u);
        EXPECT_TRUE(built.GetLights().empty());

        // Both instances hit at their own scale: "Big" spans y in [-1, 1] at
        // x = -2 (unit cube * 2), so a ray down from (-2, 5, 0) hits y = 1 at
        // t = 4; "Small" spans y in [-0.25, 0.25] at x = 2, so the same ray at
        // x = 2 hits y = 0.25 at t = 4.75.
        {
            SurfaceInteraction hit;
            ASSERT_TRUE(built.Intersect(Ray(glm::vec3(-2.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), hit));
            EXPECT_NEAR(hit.Distance, 4.0f, 1e-3f);
        }
        {
            SurfaceInteraction hit;
            ASSERT_TRUE(built.Intersect(Ray(glm::vec3(2.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), hit));
            EXPECT_NEAR(hit.Distance, 4.75f, 1e-3f);
        }
        // The excluded entity's spot is empty.
        {
            SurfaceInteraction hit;
            EXPECT_FALSE(built.Intersect(Ray(glm::vec3(0.0f, 15.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), hit))
                << "the predicate-excluded entity is present in the built world";
        }
    }
} // namespace OloEngine::Tests
