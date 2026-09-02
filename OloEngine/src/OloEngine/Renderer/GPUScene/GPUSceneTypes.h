#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/LightCommon.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

// The canonical GPU Scene record contract (issues #991, #992, #993).
//
// Every record kind below follows one identity rule set, and the rule set is
// what the L1 property tests pin:
//
//   * A record is addressed by a stable KEY. The key never changes for the
//     lifetime of the authored thing (an entity's light of one type, one
//     imported material of one mesh source, the published global environment).
//   * The registry allocates a SLOT per key: reuse the slot the key already
//     owns, else the lowest free slot, else append. Commit order is key order,
//     so registry iteration order cannot move a slot.
//   * A slot carries a GENERATION. Zero is invalid. A handle is (slot,
//     generation); a consumer that stores a handle across frames re-validates
//     it against the record's Generation field.
//   * A COMPATIBLE edit keeps the generation and only dirties the record:
//     movement, colour, intensity, range, cone, factors, flags that do not
//     change how the surface or light is classified.
//   * An INCOMPATIBLE edit advances the generation in place so temporal
//     consumers reject their history for that slot: a material's closure
//     version, material type, alpha mode or any texture swap; an environment
//     texture swap; a light type change (the type is part of the key, so it
//     retires the old slot and allocates a new one). Removal always advances
//     the generation, and the slot is retired for RetirementFrameCount frames
//     before it can be reused, so a buffered frame never reads a recycled slot.
//     A slot whose generation would wrap to zero is never reused and keeps
//     its last generation on the tombstone, so a consumer tests the record's
//     Active flag as well as the generation it holds.
//   * An owner-token change (scene reload, backend switch) resets every kind
//     at once: every live slot is tombstoned and retired, every texture heap
//     offset is dropped with the record, and the next extraction re-resolves.
//
// Parity with GLSL is pinned twice: the static_assert blocks below (mechanism
// A) and the SPIRV-Cross reflection test over include/GPUScene.glsl
// (mechanism B, GPUSceneLayoutTest.cpp). Change a record on one side only and
// the other side fails loudly.
namespace OloEngine
{
    struct GPUSceneHandle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        u32 m_Index = InvalidIndex;
        u32 m_Generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return m_Index != InvalidIndex && m_Generation != 0;
        }

        [[nodiscard]] auto operator==(const GPUSceneHandle&) const -> bool = default;
    };

    struct GPUSceneAllocationPolicy
    {
        // GPU Scene is committed once per view/frame. A removed slot cannot be
        // recycled until the frame-resource slot that could still reference it
        // has completed on the GPU.
        static constexpr u64 RetirementFrameCount = 2;

        [[nodiscard]] static constexpr u64 RetirementReadyFrame(u64 currentFrame)
        {
            constexpr u64 maxFrame = std::numeric_limits<u64>::max();
            return currentFrame > maxFrame - RetirementFrameCount
                       ? maxFrame
                       : currentFrame + RetirementFrameCount;
        }

        // Zero is the invalid generation. Returning it permanently retires a
        // max-generation slot instead of wrapping an ancient handle alive.
        [[nodiscard]] static constexpr u32 NextGeneration(u32 current)
        {
            return current == std::numeric_limits<u32>::max() ? 0u : current + 1u;
        }

        [[nodiscard]] static constexpr u32 GrowCapacity(u32 current, u32 required)
        {
            u32 capacity = std::max(current, 1u);
            while (capacity < required)
            {
                if (capacity > std::numeric_limits<u32>::max() / 2u)
                {
                    return required;
                }
                capacity *= 2u;
            }
            return capacity;
        }
    };

    // A descriptor-heap offset that no consumer may index with. The material
    // and environment records carry the RHI resource handle (index +
    // generation) of every texture AND the heap offset that resolves it. The
    // offset is whatever HeapBinding::ResolveRecordTextureOffset returned at
    // extraction: a live heap slot on Vulkan (and on GL with the bindless heap
    // enabled), or this value when the heap is not enabled. A consumer that
    // finds this value must bind through the slot path; it must never treat
    // the record as a bindless promise (#805 is not assumed here).
    inline constexpr u32 GPUSceneHeapOffsetUnresolved = RHI::HeapOffset::Invalid;

    enum GPUSceneInstanceFlag : u32
    {
        GPUSceneInstanceFlagActive = 1u << 0,
    };

    enum GPUSceneGeometryFlag : u32
    {
        GPUSceneGeometryFlagActive = 1u << 0,
    };

    // Mirrors Material's authored state one bit per knob, so the deferred
    // G-Buffer flags lane, the closure contract and the shadow filter can all
    // read the same word. PBR / TwoSided / Blend / DepthTest /
    // DisableShadowCasting mirror MaterialType + MaterialFlag; IBL mirrors
    // Material::IsIBLEnabled (the material's own opt-in, not the runtime
    // global); UseTextureMaps mirrors the legacy Phong sampling switch; the
    // *Map bits mirror PBRMaterialUBO's Use*Map ints and are set exactly when
    // the matching texture handle is valid.
    enum GPUSceneMaterialFlag : u32
    {
        GPUSceneMaterialFlagActive = 1u << 0,
        GPUSceneMaterialFlagPBR = 1u << 1,
        GPUSceneMaterialFlagTwoSided = 1u << 2,
        GPUSceneMaterialFlagBlend = 1u << 3,
        GPUSceneMaterialFlagDepthTest = 1u << 4,
        GPUSceneMaterialFlagDisableShadowCasting = 1u << 5,
        GPUSceneMaterialFlagIBL = 1u << 6,
        GPUSceneMaterialFlagUseTextureMaps = 1u << 7,
        GPUSceneMaterialFlagAlbedoMap = 1u << 8,
        GPUSceneMaterialFlagMetallicRoughnessMap = 1u << 9,
        GPUSceneMaterialFlagNormalMap = 1u << 10,
        GPUSceneMaterialFlagOcclusionMap = 1u << 11,
        GPUSceneMaterialFlagEmissiveMap = 1u << 12,
        GPUSceneMaterialFlagSpecularMap = 1u << 13,
    };

    enum GPUSceneLightFlag : u32
    {
        GPUSceneLightFlagActive = 1u << 0,
        GPUSceneLightFlagCastShadows = 1u << 1,
    };

    enum GPUSceneEnvironmentFlag : u32
    {
        GPUSceneEnvironmentFlagActive = 1u << 0,
        // Irradiance, prefilter and BRDF LUT are all valid (EnvironmentMap::HasIBL).
        GPUSceneEnvironmentFlagIBL = 1u << 1,
        // The radiance cubemap itself is valid (skybox / specular source).
        GPUSceneEnvironmentFlagEnvironmentMap = 1u << 2,
    };

    // Numbering matches PBRCommon.glsl's DIRECTIONAL_LIGHT / POINT_LIGHT /
    // SPOT_LIGHT / SPHERE_AREA_LIGHT and MultiLightData's type tags, so the
    // raster adapter is a copy, never a remap.
    enum class GPUSceneLightType : u32
    {
        Directional = 0,
        Point = 1,
        Spot = 2,
        SphereArea = 3,
    };

    // Which authored thing a material key names. The owner/slot pair is
    // interpreted per source.
    enum class GPUSceneMaterialSource : u32
    {
        // The engine default material. Owner and slot are 0.
        Default = 0,
        // A MeshSource's imported material: owner is the mesh source's vertex
        // buffer identity (RHI::HashKey, the same identity the geometry key
        // uses), slot is Submesh::m_MaterialIndex.
        Imported = 1,
        // A MaterialComponent / InstancedMeshComponent override: owner is the
        // stable entity id, slot is the override lane
        // (GPUSceneMaterialOverrideLane), so an entity carrying both keeps two
        // records. Two entities with byte-identical overrides are two records
        // on purpose: the authored data is per entity.
        EntityOverride = 2,
        // A key that names no record: an imported material on a mesh source
        // with neither an asset handle nor GPU buffers. The renderer skips
        // extraction for it, and ExtractGPUSceneMesh rejects the same source,
        // so nothing can reference it.
        Unresolvable = 3,
    };

    // Which component supplied an EntityOverride material; it is the key's
    // slot because one entity can carry both at once.
    enum class GPUSceneMaterialOverrideLane : u32
    {
        MaterialComponent = 0,
        InstancedMesh = 1,
    };

    enum class GPUSceneVertexFormat : u32
    {
        Unknown = 0,
        OloVertex = 1,
    };

    enum class GPUSceneIndexFormat : u32
    {
        Unknown = 0,
        UInt32 = 1,
    };

    // Three affine rows avoid uploading the invariant fourth matrix row. The
    // representation is intentionally shared with the std430 shader contract.
    struct alignas(16) GPUSceneTransform
    {
        glm::vec4 Row0{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec4 Row1{ 0.0f, 1.0f, 0.0f, 0.0f };
        glm::vec4 Row2{ 0.0f, 0.0f, 1.0f, 0.0f };
    };

    struct alignas(16) GPUSceneInstance
    {
        GPUSceneTransform CurrentTransform;
        GPUSceneTransform PreviousTransform;

        u32 GeometryIndex = GPUSceneHandle::InvalidIndex;
        u32 GeometryGeneration = 0;
        // The canonical material slot (GPUSceneMaterial), never a mesh-local
        // material index. MaterialGeneration is the slot generation at commit
        // time; a consumer compares it with the material record's Generation
        // to reject history across an incompatible material edit.
        u32 MaterialIndex = GPUSceneHandle::InvalidIndex;
        u32 StableIndex = GPUSceneHandle::InvalidIndex;

        u32 VisibilityMask = 0;
        u32 Flags = 0;
        u32 Generation = 0;
        u32 MaterialGeneration = 0;
    };

    struct alignas(16) GPUSceneGeometry
    {
        u32 VertexBufferIndex = RHI::ResourceHandle::InvalidIndex;
        u32 VertexBufferGeneration = 0;
        u32 IndexBufferIndex = RHI::ResourceHandle::InvalidIndex;
        u32 IndexBufferGeneration = 0;

        u64 VertexAddress = 0;
        u64 IndexAddress = 0;

        u32 VertexFormat = 0;
        u32 IndexFormat = 0;
        u32 FirstIndex = 0;
        u32 IndexCount = 0;

        i32 BaseVertex = 0;
        u32 VertexCount = 0;
        u32 Generation = 0;
        u32 Flags = 0;
    };

    // The canonical material record (issue #992). Field inventory is
    // PBRMaterialUBO + MaterialUBO, and every UBO field is either carried or
    // named here as deliberately excluded:
    //
    //   carried  BaseColorFactor (PBR base colour; the legacy Diffuse rides the
    //            same lane because it is the same albedo knob), EmissiveFactor,
    //            Metallic/Roughness/NormalScale/OcclusionStrength, AlphaCutoff,
    //            AlphaMode, PBRModel (ClosureVersion), the legacy Ambient and
    //            Specular+Shininess, the Use*Map / DoubleSided / AlphaMode /
    //            UseTextureMaps switches (Flags), and every material-owned
    //            texture as RHI handle + heap offset.
    //   excluded EnableIBL's runtime half, IBLIntensity, ApplyGammaCorrection
    //            and EnableLightProbes are pass state set by the renderer per
    //            draw, not material data; the material's own IBL opt-in is
    //            GPUSceneMaterialFlagIBL. The IBL texture trio and the
    //            environment cubemap are environment data and live in
    //            GPUSceneEnvironment: PBRMaterialUBO::HeapOffsets[1..2] are
    //            populated from the global IBL fallback today, and giving
    //            them a material home would duplicate that record per material.
    struct alignas(16) GPUSceneMaterial
    {
        glm::vec4 BaseColorFactor{ 0.0f };
        glm::vec4 EmissiveFactor{ 0.0f };
        // Legacy Phong lane. LegacyAmbient.w is unused and always 0;
        // LegacySpecular.w is the shininess exponent.
        glm::vec4 LegacyAmbient{ 0.0f };
        glm::vec4 LegacySpecular{ 0.0f };

        f32 MetallicFactor = 0.0f;
        f32 RoughnessFactor = 0.0f;
        f32 NormalScale = 0.0f;
        f32 OcclusionStrength = 0.0f;

        f32 AlphaCutoff = 0.0f;
        u32 AlphaMode = 0;
        // PBRModel (Renderer/PBRModel.h). A change is an incompatible edit.
        u32 ClosureVersion = 0;
        u32 Flags = 0;

        u32 AlbedoTextureIndex = RHI::ResourceHandle::InvalidIndex;
        u32 AlbedoTextureGeneration = 0;
        u32 MetallicRoughnessTextureIndex = RHI::ResourceHandle::InvalidIndex;
        u32 MetallicRoughnessTextureGeneration = 0;

        u32 NormalTextureIndex = RHI::ResourceHandle::InvalidIndex;
        u32 NormalTextureGeneration = 0;
        u32 OcclusionTextureIndex = RHI::ResourceHandle::InvalidIndex;
        u32 OcclusionTextureGeneration = 0;

        u32 EmissiveTextureIndex = RHI::ResourceHandle::InvalidIndex;
        u32 EmissiveTextureGeneration = 0;
        // Legacy specular map. The legacy diffuse map shares the albedo lane.
        u32 SpecularTextureIndex = RHI::ResourceHandle::InvalidIndex;
        u32 SpecularTextureGeneration = 0;

        u32 AlbedoHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 MetallicRoughnessHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 NormalHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 OcclusionHeapOffset = GPUSceneHeapOffsetUnresolved;

        u32 EmissiveHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 SpecularHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 StableIndex = GPUSceneHandle::InvalidIndex;
        u32 Generation = 0;
    };

    // The canonical analytic light record (issue #993). Field inventory is the
    // four light components, and every authored field is carried or named:
    //
    //   carried  position (render-relative, see below), direction (as authored;
    //            a spot direction passes through SanitizeSpotLightDirection and
    //            is NOT normalised, exactly like the raster packing), colour,
    //            intensity (the authored unitless multiplier every evaluator
    //            reads today), range, sphere radius, inner/outer cone as the
    //            cosines every evaluator consumes, the quadratic attenuation
    //            coefficient exactly as authored, the spot falloff exponent
    //            (1.0, the value PBRCommon's calculateSpotIntensity is fed) and
    //            CastShadows.
    //   excluded the shadow atlas / VSM layer base entry is a per-frame render
    //            allocation, not authored data; the adapter emits "none" and
    //            Scene patches the raster structs after allocation as before.
    //            DirectionalLightComponent's bias / normal bias / max distance /
    //            cascade lambda / cascade debug are CSM configuration consumed
    //            through ShadowMap::SetSettings, not photometry. Point / spot
    //            m_ShadowBias / m_ShadowNormalBias are authored but read by no
    //            GPU path today (pre-existing); they are excluded rather than
    //            given a lane nothing consumes.
    //
    // Position is render-relative (camera-relative-rendering.md: a bare light
    // position uploaded to the GPU is shifted by the render origin), matching
    // GPUSceneInstance's transforms so one consumer reads one space.
    struct alignas(16) GPUSceneLight
    {
        // xyz = position (0 for directional), w = range (0 for directional).
        glm::vec4 PositionAndRange{ 0.0f };
        // xyz = direction the light travels, w = emitter sphere radius
        // (sphere area lights only, else 0).
        glm::vec4 DirectionAndRadius{ 0.0f };
        // rgb = colour, w = intensity.
        glm::vec4 ColorAndIntensity{ 0.0f };
        // x = cos(inner cutoff), y = cos(outer cutoff), z = quadratic
        // attenuation coefficient, w = spot falloff exponent.
        glm::vec4 ShapeParams{ 0.0f };

        u32 Type = 0;
        u32 Flags = 0;
        u32 StableIndex = GPUSceneHandle::InvalidIndex;
        u32 Generation = 0;
    };

    // The environment record's home (issue #993): one slot per published
    // environment, carrying the texture identities and the intensity. Slot 0
    // mirrors Renderer3D's global IBL as published by SetGlobalIBL /
    // OverrideGlobalIrradiance. Sampling distributions are deliberately not
    // here; a future emissive/environment distribution record references this
    // slot by (StableIndex, Generation).
    struct alignas(16) GPUSceneEnvironment
    {
        u32 EnvironmentIndex = RHI::ResourceHandle::InvalidIndex;
        u32 EnvironmentGeneration = 0;
        u32 IrradianceIndex = RHI::ResourceHandle::InvalidIndex;
        u32 IrradianceGeneration = 0;

        u32 PrefilterIndex = RHI::ResourceHandle::InvalidIndex;
        u32 PrefilterGeneration = 0;
        u32 BRDFLutIndex = RHI::ResourceHandle::InvalidIndex;
        u32 BRDFLutGeneration = 0;

        u32 EnvironmentHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 IrradianceHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 PrefilterHeapOffset = GPUSceneHeapOffsetUnresolved;
        u32 BRDFLutHeapOffset = GPUSceneHeapOffsetUnresolved;

        f32 Intensity = 0.0f;
        u32 Flags = 0;
        u32 StableIndex = GPUSceneHandle::InvalidIndex;
        u32 Generation = 0;
    };

    // Parity mechanism A: the std430 contract of every record, pinned here and
    // again by reflection in GPUSceneLayoutTest.cpp.
    static_assert(sizeof(GPUSceneTransform) == 48);
    static_assert(alignof(GPUSceneTransform) == 16);
    static_assert(sizeof(GPUSceneInstance) == 128);
    static_assert(alignof(GPUSceneInstance) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneInstance>);
    static_assert(std::is_trivially_copyable_v<GPUSceneInstance>);
    static_assert(offsetof(GPUSceneInstance, CurrentTransform) == 0);
    static_assert(offsetof(GPUSceneInstance, PreviousTransform) == 48);
    static_assert(offsetof(GPUSceneInstance, GeometryIndex) == 96);
    static_assert(offsetof(GPUSceneInstance, VisibilityMask) == 112);
    static_assert(offsetof(GPUSceneInstance, MaterialGeneration) == 124);
    static_assert(sizeof(GPUSceneGeometry) == 64);
    static_assert(alignof(GPUSceneGeometry) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneGeometry>);
    static_assert(std::is_trivially_copyable_v<GPUSceneGeometry>);
    static_assert(offsetof(GPUSceneGeometry, VertexAddress) == 16);
    static_assert(offsetof(GPUSceneGeometry, VertexFormat) == 32);
    static_assert(offsetof(GPUSceneGeometry, BaseVertex) == 48);
    static_assert(offsetof(GPUSceneGeometry, Generation) == 56);
    static_assert(sizeof(GPUSceneMaterial) == 176);
    static_assert(alignof(GPUSceneMaterial) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneMaterial>);
    static_assert(std::is_trivially_copyable_v<GPUSceneMaterial>);
    static_assert(offsetof(GPUSceneMaterial, BaseColorFactor) == 0);
    static_assert(offsetof(GPUSceneMaterial, EmissiveFactor) == 16);
    static_assert(offsetof(GPUSceneMaterial, LegacyAmbient) == 32);
    static_assert(offsetof(GPUSceneMaterial, LegacySpecular) == 48);
    static_assert(offsetof(GPUSceneMaterial, MetallicFactor) == 64);
    static_assert(offsetof(GPUSceneMaterial, AlphaCutoff) == 80);
    static_assert(offsetof(GPUSceneMaterial, ClosureVersion) == 88);
    static_assert(offsetof(GPUSceneMaterial, AlbedoTextureIndex) == 96);
    static_assert(offsetof(GPUSceneMaterial, NormalTextureIndex) == 112);
    static_assert(offsetof(GPUSceneMaterial, EmissiveTextureIndex) == 128);
    static_assert(offsetof(GPUSceneMaterial, AlbedoHeapOffset) == 144);
    static_assert(offsetof(GPUSceneMaterial, EmissiveHeapOffset) == 160);
    static_assert(offsetof(GPUSceneMaterial, StableIndex) == 168);
    static_assert(offsetof(GPUSceneMaterial, Generation) == 172);
    static_assert(sizeof(GPUSceneLight) == 80);
    static_assert(alignof(GPUSceneLight) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneLight>);
    static_assert(std::is_trivially_copyable_v<GPUSceneLight>);
    static_assert(offsetof(GPUSceneLight, PositionAndRange) == 0);
    static_assert(offsetof(GPUSceneLight, DirectionAndRadius) == 16);
    static_assert(offsetof(GPUSceneLight, ColorAndIntensity) == 32);
    static_assert(offsetof(GPUSceneLight, ShapeParams) == 48);
    static_assert(offsetof(GPUSceneLight, Type) == 64);
    static_assert(offsetof(GPUSceneLight, Generation) == 76);
    static_assert(sizeof(GPUSceneEnvironment) == 64);
    static_assert(alignof(GPUSceneEnvironment) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneEnvironment>);
    static_assert(std::is_trivially_copyable_v<GPUSceneEnvironment>);
    static_assert(offsetof(GPUSceneEnvironment, EnvironmentIndex) == 0);
    static_assert(offsetof(GPUSceneEnvironment, PrefilterIndex) == 16);
    static_assert(offsetof(GPUSceneEnvironment, EnvironmentHeapOffset) == 32);
    static_assert(offsetof(GPUSceneEnvironment, Intensity) == 48);
    static_assert(offsetof(GPUSceneEnvironment, Generation) == 60);

    struct GPUSceneDirtyRange
    {
        u32 m_FirstIndex = 0;
        u32 m_Count = 0;

        [[nodiscard]] auto operator==(const GPUSceneDirtyRange&) const -> bool = default;
    };

    enum class GPUSceneUnsupportedCategory : u32
    {
        Virtualized = 0,
        SoftwareRaster,
        Procedural,
        Terrain,
        Foliage,
        Particles,
        Fluids,
        Skinned,
        LegacyModel,
        LegacySubmesh,
        Tiles,
        Cloth,
        Count,
    };

    static constexpr sizet GPUSceneUnsupportedCategoryCount =
        static_cast<sizet>(GPUSceneUnsupportedCategory::Count);

    [[nodiscard]] constexpr const char* GetGPUSceneUnsupportedCategoryName(
        GPUSceneUnsupportedCategory category)
    {
        switch (category)
        {
            case GPUSceneUnsupportedCategory::Virtualized:
                return "Virtualized";
            case GPUSceneUnsupportedCategory::SoftwareRaster:
                return "Software raster";
            case GPUSceneUnsupportedCategory::Procedural:
                return "Procedural";
            case GPUSceneUnsupportedCategory::Terrain:
                return "Terrain";
            case GPUSceneUnsupportedCategory::Foliage:
                return "Foliage";
            case GPUSceneUnsupportedCategory::Particles:
                return "Particles";
            case GPUSceneUnsupportedCategory::Fluids:
                return "Fluids";
            case GPUSceneUnsupportedCategory::Skinned:
                return "Skinned";
            case GPUSceneUnsupportedCategory::LegacyModel:
                return "Legacy model";
            case GPUSceneUnsupportedCategory::LegacySubmesh:
                return "Legacy submesh";
            case GPUSceneUnsupportedCategory::Tiles:
                return "Tiles";
            case GPUSceneUnsupportedCategory::Cloth:
                return "Cloth";
            case GPUSceneUnsupportedCategory::Count:
                break;
        }
        return "Unknown";
    }

    // Per-kind registry telemetry: the same shape for every record kind, so
    // the profiler panel and the tests treat the kinds uniformly.
    struct GPUSceneKindStats
    {
        u32 m_Live = 0;
        u32 m_SlotCount = 0;
        u32 m_BufferCapacity = 0;
        u32 m_FreeSlots = 0;
        u32 m_RetiredSlots = 0;
        u64 m_UploadBytes = 0;
    };

    struct GPUSceneFrameStats
    {
        GPUSceneKindStats m_Instances;
        GPUSceneKindStats m_Geometries;
        GPUSceneKindStats m_Materials;
        GPUSceneKindStats m_Lights;
        GPUSceneKindStats m_Environments;
        u32 m_BufferGrowthEvents = 0;
        u32 m_UnsupportedTotal = 0;
        // The sum of the five per-kind upload figures.
        u64 m_UploadBytes = 0;
        f64 m_ExtractionTimeMs = 0.0;
        std::array<u32, GPUSceneUnsupportedCategoryCount> m_UnsupportedCounts{};
    };

    struct GPUSceneFrameUpdate
    {
        std::vector<GPUSceneDirtyRange> m_InstanceDirtyRanges;
        std::vector<GPUSceneDirtyRange> m_GeometryDirtyRanges;
        std::vector<GPUSceneDirtyRange> m_MaterialDirtyRanges;
        std::vector<GPUSceneDirtyRange> m_LightDirtyRanges;
        std::vector<GPUSceneDirtyRange> m_EnvironmentDirtyRanges;
        GPUSceneFrameStats m_Stats;
    };

    struct GPUSceneGeometryKey
    {
        u64 m_VertexBuffer = 0;
        u64 m_IndexBuffer = 0;
        u32 m_SubmeshIndex = 0;

        [[nodiscard]] auto operator<=>(const GPUSceneGeometryKey&) const = default;
    };

    struct GPUSceneInstanceKey
    {
        u64 m_EntityId = 0;
        GPUSceneGeometryKey m_Geometry;
        u64 m_InstanceId = 0;

        [[nodiscard]] auto operator<=>(const GPUSceneInstanceKey&) const = default;
    };

    struct GPUSceneMaterialKey
    {
        u64 m_Owner = 0;
        u32 m_Slot = 0;
        u32 m_Source = 0; // GPUSceneMaterialSource

        [[nodiscard]] auto operator<=>(const GPUSceneMaterialKey&) const = default;
    };

    // One light component of one entity. An entity may own several lights of
    // different types; a type change is a different key, i.e. a new identity.
    struct GPUSceneLightKey
    {
        u64 m_EntityId = 0;
        u32 m_Type = 0; // GPUSceneLightType

        [[nodiscard]] auto operator<=>(const GPUSceneLightKey&) const = default;
    };

    // Owner 0 is the renderer's published global environment. Reflection
    // probes and other future environments take their entity id.
    struct GPUSceneEnvironmentKey
    {
        u64 m_Owner = 0;

        [[nodiscard]] auto operator<=>(const GPUSceneEnvironmentKey&) const = default;
    };

    struct GPUSceneGeometryInput
    {
        RHI::ResourceHandle m_VertexBuffer;
        RHI::ResourceHandle m_IndexBuffer;
        u64 m_VertexAddress = 0;
        u64 m_IndexAddress = 0;
        u32 m_VertexFormat = 0;
        u32 m_IndexFormat = 0;
        u32 m_FirstIndex = 0;
        u32 m_IndexCount = 0;
        i32 m_BaseVertex = 0;
        u32 m_VertexCount = 0;
        u32 m_Flags = 0;

        [[nodiscard]] auto operator==(const GPUSceneGeometryInput&) const -> bool = default;
    };

    struct GPUSceneInstanceInput
    {
        glm::mat4 m_WorldTransform{ 1.0f };
        GPUSceneMaterialKey m_Material;
        u32 m_VisibilityMask = std::numeric_limits<u32>::max();
        u32 m_Flags = 0;
    };

    // A texture as a record carries it: the RHI identity plus the heap offset
    // resolved for it at extraction (GPUSceneHeapOffsetUnresolved when the
    // heap is not enabled).
    struct GPUSceneTextureRef
    {
        RHI::ResourceHandle m_Handle;
        u32 m_HeapOffset = GPUSceneHeapOffsetUnresolved;

        [[nodiscard]] auto operator==(const GPUSceneTextureRef&) const -> bool = default;
    };

    struct GPUSceneMaterialInput
    {
        glm::vec4 m_BaseColorFactor{ 1.0f };
        glm::vec4 m_EmissiveFactor{ 0.0f };
        glm::vec3 m_LegacyAmbient{ 0.1f };
        glm::vec3 m_LegacySpecular{ 1.0f };
        f32 m_Shininess = 32.0f;
        f32 m_MetallicFactor = 0.0f;
        f32 m_RoughnessFactor = 1.0f;
        f32 m_NormalScale = 1.0f;
        f32 m_OcclusionStrength = 1.0f;
        f32 m_AlphaCutoff = 0.5f;
        u32 m_AlphaMode = 0;
        u32 m_ClosureVersion = 0;
        // GPUSceneMaterialFlag bits other than Active and the *Map bits, which
        // the encoder derives.
        u32 m_Flags = 0;
        GPUSceneTextureRef m_Albedo;
        GPUSceneTextureRef m_MetallicRoughness;
        GPUSceneTextureRef m_Normal;
        GPUSceneTextureRef m_Occlusion;
        GPUSceneTextureRef m_Emissive;
        GPUSceneTextureRef m_Specular;
    };

    // Authored, world-space. The registry shifts the position by the frame's
    // render origin when it encodes the record.
    struct GPUSceneLightInput
    {
        u32 m_Type = static_cast<u32>(GPUSceneLightType::Point);
        glm::vec3 m_Position{ 0.0f };
        glm::vec3 m_Direction{ 0.0f, -1.0f, 0.0f };
        glm::vec3 m_Color{ 1.0f };
        f32 m_Intensity = 1.0f;
        f32 m_Range = 0.0f;
        f32 m_Radius = 0.0f;
        f32 m_InnerCutoffDegrees = 0.0f;
        f32 m_OuterCutoffDegrees = 0.0f;
        f32 m_Attenuation = 0.0f;
        f32 m_SpotFalloff = 1.0f;
        bool m_CastShadows = false;
    };

    struct GPUSceneEnvironmentInput
    {
        GPUSceneTextureRef m_Environment;
        GPUSceneTextureRef m_Irradiance;
        GPUSceneTextureRef m_Prefilter;
        GPUSceneTextureRef m_BRDFLut;
        f32 m_Intensity = 1.0f;
    };

    // ------------------------------------------------------------------------
    // Record encoders. One function per kind, used by the registry at commit
    // and by tests and adapters that need the record for an input without a
    // registry. The stable index and generation are the caller's; the defaults
    // encode a record that belongs to no slot (what the raster adapter reads).
    // ------------------------------------------------------------------------

    [[nodiscard]] inline GPUSceneMaterial EncodeGPUSceneMaterial(const GPUSceneMaterialInput& input,
                                                                 u32 stableIndex = GPUSceneHandle::InvalidIndex,
                                                                 u32 generation = 0)
    {
        GPUSceneMaterial record{};
        record.BaseColorFactor = input.m_BaseColorFactor;
        record.EmissiveFactor = input.m_EmissiveFactor;
        record.LegacyAmbient = glm::vec4(input.m_LegacyAmbient, 0.0f);
        record.LegacySpecular = glm::vec4(input.m_LegacySpecular, input.m_Shininess);
        record.MetallicFactor = input.m_MetallicFactor;
        record.RoughnessFactor = input.m_RoughnessFactor;
        record.NormalScale = input.m_NormalScale;
        record.OcclusionStrength = input.m_OcclusionStrength;
        record.AlphaCutoff = input.m_AlphaCutoff;
        record.AlphaMode = input.m_AlphaMode;
        record.ClosureVersion = input.m_ClosureVersion;

        u32 flags = input.m_Flags | GPUSceneMaterialFlagActive;
        const auto encodeTexture = [&flags](const GPUSceneTextureRef& texture, u32 presentFlag, u32& index,
                                            u32& textureGeneration, u32& heapOffset)
        {
            index = texture.m_Handle.Index;
            textureGeneration = texture.m_Handle.Generation;
            heapOffset = texture.m_Handle.IsValid() ? texture.m_HeapOffset : GPUSceneHeapOffsetUnresolved;
            if (texture.m_Handle.IsValid())
            {
                flags |= presentFlag;
            }
        };
        encodeTexture(input.m_Albedo, GPUSceneMaterialFlagAlbedoMap, record.AlbedoTextureIndex,
                      record.AlbedoTextureGeneration, record.AlbedoHeapOffset);
        encodeTexture(input.m_MetallicRoughness, GPUSceneMaterialFlagMetallicRoughnessMap,
                      record.MetallicRoughnessTextureIndex, record.MetallicRoughnessTextureGeneration,
                      record.MetallicRoughnessHeapOffset);
        encodeTexture(input.m_Normal, GPUSceneMaterialFlagNormalMap, record.NormalTextureIndex,
                      record.NormalTextureGeneration, record.NormalHeapOffset);
        encodeTexture(input.m_Occlusion, GPUSceneMaterialFlagOcclusionMap, record.OcclusionTextureIndex,
                      record.OcclusionTextureGeneration, record.OcclusionHeapOffset);
        encodeTexture(input.m_Emissive, GPUSceneMaterialFlagEmissiveMap, record.EmissiveTextureIndex,
                      record.EmissiveTextureGeneration, record.EmissiveHeapOffset);
        encodeTexture(input.m_Specular, GPUSceneMaterialFlagSpecularMap, record.SpecularTextureIndex,
                      record.SpecularTextureGeneration, record.SpecularHeapOffset);
        record.Flags = flags;
        record.StableIndex = stableIndex;
        record.Generation = generation;
        return record;
    }

    // The texture IDENTITY of a material record: every lane's RHI index and
    // generation, never a heap offset. One projection, so a lane the encoder
    // carries is part of the identity rule by construction.
    [[nodiscard]] constexpr std::array<u32, 12> GPUSceneMaterialTextureIdentity(const GPUSceneMaterial& record)
    {
        return { record.AlbedoTextureIndex, record.AlbedoTextureGeneration,
                 record.MetallicRoughnessTextureIndex, record.MetallicRoughnessTextureGeneration,
                 record.NormalTextureIndex, record.NormalTextureGeneration,
                 record.OcclusionTextureIndex, record.OcclusionTextureGeneration,
                 record.EmissiveTextureIndex, record.EmissiveTextureGeneration,
                 record.SpecularTextureIndex, record.SpecularTextureGeneration };
    }

    // The identity rule for materials, applied to two encodings of the same
    // key: true when the edit keeps the generation. Compared on the RHI
    // texture identity, never on heap offsets, so a heap re-resolve after a
    // reset does not read as a texture swap.
    [[nodiscard]] inline bool IsCompatibleGPUSceneMaterialEdit(const GPUSceneMaterial& previous,
                                                               const GPUSceneMaterial& next)
    {
        constexpr u32 classification = GPUSceneMaterialFlagPBR;
        return previous.ClosureVersion == next.ClosureVersion && previous.AlphaMode == next.AlphaMode &&
               (previous.Flags & classification) == (next.Flags & classification) &&
               GPUSceneMaterialTextureIdentity(previous) == GPUSceneMaterialTextureIdentity(next);
    }

    // The cone cosines come from LightCommon.h's SpotConeCosine, the one
    // expression the raster packing and the reference path tracer share, so
    // the adapter's output is bit-identical to the pre-record packing.
    [[nodiscard]] inline GPUSceneLight EncodeGPUSceneLight(const GPUSceneLightInput& input,
                                                           const glm::vec3& renderOrigin,
                                                           u32 stableIndex = GPUSceneHandle::InvalidIndex,
                                                           u32 generation = 0)
    {
        const auto type = static_cast<GPUSceneLightType>(input.m_Type);
        const bool directional = type == GPUSceneLightType::Directional;
        const bool spot = type == GPUSceneLightType::Spot;
        const bool sphere = type == GPUSceneLightType::SphereArea;

        GPUSceneLight record{};
        record.PositionAndRange = directional ? glm::vec4(0.0f)
                                              : glm::vec4(input.m_Position - renderOrigin, input.m_Range);
        // A spot direction is sanitised here as well as at the Scene producer
        // (LightCommon.h), so any producer of the record gets a finite,
        // non-zero direction the adapter can normalise. Valid directions pass
        // through unchanged, so the Scene path stays bit-identical.
        record.DirectionAndRadius = glm::vec4(spot ? SanitizeSpotLightDirection(input.m_Direction) : input.m_Direction,
                                              sphere ? input.m_Radius : 0.0f);
        record.ColorAndIntensity = glm::vec4(input.m_Color, input.m_Intensity);
        record.ShapeParams = glm::vec4(spot ? SpotConeCosine(input.m_InnerCutoffDegrees) : 0.0f,
                                       spot ? SpotConeCosine(input.m_OuterCutoffDegrees) : 0.0f,
                                       (directional || sphere) ? 0.0f : input.m_Attenuation,
                                       spot ? input.m_SpotFalloff : 0.0f);
        record.Type = input.m_Type;
        record.Flags = GPUSceneLightFlagActive | (input.m_CastShadows ? GPUSceneLightFlagCastShadows : 0u);
        record.StableIndex = stableIndex;
        record.Generation = generation;
        return record;
    }

    [[nodiscard]] inline GPUSceneEnvironment EncodeGPUSceneEnvironment(const GPUSceneEnvironmentInput& input,
                                                                       u32 stableIndex = GPUSceneHandle::InvalidIndex,
                                                                       u32 generation = 0)
    {
        GPUSceneEnvironment record{};
        const auto encodeTexture = [](const GPUSceneTextureRef& texture, u32& index, u32& textureGeneration,
                                      u32& heapOffset)
        {
            index = texture.m_Handle.Index;
            textureGeneration = texture.m_Handle.Generation;
            heapOffset = texture.m_Handle.IsValid() ? texture.m_HeapOffset : GPUSceneHeapOffsetUnresolved;
        };
        encodeTexture(input.m_Environment, record.EnvironmentIndex, record.EnvironmentGeneration,
                      record.EnvironmentHeapOffset);
        encodeTexture(input.m_Irradiance, record.IrradianceIndex, record.IrradianceGeneration,
                      record.IrradianceHeapOffset);
        encodeTexture(input.m_Prefilter, record.PrefilterIndex, record.PrefilterGeneration,
                      record.PrefilterHeapOffset);
        encodeTexture(input.m_BRDFLut, record.BRDFLutIndex, record.BRDFLutGeneration, record.BRDFLutHeapOffset);
        record.Intensity = input.m_Intensity;
        record.Flags = GPUSceneEnvironmentFlagActive;
        if (input.m_Irradiance.m_Handle.IsValid() && input.m_Prefilter.m_Handle.IsValid() &&
            input.m_BRDFLut.m_Handle.IsValid())
        {
            record.Flags |= GPUSceneEnvironmentFlagIBL;
        }
        if (input.m_Environment.m_Handle.IsValid())
        {
            record.Flags |= GPUSceneEnvironmentFlagEnvironmentMap;
        }
        record.StableIndex = stableIndex;
        record.Generation = generation;
        return record;
    }

    // The texture identity of an environment record, the same shape as the
    // material projection above.
    [[nodiscard]] constexpr std::array<u32, 8> GPUSceneEnvironmentTextureIdentity(const GPUSceneEnvironment& record)
    {
        return { record.EnvironmentIndex, record.EnvironmentGeneration, record.IrradianceIndex,
                 record.IrradianceGeneration, record.PrefilterIndex, record.PrefilterGeneration,
                 record.BRDFLutIndex, record.BRDFLutGeneration };
    }

    // The identity rule for environments: any texture identity change is a
    // swap and therefore incompatible; intensity is compatible.
    [[nodiscard]] inline bool IsCompatibleGPUSceneEnvironmentEdit(const GPUSceneEnvironment& previous,
                                                                  const GPUSceneEnvironment& next)
    {
        return GPUSceneEnvironmentTextureIdentity(previous) == GPUSceneEnvironmentTextureIdentity(next);
    }
} // namespace OloEngine
