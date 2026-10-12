#include "OloEnginePCH.h"
#include "FoliageGeneration.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace OloEngine
{
    u64 FFoliageGenerationPayload::GetCpuBytes() const
    {
        u64 bytes = sizeof(*this) + Layers.GetAllocatedSize();
        for (const auto& layer : Layers)
            bytes += layer.Instances.GetAllocatedSize() + layer.CullTable.Words.GetAllocatedSize();
        if (Canonical)
        {
            const auto& registry = Canonical->Registry;
            bytes += registry.GetRecords().GetAllocatedSize() + registry.GetGroups().GetAllocatedSize();
            for (const auto& group : registry.GetGroups())
                bytes += group.m_Instances.GetAllocatedSize() + group.m_RecordIndices.GetAllocatedSize();
        }
        return bytes;
    }

    Ref<FFoliageGenerationPayload> PrepareFoliageGeneration(Ref<FFoliageGenerationInput> input)
    {
        if (!input)
            return {};
        if (input->Cancelled.load(std::memory_order_relaxed))
        {
            input->IdentityCopied.store(true, std::memory_order_release);
            return {};
        }
        if (input->Specs.Num() != input->Layers.Num())
        {
            input->IdentityCopied.store(true, std::memory_order_release);
            return {};
        }
        auto result = Ref<FFoliageGenerationPayload>::Create();
        result->Canonical = Ref<FFoliageCanonicalState>::Create();
        auto& registry = result->Canonical->Registry;
        auto stageStart = std::chrono::steady_clock::now();
        const auto finish = [&stageStart](f64& milliseconds)
        {
            const auto now = std::chrono::steady_clock::now();
            milliseconds += std::chrono::duration<f64, std::milli>(now - stageStart).count();
            stageStart = now;
        };
        if (input->Previous)
            registry.SeedRebuildIdentity(input->Previous->Registry);
        input->IdentityCopied.store(true, std::memory_order_release);
        registry.BeginGeneration(input->Layers);
        finish(result->RegistryMs);
        result->Layers.SetNum(input->Layers.Num());
        TArray<FoliagePlacement::Placement> placements;
        for (i32 index = 0; index < input->Layers.Num(); ++index)
        {
            if (input->Cancelled.load(std::memory_order_relaxed))
                return {};
            const auto& layer = input->Layers[index];
            if (!layer.Enabled || layer.Density <= 0.0f)
                continue;
            const auto& spec = input->Specs[index];
            auto& output = result->Layers[index];
            FoliagePlacement::GenerateLayer(layer, static_cast<u32>(index), input->Heights, input->Resolution,
                                            input->Masks, input->WorldSizeX, input->WorldSizeZ, input->HeightScale, placements);
            finish(result->PlacementMs);
            if (input->Cancelled.load(std::memory_order_relaxed))
                return {};
            registry.BeginLayer(static_cast<u32>(index), layer, FoliagePlacement::SeedForLayer(static_cast<u32>(index)),
                                FoliagePlacement::SpacingForDensity(layer.Density), input->WorldSizeX, input->WorldSizeZ,
                                spec.Representation, spec.VariantUnavailable, spec.BoundsProfile);
            output.Instances.Reserve(placements.Num());
            for (i32 rowIndex = 0; rowIndex < placements.Num(); ++rowIndex)
            {
                if ((rowIndex & 4095) == 0 && input->Cancelled.load(std::memory_order_relaxed))
                    return {};
                const auto& placement = placements[rowIndex];
                registry.AddInstance(placement.m_CellX, placement.m_CellZ, placement.m_Row, static_cast<u32>(rowIndex));
                auto row = placement.m_Row;
                row.RotationHeight.w = FoliageWindPhase(registry.GetRecords().Last().m_Id);
                output.Instances.Add(row);
            }
            registry.EndLayer();
            finish(result->RegistryMs);
            glm::vec3 lo(std::numeric_limits<f32>::max()), hi(std::numeric_limits<f32>::lowest());
            for (const auto& row : output.Instances)
            {
                const glm::vec3 root(row.PositionScale);
                const auto box = FoliageInstanceBounds(root, row.PositionScale.w, row.RotationHeight.y, spec.BoundsProfile);
                lo = glm::min(lo, box.Min);
                hi = glm::max(hi, box.Max);
                const auto reach = glm::max(box.Max - root, root - box.Min);
                output.PlantReach = std::max({ output.PlantReach, reach.x, reach.y, reach.z });
            }
            output.Bounds = output.Instances.IsEmpty() ? BoundingBox(glm::vec3(0), glm::vec3(0)) : BoundingBox(lo, hi);
            finish(result->BoundsMs);
        }
        if (input->Cancelled.load(std::memory_order_relaxed))
            return {};
        registry.EndGeneration();
        finish(result->RegistryMs);
        for (i32 index = 0; index < result->Layers.Num(); ++index)
        {
            if (input->Cancelled.load(std::memory_order_relaxed))
                return {};
            auto& layer = result->Layers[index];
            layer.CullTable = FoliageGPUCuller::PrepareLayer(registry, static_cast<u32>(index),
                                                             static_cast<u32>(layer.Instances.Num()), input->Specs[index].BoundsProfile);
        }
        finish(result->CullPreparationMs);
        return input->Cancelled.load(std::memory_order_relaxed) ? Ref<FFoliageGenerationPayload>{} : result;
    }
} // namespace OloEngine
