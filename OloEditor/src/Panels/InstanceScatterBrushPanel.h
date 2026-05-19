#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class CommandHistory;

    // Surface scatter brush for `InstancedMeshComponent` — paints placements
    // onto the terrain heightmap each frame the left mouse is held. Designed
    // to mirror `TerrainEditorPanel` so the EditorLayer plumbing
    // (raycast / mouse intercept / overlay rendering) follows the same shape.
    //
    // Mode lifecycle: `Off` is the default (no viewport interception). `Paint`
    // enters tool mode — left-click + drag inside the viewport triggers
    // stroke deposits at the brush footprint each `OnUpdate()`. Stroke
    // boundaries (mouse-down / mouse-up) are detected from the `mouseDown`
    // flag transitioning across consecutive frames.
    //
    // Each stroke captures the target component's pre-stroke `Instances`
    // vector for the `CommandHistory` undo stack. Strokes deposit directly
    // into the live component; the undo command restores the snapshot.
    //
    // **Out of scope** (deferred):
    //   - Mesh-surface scatter (no BVH ray-cast yet — see
    //     `docs/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md` §1.2).
    //   - Brush ring preview in the 3D overlay (debug-line ring helper
    //     would have to be added to Renderer3D; not blocking the workflow).
    class InstanceScatterBrushPanel
    {
      public:
        enum class Mode : u8
        {
            Off = 0, // No viewport interception
            Paint    // Left-click + drag scatters within brush footprint
        };

        InstanceScatterBrushPanel() = default;

        void SetContext(const Ref<Scene>& scene)
        {
            m_Context = scene;
        }
        void SetCommandHistory(CommandHistory* history)
        {
            m_CommandHistory = history;
        }

        // Render the side-panel UI (mode toggle, brush settings, target
        // entity selector). The target entity is the `InstancedMeshComponent`
        // that receives painted placements.
        void OnImGuiRender();

        // Called from EditorLayer each frame with terrain raycast info.
        // `surfaceNormal` should be the world-space normal at `hitPos`;
        // pass `vec3(0, 1, 0)` for surfaces without a meaningful normal
        // (the slope filter then trivially passes for any threshold ≤ 1).
        void OnUpdate(f32 deltaTime, const glm::vec3& hitPos, const glm::vec3& surfaceNormal,
                      bool hasHit, bool mouseDown);

        [[nodiscard]] Mode GetMode() const
        {
            return m_Mode;
        }
        [[nodiscard]] bool IsActive() const
        {
            return m_Mode != Mode::Off && m_TargetEntity;
        }
        [[nodiscard]] const glm::vec3& GetBrushWorldPos() const
        {
            return m_BrushWorldPos;
        }
        [[nodiscard]] f32 GetBrushRadius() const
        {
            return m_BrushRadius;
        }
        [[nodiscard]] bool HasBrushHit() const
        {
            return m_HasBrushHit;
        }

        // Lets EditorLayer pick the target via Scene-Hierarchy click. The
        // panel will refuse to paint unless the selected entity carries an
        // `InstancedMeshComponent` (validated at paint time, not here).
        void SetTargetEntity(Entity entity)
        {
            m_TargetEntity = entity;
        }

        bool Visible = true;

      private:
        // Deposit one tick worth of placements at the brush footprint. Called
        // from `OnUpdate()` while the mouse is held and a hit point exists.
        void DepositStrokeTick(const glm::vec3& centre, const glm::vec3& surfaceNormal);

        // Snapshot the component's `Instances` vector and store it on the
        // panel; the corresponding `EditorCommand` is pushed onto the undo
        // stack only when the stroke ends (mouse-up) so a single Ctrl+Z
        // reverses the whole stroke rather than per-tick deposits.
        void BeginStroke();
        void EndStroke();

        Ref<Scene> m_Context;
        CommandHistory* m_CommandHistory = nullptr;
        Entity m_TargetEntity;

        Mode m_Mode = Mode::Off;

        // Brush settings
        f32 m_BrushRadius = 5.0f;       // World units
        i32 m_DepositsPerTick = 8;      // Candidate placements per frame while held
        f32 m_PoissonMinSpacing = 1.0f; // Min XZ distance between *new* and *existing* placements
        f32 m_ScaleMin = 0.8f;
        f32 m_ScaleMax = 1.2f;
        bool m_RandomYRot = true;
        // 0 disables slope filtering. Default ≈ 45° (cos 45° = 0.707).
        f32 m_SlopeMinDot = 0.0f;
        // Aligns the placement's local Y to the surface normal when true.
        // Off by default — most foliage looks better gravity-aligned.
        bool m_AlignToNormal = false;
        // Per-instance variant index (stored in InstanceData.Custom as a
        // 0..1 normalised float). Shader-side variant rendering is future
        // work, but the storage is wired so authors can paint variants now
        // and a future shader can branch on `Custom`.
        i32 m_VariantCount = 1;

        // Persistence (§1.7)
        std::array<char, 256> m_BakePathBuf{}; // editor textbox state
        bool m_BakePathInitialised = false;

        // Brush hit state (from viewport raycast)
        glm::vec3 m_BrushWorldPos{ 0.0f };
        glm::vec3 m_BrushSurfaceNormal{ 0.0f, 1.0f, 0.0f };
        bool m_HasBrushHit = false;

        // Stroke tracking for undo
        bool m_StrokeActive = false;
        bool m_PrevMouseDown = false;
        std::vector<InstanceData> m_StrokePreSnapshot;
    };
} // namespace OloEngine
