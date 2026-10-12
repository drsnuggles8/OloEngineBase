#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include "OloEngine/Terrain/Foliage/FoliagePlacement.h"
#include "OloEngine/Terrain/Foliage/FoliageCullTable.h"

#include <atomic>

namespace OloEngine
{
    // Records stay immutable while a replacement reads identity. Only the
    // published state's cached world-group bounds follow the terrain transform.
    class FFoliageCanonicalState final : public RefCounted
    {
      public:
        FoliageInstanceRegistry Registry;
    };

    struct FFoliageGenerationLayerSpec
    {
        FoliageBoundsProfile BoundsProfile;
        FoliageRepresentation Representation = FoliageRepresentation::MeshCard;
        bool VariantUnavailable = false;
    };

    class FFoliageGenerationInput final : public RefCounted
    {
      public:
        // Snapshot on the owner thread, with every runtime texture Ref cleared.
        TArray<FoliageLayer> Layers;
        TArray<f32> Heights;
        FoliagePlacement::MaskSnapshot Masks;
        TArray<FFoliageGenerationLayerSpec> Specs;
        Ref<FFoliageCanonicalState> Previous;
        u32 Resolution = 0;
        f32 WorldSizeX = 0;
        f32 WorldSizeZ = 0;
        f32 HeightScale = 0;
        std::atomic<bool> Cancelled = false;
        // The worker reads Previous only while seeding its private registry.
        // The owner may replace that stable registry's backing after this fence.
        std::atomic<bool> IdentityCopied = false;
    };

    struct FFoliageGeneratedLayer
    {
        TArray<FoliageInstanceData> Instances;
        FFoliageCullTable CullTable;
        BoundingBox Bounds;
        f32 PlantReach = 0;
    };

    template<>
    struct TIsTriviallyRelocatable<FFoliageGeneratedLayer>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(FFoliageGeneratedLayer::Instances)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageGeneratedLayer::CullTable)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageGeneratedLayer::Bounds)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageGeneratedLayer::PlantReach)>;
    };

    class FFoliageGenerationPayload final : public RefCounted
    {
      public:
        Ref<FFoliageCanonicalState> Canonical;
        TArray<FFoliageGeneratedLayer> Layers;
        f64 PlacementMs = 0;
        f64 RegistryMs = 0;
        f64 BoundsMs = 0;
        f64 CullPreparationMs = 0;
        [[nodiscard]] u64 GetCpuBytes() const;
    };

    // Pure CPU; no renderer, Scene, TerrainData, TerrainMaterial or GPU object
    // is captured. Cancellation produces no publishable result.
    [[nodiscard]] Ref<FFoliageGenerationPayload> PrepareFoliageGeneration(Ref<FFoliageGenerationInput> input);
} // namespace OloEngine
