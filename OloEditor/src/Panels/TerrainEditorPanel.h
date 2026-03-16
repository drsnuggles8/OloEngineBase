#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"
#include "OloEngine/Terrain/Editor/TerrainErosion.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class CommandHistory;

    enum class TerrainEditMode : u8
    {
        None = 0,
        Sculpt,
        Paint,
        Erosion
    };

    class TerrainEditorPanel
    {
      public:
        TerrainEditorPanel() = default;

        void SetContext(const Ref<Scene>& scene)
        {
            m_Context = scene;
        }
        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }
        void OnImGuiRender();

        // Called from EditorLayer each frame with terrain hit info
        void OnUpdate(f32 deltaTime, const glm::vec3& hitPos, bool hasHit, bool mouseDown);

        [[nodiscard]] TerrainEditMode GetEditMode() const
        {
            return m_EditMode;
        }
        [[nodiscard]] bool IsActive() const
        {
            return m_EditMode != TerrainEditMode::None;
        }

        // Get brush position for preview rendering
        [[nodiscard]] const glm::vec3& GetBrushWorldPos() const
        {
            return m_BrushWorldPos;
        }
        [[nodiscard]] f32 GetBrushRadius() const;
        [[nodiscard]] f32 GetBrushFalloff() const;
        [[nodiscard]] bool HasBrushHit() const
        {
            return m_HasBrushHit;
        }

        bool Visible = true;

      private:
        void DrawSculptUI();
        void DrawPaintUI();
        void DrawErosionUI();

        Ref<Scene> m_Context;
        CommandHistory* m_CommandHistory = nullptr;
        TerrainEditMode m_EditMode = TerrainEditMode::None;

        // Sculpt settings
        TerrainBrushSettings m_SculptSettings;

        // Paint settings
        TerrainPaintSettings m_PaintSettings;

        // Erosion
        TerrainErosion m_Erosion;
        ErosionSettings m_ErosionSettings;
        u32 m_ErosionIterations = 1;

        // Brush hit state (from viewport raycast)
        glm::vec3 m_BrushWorldPos{ 0.0f };
        bool m_HasBrushHit = false;

        // Stroke tracking for undo
        bool m_StrokeActive = false;
        // Accumulated dirty region across entire stroke
        u32 m_StrokeDirtyX = 0;
        u32 m_StrokeDirtyY = 0;
        u32 m_StrokeDirtyW = 0;
        u32 m_StrokeDirtyH = 0;
        // Snapshot of the height data before the stroke started
        std::vector<f32> m_StrokeOldHeights;
        // Snapshot of splatmap data before paint stroke
        std::vector<u8> m_StrokeOldSplatmap0;
        std::vector<u8> m_StrokeOldSplatmap1;
        // Terrain references for stroke undo
        Ref<TerrainData> m_StrokeTerrainData;
        Ref<TerrainChunkManager> m_StrokeChunkManager;
        Ref<TerrainMaterial> m_StrokeMaterial;
        f32 m_StrokeWorldSizeX = 0.0f;
        f32 m_StrokeWorldSizeZ = 0.0f;
        f32 m_StrokeHeightScale = 0.0f;
    };
} // namespace OloEngine
