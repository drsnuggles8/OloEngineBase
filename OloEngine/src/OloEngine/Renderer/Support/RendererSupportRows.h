#pragma once

#include "OloEngine/Renderer/Support/RendererSupport.h"

namespace OloEngine::RendererSupport
{
    // Representative production requests, not a Cartesian product. Keep the
    // evidence and limitations beside each ID in docs/guides/renderer-support-matrix.md.
    struct CoverageRow
    {
        std::string_view Id;
        Request Requested;
        Capabilities Device;
        Decision Expected;
    };

    [[nodiscard]] constexpr Request MakeCoverageRequest(
        Geometry geometry, Closure closure = Closure::Opaque,
        Technique technique = Technique::Raster, Shadow shadow = Shadow::Raster,
        Backend backend = Backend::OpenGL, Motion motion = Motion::Static,
        bool morphTargets = false)
    {
        Request request;
        request.Api = backend;
        request.GeometryFamily = geometry;
        request.SurfaceCategory = geometry == Geometry::Groom ? MaterialFamily::Fibre : geometry == Geometry::Water ? MaterialFamily::Water
                                                                                    : geometry == Geometry::Foliage ? MaterialFamily::Foliage
                                                                                                                    : MaterialFamily::Standard;
        if (geometry == Geometry::Foliage)
            request.AuthoredKind = OloEngine::MaterialKind::Foliage;
        request.SurfaceClosure = closure;
        request.LightingTechnique = technique;
        request.ShadowTechnique = shadow;
        request.MotionKind = motion;
        request.MorphTargets = morphTargets;
        return request;
    }

    inline constexpr Capabilities RasterDevice{ false, false, 4 };
    inline constexpr Capabilities RayQueryDevice{ true, false, 4 };
    inline constexpr Capabilities DeformedRayDevice{ true, false, 4, true };
    inline constexpr Capabilities TemporalDevice{ false, true, 4 };

    inline constexpr std::array<CoverageRow, 17> GeometryCoverageRows = { { { "virtual-static-opaque", MakeCoverageRequest(Geometry::VirtualStatic), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "virtual-skinned-deformed", MakeCoverageRequest(Geometry::VirtualSkinned, Closure::Opaque, Technique::Raster, Shadow::Raster, Backend::OpenGL, Motion::Deformed), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "virtual-masked-raster-shadow", MakeCoverageRequest(Geometry::VirtualStatic, Closure::Masked), RasterDevice, { Outcome::Approximate, Reason::VirtualMaskedShadowMissing } },
                                                                            { "virtual-blended", MakeCoverageRequest(Geometry::VirtualStatic, Closure::Blended), RasterDevice, { Outcome::Unsupported, Reason::VirtualBlendMissing } },
                                                                            { "virtual-morph", MakeCoverageRequest(Geometry::VirtualSkinned, Closure::Opaque, Technique::Raster, Shadow::Raster, Backend::OpenGL, Motion::Deformed, true), RasterDevice, { Outcome::Unsupported, Reason::VirtualMorphMissing } },
                                                                            { "virtual-static-ray-proxy", MakeCoverageRequest(Geometry::VirtualStatic, Closure::Opaque, Technique::RayTracedShadow, Shadow::RayTraced, Backend::Vulkan), RayQueryDevice, { Outcome::Approximate, Reason::VirtualRayProxy } },
                                                                            { "virtual-skinned-ray-refusal", MakeCoverageRequest(Geometry::VirtualSkinned, Closure::Opaque, Technique::RayTracedShadow, Shadow::RayTraced, Backend::Vulkan, Motion::Deformed), RayQueryDevice, { Outcome::Unsupported, Reason::VirtualSkinnedRayTracingMissing } },
                                                                            { "virtual-wpo", MakeCoverageRequest(Geometry::VirtualWPO), RasterDevice, { Outcome::Unsupported, Reason::VirtualFeatureMissing } },
                                                                            { "virtual-displacement", MakeCoverageRequest(Geometry::VirtualDisplacement), RasterDevice, { Outcome::Unsupported, Reason::VirtualFeatureMissing } },
                                                                            { "virtual-spline-landscape", MakeCoverageRequest(Geometry::VirtualSplineLandscape), RasterDevice, { Outcome::Unsupported, Reason::VirtualFeatureMissing } },
                                                                            { "virtual-programmable", MakeCoverageRequest(Geometry::VirtualProgrammable), RasterDevice, { Outcome::Unsupported, Reason::VirtualFeatureMissing } },
                                                                            { "gaussian-splat-scene", MakeCoverageRequest(Geometry::GaussianSplat), RasterDevice, { Outcome::Unsupported, Reason::SplatSceneIntegrationMissing } },
                                                                            { "groom-raster-unshadowed", MakeCoverageRequest(Geometry::Groom, Closure::Opaque, Technique::Raster, Shadow::None), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "groom-raster-scene-shadow", MakeCoverageRequest(Geometry::Groom), RasterDevice, { Outcome::Unsupported, Reason::GroomSceneShadowMissing } },
                                                                            { "groom-ray-proxy", MakeCoverageRequest(Geometry::Groom, Closure::Opaque, Technique::RayTracedShadow, Shadow::RayTraced, Backend::Vulkan), RayQueryDevice, { Outcome::Approximate, Reason::GroomRayProxy } },
                                                                            { "skinned-ray-stream-missing", MakeCoverageRequest(Geometry::SkinnedMesh, Closure::Opaque, Technique::RayTracedShadow, Shadow::RayTraced, Backend::Vulkan, Motion::Deformed), RayQueryDevice, { Outcome::Unsupported, Reason::DeformedRayStreamMissing } },
                                                                            { "skinned-ray-stream-ready", MakeCoverageRequest(Geometry::SkinnedMesh, Closure::Opaque, Technique::RayTracedShadow, Shadow::RayTraced, Backend::Vulkan, Motion::Deformed), DeformedRayDevice, { Outcome::Supported, Reason::None } } } };

    [[nodiscard]] constexpr Request MakeMaterialCoverageRequest(
        Geometry geometry, MaterialFamily category, OloEngine::MaterialKind kind,
        PBRModel model, Closure closure, bool thinTransmission = false,
        RenderingPath path = RenderingPath::Deferred)
    {
        Request request = MakeCoverageRequest(geometry, closure);
        request.SurfaceCategory = category;
        request.AuthoredKind = kind;
        request.ClosureModel = model;
        request.ThinTransmission = thinTransmission;
        request.Path = path;
        return request;
    }

    inline constexpr std::array<CoverageRow, 11> MaterialCoverageRows = { { { "generic-legacy-opaque", MakeMaterialCoverageRequest(Geometry::StaticMesh, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::Legacy, Closure::Opaque), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "generic-v2-opaque", MakeMaterialCoverageRequest(Geometry::StaticMesh, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::ClosureV2, Closure::Opaque), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "skin-v2-opaque", MakeMaterialCoverageRequest(Geometry::SkinnedMesh, MaterialFamily::Skin, OloEngine::MaterialKind::Skin, PBRModel::ClosureV2, Closure::Opaque), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "foliage-thin-transmission", MakeMaterialCoverageRequest(Geometry::Foliage, MaterialFamily::Foliage, OloEngine::MaterialKind::Foliage, PBRModel::ClosureV2, Closure::Opaque, true), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "water-matched-material", MakeMaterialCoverageRequest(Geometry::Water, MaterialFamily::Water, OloEngine::MaterialKind::Generic, PBRModel::Legacy, Closure::Opaque), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "water-wrong-material", MakeMaterialCoverageRequest(Geometry::Water, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::Legacy, Closure::Opaque), RasterDevice, { Outcome::Unsupported, Reason::MaterialGeometryMismatch } },
                                                                            { "groom-wrong-material", MakeMaterialCoverageRequest(Geometry::Groom, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::Legacy, Closure::Opaque), RasterDevice, { Outcome::Unsupported, Reason::MaterialGeometryMismatch } },
                                                                            { "classic-masked-raster-shadow", MakeMaterialCoverageRequest(Geometry::StaticMesh, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::Legacy, Closure::Masked), RasterDevice, { Outcome::Approximate, Reason::MaskedRasterShadowMissing } },
                                                                            { "classic-blend-deferred", MakeMaterialCoverageRequest(Geometry::StaticMesh, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::Legacy, Closure::Blended), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                            { "classic-refractive-forward", MakeMaterialCoverageRequest(Geometry::StaticMesh, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::ClosureV2, Closure::Transmission, false, RenderingPath::Forward), RasterDevice, { Outcome::Approximate, Reason::TransmissionApproximation } },
                                                                            { "classic-refractive-deferred", MakeMaterialCoverageRequest(Geometry::StaticMesh, MaterialFamily::Standard, OloEngine::MaterialKind::Generic, PBRModel::ClosureV2, Closure::Transmission), RasterDevice, { Outcome::Approximate, Reason::TransmissionApproximation } } } };

    [[nodiscard]] constexpr Request MakePathCoverageRequest(
        Backend backend, RenderingPath path, u32 samples = 1,
        Reconstruction upscale = Reconstruction::Native,
        Technique technique = Technique::Raster)
    {
        Request request;
        request.Api = backend;
        request.Path = path;
        request.Samples = samples;
        request.Upscale = upscale;
        request.LightingTechnique = technique;
        request.ShadowTechnique = technique == Technique::Raster ? Shadow::Raster : Shadow::RayTraced;
        return request;
    }

    inline constexpr std::array<CoverageRow, 14> PathCoverageRows = { { { "gl-forward-native", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Forward), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "gl-forward-plus-native", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::ForwardPlus), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "gl-deferred-native", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Deferred), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "vk-forward-native", MakePathCoverageRequest(Backend::Vulkan, RenderingPath::Forward), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "vk-forward-plus-native", MakePathCoverageRequest(Backend::Vulkan, RenderingPath::ForwardPlus), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "vk-deferred-native", MakePathCoverageRequest(Backend::Vulkan, RenderingPath::Deferred), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "gl-deferred-msaa4", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Deferred, 4), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "gl-deferred-spatial-msaa4", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Deferred, 4, Reconstruction::Spatial), RasterDevice, { Outcome::Supported, Reason::None } },
                                                                        { "gl-deferred-temporal", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Deferred, 1, Reconstruction::Temporal), TemporalDevice, { Outcome::Supported, Reason::None } },
                                                                        { "gl-deferred-temporal-msaa4", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Deferred, 4, Reconstruction::Temporal), TemporalDevice, { Outcome::Unsupported, Reason::TemporalNeedsSingleSample } },
                                                                        { "vk-deferred-temporal", MakePathCoverageRequest(Backend::Vulkan, RenderingPath::Deferred, 1, Reconstruction::Temporal), TemporalDevice, { Outcome::Unsupported, Reason::TemporalBackendUnavailable } },
                                                                        { "gl-deferred-ray-query", MakePathCoverageRequest(Backend::OpenGL, RenderingPath::Deferred, 1, Reconstruction::Native, Technique::RayTracedShadow), RasterDevice, { Outcome::Unsupported, Reason::BackendHasNoRayQueries } },
                                                                        { "vk-forward-ray-query", MakePathCoverageRequest(Backend::Vulkan, RenderingPath::Forward, 1, Reconstruction::Native, Technique::RayTracedShadow), RayQueryDevice, { Outcome::Unsupported, Reason::RequiresDeferred } },
                                                                        { "vk-deferred-ray-query-unavailable", MakePathCoverageRequest(Backend::Vulkan, RenderingPath::Deferred, 1, Reconstruction::Native, Technique::RayTracedShadow), RasterDevice, { Outcome::Unsupported, Reason::RayQueryUnavailable } } } };

    [[nodiscard]] constexpr bool GeometryCoverageMatchesRegistry()
    {
        const auto matches = [](const auto& rows)
        {
            for (const auto& row : rows)
            {
                const Decision actual = Evaluate(row.Requested, row.Device);
                if (actual.Status != row.Expected.Status || actual.Why != row.Expected.Why)
                    return false;
            }
            return true;
        };
        return matches(GeometryCoverageRows) && matches(MaterialCoverageRows) && matches(PathCoverageRows);
    }

    static_assert(GeometryCoverageMatchesRegistry(), "Geometry coverage rows disagree with the support registry");
} // namespace OloEngine::RendererSupport
