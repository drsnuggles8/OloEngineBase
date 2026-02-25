#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"
#include "OloEngine/Terrain/Editor/TerrainErosion.h"

#include <glm/glm.hpp>

namespace OloEngine
{
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

        void SetContext(const Ref<Scene>& scene) { m_Context = scene; }
        void OnImGuiRender();

        // Called from EditorLayer each frame with terrain hit info
        void OnUpdate(f32 deltaTime, const glm::vec3& hitPos, bool hasHit, bool mouseDown);

        [[nodiscard]] TerrainEditMode GetEditMode() const { return m_EditMode; }
        [[nodiscard]] bool IsActive() const { return m_EditMode != TerrainEditMode::None; }

        // Get brush position for preview rendering
        [[nodiscard]] const glm::vec3& GetBrushWorldPos() const { return m_BrushWorldPos; }
        [[nodiscard]] f32 GetBrushRadius() const;
        [[nodiscard]] f32 GetBrushFalloff() const;
        [[nodiscard]] bool HasBrushHit() const { return m_HasBrushHit; }

        bool Visible = true;

      private:
        void DrawSculptUI();
        void DrawPaintUI();
        void DrawErosionUI();

        Ref<Scene> m_Context;
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
        glm::vec3 m_BrushWorldPos{0.0f};
        bool m_HasBrushHit = false;
    };
} // namespace OloEngine
