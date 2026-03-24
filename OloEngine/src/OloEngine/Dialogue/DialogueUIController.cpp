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
    static constexpr f32 kDialoguePanelAlpha = 0.85f;

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

            m_CanvasEntity = canvasEntity.GetUUID();
        }

        // Panel entity — dialogue box background
        {
            Entity panelEntity = scene.CreateEntity("DialoguePanel");
            auto& rect = panelEntity.AddComponent<UIRectTransformComponent>();
            rect.m_AnchorMin = { 0.1f, 0.0f };
            rect.m_AnchorMax = { 0.9f, 0.25f };
            rect.m_AnchoredPosition = { 0.0f, 20.0f };

            auto& panel = panelEntity.AddComponent<UIPanelComponent>();
            panel.m_BackgroundColor = { 0.1f, 0.1f, 0.15f, kDialoguePanelAlpha };

            // Parent to canvas
            auto& rel = panelEntity.AddComponent<RelationshipComponent>();
            if (static_cast<u64>(m_CanvasEntity) != 0)
            {
                Entity canvasEnt = scene.GetEntityByUUID(m_CanvasEntity);
                if (canvasEnt)
                {
                    rel.m_ParentHandle = canvasEnt.GetUUID();
                    if (!canvasEnt.HasComponent<RelationshipComponent>())
                        canvasEnt.AddComponent<RelationshipComponent>();
                    canvasEnt.GetComponent<RelationshipComponent>().m_Children.push_back(panelEntity.GetUUID());
                }
            }

            m_PanelEntity = panelEntity.GetUUID();
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
            if (static_cast<u64>(m_PanelEntity) != 0)
            {
                Entity panelEnt = scene.GetEntityByUUID(m_PanelEntity);
                if (panelEnt)
                {
                    rel.m_ParentHandle = panelEnt.GetUUID();
                    panelEnt.GetComponent<RelationshipComponent>().m_Children.push_back(speakerEntity.GetUUID());
                }
            }

            m_SpeakerNameEntity = speakerEntity.GetUUID();
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
            if (static_cast<u64>(m_PanelEntity) != 0)
            {
                Entity panelEnt = scene.GetEntityByUUID(m_PanelEntity);
                if (panelEnt)
                {
                    rel.m_ParentHandle = panelEnt.GetUUID();
                    panelEnt.GetComponent<RelationshipComponent>().m_Children.push_back(bodyEntity.GetUUID());
                }
            }

            m_DialogueBodyEntity = bodyEntity.GetUUID();
        }

        HideDialogueBox(scene);
    }

    void DialogueUIController::Shutdown(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        ClearChoiceEntities(scene);

        auto destroyIfValid = [&](UUID& uuid)
        {
            if (static_cast<u64>(uuid) != 0)
            {
                Entity ent = scene.GetEntityByUUID(uuid);
                if (ent)
                    scene.DestroyEntity(ent);
                uuid = 0;
            }
        };

        destroyIfValid(m_DialogueBodyEntity);
        destroyIfValid(m_SpeakerNameEntity);
        destroyIfValid(m_PortraitEntity);
        destroyIfValid(m_PanelEntity);
        destroyIfValid(m_CanvasEntity);

        m_IsVisible = false;
        m_ActiveNpcEntity = 0;
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
                if (state.m_State != DialogueState::Inactive)
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
                ClearChoiceEntities(scene);
                HideDialogueBox(scene);
            }
            m_AdvanceKeyWasPressed = false;
            return;
        }

        Entity npcEntity{ activeEntity, &scene };
        m_ActiveNpcEntity = npcEntity.GetUUID();
        auto& state = npcEntity.GetComponent<DialogueStateComponent>();

        if (!m_IsVisible)
        {
            ShowDialogueBox(scene);
        }

        // Update speaker name
        if (static_cast<u64>(m_SpeakerNameEntity) != 0)
        {
            Entity speakerEnt = scene.GetEntityByUUID(m_SpeakerNameEntity);
            if (speakerEnt && speakerEnt.HasComponent<UITextComponent>())
            {
                speakerEnt.GetComponent<UITextComponent>().m_Text = state.m_CurrentSpeaker;
            }
        }

        // Update dialogue body with typewriter effect
        if (static_cast<u64>(m_DialogueBodyEntity) != 0)
        {
            Entity bodyEnt = scene.GetEntityByUUID(m_DialogueBodyEntity);
            if (bodyEnt && bodyEnt.HasComponent<UITextComponent>())
            {
                if (state.m_TextRevealProgress >= 1.0f || state.m_CurrentText.empty())
                {
                    bodyEnt.GetComponent<UITextComponent>().m_Text = state.m_CurrentText;
                }
                else
                {
                    // Compute byte offset that doesn't split a UTF-8 code point
                    auto targetBytes = static_cast<size_t>(
                        state.m_TextRevealProgress * static_cast<f32>(state.m_CurrentText.size()));
                    // Walk back if we're in the middle of a multi-byte sequence
                    while (targetBytes > 0 && targetBytes < state.m_CurrentText.size() && (static_cast<unsigned char>(state.m_CurrentText[targetBytes]) & 0xC0u) == 0x80u)
                    {
                        --targetBytes;
                    }
                    bodyEnt.GetComponent<UITextComponent>().m_Text = state.m_CurrentText.substr(0, targetBytes);
                }
            }
        }

        // Update choices
        if (state.m_State == DialogueState::WaitingForChoice)
        {
            UpdateChoiceEntities(scene);
        }
        else
        {
            ClearChoiceEntities(scene);
        }

        // Handle input for advancing dialogue (rising-edge detection)
        bool keyPressed = Input::IsKeyPressed(Key::Space) || Input::IsKeyPressed(Key::Enter);
        if (state.m_State == DialogueState::Displaying)
        {
            if (keyPressed && !m_AdvanceKeyWasPressed)
            {
                auto* dialogueSystem = scene.GetDialogueSystem();
                if (dialogueSystem)
                {
                    dialogueSystem->AdvanceDialogue(npcEntity);
                }
            }
        }
        else if (state.m_State == DialogueState::WaitingForChoice)
        {
            // Arrow keys to navigate choices
            i32 const choiceCount = static_cast<i32>(state.m_AvailableChoices.size());
            if (choiceCount > 0)
            {
                if (Input::IsKeyPressed(Key::Up) && !m_ArrowKeyWasPressed)
                {
                    state.m_HoveredChoiceIndex = (state.m_HoveredChoiceIndex <= 0)
                                                     ? choiceCount - 1
                                                     : state.m_HoveredChoiceIndex - 1;
                }
                if (Input::IsKeyPressed(Key::Down) && !m_ArrowKeyWasPressed)
                {
                    state.m_HoveredChoiceIndex = (state.m_HoveredChoiceIndex + 1) % choiceCount;
                }
                // Auto-select first choice if none hovered
                if (state.m_HoveredChoiceIndex < 0)
                    state.m_HoveredChoiceIndex = 0;
            }

            if (keyPressed && !m_AdvanceKeyWasPressed && state.m_HoveredChoiceIndex >= 0)
            {
                auto* dialogueSystem = scene.GetDialogueSystem();
                if (dialogueSystem)
                {
                    dialogueSystem->SelectChoice(npcEntity, state.m_HoveredChoiceIndex);
                }
            }
        }
        m_AdvanceKeyWasPressed = keyPressed;
        m_ArrowKeyWasPressed = Input::IsKeyPressed(Key::Up) || Input::IsKeyPressed(Key::Down);
    }

    void DialogueUIController::ShowDialogueBox(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        if (static_cast<u64>(m_CanvasEntity) != 0)
        {
            Entity canvasEnt = scene.GetEntityByUUID(m_CanvasEntity);
            if (canvasEnt && canvasEnt.HasComponent<UICanvasComponent>())
            {
                canvasEnt.GetComponent<UICanvasComponent>().m_SortOrder = 100;
            }
        }
        if (static_cast<u64>(m_PanelEntity) != 0)
        {
            Entity panelEnt = scene.GetEntityByUUID(m_PanelEntity);
            if (panelEnt && panelEnt.HasComponent<UIPanelComponent>())
            {
                panelEnt.GetComponent<UIPanelComponent>().m_BackgroundColor.a = kDialoguePanelAlpha;
            }
        }
        m_IsVisible = true;
    }

    void DialogueUIController::HideDialogueBox(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        if (static_cast<u64>(m_CanvasEntity) != 0)
        {
            Entity canvasEnt = scene.GetEntityByUUID(m_CanvasEntity);
            if (canvasEnt && canvasEnt.HasComponent<UICanvasComponent>())
            {
                canvasEnt.GetComponent<UICanvasComponent>().m_SortOrder = -9999;
            }
        }
        if (static_cast<u64>(m_PanelEntity) != 0)
        {
            Entity panelEnt = scene.GetEntityByUUID(m_PanelEntity);
            if (panelEnt && panelEnt.HasComponent<UIPanelComponent>())
            {
                panelEnt.GetComponent<UIPanelComponent>().m_BackgroundColor.a = 0.0f;
            }
        }
        m_IsVisible = false;
        m_ActiveNpcEntity = 0;
    }

    void DialogueUIController::UpdateChoiceEntities(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        Entity npcEntity = scene.GetEntityByUUID(m_ActiveNpcEntity);
        if (!npcEntity || !npcEntity.HasComponent<DialogueStateComponent>())
            return;

        auto& state = npcEntity.GetComponent<DialogueStateComponent>();

        // Ensure we have the right number of choice entities
        while (m_ChoiceEntities.size() < state.m_AvailableChoices.size())
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
            if (static_cast<u64>(m_PanelEntity) != 0)
            {
                Entity panelEnt = scene.GetEntityByUUID(m_PanelEntity);
                if (panelEnt)
                {
                    rel.m_ParentHandle = panelEnt.GetUUID();
                    if (!panelEnt.HasComponent<RelationshipComponent>())
                        panelEnt.AddComponent<RelationshipComponent>();
                    panelEnt.GetComponent<RelationshipComponent>().m_Children.push_back(choiceEntity.GetUUID());
                }
            }

            m_ChoiceEntities.push_back(choiceEntity.GetUUID());
        }

        // Update text and state for each choice
        for (size_t i = 0; i < state.m_AvailableChoices.size(); ++i)
        {
            Entity choiceEnt = scene.GetEntityByUUID(m_ChoiceEntities[i]);
            if (!choiceEnt)
                continue;
            if (choiceEnt.HasComponent<UITextComponent>())
            {
                choiceEnt.GetComponent<UITextComponent>().m_Text = state.m_AvailableChoices[i].Text;
            }
            if (choiceEnt.HasComponent<UIButtonComponent>())
            {
                auto& btn = choiceEnt.GetComponent<UIButtonComponent>();
                btn.m_State = (static_cast<i32>(i) == state.m_HoveredChoiceIndex) ? UIButtonState::Hovered : UIButtonState::Normal;
            }
        }

        // Destroy excess choice entities
        for (size_t i = state.m_AvailableChoices.size(); i < m_ChoiceEntities.size(); ++i)
        {
            Entity choiceEnt = scene.GetEntityByUUID(m_ChoiceEntities[i]);
            if (choiceEnt)
                scene.DestroyEntity(choiceEnt);
        }
        m_ChoiceEntities.resize(state.m_AvailableChoices.size());
    }

    void DialogueUIController::ClearChoiceEntities(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        for (auto uuid : m_ChoiceEntities)
        {
            Entity ent = scene.GetEntityByUUID(uuid);
            if (ent)
                scene.DestroyEntity(ent);
        }
        m_ChoiceEntities.clear();
    }

} // namespace OloEngine
