// OLO_TEST_LAYER: plumbing
// =============================================================================
// GpuPathTracerContractTest.cpp — the headless half of the GPU reference path
// tracer (issue #1055). Everything here runs without a device, which is the
// state every CI runner is in, so it pins the parts of the contract that must
// hold BEFORE a ray is traced:
//
//   * the emissive-triangle table the GPU samples area lights from is the CPU
//     reference's emitter set, triangle for triangle (position, normal, area,
//     order, CDF) — the light density the MIS weights use is a property of
//     that set, and a table that differed would bias every NEE sample;
//   * the accumulation restarts for exactly the settings that change the
//     integral or the sample sequence, and for nothing display-only;
//   * a scene mutation restarts only the path tracer's histories through the
//     registry, leaving the reprojecting effects alone;
//   * every fallback reason and debug view has a name, so a stood-down tracer
//     can always say why;
//   * the 32-byte vertex layout the ray-query helpers hard-code
//     (OLO_RT_VERTEX_STRIDE, the normal at 12, the texcoord at 24) is
//     OloEngine::Vertex's — the "RayTracingAlphaParityTest"
//     include/RayTracingAlphaTest.glsl cites.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "ReferenceSceneFixtures.h"

#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"
#include "OloEngine/Renderer/PathTracing/GpuPathTracerTypes.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/Passes/GpuPathTracerPass.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <filesystem>
#include <fstream>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;
    using namespace OloEngine::Tests::PathTracingFixtures;

    namespace
    {
        // Build the GPU-side table from the reference scene's OWN geometry and
        // instances, the way the device parity test does, so what is compared
        // here is the table-building code on the exact triangles the CPU
        // reference sampled.
        struct BuiltTable
        {
            std::vector<EmissiveTriangleRecord> Records;
            f32 TotalArea = 0.0f;
        };

        [[nodiscard]] BuiltTable BuildTableFromScene(const ReferenceScene& scene, const glm::vec3& renderOrigin)
        {
            BuiltTable built;
            for (const ReferenceInstance& instance : scene.GetInstances())
            {
                const ReferenceMaterial& material = scene.GetMaterial(instance.MaterialIndex);
                if (!(std::max({ material.Emissive.x, material.Emissive.y, material.Emissive.z }) > 0.0f))
                    continue;
                const ReferenceGeometry& geometry = scene.GetGeometry(instance.GeometryIndex);
                built.TotalArea = EmissiveTriangleTable::AppendTriangles(
                    std::span<const Vertex>(geometry.GetVertices()), std::span<const u32>(geometry.GetIndices()), 0u,
                    static_cast<u32>(geometry.GetIndices().size()), 0, instance.Transform, renderOrigin,
                    material.Emissive, material.TwoSidedEmission, built.TotalArea, built.Records);
            }
            EmissiveTriangleTable::Finalize(built.Records, built.TotalArea);
            return built;
        }

        [[nodiscard]] bool Near(const glm::vec3& a, const glm::vec3& b, f32 tolerance)
        {
            return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
                   std::abs(a.z - b.z) <= tolerance;
        }

        // `relative` is a path under OloEditor/assets/shaders ("include/X.glsl",
        // "GpuPathTracer.glsl"), searched upward from the working directory.
        [[nodiscard]] std::filesystem::path ResolveShaderPath(const char* relative)
        {
            namespace fs = std::filesystem;
            const fs::path candidates[] = {
                fs::path("OloEditor") / "assets" / "shaders" / relative,
                fs::path("assets") / "shaders" / relative,
                fs::path("..") / "OloEditor" / "assets" / "shaders" / relative,
            };
            fs::path current = fs::current_path();
            for (int depth = 0; depth < 6; ++depth)
            {
                for (const auto& candidate : candidates)
                {
                    if (fs::exists(current / candidate))
                        return current / candidate;
                }
                if (!current.has_parent_path() || current.parent_path() == current)
                    break;
                current = current.parent_path();
            }
            return {};
        }

        [[nodiscard]] std::filesystem::path ResolveShaderInclude(const char* fileName)
        {
            return ResolveShaderPath((std::string("include/") + fileName).c_str());
        }

        [[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        [[nodiscard]] u32 ParseDefine(const std::string& source, const char* name)
        {
            const std::regex pattern(std::string("#define\\s+") + name + "\\s+(\\d+)u?");
            std::smatch match;
            if (!std::regex_search(source, match, pattern))
                return 0u;
            return static_cast<u32>(std::stoul(match[1].str()));
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The emissive table is the reference's emitter set
    // -------------------------------------------------------------------------

    TEST(GpuPathTracerContract, EmissiveTableIsTheReferenceEmitterSetTriangleForTriangle)
    {
        const CornellBoxScene fixture = MakeCornellBoxScene();
        const ReferenceScene& scene = fixture.Scene;
        const auto& reference = scene.GetEmissiveTriangles();
        ASSERT_FALSE(reference.empty()) << "the Cornell fixture has an emitter; a fixture change broke this test's premise";

        const BuiltTable built = BuildTableFromScene(scene, glm::vec3(0.0f));
        ASSERT_EQ(built.Records.size(), reference.size());

        constexpr f32 kTolerance = 1e-5f;
        for (sizet i = 0; i < reference.size(); ++i)
        {
            const EmissiveTriangle& expected = reference[i];
            const EmissiveTriangleRecord& actual = built.Records[i];
            EXPECT_TRUE(Near(glm::vec3(actual.V0), expected.V0, kTolerance)) << "triangle " << i << " V0";
            EXPECT_TRUE(Near(glm::vec3(actual.V1), expected.V1, kTolerance)) << "triangle " << i << " V1";
            EXPECT_TRUE(Near(glm::vec3(actual.V2), expected.V2, kTolerance)) << "triangle " << i << " V2";
            EXPECT_TRUE(Near(glm::vec3(actual.NormalAndCdf), expected.Normal, kTolerance)) << "triangle " << i << " normal";
            EXPECT_NEAR(actual.V0.w, expected.Area, kTolerance) << "triangle " << i << " area";
            EXPECT_TRUE(Near(glm::vec3(actual.RadianceAndFlags), scene.GetMaterial(expected.MaterialIndex).Emissive, kTolerance))
                << "triangle " << i << " radiance";
        }

        // The density the MIS weights share: one constant over the whole set.
        EXPECT_NEAR(built.TotalArea, scene.GetTotalEmissiveArea(), 1e-5f);
        EXPECT_NEAR(1.0f / built.TotalArea, scene.EmissivePdfArea(), 1e-5f);

        // The CDF is monotone, area-proportional, and ends at EXACTLY one — a
        // selection value of 1.0 must not walk off the end.
        f32 running = 0.0f;
        for (sizet i = 0; i < built.Records.size(); ++i)
        {
            running += built.Records[i].V0.w;
            EXPECT_NEAR(built.Records[i].NormalAndCdf.w, running / built.TotalArea, 1e-5f) << "cdf " << i;
            if (i > 0)
                EXPECT_GE(built.Records[i].NormalAndCdf.w, built.Records[i - 1].NormalAndCdf.w);
        }
        EXPECT_FLOAT_EQ(built.Records.back().NormalAndCdf.w, 1.0f);
    }

    TEST(GpuPathTracerContract, EmissiveTableIsRenderRelativeLikeTheAccelerationStructure)
    {
        const CornellBoxScene fixture = MakeCornellBoxScene();
        const glm::vec3 origin(10.0f, -3.0f, 7.5f);
        const BuiltTable atOrigin = BuildTableFromScene(fixture.Scene, glm::vec3(0.0f));
        const BuiltTable rebased = BuildTableFromScene(fixture.Scene, origin);
        ASSERT_EQ(atOrigin.Records.size(), rebased.Records.size());
        for (sizet i = 0; i < atOrigin.Records.size(); ++i)
        {
            EXPECT_TRUE(Near(glm::vec3(rebased.Records[i].V0), glm::vec3(atOrigin.Records[i].V0) - origin, 1e-5f));
            EXPECT_TRUE(Near(glm::vec3(rebased.Records[i].NormalAndCdf), glm::vec3(atOrigin.Records[i].NormalAndCdf), 1e-6f))
                << "a translation must not move a normal";
            EXPECT_NEAR(rebased.Records[i].V0.w, atOrigin.Records[i].V0.w, 1e-6f) << "a translation must not change an area";
        }
    }

    TEST(GpuPathTracerContract, EmissiveTableSkipsDegenerateAndOutOfRangeTriangles)
    {
        const std::vector<Vertex> vertices = {
            Vertex(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f)),
            Vertex(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f)),
            Vertex(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f)),
            Vertex(glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f)),
        };
        // Triangle 0 is real (area 0.5); triangle 1 is collinear (degenerate);
        // triangle 2 names a vertex that does not exist.
        const std::vector<u32> indices = { 0u, 1u, 2u, 0u, 1u, 3u, 0u, 1u, 9u };

        std::vector<EmissiveTriangleRecord> records;
        const f32 area = EmissiveTriangleTable::AppendTriangles(
            vertices, indices, 0u, static_cast<u32>(indices.size()), 0, glm::mat4(1.0f), glm::vec3(0.0f),
            glm::vec3(1.0f), false, 0.0f, records);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_NEAR(area, 0.5f, 1e-6f);
        EXPECT_TRUE(Near(glm::vec3(records[0].NormalAndCdf), glm::vec3(0.0f, 0.0f, 1.0f), 1e-6f));

        // A range past the index buffer, or a non-multiple of three, appends nothing.
        std::vector<EmissiveTriangleRecord> none;
        EXPECT_EQ(EmissiveTriangleTable::AppendTriangles(vertices, indices, 6u, 6u, 0, glm::mat4(1.0f), glm::vec3(0.0f),
                                                         glm::vec3(1.0f), false, 0.0f, none),
                  0.0f);
        EXPECT_TRUE(none.empty());

        // Finalize on an empty table is a no-op, not a crash.
        EmissiveTriangleTable::Finalize(none, 0.0f);
        EXPECT_TRUE(none.empty());
    }

    // -------------------------------------------------------------------------
    // Accumulation restarts
    // -------------------------------------------------------------------------

    TEST(GpuPathTracerContract, OnlyIntegralChangingSettingsRestartTheAccumulation)
    {
        const GpuPathTracerSettings base{};
        EXPECT_FALSE(GpuPathTracerPass::SettingsChangeInvalidatesAccumulation(base, base));

        // Display-only and budget-only knobs: the SAME sequence is drawn, just
        // more or less of it per frame, or shown differently.
        {
            GpuPathTracerSettings next = base;
            next.SamplesPerFrame = 8;
            next.MaxSamples = 1024;
            next.DebugView = GpuPathTracerDebugView::Variance;
            next.SampleCountDisplayScale = 0.5f;
            next.VarianceDisplayScale = 3.0f;
            EXPECT_FALSE(GpuPathTracerPass::SettingsChangeInvalidatesAccumulation(base, next));
        }

        const auto expectRestart = [&](auto&& mutate, const char* what)
        {
            GpuPathTracerSettings next = base;
            mutate(next);
            EXPECT_TRUE(GpuPathTracerPass::SettingsChangeInvalidatesAccumulation(base, next)) << what;
        };
        expectRestart([](auto& s)
                      { s.Enabled = !s.Enabled; }, "Enabled");
        expectRestart([](auto& s)
                      { s.MaxBounces += 1; }, "MaxBounces");
        expectRestart([](auto& s)
                      { s.RussianRouletteStartBounce = 0; }, "RussianRouletteStartBounce");
        expectRestart([](auto& s)
                      { s.Seed += 1; }, "Seed");
        expectRestart([](auto& s)
                      { s.EnableNextEventEstimation = false; }, "EnableNextEventEstimation");
        expectRestart([](auto& s)
                      { s.MaxRadianceClamp = 10.0f; }, "MaxRadianceClamp");
        expectRestart([](auto& s)
                      { s.RayEpsilon *= 2.0f; }, "RayEpsilon");
        expectRestart([](auto& s)
                      { s.MaxRayDistance *= 0.5f; }, "MaxRayDistance");
        expectRestart([](auto& s)
                      { s.UniformEnvironmentRadiance = glm::vec3(1.0f); }, "UniformEnvironmentRadiance");
        expectRestart([](auto& s)
                      { s.EnvironmentCubeIntensity = 0.0f; }, "EnvironmentCubeIntensity");
    }

    TEST(GpuPathTracerContract, CameraMoveRequestsARestartOncePerChange)
    {
        GpuPathTracerPass pass;
        const glm::mat4 view(1.0f);
        const glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);

        pass.SetCameraMatrices(view, projection, glm::vec3(0.0f));
        EXPECT_FALSE(pass.ConsumeAccumulationRestartRequest().has_value()) << "the first pose is not a change";

        pass.SetCameraMatrices(view, projection, glm::vec3(0.0f));
        EXPECT_FALSE(pass.ConsumeAccumulationRestartRequest().has_value()) << "a held camera keeps converging";

        glm::mat4 moved = view;
        moved[3][0] += 1e-6f; // any change, however small — a path tracer cannot reproject
        pass.SetCameraMatrices(moved, projection, glm::vec3(0.0f));
        const auto restart = pass.ConsumeAccumulationRestartRequest();
        ASSERT_TRUE(restart.has_value());
        EXPECT_EQ(*restart, TemporalHistoryInvalidationCause::CameraCut);
        EXPECT_FALSE(pass.ConsumeAccumulationRestartRequest().has_value()) << "consumed once";

        // A render-origin rebase is a camera change too: the TLAS moves.
        pass.SetCameraMatrices(moved, projection, glm::vec3(100.0f, 0.0f, 0.0f));
        EXPECT_TRUE(pass.ConsumeAccumulationRestartRequest().has_value());

        GpuPathTracerSettings settings{};
        pass.SetSettings(settings);
        settings.Seed = 12345u;
        pass.SetSettings(settings);
        const auto toggled = pass.ConsumeAccumulationRestartRequest();
        ASSERT_TRUE(toggled.has_value());
        EXPECT_EQ(*toggled, TemporalHistoryInvalidationCause::FeatureToggled);
    }

    TEST(GpuPathTracerContract, SceneMutationRestartsOnlyThePathTracersHistories)
    {
        TemporalHistoryRegistry registry;
        TemporalHistoryDescriptor descriptor;
        descriptor.Width = 64;
        descriptor.Height = 64;
        descriptor.Format = ImageFormat::RGBA32F;

        // What a reprojecting history declares (TAA's mask) and what the
        // path tracer's planes declare on top of it: the scene's content.
        constexpr TemporalHistoryDependency reprojecting =
            TemporalHistoryDependency::ViewTransform | TemporalHistoryDependency::Projection |
            TemporalHistoryDependency::Viewport | TemporalHistoryDependency::RenderScale |
            TemporalHistoryDependency::Scene | TemporalHistoryDependency::Backend |
            TemporalHistoryDependency::FeatureState | TemporalHistoryDependency::Jitter;
        constexpr TemporalHistoryDependency accumulating = reprojecting | TemporalHistoryDependency::SceneContent;

        const TemporalHistoryKey pathTracerKey{ .Effect = TemporalHistoryEffect::PathTracer,
                                                .View = 0,
                                                .Resolution = TemporalHistoryResolution::Scene,
                                                .Plane = TemporalHistoryPlane::Signal };
        const TemporalHistoryKey albedoKey{ .Effect = TemporalHistoryEffect::PathTracer,
                                            .View = 0,
                                            .Resolution = TemporalHistoryResolution::Scene,
                                            .Plane = TemporalHistoryPlane::Albedo };
        const TemporalHistoryKey taaKey{ .Effect = TemporalHistoryEffect::TAA,
                                         .View = 0,
                                         .Resolution = TemporalHistoryResolution::Scene,
                                         .Plane = TemporalHistoryPlane::Signal };

        const auto accum = registry.Acquire(pathTracerKey, descriptor, accumulating);
        const auto albedo = registry.Acquire(albedoKey, descriptor, accumulating);
        const auto taa = registry.Acquire(taaKey, descriptor, reprojecting);

        // SceneMutated maps to the SceneContent dependency, which only the
        // accumulating history declares: the invalidation is UNSCOPED and the
        // reprojecting history still survives it by its own declaration, not
        // by an effect filter at the call site.
        EXPECT_EQ(TemporalHistoryRegistry::DependencyForCause(TemporalHistoryInvalidationCause::SceneMutated),
                  TemporalHistoryDependency::SceneContent);
        EXPECT_EQ(registry.Invalidate(TemporalHistoryInvalidationCause::SceneMutated), 2u);
        EXPECT_FALSE(registry.IsCurrent(accum.Token));
        EXPECT_FALSE(registry.IsCurrent(albedo.Token));
        EXPECT_TRUE(registry.IsCurrent(taa.Token));

        // And a camera cut scoped to the path tracer — the pass's own restart —
        // likewise leaves TAA alone.
        const auto accum2 = registry.Acquire(pathTracerKey, descriptor, accumulating);
        EXPECT_EQ(registry.Invalidate(TemporalHistoryInvalidationCause::CameraCut, TemporalHistoryEffect::PathTracer), 2u);
        EXPECT_FALSE(registry.IsCurrent(accum2.Token));
        EXPECT_TRUE(registry.IsCurrent(taa.Token));
    }

    // -------------------------------------------------------------------------
    // Names
    // -------------------------------------------------------------------------

    TEST(GpuPathTracerContract, EveryFallbackReasonAndDebugViewHasAName)
    {
        for (u32 reason = 0; reason < static_cast<u32>(GpuPathTracerFallbackReason::Count); ++reason)
        {
            const auto text = ToString(static_cast<GpuPathTracerFallbackReason>(reason));
            EXPECT_FALSE(text.empty());
            EXPECT_NE(text, "unknown") << "fallback reason " << reason;
        }
        for (u32 view = 0; view < static_cast<u32>(GpuPathTracerDebugView::Count); ++view)
        {
            const auto text = ToString(static_cast<GpuPathTracerDebugView>(view));
            EXPECT_FALSE(text.empty());
            EXPECT_NE(text, "unknown") << "debug view " << view;
        }
    }

    // -------------------------------------------------------------------------
    // The vertex layout the ray-query helpers hard-code — the
    // RayTracingAlphaParityTest that include/RayTracingAlphaTest.glsl cites.
    // -------------------------------------------------------------------------

    TEST(RayTracingAlphaParity, VertexStrideAndOffsetsMatchOloEngineVertex)
    {
        static_assert(sizeof(Vertex) == 32, "the ray-query helpers index a 32-byte vertex stream");
        static_assert(offsetof(Vertex, Position) == 0);
        static_assert(offsetof(Vertex, Normal) == 12, "GpuPathTracer.glsl / RayTracedReflection.glsl read the normal at byte 12");
        static_assert(offsetof(Vertex, TexCoord) == 24, "RayTracingAlphaTest.glsl reads the texcoord at byte 24");

        const auto path = ResolveShaderInclude("RayTracingAlphaTest.glsl");
        if (path.empty())
            GTEST_SKIP() << "Could not locate OloEditor/assets/shaders/include from the working directory.";

        const std::string source = ReadTextFile(path);
        ASSERT_FALSE(source.empty()) << path.string();

        EXPECT_EQ(ParseDefine(source, "OLO_RT_VERTEX_STRIDE"), static_cast<u32>(sizeof(Vertex)));
        EXPECT_EQ(ParseDefine(source, "OLO_RT_VERTEX_NORMAL_OFFSET"), static_cast<u32>(offsetof(Vertex, Normal)));
        EXPECT_EQ(ParseDefine(source, "OLO_RT_VERTEX_TEXCOORD_OFFSET"), static_cast<u32>(offsetof(Vertex, TexCoord)));
    }

    // -------------------------------------------------------------------------
    // The C++ / GLSL mirrors of the shader's loop bounds, flag bits and view
    // numbers. A renumbered flag or a raised bound on one side compiles
    // cleanly and every other test still passes; this is the only thing that
    // would notice.
    // -------------------------------------------------------------------------

    TEST(GpuPathTracerContract, ShaderMacrosMirrorTheSharedConstants)
    {
        const auto path = ResolveShaderPath("GpuPathTracer.glsl");
        if (path.empty())
            GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";
        const std::string source = ReadTextFile(path);
        ASSERT_FALSE(source.empty()) << path.string();

        EXPECT_EQ(ParseDefine(source, "OLO_PT_MAX_BOUNCES"), kGpuPathTracerMaxBounces);
        EXPECT_EQ(ParseDefine(source, "OLO_PT_MAX_SAMPLES_PER_FRAME"), kGpuPathTracerMaxSamplesPerFrame);
        EXPECT_EQ(ParseDefine(source, "OLO_PT_MAX_LIGHTS"), kGpuPathTracerMaxLights);

        EXPECT_EQ(ParseDefine(source, "OLO_PT_FLAG_NEE"), kGpuPathTracerFlagNextEventEstimation);
        EXPECT_EQ(ParseDefine(source, "OLO_PT_FLAG_ENVIRONMENT_CUBE"), kGpuPathTracerFlagEnvironmentCube);
        EXPECT_EQ(ParseDefine(source, "OLO_PT_FLAG_HISTORY_VALID"), kGpuPathTracerFlagHistoryValid);

        EXPECT_EQ(ParseDefine(source, "OLO_PT_VIEW_RADIANCE"), std::to_underlying(GpuPathTracerDebugView::Radiance));
        EXPECT_EQ(ParseDefine(source, "OLO_PT_VIEW_ALBEDO"), std::to_underlying(GpuPathTracerDebugView::Albedo));
        EXPECT_EQ(ParseDefine(source, "OLO_PT_VIEW_NORMAL"), std::to_underlying(GpuPathTracerDebugView::Normal));
        EXPECT_EQ(ParseDefine(source, "OLO_PT_VIEW_VARIANCE"), std::to_underlying(GpuPathTracerDebugView::Variance));
        EXPECT_EQ(ParseDefine(source, "OLO_PT_VIEW_SAMPLE_COUNT"),
                  std::to_underlying(GpuPathTracerDebugView::SampleCount));
    }

    // -------------------------------------------------------------------------
    // The one sanitizer every write path shares.
    // -------------------------------------------------------------------------

    TEST(GpuPathTracerContract, SanitizerHoldsEveryKnobToItsRangeAndReplacesNonFiniteWithTheDefault)
    {
        using namespace GpuPathTracerLimits;
        const GpuPathTracerSettings defaults{};

        GpuPathTracerSettings in{};
        in.SamplesPerFrame = 0;
        in.MaxSamples = kMaxSamplesCap + 1000u;
        in.MaxBounces = kGpuPathTracerMaxBounces + 1u;
        in.RussianRouletteStartBounce = kGpuPathTracerMaxBounces + 9u;
        in.MaxRadianceClamp = -5.0f;
        in.RayEpsilon = std::numeric_limits<f32>::quiet_NaN();
        in.MaxRayDistance = std::numeric_limits<f32>::infinity();
        in.UniformEnvironmentRadiance = glm::vec3(-1.0f, std::numeric_limits<f32>::quiet_NaN(), 1.0e9f);
        in.EnvironmentCubeIntensity = 1.0e6f;
        in.DebugView = static_cast<GpuPathTracerDebugView>(999u);
        in.SampleCountDisplayScale = 7.0f;
        in.VarianceDisplayScale = -std::numeric_limits<f32>::infinity();

        const GpuPathTracerSettings out = SanitizeGpuPathTracerSettings(in);
        EXPECT_EQ(out.SamplesPerFrame, 1u);
        EXPECT_EQ(out.MaxSamples, kMaxSamplesCap);
        EXPECT_EQ(out.MaxBounces, kGpuPathTracerMaxBounces);
        EXPECT_EQ(out.RussianRouletteStartBounce, kGpuPathTracerMaxBounces);
        EXPECT_FLOAT_EQ(out.MaxRadianceClamp, 0.0f);
        EXPECT_FLOAT_EQ(out.RayEpsilon, defaults.RayEpsilon);
        EXPECT_FLOAT_EQ(out.MaxRayDistance, defaults.MaxRayDistance);
        EXPECT_FLOAT_EQ(out.UniformEnvironmentRadiance.x, 0.0f);
        EXPECT_FLOAT_EQ(out.UniformEnvironmentRadiance.y, defaults.UniformEnvironmentRadiance.y);
        EXPECT_FLOAT_EQ(out.UniformEnvironmentRadiance.z, kMaxUniformEnvironmentRadiance);
        EXPECT_FLOAT_EQ(out.EnvironmentCubeIntensity, kMaxEnvironmentCubeIntensity);
        EXPECT_EQ(out.DebugView, GpuPathTracerDebugView::Radiance);
        EXPECT_FLOAT_EQ(out.SampleCountDisplayScale, kMaxSampleCountDisplayScale);
        EXPECT_FLOAT_EQ(out.VarianceDisplayScale, defaults.VarianceDisplayScale);

        // A value inside every range comes back untouched — the sanitizer is
        // not allowed to move a legal setting.
        const GpuPathTracerSettings legal = defaults;
        EXPECT_TRUE(SanitizeGpuPathTracerSettings(legal) == legal);
    }
} // namespace OloEngine::Tests
