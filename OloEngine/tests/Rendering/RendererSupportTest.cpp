// OLO_TEST_LAYER: L1

#include "OloEngine/Renderer/Support/RendererSupport.h"
#include "OloEngine/Renderer/Support/RendererSupportRows.h"

#include <gtest/gtest.h>
#include <string>

namespace OloEngine::RendererSupport
{
    namespace
    {
        constexpr Capabilities kRaster{ .RayQueries = false, .TemporalUpscaler = false, .MaxSamples = 4 };
        constexpr Capabilities kHybrid{ .RayQueries = true, .TemporalUpscaler = true, .MaxSamples = 4 };

        void ExpectDecision(const Request& request, const Capabilities& capabilities,
                            Outcome status, Reason reason)
        {
            const Decision decision = Evaluate(request, capabilities);
            EXPECT_EQ(decision.Status, status);
            EXPECT_EQ(decision.Why, reason);
        }
    } // namespace

    TEST(RendererSupport, RasterPathAndGeometryPairs)
    {
        Request request{};
        request.Api = Backend::OpenGL;
        request.Path = RenderingPath::Forward;
        request.GeometryFamily = Geometry::StaticMesh;
        ExpectDecision(request, kRaster, Outcome::Supported, Reason::None);

        request.GeometryFamily = Geometry::VirtualStatic;
        ExpectDecision(request, kRaster, Outcome::Unsupported, Reason::RequiresDeferred);

        request.Path = RenderingPath::Deferred;
        ExpectDecision(request, kRaster, Outcome::Supported, Reason::None);

        request.SurfaceClosure = Closure::Blended;
        ExpectDecision(request, kRaster, Outcome::Unsupported, Reason::VirtualBlendMissing);

        request.SurfaceClosure = Closure::Masked;
        ExpectDecision(request, kRaster, Outcome::Approximate, Reason::VirtualMaskedShadowMissing);
    }

    TEST(RendererSupport, TechniqueBackendAndCapabilityPairs)
    {
        Request request{};
        request.LightingTechnique = Technique::RayTracedShadow;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::BackendHasNoRayQueries);

        request.Api = Backend::Vulkan;
        ExpectDecision(request, kRaster, Outcome::Unsupported, Reason::RayQueryUnavailable);
        ExpectDecision(request, kHybrid, Outcome::Supported, Reason::None);

        request.SurfaceClosure = Closure::Masked;
        ExpectDecision(request, kRaster, Outcome::Unsupported, Reason::RayQueryUnavailable);
        request.SurfaceClosure = Closure::Opaque;

        request.GeometryFamily = Geometry::VirtualStatic;
        ExpectDecision(request, kHybrid, Outcome::Approximate, Reason::VirtualRayProxy);

        request.GeometryFamily = Geometry::VirtualSkinned;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::VirtualSkinnedRayTracingMissing);
    }

    TEST(RendererSupport, ReconstructionSampleAndCapabilityPairs)
    {
        Request request{};
        request.Samples = 0;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::InvalidSampleCount);

        request.Samples = 4;
        request.Upscale = Reconstruction::Temporal;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::TemporalNeedsSingleSample);

        request.Samples = 1;
        ExpectDecision(request, kRaster, Outcome::Unsupported, Reason::TemporalBackendUnavailable);
        ExpectDecision(request, kHybrid, Outcome::Supported, Reason::None);
    }

    TEST(RendererSupport, MissingFamiliesRefuseExplicitly)
    {
        Request request{};
        request.GeometryFamily = Geometry::GaussianSplat;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::SplatSceneIntegrationMissing);

        request.GeometryFamily = Geometry::VirtualWPO;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::VirtualFeatureMissing);

        request.GeometryFamily = Geometry::Groom;
        request.SurfaceCategory = MaterialFamily::Fibre;
        ExpectDecision(request, kHybrid, Outcome::Unsupported, Reason::GroomSceneShadowMissing);
    }

    TEST(RendererSupport, CoverageRowsMatchRegistry)
    {
        for (const auto& row : GeometryCoverageRows)
        {
            SCOPED_TRACE(std::string(row.Id));
            const Decision actual = Evaluate(row.Requested, row.Device);
            EXPECT_EQ(actual.Status, row.Expected.Status);
            EXPECT_EQ(actual.Why, row.Expected.Why);
        }
    }

    TEST(RendererSupport, MaterialClosureAndGeometryPairs)
    {
        Request request{};
        request.GeometryFamily = Geometry::Groom;
        request.ShadowTechnique = Shadow::None;
        ExpectDecision(request, kRaster, Outcome::Unsupported, Reason::MaterialGeometryMismatch);

        request.SurfaceCategory = MaterialFamily::Fibre;
        ExpectDecision(request, kRaster, Outcome::Supported, Reason::None);

        request.GeometryFamily = Geometry::StaticMesh;
        request.SurfaceCategory = MaterialFamily::Standard;
        request.SurfaceClosure = Closure::Transmission;
        ExpectDecision(request, kRaster, Outcome::Approximate, Reason::TransmissionApproximation);

        request.AuthoredKind = OloEngine::MaterialKind::Foliage;
        ExpectDecision(request, kRaster, Outcome::Supported, Reason::None);
    }

    TEST(RendererSupport, ProductionPresetsRequireTheirRows)
    {
        EXPECT_EQ(EvaluatePreset(Preset::GLForwardNative, kRaster).Status, Outcome::Supported);
        EXPECT_EQ(EvaluatePreset(Preset::GLForwardPlusNative, kRaster).Status, Outcome::Supported);
        EXPECT_EQ(EvaluatePreset(Preset::GLDeferredNative, kRaster).Status, Outcome::Supported);
        EXPECT_EQ(EvaluatePreset(Preset::VulkanForwardNative, kRaster).Status, Outcome::Supported);
        EXPECT_EQ(EvaluatePreset(Preset::VulkanForwardPlusNative, kRaster).Status, Outcome::Supported);
        EXPECT_EQ(EvaluatePreset(Preset::VulkanDeferredNative, kRaster).Status, Outcome::Supported);
        const Decision hybrid = EvaluatePreset(Preset::VulkanHybridNative, kHybrid);
        EXPECT_EQ(hybrid.Status, Outcome::Approximate);
        EXPECT_EQ(hybrid.Why, Reason::VirtualRayProxy);

        Capabilities noRayQueries = kHybrid;
        noRayQueries.RayQueries = false;
        ExpectDecision(Request{ .Api = Backend::Vulkan, .LightingTechnique = Technique::RayTracedShadow },
                       noRayQueries, Outcome::Unsupported, Reason::RayQueryUnavailable);
        const Decision refused = EvaluatePreset(Preset::VulkanHybridNative, noRayQueries);
        EXPECT_EQ(refused.Status, Outcome::Unsupported);
        EXPECT_EQ(refused.Why, Reason::RayQueryUnavailable);
    }
} // namespace OloEngine::RendererSupport
