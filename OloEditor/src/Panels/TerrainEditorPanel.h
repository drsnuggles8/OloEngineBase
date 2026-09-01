#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"
#include "OloEngine/Terrain/Editor/TerrainErosion.h"
#include "OloEngine/Terrain/Editor/TerrainGPUBrush.h"
#include "OloEngine/Terrain/Editor/TerrainTextureUndoStack.h"
#include "OloEngine/Terrain/Voxel/VoxelEdit.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class CommandHistory;

    enum class TerrainEditMode : u8
    {
        None = 0,
        Generate,
        Sculpt,
        Paint,
        Voxel,
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

        // Called from EditorLayer each frame with terrain hit info. Only runs while
        // the VIEWPORT is hovered, which is correct for a brush stroke and wrong for
        // anything driven from the panel itself — see OnFrameTick.
        void OnUpdate(f32 deltaTime, const glm::vec3& hitPos, bool hasHit, bool mouseDown);

        // Every frame, unconditionally — including throttled frames, and including
        // frames where `editingAllowed` is false. Continuous erosion lives here
        // rather than in OnUpdate because OnUpdate is gated on m_ViewportHovered:
        // the user turns the mode on, drags the rate slider and unticks the box all
        // with the cursor over the PANEL, so an erosion session driven from OnUpdate
        // would never step and — worse — never settle, leaving the undo entry
        // unpushed and the collision body on the old surface.
        //
        // `editingAllowed` (the panel is shown and the editor is in Edit mode) gates
        // whether a session may STEP, not whether this is called: an active session
        // that becomes disallowed — Play pressed, panel closed — is settled here
        // rather than stranded.
        void OnFrameTick(bool editingAllowed);
        // Voxel picking supplies the exact grid cell and placement face rather
        // than inferring one from the rendered surface.
        //
        // By value, not by const reference: Ref<T> propagates constness, so a
        // const Ref hands out a const VoxelOverride& that the brush — which
        // writes cells — cannot bind to. Copying the Ref is one refcount bump
        // and matches the shared ownership the panel takes for the stroke.
        void OnVoxelUpdate(Ref<VoxelOverride> voxels, const VoxelRayHit& hit, bool mouseDown);

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
        void DrawGenerateUI();
        void DrawSculptUI();
        void DrawPaintUI();
        void DrawVoxelUI();
        void DrawErosionUI();

        // Lazily create the snapshot ring. Deferred rather than constructed with
        // the panel because it is pure VRAM: a session that never edits terrain
        // should not pay for it.
        TerrainTextureUndoStack& EnsureUndoStack();
        // Run one continuous-erosion frame and manage its undo session. Called from
        // OnFrameTick (not OnUpdate), because OnUpdate only runs while the viewport
        // is hovered and this is driven by a checkbox on the panel itself.
        void UpdateContinuousErosion();
        // Push the settled stroke's undo command for the GPU path.
        void CommitGPUSculptStroke();
        void CommitGPUPaintStroke();
        // Close a continuous-erosion session: push its undo entry and bring the
        // CPU-side consumers (chunk meshes, collision body) up to date.
        void EndContinuousErosionSession();
        // Shared settle path for both erosion entry points — the continuous session
        // and the one-shot Apply button — so the two cannot drift on which
        // follow-ups an erosion pass owes.
        // chunkManager BY VALUE, not by const reference: Ref<T> propagates
        // constness, so a const Ref hands out a const TerrainChunkManager& that
        // the non-const GenerateAllChunks cannot be called on. Same trap the
        // OnVoxelUpdate comment above records, and one refcount bump to avoid.
        void SettleErosionEdit(const Ref<TerrainData>& data,
                               Ref<TerrainChunkManager> chunkManager,
                               entt::entity entity,
                               f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                               const Ref<Texture2D>& preImage);

        Ref<Scene> m_Context;
        CommandHistory* m_CommandHistory = nullptr;
        TerrainEditMode m_EditMode = TerrainEditMode::None;

        // Sculpt settings
        TerrainBrushSettings m_SculptSettings;

        // Paint settings
        TerrainPaintSettings m_PaintSettings;
        VoxelBrushSettings m_VoxelSettings;

        // GPU authoring path (issue #716). The CPU brushes stay as the fallback
        // for a session where these kernels did not compile.
        TerrainGPUBrush m_GPUBrush;
        Ref<TerrainTextureUndoStack> m_UndoStack;

        // Erosion
        TerrainErosion m_Erosion;
        ErosionSettings m_ErosionSettings;
        u32 m_ErosionIterations = 1;
        // Continuous mode: iterate every frame so the surface visibly converges
        // while the rate slider is being dragged, instead of one batch per button
        // press. This is only affordable because the dispatch no longer reads the
        // heightmap back (issue #716) — with the old per-iteration full-map
        // GetData it would have been one whole-map stall per frame.
        bool m_ErosionContinuous = false;
        u32 m_ErosionIterationsPerFrame = 1;
        bool m_ErosionSessionActive = false;
        Ref<TerrainData> m_ErosionTerrainData;
        Ref<TerrainChunkManager> m_ErosionChunkManager;
        entt::entity m_ErosionEntity = entt::null;
        f32 m_ErosionWorldSizeX = 0.0f;
        f32 m_ErosionWorldSizeZ = 0.0f;
        f32 m_ErosionHeightScale = 0.0f;

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
        // Snapshot of the height data before the stroke started (CPU fallback path)
        std::vector<f32> m_StrokeOldHeights;
        // GPU path: whether this stroke went through TerrainGPUBrush, and the
        // reusable full-image pre-stroke copies the settle-time snapshots are
        // taken from. A stroke does not know its final rect until the mouse comes
        // up, so the BEFORE state has to be parked somewhere until then — see
        // TerrainTextureUndoStack::EnsureFullCopy.
        bool m_StrokeUsesGPU = false;
        Ref<Texture2D> m_StrokePreHeight;
        Ref<Texture2D> m_StrokePreSplat0;
        Ref<Texture2D> m_StrokePreSplat1;
        // Flatten/Level target, captured ONCE on press. Re-sampling it per frame
        // as the CPU brush did would mean a GPU->CPU height query per stroke
        // frame, which is the readback this issue exists to remove.
        f32 m_StrokeTargetHeight = 0.0f;
        // Snapshot of splatmap data before paint stroke
        std::vector<u8> m_StrokeOldSplatmap0;
        std::vector<u8> m_StrokeOldSplatmap1;
        // Terrain references for stroke undo
        Ref<TerrainData> m_StrokeTerrainData;
        Ref<TerrainChunkManager> m_StrokeChunkManager;
        Ref<TerrainMaterial> m_StrokeMaterial;
        Ref<VoxelOverride> m_StrokeVoxels;
        VoxelEditStroke m_VoxelStroke;
        f32 m_StrokeWorldSizeX = 0.0f;
        f32 m_StrokeWorldSizeZ = 0.0f;
        f32 m_StrokeHeightScale = 0.0f;
        // Owning terrain entity of the active sculpt stroke, so its collision body can be
        // refreshed once at stroke settle when physics is running (issue #469).
        entt::entity m_StrokeEntity = entt::null;
    };
} // namespace OloEngine
