#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Tilemap/TilemapComponent.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class CommandHistory;

    // Tile-painting tool for `TilemapComponent` (issue #646).
    //
    // Shaped after `InstanceScatterBrushPanel` so the EditorLayer plumbing is the
    // same: a mode enum whose non-`Off` states intercept the viewport left-click,
    // an `OnUpdate` fed once per frame from the viewport, and one undo entry per
    // completed stroke rather than per painted tile.
    //
    // Unlike the scatter brush, the panel does its own picking: it intersects the
    // viewport ray with the target tilemap's own plane. A tilemap is flat and its
    // plane is known from the entity transform, so routing that through
    // EditorLayer's mesh/terrain raycast would be both wrong and more wiring.
    class TilemapPainterPanel
    {
      public:
        enum class Mode : u8
        {
            Off = 0,  ///< No viewport interception.
            Paint,    ///< Left-drag writes the selected tile.
            Erase,    ///< Left-drag clears tiles.
            RectFill, ///< Press, drag, release fills the dragged rectangle.
        };

        TilemapPainterPanel() = default;

        void SetContext(const Ref<Scene>& scene)
        {
            m_Context = scene;
        }
        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }
        // Fed from the Scene Hierarchy selection. Painting is refused unless the
        // entity actually carries a TilemapComponent, checked at paint time.
        void SetTargetEntity(Entity entity)
        {
            m_TargetEntity = entity;
        }

        void OnImGuiRender();

        // @param mouseRay  viewport ray under the cursor, in world space.
        // @param hasRay    false when the cursor is outside the viewport.
        // @param mouseDown left button held, with the editor's own suppressions
        //                  (gizmo hover, alt-orbit) already applied.
        void OnUpdate(const Ray& mouseRay, bool hasRay, bool mouseDown);

        [[nodiscard]] Mode GetMode() const
        {
            return m_Mode;
        }
        // True when the panel wants the viewport's left-click. EditorLayer uses
        // this to suppress entity picking, exactly as it does for the other brushes.
        [[nodiscard]] bool IsActive() const
        {
            return m_Mode != Mode::Off && m_TargetEntity;
        }

        bool Visible = true;

      private:
        // Intersect `ray` with the target tilemap's plane and convert to tile
        // coordinates. Returns false when the entity has no tilemap, the ray runs
        // parallel to the plane, the hit is behind the camera, or the hit falls
        // outside the grid.
        [[nodiscard]] bool PickTile(const Ray& ray, u32& outX, u32& outY) const;

        void BeginStroke();
        void EndStroke();
        // Writes `value` at (x, y) of the active layer, no-op if unchanged.
        void ApplyTile(u32 x, u32 y, u32 value);
        void ApplyRect(u32 x0, u32 y0, u32 x1, u32 y1, u32 value);

        void DrawTilesetPicker();
        void DrawLayerList();
        void DrawGridSettings();

        Ref<Scene> m_Context;
        CommandHistory* m_CommandHistory = nullptr;
        Entity m_TargetEntity;

        Mode m_Mode = Mode::Off;

        // Biased tile value the Paint tool writes (0 would erase, so the picker
        // never selects it; 1 is the first atlas tile).
        u32 m_SelectedTile = 1;
        sizet m_ActiveLayer = 0;
        // Zoom of the atlas thumbnails in the picker, in screen pixels per tile.
        f32 m_PickerTileSize = 32.0f;

        // Stroke tracking. The pre-stroke snapshot is the whole layer vector: a
        // rectangle fill or a long drag can touch thousands of cells, and a
        // per-cell command list would make Ctrl+Z undo one tile at a time.
        bool m_StrokeActive = false;
        bool m_PrevMouseDown = false;
        std::vector<TileLayer> m_StrokePreSnapshot;

        // Pending grid size for the explicit "Apply Size" button, re-seeded
        // whenever the selected tilemap changes.
        UUID m_PendingSizeEntity = 0;
        i32 m_PendingWidth = 0;
        i32 m_PendingHeight = 0;

        // RectFill drag anchor, in tile coordinates.
        bool m_HasRectAnchor = false;
        u32 m_RectAnchorX = 0;
        u32 m_RectAnchorY = 0;
    };
} // namespace OloEngine
