#include "OloEnginePCH.h"
#include "OloEngine/Accessibility/SubtitleSystem.h"

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Above DialogueUIController's canvas (100) so a caption is never
        // covered by the dialogue box it is captioning.
        constexpr i32 kSubtitleCanvasSortOrder = 200;
        constexpr i32 kHiddenCanvasSortOrder = -9999;

        // Bottom-centre letterbox, inset from the edges so the plate does not
        // collide with a HUD hugging the screen border.
        constexpr glm::vec2 kPanelAnchorMin{ 0.15f, 0.03f };
        constexpr glm::vec2 kPanelAnchorMax{ 0.85f, 0.18f };
    } // namespace

    std::string SubtitleSystem::ComposeCaption(const std::string& text,
                                               const std::string& speaker,
                                               bool showSpeaker)
    {
        // Route through the existing localization system rather than inventing
        // a parallel string path: a line stored as "@key:npc.greeting" resolves
        // against the active locale, and a literal string passes through
        // verbatim (LocalizationManager::ResolveLocalizedText).
        const std::string body = LocalizationManager::ResolveLocalizedText(text);
        if (body.empty())
            return {};

        if (!showSpeaker)
            return body;

        const std::string who = LocalizationManager::ResolveLocalizedText(speaker);
        if (who.empty())
            return body;

        return who + ": " + body;
    }

    void SubtitleSystem::ShowCaption(const std::string& text, const std::string& speaker, f32 durationSeconds)
    {
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0f)
        {
            // A caller that computed a bad duration gets nothing rather than a
            // caption pinned on screen for the rest of the session.
            ClearCaption();
            return;
        }

        m_PendingText = text;
        m_PendingSpeaker = speaker;
        m_PendingSecondsRemaining = durationSeconds;
    }

    void SubtitleSystem::ClearCaption() noexcept
    {
        m_PendingText.clear();
        m_PendingSpeaker.clear();
        m_PendingSecondsRemaining = 0.0f;
    }

    void SubtitleSystem::DestroyEntities(Scene& scene)
    {
        const auto destroyIfValid = [&scene](UUID& uuid)
        {
            if (static_cast<u64>(uuid) != 0)
            {
                if (Entity ent = scene.GetEntityByUUID(uuid))
                    scene.DestroyEntity(ent);
                uuid = 0;
            }
        };

        // Children first — Scene::DestroyEntity leaves children as orphans (only
        // DestroyEntityAndChildren cascades), so the canvas must go last or the
        // panel/text UUIDs this system tracks would dangle.
        destroyIfValid(m_TextEntity);
        destroyIfValid(m_PanelEntity);
        destroyIfValid(m_CanvasEntity);

        m_VisibleText.clear();
        m_IsVisible = false;
    }

    void SubtitleSystem::Shutdown(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        DestroyEntities(scene);
        ClearCaption();
    }

    bool SubtitleSystem::EnsureEntities(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        // All three, not just the text entity: a partially-destroyed hierarchy
        // would otherwise be kept and the caption would render unparented.
        const bool intact = static_cast<u64>(m_TextEntity) != 0 &&
                            static_cast<u64>(m_PanelEntity) != 0 &&
                            static_cast<u64>(m_CanvasEntity) != 0 &&
                            scene.GetEntityByUUID(m_TextEntity) &&
                            scene.GetEntityByUUID(m_PanelEntity) &&
                            scene.GetEntityByUUID(m_CanvasEntity);
        if (intact)
            return true;

        // Something destroyed part of the hierarchy out from under us (a scene
        // reload, a script). Tear down whatever survives and rebuild, so the
        // parent links can never point at dead entities.
        //
        // DestroyEntities, NOT Shutdown: Shutdown also clears the pending
        // caption, and this runs from Update AFTER the caption to display has
        // been chosen — so calling it here would drop the very caption that
        // triggered the creation, showing it for exactly one frame.
        DestroyEntities(scene);

        Entity canvasEntity = scene.CreateEntity("SubtitleCanvas");
        if (!canvasEntity)
            return false;
        {
            auto& canvas = canvasEntity.AddComponent<UICanvasComponent>();
            canvas.m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
            canvas.m_SortOrder = kSubtitleCanvasSortOrder;

            auto& rect = canvasEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.0f, 0.0f };
            rect.m_AnchorMax = { 1.0f, 1.0f };

            m_CanvasEntity = canvasEntity.GetUUID();
        }

        Entity panelEntity = scene.CreateEntity("SubtitlePanel");
        if (!panelEntity)
        {
            DestroyEntities(scene);
            return false;
        }
        {
            auto& rect = panelEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = kPanelAnchorMin;
            rect.m_AnchorMax = kPanelAnchorMax;

            auto& panel = panelEntity.AddComponent<UIPanelComponent>();
            panel.m_BackgroundColor = { 0.0f, 0.0f, 0.0f, 0.0f };

            auto& rel = panelEntity.AddComponent<RelationshipComponent>();
            rel.m_ParentHandle = m_CanvasEntity;
            if (!canvasEntity.HasComponent<RelationshipComponent>())
                canvasEntity.AddComponent<RelationshipComponent>();
            canvasEntity.GetComponent<RelationshipComponent>().m_Children.push_back(panelEntity.GetUUID());

            m_PanelEntity = panelEntity.GetUUID();
        }

        Entity textEntity = scene.CreateEntity("SubtitleText");
        if (!textEntity)
        {
            DestroyEntities(scene);
            return false;
        }
        {
            auto& rect = textEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.02f, 0.05f };
            rect.m_AnchorMax = { 0.98f, 0.95f };

            auto& text = textEntity.AddComponent<UITextComponent>();
            text.m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
            text.m_Alignment = UITextAlignment::MiddleCenter;

            auto& rel = textEntity.AddComponent<RelationshipComponent>();
            rel.m_ParentHandle = m_PanelEntity;
            panelEntity.GetComponent<RelationshipComponent>().m_Children.push_back(textEntity.GetUUID());

            m_TextEntity = textEntity.GetUUID();
        }

        return true;
    }

    void SubtitleSystem::SetVisible(Scene& scene, bool visible)
    {
        // Hiding matches DialogueUIController: push the canvas behind everything
        // and zero the plate alpha, rather than destroying entities. Churning
        // three entities per line would be a structural registry change every
        // time a character draws breath.
        if (static_cast<u64>(m_CanvasEntity) != 0)
        {
            if (Entity canvasEnt = scene.GetEntityByUUID(m_CanvasEntity);
                canvasEnt && canvasEnt.HasComponent<UICanvasComponent>())
            {
                canvasEnt.GetComponent<UICanvasComponent>().m_SortOrder =
                    visible ? kSubtitleCanvasSortOrder : kHiddenCanvasSortOrder;
            }
        }

        if (static_cast<u64>(m_PanelEntity) != 0)
        {
            if (Entity panelEnt = scene.GetEntityByUUID(m_PanelEntity);
                panelEnt && panelEnt.HasComponent<UIPanelComponent>())
            {
                const f32 alpha = visible ? Accessibility::Get().SubtitleBackgroundOpacity : 0.0f;
                panelEnt.GetComponent<UIPanelComponent>().m_BackgroundColor = { 0.0f, 0.0f, 0.0f, alpha };
            }
        }

        m_IsVisible = visible;
    }

    void SubtitleSystem::PublishText(Scene& scene, const std::string& text)
    {
        if (static_cast<u64>(m_TextEntity) == 0)
            return;

        Entity textEnt = scene.GetEntityByUUID(m_TextEntity);
        if (!textEnt || !textEnt.HasComponent<UITextComponent>())
            return;

        auto& textComp = textEnt.GetComponent<UITextComponent>();
        textComp.m_Text = text;
        // The AUTHORED size only. UIRenderer multiplies the global text scale on
        // top at draw time, so the two accessibility settings compose instead of
        // one silently overriding the other.
        textComp.m_FontSize = Accessibility::Get().SubtitleFontSize;
        m_VisibleText = text;
    }

    void SubtitleSystem::Update(Scene& scene, Timestep ts)
    {
        OLO_PROFILE_FUNCTION();

        const AccessibilitySettings& settings = Accessibility::Get();

        // Age the pushed caption regardless of the toggle, so turning subtitles
        // back on mid-line does not resurrect one that should already have
        // expired.
        if (m_PendingSecondsRemaining > 0.0f)
        {
            const f32 dt = ts.GetSeconds();
            m_PendingSecondsRemaining -= std::isfinite(dt) ? dt : 0.0f;
            if (m_PendingSecondsRemaining <= 0.0f)
                ClearCaption();
        }

        if (!settings.SubtitlesEnabled)
        {
            if (m_IsVisible)
            {
                SetVisible(scene, false);
                PublishText(scene, {});
            }
            m_VisibleText.clear();
            return;
        }

        // --- Choose the source ---
        std::string caption;
        if (!m_PendingText.empty() && m_PendingSecondsRemaining > 0.0f)
        {
            caption = ComposeCaption(m_PendingText, m_PendingSpeaker, settings.SubtitleShowSpeaker);
        }
        else
        {
            for (auto view = scene.GetAllEntitiesWith<DialogueStateComponent>(); auto e : view)
            {
                const auto& state = view.get<DialogueStateComponent>(e);
                if (state.m_State == DialogueState::Inactive || state.m_CurrentText.empty())
                    continue;

                // The FULL line, not the typewriter-revealed prefix. A caption
                // exists so the line can be read at the reader's own pace; gating
                // it on m_TextRevealProgress would make the accessibility path
                // slower than the audio it stands in for.
                caption = ComposeCaption(state.m_CurrentText, state.m_CurrentSpeaker, settings.SubtitleShowSpeaker);
                break;
            }
        }

        if (caption.empty())
        {
            if (m_IsVisible)
            {
                SetVisible(scene, false);
                PublishText(scene, {});
            }
            m_VisibleText.clear();
            return;
        }

        if (!EnsureEntities(scene))
            return;

        PublishText(scene, caption);
        SetVisible(scene, true);
    }
} // namespace OloEngine
