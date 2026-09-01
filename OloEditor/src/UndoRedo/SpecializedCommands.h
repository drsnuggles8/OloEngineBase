#pragma once

#include "EditorCommand.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainTextureUndoStack.h"
#include "OloEngine/Terrain/Voxel/VoxelEdit.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine
{
    // =========================================================================
    // PostProcessSettings undo — stores old/new snapshots of the entire POD struct
    // =========================================================================
    class PostProcessChangeCommand : public EditorCommand
    {
      public:
        PostProcessChangeCommand(PostProcessSettings oldSettings, PostProcessSettings newSettings, std::string description = "Post-Process Change")
            : m_OldSettings(oldSettings), m_NewSettings(newSettings), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            Renderer3D::GetPostProcessSettings() = m_NewSettings;
        }

        void Undo() override
        {
            Renderer3D::GetPostProcessSettings() = m_OldSettings;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        PostProcessSettings m_OldSettings;
        PostProcessSettings m_NewSettings;
        std::string m_Description;
    };

    // =========================================================================
    // Combined snapshot of all renderer settings editable via PostProcessSettingsPanel
    // =========================================================================
    struct PostProcessFullSnapshot
    {
        PostProcessSettings PostProcess;
        SnowSettings Snow;
        WindSettings Wind;
        SnowAccumulationSettings SnowAccumulation;
        SnowEjectaSettings SnowEjecta;
        PrecipitationSettings Precipitation;
        FogSettings Fog;

        static PostProcessFullSnapshot Capture()
        {
            return {
                Renderer3D::GetPostProcessSettings(),
                Renderer3D::GetSnowSettings(),
                Renderer3D::GetWindSettings(),
                Renderer3D::GetSnowAccumulationSettings(),
                Renderer3D::GetSnowEjectaSettings(),
                Renderer3D::GetPrecipitationSettings(),
                Renderer3D::GetFogSettings()
            };
        }

        void Apply() const
        {
            Renderer3D::GetPostProcessSettings() = PostProcess;
            Renderer3D::GetSnowSettings() = Snow;
            Renderer3D::GetWindSettings() = Wind;
            Renderer3D::GetSnowAccumulationSettings() = SnowAccumulation;
            Renderer3D::GetSnowEjectaSettings() = SnowEjecta;
            Renderer3D::GetPrecipitationSettings() = Precipitation;
            Renderer3D::GetFogSettings() = Fog;
        }

        bool operator==(const PostProcessFullSnapshot&) const = default;
    };

    // Undo command for the full snapshot
    class PostProcessFullChangeCommand : public EditorCommand
    {
      public:
        PostProcessFullChangeCommand(PostProcessFullSnapshot oldSnapshot, PostProcessFullSnapshot newSnapshot,
                                     std::string description = "Post-Process Change")
            : m_OldSnapshot(std::move(oldSnapshot)), m_NewSnapshot(std::move(newSnapshot)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            m_NewSnapshot.Apply();
        }

        void Undo() override
        {
            m_OldSnapshot.Apply();
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        PostProcessFullSnapshot m_OldSnapshot;
        PostProcessFullSnapshot m_NewSnapshot;
        std::string m_Description;
    };

    // =========================================================================
    // Script field undo — stores entity UUID, field name, old/new float value
    // Currently only Float fields are editable in the UI; extend for other types as needed
    // =========================================================================
    class ScriptFieldChangeCommand : public EditorCommand
    {
      public:
        ScriptFieldChangeCommand(Ref<Scene> scene, UUID entityUUID,
                                 std::string fieldName,
                                 f32 oldValue, f32 newValue)
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_FieldName(std::move(fieldName)), m_OldValue(oldValue), m_NewValue(newValue)
        {
        }

        void Execute() override
        {
            ApplyValue(m_NewValue);
        }

        void Undo() override
        {
            ApplyValue(m_OldValue);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Script Field Change (" + m_FieldName + ")";
        }

      private:
        void ApplyValue(f32 value)
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (!entityOpt)
            {
                return;
            }

            auto& entityFields = ScriptEngine::GetScriptFieldMap(*entityOpt);
            if (auto it = entityFields.find(m_FieldName); it != entityFields.end())
            {
                it->second.SetValue(value);
            }
        }

        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        std::string m_FieldName;
        f32 m_OldValue;
        f32 m_NewValue;
    };

    // =========================================================================
    // Terrain sculpt undo — stores a region of height data before/after a stroke
    // =========================================================================
    class TerrainSculptCommand : public EditorCommand
    {
      public:
        TerrainSculptCommand(Ref<TerrainData> terrainData,
                             Ref<TerrainChunkManager> chunkManager,
                             f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                             u32 regionX, u32 regionY, u32 regionW, u32 regionH,
                             std::vector<f32> oldHeights, std::vector<f32> newHeights,
                             WeakRef<Scene> scene = {}, UUID terrainEntity = UUID(0))
            : m_TerrainData(std::move(terrainData)), m_ChunkManager(std::move(chunkManager)), m_WorldSizeX(worldSizeX), m_WorldSizeZ(worldSizeZ), m_HeightScale(heightScale), m_RegionX(regionX), m_RegionY(regionY), m_RegionW(regionW), m_RegionH(regionH), m_OldHeights(std::move(oldHeights)), m_NewHeights(std::move(newHeights)), m_Scene(std::move(scene)), m_TerrainEntity(terrainEntity)
        {
        }

        void Execute() override
        {
            ApplyHeights(m_NewHeights);
        }

        void Undo() override
        {
            ApplyHeights(m_OldHeights);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Terrain Sculpt";
        }

      private:
        void ApplyHeights(const std::vector<f32>& heights)
        {
            if (!m_TerrainData)
            {
                return;
            }

            auto& fullData = m_TerrainData->GetHeightData();
            u32 resolution = m_TerrainData->GetResolution();

            // Validate region bounds
            if (m_RegionX + m_RegionW > resolution || m_RegionY + m_RegionH > resolution)
            {
                return;
            }
            if (heights.size() < static_cast<sizet>(m_RegionW) * m_RegionH)
            {
                return;
            }

            for (u32 row = 0; row < m_RegionH; ++row)
            {
                u32 srcIdx = row * m_RegionW;
                u32 dstIdx = (m_RegionY + row) * resolution + m_RegionX;
                std::memcpy(&fullData[dstIdx], &heights[srcIdx], m_RegionW * sizeof(f32));
            }

            m_TerrainData->UploadRegionToGPU(m_RegionX, m_RegionY, m_RegionW, m_RegionH);

            if (m_ChunkManager)
            {
                TerrainBrush::DirtyRegion dirty{ m_RegionX, m_RegionY, m_RegionW, m_RegionH };
                TerrainBrush::RebuildDirtyChunks(*m_ChunkManager, *m_TerrainData, dirty,
                                                 m_WorldSizeX, m_WorldSizeZ, m_HeightScale);
            }

            // Keep physics collision in step with the height edit on redo AND undo (issue
            // #469 review): the stroke-settle path in the panel only covers the initial
            // stroke, so an undo/redo during Play/Simulate would otherwise leave the
            // collision body on the stale surface. No-op in edit mode (JoltScene null) or
            // if the scene / entity is gone; the scene is held weakly so the undo history
            // never keeps it alive.
            if (Ref<Scene> scene = m_Scene.Lock())
            {
                Entity terrainEntity = scene->GetEntityByUUID(m_TerrainEntity);
                if (terrainEntity)
                    scene->UpdateTerrainCollisionAfterEdit(terrainEntity, m_RegionX, m_RegionY, m_RegionW, m_RegionH);
            }
        }

        Ref<TerrainData> m_TerrainData;
        Ref<TerrainChunkManager> m_ChunkManager;
        f32 m_WorldSizeX;
        f32 m_WorldSizeZ;
        f32 m_HeightScale;
        u32 m_RegionX;
        u32 m_RegionY;
        u32 m_RegionW;
        u32 m_RegionH;
        std::vector<f32> m_OldHeights;
        std::vector<f32> m_NewHeights;
        // Held WEAKLY so an entry in the undo history never keeps the whole Scene alive;
        // used to refresh terrain collision on redo/undo (issue #469 review).
        WeakRef<Scene> m_Scene;
        UUID m_TerrainEntity;
    };

    // =========================================================================
    // Terrain paint undo — stores splatmap region before/after a stroke
    // =========================================================================
    class TerrainPaintCommand : public EditorCommand
    {
      public:
        TerrainPaintCommand(Ref<TerrainMaterial> material,
                            u32 splatmapIndex,
                            u32 regionX, u32 regionY, u32 regionW, u32 regionH,
                            std::vector<u8> oldData, std::vector<u8> newData)
            : m_Material(std::move(material)), m_SplatmapIndex(splatmapIndex), m_RegionX(regionX), m_RegionY(regionY), m_RegionW(regionW), m_RegionH(regionH), m_OldData(std::move(oldData)), m_NewData(std::move(newData))
        {
        }

        void Execute() override
        {
            ApplyData(m_NewData);
        }

        void Undo() override
        {
            ApplyData(m_OldData);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Terrain Paint";
        }

      private:
        void ApplyData(const std::vector<u8>& data)
        {
            if (!m_Material || !m_Material->HasCPUSplatmaps())
            {
                return;
            }

            if (m_SplatmapIndex >= 2)
            {
                return;
            }

            auto& splatmap = m_Material->GetSplatmapData(m_SplatmapIndex);
            u32 resolution = m_Material->GetSplatmapResolution();
            constexpr u32 channels = 4; // RGBA8

            // Validate region bounds
            if (m_RegionX + m_RegionW > resolution || m_RegionY + m_RegionH > resolution)
            {
                return;
            }
            if (data.size() < static_cast<sizet>(m_RegionW) * m_RegionH * channels)
            {
                return;
            }

            for (u32 row = 0; row < m_RegionH; ++row)
            {
                u32 srcIdx = row * m_RegionW * channels;
                u32 dstIdx = ((m_RegionY + row) * resolution + m_RegionX) * channels;
                std::memcpy(&splatmap[dstIdx], &data[srcIdx], m_RegionW * channels);
            }

            m_Material->UploadSplatmapRegion(m_SplatmapIndex, m_RegionX, m_RegionY, m_RegionW, m_RegionH);
        }

        Ref<TerrainMaterial> m_Material;
        u32 m_SplatmapIndex;
        u32 m_RegionX;
        u32 m_RegionY;
        u32 m_RegionW;
        u32 m_RegionH;
        std::vector<u8> m_OldData;
        std::vector<u8> m_NewData;
    };

    // =========================================================================
    // GPU terrain undo (issue #716) — restores a texture region by blit
    // =========================================================================
    //
    // The CPU commands above (TerrainSculptCommand / TerrainPaintCommand) copy a
    // std::vector region in and out of the CPU mirror. With the brushes GPU-
    // resident there is no CPU array to copy at stroke time, so these hold two
    // TerrainTextureUndoStack snapshot ids per target — the state before the
    // stroke and after it — and undo/redo is a rect blit either way.
    //
    // The CPU commands are KEPT, not replaced: they are what a session without a
    // working brush compute shader (no GL 4.6, a shader that failed to compile)
    // still records, and the panel picks between the two families by asking
    // TerrainGPUBrush::IsReady(). Both restore paths end in the same three
    // follow-ups — mark the mirror stale, rebuild the affected chunks, resync
    // collision — so an undo during Play leaves the collision body on the surface
    // the player can see, which is the trap issue #469 fixed for the CPU path.
    //
    // A snapshot that has aged out of the bounded ring makes Restore() fail. That
    // is reported and the command becomes a no-op rather than silently writing
    // whatever texture happens to still be at that id.
    class TerrainGPUSculptCommand : public EditorCommand
    {
      public:
        TerrainGPUSculptCommand(Ref<TerrainData> terrainData,
                                Ref<TerrainChunkManager> chunkManager,
                                Ref<TerrainTextureUndoStack> undoStack,
                                f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                u32 regionX, u32 regionY, u32 regionW, u32 regionH,
                                TerrainTextureUndoStack::SnapshotId before,
                                TerrainTextureUndoStack::SnapshotId after,
                                WeakRef<Scene> scene = {}, UUID terrainEntity = UUID(0))
            : m_TerrainData(std::move(terrainData)), m_ChunkManager(std::move(chunkManager)),
              m_UndoStack(std::move(undoStack)), m_WorldSizeX(worldSizeX), m_WorldSizeZ(worldSizeZ),
              m_HeightScale(heightScale), m_RegionX(regionX), m_RegionY(regionY), m_RegionW(regionW),
              m_RegionH(regionH), m_Before(before), m_After(after), m_Scene(std::move(scene)),
              m_TerrainEntity(terrainEntity)
        {
        }

        ~TerrainGPUSculptCommand() override
        {
            // Give the ring its VRAM back when this history entry dies — a redo
            // branch being discarded, or the whole history being cleared.
            if (m_UndoStack)
            {
                m_UndoStack->Release(m_Before);
                m_UndoStack->Release(m_After);
            }
        }

        TerrainGPUSculptCommand(const TerrainGPUSculptCommand&) = delete;
        TerrainGPUSculptCommand& operator=(const TerrainGPUSculptCommand&) = delete;
        TerrainGPUSculptCommand(TerrainGPUSculptCommand&&) = delete;
        TerrainGPUSculptCommand& operator=(TerrainGPUSculptCommand&&) = delete;

        void Execute() override
        {
            RestoreSnapshot(m_After);
        }

        void Undo() override
        {
            RestoreSnapshot(m_Before);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Terrain Sculpt";
        }

      private:
        void RestoreSnapshot(TerrainTextureUndoStack::SnapshotId id)
        {
            if (!m_TerrainData || !m_UndoStack)
            {
                return;
            }

            if (!m_UndoStack->Restore(id, m_TerrainData->GetGPUHeightmap()))
            {
                OLO_CORE_WARN("TerrainGPUSculptCommand: snapshot no longer available - this step "
                              "has aged out of the bounded undo ring");
                return;
            }

            // The GPU heightmap moved; the CPU mirror is now behind it. Marking
            // rather than syncing is the whole point — a chain of undos costs one
            // readback at the next CPU consumer, not one per step.
            m_TerrainData->MarkGPUModified();

            if (m_ChunkManager)
            {
                TerrainBrush::DirtyRegion dirty{ m_RegionX, m_RegionY, m_RegionW, m_RegionH };
                TerrainBrush::RebuildDirtyChunks(*m_ChunkManager, *m_TerrainData, dirty,
                                                 m_WorldSizeX, m_WorldSizeZ, m_HeightScale);
            }

            if (Ref<Scene> scene = m_Scene.Lock())
            {
                Entity terrainEntity = scene->GetEntityByUUID(m_TerrainEntity);
                if (terrainEntity)
                    scene->UpdateTerrainCollisionAfterEdit(terrainEntity, m_RegionX, m_RegionY, m_RegionW, m_RegionH);
            }
        }

        Ref<TerrainData> m_TerrainData;
        Ref<TerrainChunkManager> m_ChunkManager;
        Ref<TerrainTextureUndoStack> m_UndoStack;
        f32 m_WorldSizeX;
        f32 m_WorldSizeZ;
        f32 m_HeightScale;
        u32 m_RegionX;
        u32 m_RegionY;
        u32 m_RegionW;
        u32 m_RegionH;
        TerrainTextureUndoStack::SnapshotId m_Before;
        TerrainTextureUndoStack::SnapshotId m_After;
        // Weak for the same reason as TerrainSculptCommand's: an undo history entry
        // must never keep the whole Scene alive.
        WeakRef<Scene> m_Scene;
        UUID m_TerrainEntity;
    };

    // Splatmap twin of the above. Carries a snapshot pair PER SPLATMAP because the
    // paint kernel re-normalises across all eight channels, so a stroke on a layer
    // in splatmap 0 changes splatmap 1 too whenever more than four layers exist —
    // restoring only the painted one would leave the weights not summing to 1.
    class TerrainGPUPaintCommand : public EditorCommand
    {
      public:
        TerrainGPUPaintCommand(Ref<TerrainMaterial> material,
                               Ref<TerrainTextureUndoStack> undoStack,
                               TerrainTextureUndoStack::SnapshotId before0,
                               TerrainTextureUndoStack::SnapshotId after0,
                               TerrainTextureUndoStack::SnapshotId before1,
                               TerrainTextureUndoStack::SnapshotId after1)
            : m_Material(std::move(material)), m_UndoStack(std::move(undoStack)),
              m_Before0(before0), m_After0(after0), m_Before1(before1), m_After1(after1)
        {
        }

        ~TerrainGPUPaintCommand() override
        {
            if (m_UndoStack)
            {
                m_UndoStack->Release(m_Before0);
                m_UndoStack->Release(m_After0);
                m_UndoStack->Release(m_Before1);
                m_UndoStack->Release(m_After1);
            }
        }

        TerrainGPUPaintCommand(const TerrainGPUPaintCommand&) = delete;
        TerrainGPUPaintCommand& operator=(const TerrainGPUPaintCommand&) = delete;
        TerrainGPUPaintCommand(TerrainGPUPaintCommand&&) = delete;
        TerrainGPUPaintCommand& operator=(TerrainGPUPaintCommand&&) = delete;

        void Execute() override
        {
            RestorePair(m_After0, m_After1);
        }

        void Undo() override
        {
            RestorePair(m_Before0, m_Before1);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Terrain Paint";
        }

      private:
        void RestorePair(TerrainTextureUndoStack::SnapshotId id0, TerrainTextureUndoStack::SnapshotId id1)
        {
            if (!m_Material || !m_UndoStack)
            {
                return;
            }

            // BOTH or neither, not "any". The paint kernel re-normalises across all
            // eight channels, so restoring one splatmap and not the other leaves the
            // weights not summing to 1 and silently reweights every other layer at
            // those texels. `|| restoredAny` let one success mask the other's
            // failure and produced exactly that half-restore.
            //
            // Evaluated into locals first: && would short-circuit and skip the second
            // Restore, which is a side-effecting call, not a predicate.
            const bool restored0 = m_UndoStack->Restore(id0, m_Material->GetSplatmap(0));
            // id1 is kInvalidSnapshot for a <=4-layer material, where the second
            // splatmap never participates — that is not a failure of this command.
            const bool needsSecond = (id1 != TerrainTextureUndoStack::kInvalidSnapshot);
            const bool restored1 = needsSecond && m_UndoStack->Restore(id1, m_Material->GetSplatmap(1));

            if (!restored0 || (needsSecond && !restored1))
            {
                OLO_CORE_WARN("TerrainGPUPaintCommand: snapshot no longer available - this step "
                              "has aged out of the bounded undo ring");
                return;
            }

            m_Material->MarkSplatmapsGPUModified();
        }

        Ref<TerrainMaterial> m_Material;
        Ref<TerrainTextureUndoStack> m_UndoStack;
        TerrainTextureUndoStack::SnapshotId m_Before0;
        TerrainTextureUndoStack::SnapshotId m_After0;
        TerrainTextureUndoStack::SnapshotId m_Before1;
        TerrainTextureUndoStack::SnapshotId m_After1;
    };

    // A voxel stroke snapshots only chunks it changed. Unlike a cell-delta
    // command this also restores sparse allocation exactly when a brush creates
    // or removes the last meaningful data in a chunk.
    class VoxelEditCommand : public EditorCommand
    {
      public:
        VoxelEditCommand(Ref<VoxelOverride> voxels, VoxelEditStroke stroke)
            : m_Voxels(std::move(voxels)), m_Stroke(std::move(stroke))
        {
        }

        void Execute() override
        {
            if (m_Voxels)
                m_Stroke.ApplyAfter(*m_Voxels);
        }

        void Undo() override
        {
            if (m_Voxels)
                m_Stroke.ApplyBefore(*m_Voxels);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Voxel Edit";
        }

      private:
        Ref<VoxelOverride> m_Voxels;
        VoxelEditStroke m_Stroke;
    };

    // =========================================================================
    // Streaming settings undo — snapshot-based, stores old/new StreamingSettings
    // =========================================================================
    class StreamingSettingsChangeCommand : public EditorCommand
    {
      public:
        // Apply callback: the panel provides a function to write settings back
        using ApplyFn = std::function<void(const StreamingSettings&)>;

        StreamingSettingsChangeCommand(StreamingSettings oldSettings, StreamingSettings newSettings, ApplyFn applyFn)
            : m_OldSettings(std::move(oldSettings)), m_NewSettings(std::move(newSettings)), m_ApplyFn(std::move(applyFn))
        {
        }

        void Execute() override
        {
            if (m_ApplyFn)
            {
                m_ApplyFn(m_NewSettings);
            }
        }

        void Undo() override
        {
            if (m_ApplyFn)
            {
                m_ApplyFn(m_OldSettings);
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Streaming Settings Change";
        }

      private:
        StreamingSettings m_OldSettings;
        StreamingSettings m_NewSettings;
        ApplyFn m_ApplyFn;
    };

    // =========================================================================
    // Dialogue editor undo — snapshot-based, stores full node/connection state
    // =========================================================================
    class DialogueEditorChangeCommand : public EditorCommand
    {
      public:
        using ApplyFn = std::function<void(const DialogueEditorSnapshot&)>;

        DialogueEditorChangeCommand(DialogueEditorSnapshot oldState, DialogueEditorSnapshot newState,
                                    ApplyFn applyFn, std::string description = "Dialogue Change")
            : m_OldState(std::move(oldState)), m_NewState(std::move(newState)),
              m_ApplyFn(std::move(applyFn)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            if (m_ApplyFn)
            {
                m_ApplyFn(m_NewState);
            }
        }

        void Undo() override
        {
            if (m_ApplyFn)
            {
                m_ApplyFn(m_OldState);
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        DialogueEditorSnapshot m_OldState;
        DialogueEditorSnapshot m_NewState;
        ApplyFn m_ApplyFn;
        std::string m_Description;
    };

    // =========================================================================
    // Skill tree editor undo — snapshot-based, stores full node list + tree ids
    // =========================================================================
    struct SkillTreeEditorSnapshot
    {
        std::string TreeID;
        std::string DisplayName;
        std::vector<SkillTreeNode> Nodes;

        auto operator==(const SkillTreeEditorSnapshot&) const -> bool = default;
    };

    class SkillTreeEditorChangeCommand : public EditorCommand
    {
      public:
        using ApplyFn = std::function<void(const SkillTreeEditorSnapshot&)>;

        SkillTreeEditorChangeCommand(SkillTreeEditorSnapshot oldState, SkillTreeEditorSnapshot newState,
                                     ApplyFn applyFn, std::string description = "Skill Tree Change")
            : m_OldState(std::move(oldState)), m_NewState(std::move(newState)),
              m_ApplyFn(std::move(applyFn)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            if (m_ApplyFn)
            {
                m_ApplyFn(m_NewState);
            }
        }

        void Undo() override
        {
            if (m_ApplyFn)
            {
                m_ApplyFn(m_OldState);
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        SkillTreeEditorSnapshot m_OldState;
        SkillTreeEditorSnapshot m_NewState;
        ApplyFn m_ApplyFn;
        std::string m_Description;
    };

    // =========================================================================
    // Input settings undo — snapshot-based, stores full InputActionMap
    // =========================================================================
    class InputActionMapChangeCommand : public EditorCommand
    {
      public:
        using NotifyFn = std::function<void()>;

        InputActionMapChangeCommand(InputContextType context, InputActionMap oldMap, InputActionMap newMap,
                                    std::string description = "Input Settings Change",
                                    NotifyFn onApply = nullptr)
            : m_Context(context), m_OldMap(std::move(oldMap)), m_NewMap(std::move(newMap)), m_Description(std::move(description)),
              m_OnApply(std::move(onApply))
        {
        }

        void Execute() override
        {
            InputActionManager::SetActionMap(m_Context, m_NewMap);
            if (m_OnApply)
            {
                m_OnApply();
            }
        }

        void Undo() override
        {
            InputActionManager::SetActionMap(m_Context, m_OldMap);
            if (m_OnApply)
            {
                m_OnApply();
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        InputContextType m_Context;
        InputActionMap m_OldMap;
        InputActionMap m_NewMap;
        std::string m_Description;
        NotifyFn m_OnApply;
    };

} // namespace OloEngine
