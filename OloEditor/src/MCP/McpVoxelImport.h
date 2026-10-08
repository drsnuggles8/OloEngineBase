#pragma once

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/Voxel/VoxelGreedyMeshBuilder.h"
#include "UndoRedo/EditorCommand.h"

#include <utility>
#include <span>
#include <string>

namespace OloEngine::MCP
{
    // A uniform VOX1 chunk is only 23 encoded bytes but expands to 160 KiB
    // including optional materials. Bound expanded allocation before decoding.
    [[nodiscard]] inline bool VoxelImportWithinBudget(std::span<const u8> bytes)
    {
        if (bytes.size() < 12)
            return false;
        const u32 chunks = static_cast<u32>(bytes[8]) | (static_cast<u32>(bytes[9]) << 8) |
                           (static_cast<u32>(bytes[10]) << 16) | (static_cast<u32>(bytes[11]) << 24);
        return chunks <= 1024;
    }

    // Import uses the existing VOX1 decoder. The imported volume is authored
    // content: the scene file and save games persist it (#1566).
    class VoxelImportCommand final : public EditorCommand
    {
      public:
        VoxelImportCommand(Ref<Scene> scene, UUID entity, Ref<VoxelOverride> replacement)
            : m_Scene(std::move(scene)), m_Entity(entity), m_After(std::move(replacement))
        {
            const auto& terrain = m_Scene->GetEntityByUUID(m_Entity).GetComponent<TerrainComponent>();
            m_Before = terrain.m_VoxelOverride;
        }

        void Execute() override
        {
            SetVolume(m_After);
        }
        void Undo() override
        {
            SetVolume(m_Before);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Import voxel override";
        }

      private:
        // A volume carries its own seeding state: the import is authored, and the
        // volume it replaced keeps whatever it was.
        void SetVolume(Ref<VoxelOverride> volume)
        {
            Entity entity = m_Scene->GetEntityByUUID(m_Entity);
            if (!entity || !entity.HasComponent<TerrainComponent>())
                return;
            auto& terrain = entity.GetComponent<TerrainComponent>();
            terrain.m_VoxelOverride = volume;
            terrain.m_VoxelMeshes.clear();
            terrain.m_VoxelQuadMeshes = nullptr;
            if (volume)
                volume->MarkAllChunksDirty();
        }

        Ref<Scene> m_Scene;
        UUID m_Entity;
        Ref<VoxelOverride> m_Before;
        Ref<VoxelOverride> m_After;
    };
} // namespace OloEngine::MCP
