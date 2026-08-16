#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/UUID.h"

#include <string>

namespace OloEngine
{
    class Scene;

    // @brief Timed caption overlay (issue #458).
    //
    // Two sources feed it, in priority order:
    //
    //  1. An explicitly pushed caption (ShowCaption) with time remaining. This
    //     is the hook for anything that is not a dialogue tree — a scripted
    //     voice line, a cinematic beat, an ambient barks system.
    //  2. The first entity with an active DialogueStateComponent, which already
    //     carries the line, the speaker and the reveal timing.
    //
    // There is deliberately NO audio-driven source: DialogueNodeData carries no
    // audio asset or duration and nothing in the engine plays a voice line, so
    // an "audio-synced captions" claim would be unfounded. ShowCaption is the
    // seam an audio path would call into when one exists.
    //
    // Follows DialogueUIController's ownership pattern — the UI entities are
    // held by UUID and resolved through the Scene each tick, so no new ECS
    // component is introduced and Scene::Copy has nothing extra to carry.
    // Entities are created LAZILY on the first caption, so a project that never
    // shows one pays nothing.
    class SubtitleSystem
    {
      public:
        SubtitleSystem() = default;
        ~SubtitleSystem() = default;

        SubtitleSystem(const SubtitleSystem&) = delete;
        SubtitleSystem& operator=(const SubtitleSystem&) = delete;

        // Destroys the owned UI entities. Safe to call when none were created.
        void Shutdown(Scene& scene);

        // Advances the pushed-caption timer and republishes the visible line.
        // A no-op beyond hiding the overlay while SubtitlesEnabled is false.
        void Update(Scene& scene, Timestep ts);

        // Push a caption to display for `durationSeconds`. A non-positive or
        // non-finite duration clears instead of showing, so a bad value can
        // never pin a caption on screen forever. `text` and `speaker` may use
        // the engine's "@key:" localization prefix.
        void ShowCaption(const std::string& text, const std::string& speaker, f32 durationSeconds);

        // Drop the pushed caption immediately. Dialogue-sourced captions are
        // unaffected — they end when the dialogue does.
        void ClearCaption() noexcept;

        // --- Introspection, for tests and tooling ---

        // The exact string currently published to the caption UI (speaker
        // prefix included), or empty when nothing is shown.
        [[nodiscard]] const std::string& GetVisibleText() const noexcept
        {
            return m_VisibleText;
        }

        [[nodiscard]] bool IsVisible() const noexcept
        {
            return m_IsVisible;
        }

        // UUID of the caption's UITextComponent entity, or 0 before the first
        // caption creates it.
        [[nodiscard]] UUID GetTextEntity() const noexcept
        {
            return m_TextEntity;
        }

        // Compose the published line from its parts. Static and pure so the
        // formatting contract is testable without a Scene; both localization
        // resolution and the speaker prefix live here.
        [[nodiscard]] static std::string ComposeCaption(const std::string& text,
                                                        const std::string& speaker,
                                                        bool showSpeaker);

      private:
        // Creates the canvas/panel/text hierarchy if it does not exist yet.
        // Returns false when the scene could not host it.
        bool EnsureEntities(Scene& scene);
        // Destroys the owned entities WITHOUT touching the pending caption —
        // the rebuild path in EnsureEntities needs the caption to survive.
        void DestroyEntities(Scene& scene);
        void SetVisible(Scene& scene, bool visible);
        void PublishText(Scene& scene, const std::string& text);

        UUID m_CanvasEntity = 0;
        UUID m_PanelEntity = 0;
        UUID m_TextEntity = 0;

        // Pushed-caption state (source 1).
        std::string m_PendingText;
        std::string m_PendingSpeaker;
        f32 m_PendingSecondsRemaining = 0.0f;

        std::string m_VisibleText;
        bool m_IsVisible = false;
    };
} // namespace OloEngine
