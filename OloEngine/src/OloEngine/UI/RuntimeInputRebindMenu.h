#pragma once

#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputRebindController.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Scene;

    // In-game (runtime) input-rebinding panel built on the ECS UI toolkit and driven by
    // InputRebindController. Open() builds a UI canvas into the given scene — one row per
    // action showing its bindings, with Rebind / Pad (gamepad) / Reset buttons, plus footer
    // Reset-All / Save / Close buttons and modal Capture and Conflict overlays. OnUpdate()
    // (call once per frame AFTER the scene's UI input pass) polls button clicks, drives
    // gamepad capture, and refreshes labels; OnEvent() feeds keyboard/mouse capture.
    //
    // The panel is a plain UI overlay, not an ECS component, so it adds no cross-binding
    // touch-points and is instantiated by whatever owns the running scene (the runtime layer,
    // or the editor during Play).
    class RuntimeInputRebindMenu
    {
      public:
        RuntimeInputRebindMenu() = default;

        // Build the panel into `scene`, targeting `ctx`'s action map. `savePath` is where Save
        // persists (empty disables the Save button's effect). Rebuilds if already open.
        void Open(Scene& scene, InputContextType ctx = InputContextType::Gameplay, std::filesystem::path savePath = {});
        // Tear down all panel entities.
        void Close();
        [[nodiscard]] bool IsOpen() const
        {
            return m_Open;
        }

        // Poll UI interactions, drive gamepad capture, and refresh labels/overlays. Call once
        // per frame after Scene::OnUpdateRuntime (which runs the UI layout + input pass).
        void OnUpdate();
        // Feed keyboard/mouse events into an active capture. Returns true if consumed.
        bool OnEvent(Event& e);

        [[nodiscard]] InputRebindController& Controller()
        {
            return m_Controller;
        }

      private:
        struct Row
        {
            std::string Action;
            Entity BindingsLabel;
            Entity RebindButton;
            Entity PadButton;
            Entity ResetButton;
        };

        // --- Builders ---
        Entity MakePanel(Entity parent, glm::vec2 anchorMin, glm::vec2 anchorMax, glm::vec2 anchoredPos, glm::vec2 size, glm::vec4 color);
        Entity MakeText(Entity parent, const std::string& text, glm::vec2 anchoredPos, glm::vec2 size, f32 fontSize, glm::vec4 color);
        // Returns the button entity; parents a centered text label under it.
        Entity MakeButton(Entity parent, const std::string& label, glm::vec2 anchoredPos, glm::vec2 size);

        // --- Frame helpers ---
        void RefreshLabels();
        void RefreshOverlays();
        // Release-edge click detection against the previous frame's button state (read-only).
        [[nodiscard]] bool Clicked(Entity button) const;
        // Record every tracked button's current state for next frame's edge detection.
        void UpdateButtonStates();
        // Show/hide an overlay subtree by moving its root on/off screen.
        static void SetActive(Entity root, bool visible);

        Scene* m_Scene = nullptr;
        InputRebindController m_Controller;
        bool m_Open = false;
        std::filesystem::path m_SavePath;

        Entity m_Canvas;
        std::vector<Row> m_Rows;
        Entity m_ResetAllButton;
        Entity m_SaveButton;
        Entity m_CloseButton;
        Entity m_StatusLabel;

        Entity m_CaptureOverlay;
        Entity m_CaptureLabel;

        Entity m_ConflictOverlay;
        Entity m_ConflictLabel;
        Entity m_ReplaceButton;
        Entity m_SwapButton;
        Entity m_CancelButton;

        // Every interactive button in the panel — polled for clicks and state tracking.
        std::vector<Entity> m_AllButtons;
        // Previous-frame button state, keyed by entity UUID, for edge-detected clicks.
        std::unordered_map<u64, UIButtonState> m_PrevButtonState;
        // Set when the Close button is clicked; the owner polls IsOpen() after OnUpdate().
        bool m_CloseRequested = false;
    };

} // namespace OloEngine
