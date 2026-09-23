#pragma once

#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/PBRModel.h"

#include <array>
#include <string_view>

namespace OloEngine::RendererSupport
{
    enum class Backend : u8
    {
        OpenGL,
        Vulkan
    };
    enum class Geometry : u8
    {
        StaticMesh,
        SkinnedMesh,
        Groom,
        Foliage,
        Terrain,
        Water,
        VirtualStatic,
        VirtualSkinned,
        VirtualWPO,
        VirtualDisplacement,
        VirtualSplineLandscape,
        VirtualProgrammable,
        GaussianSplat
    };
    enum class MaterialFamily : u8
    {
        Standard,
        Skin,
        Foliage,
        Fibre,
        Water,
        Unlit
    };
    enum class Closure : u8
    {
        Opaque,
        Masked,
        Blended,
        Transmission
    };
    enum class Technique : u8
    {
        Raster,
        RayTracedShadow,
        RayTracedDirect,
        RayTracedGI,
        PathTracing
    };
    enum class Motion : u8
    {
        Static,
        Deformed
    };
    enum class Shadow : u8
    {
        None,
        Raster,
        RayTraced
    };
    enum class Reconstruction : u8
    {
        Native,
        Spatial,
        Temporal
    };
    enum class Outcome : u8
    {
        Supported,
        Approximate,
        Unsupported
    };
    enum class Reason : u8
    {
        None,
        BackendHasNoRayQueries,
        RayQueryUnavailable,
        RequiresDeferred,
        TemporalNeedsSingleSample,
        TemporalBackendUnavailable,
        VirtualFeatureMissing,
        VirtualBlendMissing,
        VirtualMaskedShadowMissing,
        VirtualMorphMissing,
        VirtualSkinnedRayTracingMissing,
        DeformedRayStreamMissing,
        VirtualRayProxy,
        GroomRayProxy,
        GroomSceneShadowMissing,
        SplatSceneIntegrationMissing,
        InvalidSampleCount,
        MaterialGeometryMismatch,
        TransmissionApproximation,
        MaskedRasterShadowMissing
    };

    [[nodiscard]] constexpr std::string_view ToString(Reason reason)
    {
        switch (reason)
        {
            case Reason::None:
                return "None";
            case Reason::BackendHasNoRayQueries:
                return "BackendHasNoRayQueries";
            case Reason::RayQueryUnavailable:
                return "RayQueryUnavailable";
            case Reason::RequiresDeferred:
                return "RequiresDeferred";
            case Reason::TemporalNeedsSingleSample:
                return "TemporalNeedsSingleSample";
            case Reason::TemporalBackendUnavailable:
                return "TemporalBackendUnavailable";
            case Reason::VirtualFeatureMissing:
                return "VirtualFeatureMissing";
            case Reason::VirtualBlendMissing:
                return "VirtualBlendMissing";
            case Reason::VirtualMaskedShadowMissing:
                return "VirtualMaskedShadowMissing";
            case Reason::VirtualMorphMissing:
                return "VirtualMorphMissing";
            case Reason::VirtualSkinnedRayTracingMissing:
                return "VirtualSkinnedRayTracingMissing";
            case Reason::DeformedRayStreamMissing:
                return "DeformedRayStreamMissing";
            case Reason::VirtualRayProxy:
                return "VirtualRayProxy";
            case Reason::GroomRayProxy:
                return "GroomRayProxy";
            case Reason::GroomSceneShadowMissing:
                return "GroomSceneShadowMissing";
            case Reason::SplatSceneIntegrationMissing:
                return "SplatSceneIntegrationMissing";
            case Reason::InvalidSampleCount:
                return "InvalidSampleCount";
            case Reason::MaterialGeometryMismatch:
                return "MaterialGeometryMismatch";
            case Reason::TransmissionApproximation:
                return "TransmissionApproximation";
            case Reason::MaskedRasterShadowMissing:
                return "MaskedRasterShadowMissing";
        }
        return "Unknown";
    }

    struct Request
    {
        Backend Api = Backend::OpenGL;
        RenderingPath Path = RenderingPath::Deferred;
        Geometry GeometryFamily = Geometry::StaticMesh;
        MaterialFamily SurfaceCategory = MaterialFamily::Standard;
        OloEngine::MaterialKind AuthoredKind = OloEngine::MaterialKind::Generic;
        PBRModel ClosureModel = PBRModel::Legacy;
        Closure SurfaceClosure = Closure::Opaque;
        bool ThinTransmission = false;
        Technique LightingTechnique = Technique::Raster;
        Motion MotionKind = Motion::Static;
        bool MorphTargets = false;
        Shadow ShadowTechnique = Shadow::Raster;
        u32 Samples = 1;
        Reconstruction Upscale = Reconstruction::Native;
    };

    struct Capabilities
    {
        bool RayQueries = false;
        bool TemporalUpscaler = false;
        u32 MaxSamples = 1;
        bool DeformedRayStream = false;
    };

    struct Decision
    {
        Outcome Status;
        Reason Why;
    };

    // Static compatibility is deliberately separate from runtime production and
    // consumption. A present GPU Scene record or a black pixel proves neither.
    [[nodiscard]] constexpr Decision Evaluate(const Request& request, const Capabilities& capabilities)
    {
        if (request.Samples == 0 || request.Samples > capabilities.MaxSamples)
            return { Outcome::Unsupported, Reason::InvalidSampleCount };
        if (request.Upscale == Reconstruction::Temporal && request.Samples > 1)
            return { Outcome::Unsupported, Reason::TemporalNeedsSingleSample };
        if (request.Upscale == Reconstruction::Temporal &&
            (request.Api != Backend::OpenGL || !capabilities.TemporalUpscaler))
            return { Outcome::Unsupported, Reason::TemporalBackendUnavailable };
        if (request.GeometryFamily == Geometry::GaussianSplat)
            return { Outcome::Unsupported, Reason::SplatSceneIntegrationMissing };
        if ((request.GeometryFamily == Geometry::Groom) != (request.SurfaceCategory == MaterialFamily::Fibre) ||
            (request.GeometryFamily == Geometry::Water) != (request.SurfaceCategory == MaterialFamily::Water) ||
            (request.GeometryFamily == Geometry::Foliage && request.SurfaceCategory == MaterialFamily::Skin))
            return { Outcome::Unsupported, Reason::MaterialGeometryMismatch };
        if (request.GeometryFamily == Geometry::VirtualWPO ||
            request.GeometryFamily == Geometry::VirtualDisplacement ||
            request.GeometryFamily == Geometry::VirtualSplineLandscape ||
            request.GeometryFamily == Geometry::VirtualProgrammable)
            return { Outcome::Unsupported, Reason::VirtualFeatureMissing };
        const bool virtualMesh = request.GeometryFamily == Geometry::VirtualStatic ||
                                 request.GeometryFamily == Geometry::VirtualSkinned;
        if (virtualMesh)
        {
            if (request.Path != RenderingPath::Deferred)
                return { Outcome::Unsupported, Reason::RequiresDeferred };
            if (request.SurfaceClosure == Closure::Blended)
                return { Outcome::Unsupported, Reason::VirtualBlendMissing };
            if (request.MorphTargets)
                return { Outcome::Unsupported, Reason::VirtualMorphMissing };
            if (request.GeometryFamily == Geometry::VirtualSkinned &&
                (request.LightingTechnique != Technique::Raster || request.ShadowTechnique == Shadow::RayTraced))
                return { Outcome::Unsupported, Reason::VirtualSkinnedRayTracingMissing };
        }
        if (request.LightingTechnique != Technique::Raster || request.ShadowTechnique == Shadow::RayTraced)
        {
            if (request.Path != RenderingPath::Deferred)
                return { Outcome::Unsupported, Reason::RequiresDeferred };
            if (request.Api != Backend::Vulkan)
                return { Outcome::Unsupported, Reason::BackendHasNoRayQueries };
            if (!capabilities.RayQueries)
                return { Outcome::Unsupported, Reason::RayQueryUnavailable };
            if (request.GeometryFamily == Geometry::SkinnedMesh && request.MotionKind == Motion::Deformed &&
                !capabilities.DeformedRayStream)
                return { Outcome::Unsupported, Reason::DeformedRayStreamMissing };
            if (virtualMesh)
                return { Outcome::Approximate, Reason::VirtualRayProxy };
            if (request.GeometryFamily == Geometry::Groom)
                return { Outcome::Approximate, Reason::GroomRayProxy };
        }
        if (request.GeometryFamily == Geometry::Groom && request.ShadowTechnique == Shadow::Raster)
            return { Outcome::Unsupported, Reason::GroomSceneShadowMissing };
        if (request.SurfaceClosure == Closure::Masked && request.ShadowTechnique == Shadow::Raster)
            return { Outcome::Approximate, virtualMesh ? Reason::VirtualMaskedShadowMissing
                                                       : Reason::MaskedRasterShadowMissing };
        if ((request.SurfaceClosure == Closure::Transmission || request.ThinTransmission) &&
            request.AuthoredKind != OloEngine::MaterialKind::Foliage)
            return { Outcome::Approximate, Reason::TransmissionApproximation };
        return { Outcome::Supported, Reason::None };
    }

    enum class Preset : u8
    {
        GLForwardNative,
        GLForwardPlusNative,
        GLDeferredNative,
        VulkanForwardNative,
        VulkanForwardPlusNative,
        VulkanDeferredNative,
        VulkanHybridNative
    };
    struct PresetDefinition
    {
        Preset Id;
        std::string_view Name;
        std::string_view QualityIntent;
        Backend Api;
        RenderingPath Path;
        Technique LightingTechnique;
        std::string_view RequiredRepresentations;
        std::string_view ValidationManifest;
        std::string_view BudgetEvidence;
    };

    inline constexpr std::array<PresetDefinition, 7> Presets = { { { Preset::GLForwardNative, "gl-forward-native", "small light count raster", Backend::OpenGL,
                                                                     RenderingPath::Forward, Technique::Raster, "static,skinned,foliage,water", "integrated-forward.yaml", "#1338" },
                                                                   { Preset::GLForwardPlusNative, "gl-forward-plus-native", "tiled raster lighting", Backend::OpenGL,
                                                                     RenderingPath::ForwardPlus, Technique::Raster, "static,skinned,foliage,water", "integrated-forward-plus.yaml", "#1338" },
                                                                   { Preset::GLDeferredNative, "gl-deferred-native", "mixed raster scene", Backend::OpenGL,
                                                                     RenderingPath::Deferred, Technique::Raster, "static,skinned,foliage,water,virtual-static", "integrated-deferred.yaml", "#1338" },
                                                                   { Preset::VulkanForwardNative, "vk-forward-native", "small light count raster", Backend::Vulkan,
                                                                     RenderingPath::Forward, Technique::Raster, "static,skinned,foliage,water", "integrated-forward.yaml", "#1338" },
                                                                   { Preset::VulkanForwardPlusNative, "vk-forward-plus-native", "tiled raster lighting", Backend::Vulkan,
                                                                     RenderingPath::ForwardPlus, Technique::Raster, "static,skinned,foliage,water", "integrated-forward-plus.yaml", "#1338" },
                                                                   { Preset::VulkanDeferredNative, "vk-deferred-native", "mixed raster scene", Backend::Vulkan,
                                                                     RenderingPath::Deferred, Technique::Raster, "static,skinned,foliage,water,virtual-static", "integrated-deferred.yaml", "#1338" },
                                                                   { Preset::VulkanHybridNative, "vk-hybrid-native", "raster with ray-query lighting", Backend::Vulkan,
                                                                     RenderingPath::Deferred, Technique::RayTracedShadow, "static,skinned,foliage,water,virtual-static-proxy", "integrated-hybrid.yaml", "#1338" } } };

    [[nodiscard]] constexpr const PresetDefinition& Describe(Preset preset)
    {
        return Presets[static_cast<u8>(preset)];
    }

    // This list is the executable half of RequiredRepresentations. Preset
    // admission checks it regardless of which GPU tests happen to be selected.
    inline constexpr std::array<Geometry, 5> RasterRequired = {
        Geometry::StaticMesh, Geometry::SkinnedMesh, Geometry::Foliage,
        Geometry::Water, Geometry::VirtualStatic
    };

    [[nodiscard]] constexpr Decision EvaluatePreset(Preset preset, const Capabilities& capabilities)
    {
        const auto& definition = Describe(preset);
        Decision aggregate{ Outcome::Supported, Reason::None };
        for (const auto geometry : RasterRequired)
        {
            if (geometry == Geometry::VirtualStatic && definition.Path != RenderingPath::Deferred)
                continue;
            Request request;
            request.Api = definition.Api;
            request.Path = definition.Path;
            request.GeometryFamily = geometry;
            if (geometry == Geometry::Water)
                request.SurfaceCategory = MaterialFamily::Water;
            if (geometry == Geometry::Foliage)
            {
                request.SurfaceCategory = MaterialFamily::Foliage;
                request.AuthoredKind = OloEngine::MaterialKind::Foliage;
                request.ThinTransmission = true;
            }
            // Hybrid rays are required for static meshes and their virtual
            // proxy. Animated and specialized surfaces retain raster shadows.
            const bool hybridRay = preset == Preset::VulkanHybridNative &&
                                   (geometry == Geometry::StaticMesh || geometry == Geometry::VirtualStatic);
            request.LightingTechnique = hybridRay ? Technique::RayTracedShadow : Technique::Raster;
            request.ShadowTechnique = hybridRay ? Shadow::RayTraced : Shadow::Raster;
            const auto decision = Evaluate(request, capabilities);
            if (decision.Status == Outcome::Unsupported)
                return decision;
            if (decision.Status == Outcome::Approximate && aggregate.Status == Outcome::Supported)
                aggregate = decision;
        }
        return aggregate;
    }

    enum class Observation : u8
    {
        Unknown,
        Absent,
        Present
    };

    struct Engagement
    {
        bool Requested = false;
        bool Capable = false;
        bool Selected = false;
        // Unknown is distinct from an observed absence and from a valid black
        // output. A selector alone cannot assert either stage.
        Observation Produced = Observation::Unknown;
        Observation Consumed = Observation::Unknown;
        Reason Degradation = Reason::None;
    };
} // namespace OloEngine::RendererSupport
