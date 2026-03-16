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
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
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
                             std::vector<f32> oldHeights, std::vector<f32> newHeights)
            : m_TerrainData(std::move(terrainData)), m_ChunkManager(std::move(chunkManager)), m_WorldSizeX(worldSizeX), m_WorldSizeZ(worldSizeZ), m_HeightScale(heightScale), m_RegionX(regionX), m_RegionY(regionY), m_RegionW(regionW), m_RegionH(regionH), m_OldHeights(std::move(oldHeights)), m_NewHeights(std::move(newHeights))
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
    // Input settings undo — snapshot-based, stores full InputActionMap
    // =========================================================================
    class InputActionMapChangeCommand : public EditorCommand
    {
      public:
        InputActionMapChangeCommand(InputActionMap oldMap, InputActionMap newMap,
                                    std::string description = "Input Settings Change")
            : m_OldMap(std::move(oldMap)), m_NewMap(std::move(newMap)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            InputActionManager::SetActionMap(m_NewMap);
        }

        void Undo() override
        {
            InputActionManager::SetActionMap(m_OldMap);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        InputActionMap m_OldMap;
        InputActionMap m_NewMap;
        std::string m_Description;
    };

} // namespace OloEngine
