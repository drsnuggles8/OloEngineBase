#include "OloEnginePCH.h"
#include "OloEngine/Dialogue/DialogueUIController.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"

namespace OloEngine
{
    void DialogueUIController::Initialize(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        // Create the dialogue UI entity hierarchy programmatically

        // Canvas entity — root of the dialogue UI
        {
            Entity canvasEntity = scene.CreateEntity("DialogueCanvas");
            auto& canvas = canvasEntity.AddComponent<UICanvasComponent>();
            canvas.m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
            canvas.m_SortOrder = 100;

            auto& rect = canvasEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.0f, 0.0f };
            rect.m_AnchorMax = { 1.0f, 1.0f };

            m_CanvasEntity = static_cast<entt::entity>(canvasEntity);
        }

        // Panel entity — dialogue box background
        {
            Entity panelEntity = scene.CreateEntity("DialoguePanel");
            auto& rect = panelEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.1f, 0.0f };
            rect.m_AnchorMax = { 0.9f, 0.25f };
            rect.m_AnchoredPosition = { 0.0f, 20.0f };

            auto& panel = panelEntity.AddComponent<UIPanelComponent>();
            panel.m_BackgroundColor = { 0.1f, 0.1f, 0.15f, 0.85f };

            // Parent to canvas
            auto& rel = panelEntity.AddComponent<RelationshipComponent>();
            if (m_CanvasEntity != entt::null)
            {
                Entity canvasEnt{ m_CanvasEntity, &scene };
                rel.m_ParentHandle = canvasEnt.GetUUID();
                if (!canvasEnt.HasComponent<RelationshipComponent>())
                    canvasEnt.AddComponent<RelationshipComponent>();
                canvasEnt.GetComponent<RelationshipComponent>().m_Children.push_back(panelEntity.GetUUID());
            }

            m_PanelEntity = static_cast<entt::entity>(panelEntity);
        }

        // Speaker name entity
        {
            Entity speakerEntity = scene.CreateEntity("DialogueSpeakerName");
            auto& rect = speakerEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.05f, 0.7f };
            rect.m_AnchorMax = { 0.5f, 0.95f };

            auto& text = speakerEntity.AddComponent<UITextComponent>();
            text.m_FontSize = 28.0f;
            text.m_Color = { 1.0f, 0.85f, 0.4f, 1.0f }; // Gold color for speaker name
            text.m_Alignment = UITextAlignment::MiddleLeft;

            // Parent to panel
            auto& rel = speakerEntity.AddComponent<RelationshipComponent>();
            if (m_PanelEntity != entt::null)
            {
                Entity panelEnt{ m_PanelEntity, &scene };
                rel.m_ParentHandle = panelEnt.GetUUID();
                panelEnt.GetComponent<RelationshipComponent>().m_Children.push_back(speakerEntity.GetUUID());
            }

            m_SpeakerNameEntity = static_cast<entt::entity>(speakerEntity);
        }

        // Dialogue body text entity
        {
            Entity bodyEntity = scene.CreateEntity("DialogueBody");
            auto& rect = bodyEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.05f, 0.05f };
            rect.m_AnchorMax = { 0.95f, 0.65f };

            auto& text = bodyEntity.AddComponent<UITextComponent>();
            text.m_FontSize = 22.0f;
            text.m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
            text.m_Alignment = UITextAlignment::TopLeft;

            // Parent to panel
            auto& rel = bodyEntity.AddComponent<RelationshipComponent>();
            if (m_PanelEntity != entt::null)
            {
                Entity panelEnt{ m_PanelEntity, &scene };
                rel.m_ParentHandle = panelEnt.GetUUID();
                panelEnt.GetComponent<RelationshipComponent>().m_Children.push_back(bodyEntity.GetUUID());
            }

            m_DialogueBodyEntity = static_cast<entt::entity>(bodyEntity);
        }

        HideDialogueBox(scene);
    }

    void DialogueUIController::Shutdown(Scene& scene)
    {
        ClearChoiceEntities(scene);

        auto destroyIfValid = [&](entt::entity& handle)
        {
            if (handle != entt::null)
            {
                Entity ent{ handle, &scene };
                scene.DestroyEntity(ent);
                handle = entt::null;
            }
        };

        destroyIfValid(m_DialogueBodyEntity);
        destroyIfValid(m_SpeakerNameEntity);
        destroyIfValid(m_PortraitEntity);
        destroyIfValid(m_PanelEntity);
        destroyIfValid(m_CanvasEntity);

        m_IsVisible = false;
        m_ActiveNpcEntity = entt::null;
    }

    void DialogueUIController::Update(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        // Find the first entity with an active DialogueStateComponent
        entt::entity activeEntity = entt::null;
        {
            auto view = scene.GetAllEntitiesWith<DialogueStateComponent>();
            for (auto e : view)
            {
                auto& state = view.get<DialogueStateComponent>(e);
                if (state.State != DialogueState::Inactive)
                {
                    activeEntity = e;
                    break;
                }
            }
        }

        if (activeEntity == entt::null)
        {
            if (m_IsVisible)
            {
                HideDialogueBox(scene);
            }
            return;
        }

        m_ActiveNpcEntity = activeEntity;
        Entity npcEntity{ activeEntity, &scene };
        auto& state = npcEntity.GetComponent<DialogueStateComponent>();

        if (!m_IsVisible)
        {
            ShowDialogueBox(scene);
        }

        // Update speaker name
        if (m_SpeakerNameEntity != entt::null)
        {
            Entity speakerEnt{ m_SpeakerNameEntity, &scene };
            if (speakerEnt.HasComponent<UITextComponent>())
            {
                speakerEnt.GetComponent<UITextComponent>().m_Text = state.CurrentSpeaker;
            }
        }

        // Update dialogue body with typewriter effect
        if (m_DialogueBodyEntity != entt::null)
        {
            Entity bodyEnt{ m_DialogueBodyEntity, &scene };
            if (bodyEnt.HasComponent<UITextComponent>())
            {
                if (state.TextRevealProgress >= 1.0f || state.CurrentText.empty())
                {
                    bodyEnt.GetComponent<UITextComponent>().m_Text = state.CurrentText;
                }
                else
                {
                    auto charCount = static_cast<size_t>(
                        state.TextRevealProgress * static_cast<f32>(state.CurrentText.size()));
                    bodyEnt.GetComponent<UITextComponent>().m_Text = state.CurrentText.substr(0, charCount);
                }
            }
        }

        // Update choices
        if (state.State == DialogueState::WaitingForChoice)
        {
            UpdateChoiceEntities(scene);
        }
        else
        {
            ClearChoiceEntities(scene);
        }

        // Handle input for advancing dialogue
        if (state.State == DialogueState::Displaying)
        {
            if (Input::IsKeyPressed(Key::Space) || Input::IsKeyPressed(Key::Enter))
            {
                auto* dialogueSystem = scene.GetDialogueSystem();
                if (dialogueSystem)
                {
                    dialogueSystem->AdvanceDialogue(npcEntity);
                }
            }
        }
    }

    void DialogueUIController::ShowDialogueBox(Scene& scene)
    {
        // Make canvas visible by ensuring it has a UICanvasComponent
        if (m_CanvasEntity != entt::null)
        {
            Entity canvasEnt{ m_CanvasEntity, &scene };
            if (canvasEnt.HasComponent<UICanvasComponent>())
            {
                canvasEnt.GetComponent<UICanvasComponent>().m_SortOrder = 100;
            }
        }
        m_IsVisible = true;
    }

    void DialogueUIController::HideDialogueBox(Scene& scene)
    {
        // Hide by setting sort order to a negative value (below render threshold)
        if (m_CanvasEntity != entt::null)
        {
            Entity canvasEnt{ m_CanvasEntity, &scene };
            if (canvasEnt.HasComponent<UICanvasComponent>())
            {
                canvasEnt.GetComponent<UICanvasComponent>().m_SortOrder = -9999;
            }
        }
        m_IsVisible = false;
        m_ActiveNpcEntity = entt::null;
    }

    void DialogueUIController::UpdateChoiceEntities(Scene& scene)
    {
        Entity npcEntity{ m_ActiveNpcEntity, &scene };
        if (!npcEntity.HasComponent<DialogueStateComponent>())
            return;

        auto& state = npcEntity.GetComponent<DialogueStateComponent>();

        // Ensure we have the right number of choice entities
        while (m_ChoiceEntities.size() < state.AvailableChoices.size())
        {
            Entity choiceEntity = scene.CreateEntity("DialogueChoice_" + std::to_string(m_ChoiceEntities.size()));
            auto& rect = choiceEntity.AddComponent<UIRectTransformComponent>();
            f32 const index = static_cast<f32>(m_ChoiceEntities.size());
            rect.m_AnchorMin = { 0.1f, 0.0f };
            rect.m_AnchorMax = { 0.9f, 0.0f };
            rect.m_AnchoredPosition = { 0.0f, -30.0f - index * 35.0f };
            rect.m_SizeDelta = { 0.0f, 30.0f };

            auto& btn = choiceEntity.AddComponent<UIButtonComponent>();
            btn.m_NormalColor = { 0.15f, 0.15f, 0.2f, 0.9f };
            btn.m_HoveredColor = { 0.25f, 0.25f, 0.35f, 0.95f };

            auto& text = choiceEntity.AddComponent<UITextComponent>();
            text.m_FontSize = 20.0f;
            text.m_Alignment = UITextAlignment::MiddleLeft;

            // Parent to panel
            auto& rel = choiceEntity.AddComponent<RelationshipComponent>();
            if (m_PanelEntity != entt::null)
            {
                Entity panelEnt{ m_PanelEntity, &scene };
                rel.m_ParentHandle = panelEnt.GetUUID();
                panelEnt.GetComponent<RelationshipComponent>().m_Children.push_back(choiceEntity.GetUUID());
            }

            m_ChoiceEntities.push_back(static_cast<entt::entity>(choiceEntity));
        }

        // Update text and state for each choice
        for (size_t i = 0; i < state.AvailableChoices.size(); ++i)
        {
            Entity choiceEnt{ m_ChoiceEntities[i], &scene };
            if (choiceEnt.HasComponent<UITextComponent>())
            {
                choiceEnt.GetComponent<UITextComponent>().m_Text = state.AvailableChoices[i].Text;
            }
            if (choiceEnt.HasComponent<UIButtonComponent>())
            {
                auto& btn = choiceEnt.GetComponent<UIButtonComponent>();
                btn.m_State = (static_cast<i32>(i) == state.HoveredChoiceIndex) ? UIButtonState::Hovered : UIButtonState::Normal;
            }
        }

        // Hide excess choice entities
        for (size_t i = state.AvailableChoices.size(); i < m_ChoiceEntities.size(); ++i)
        {
            Entity choiceEnt{ m_ChoiceEntities[i], &scene };
            if (choiceEnt.HasComponent<UITextComponent>())
            {
                choiceEnt.GetComponent<UITextComponent>().m_Text.clear();
            }
        }
    }

    void DialogueUIController::ClearChoiceEntities(Scene& scene)
    {
        for (auto e : m_ChoiceEntities)
        {
            Entity ent{ e, &scene };
            scene.DestroyEntity(ent);
        }
        m_ChoiceEntities.clear();
    }

} // namespace OloEngine
