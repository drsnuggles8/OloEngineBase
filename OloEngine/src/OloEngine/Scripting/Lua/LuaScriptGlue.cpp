#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/SaveGame/SaveGameManager.h"

namespace OloEngine
{
    namespace Scripting
    {
        extern sol::state* GetState();
    }

    void LuaScriptGlue::RegisterAllTypes()
    {
        sol::state& lua = *Scripting::GetState();

        // --- GLM vector types (needed before components that use them) ---
        lua.new_usertype<glm::vec2>("vec2",
                                    sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>(),
                                    "x", &glm::vec2::x,
                                    "y", &glm::vec2::y);

        lua.new_usertype<glm::vec3>("vec3",
                                    sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
                                    "x", &glm::vec3::x,
                                    "y", &glm::vec3::y,
                                    "z", &glm::vec3::z);

        lua.new_usertype<glm::vec4>("vec4",
                                    sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(),
                                    "x", &glm::vec4::x,
                                    "y", &glm::vec4::y,
                                    "z", &glm::vec4::z,
                                    "w", &glm::vec4::w);

        // --- UICanvasComponent ---
        lua.new_usertype<UICanvasComponent>("UICanvasComponent",
                                            "renderMode", &UICanvasComponent::m_RenderMode,
                                            "scaleMode", &UICanvasComponent::m_ScaleMode,
                                            "sortOrder", &UICanvasComponent::m_SortOrder,
                                            "referenceResolution", &UICanvasComponent::m_ReferenceResolution);

        // --- UIRectTransformComponent ---
        lua.new_usertype<UIRectTransformComponent>("UIRectTransformComponent",
                                                   "anchorMin", &UIRectTransformComponent::m_AnchorMin,
                                                   "anchorMax", &UIRectTransformComponent::m_AnchorMax,
                                                   "anchoredPosition", &UIRectTransformComponent::m_AnchoredPosition,
                                                   "sizeDelta", &UIRectTransformComponent::m_SizeDelta,
                                                   "pivot", &UIRectTransformComponent::m_Pivot,
                                                   "rotation", &UIRectTransformComponent::m_Rotation,
                                                   "scale", &UIRectTransformComponent::m_Scale);

        // --- UIImageComponent ---
        lua.new_usertype<UIImageComponent>("UIImageComponent",
                                           "color", &UIImageComponent::m_Color,
                                           "borderInsets", &UIImageComponent::m_BorderInsets);

        // --- UIPanelComponent ---
        lua.new_usertype<UIPanelComponent>("UIPanelComponent",
                                           "backgroundColor", &UIPanelComponent::m_BackgroundColor);

        // --- UITextComponent ---
        lua.new_usertype<UITextComponent>("UITextComponent",
                                          "text", &UITextComponent::m_Text,
                                          "fontSize", &UITextComponent::m_FontSize,
                                          "color", &UITextComponent::m_Color,
                                          "alignment", &UITextComponent::m_Alignment,
                                          "kerning", &UITextComponent::m_Kerning,
                                          "lineSpacing", &UITextComponent::m_LineSpacing);

        // --- UIButtonComponent ---
        lua.new_usertype<UIButtonComponent>("UIButtonComponent",
                                            "normalColor", &UIButtonComponent::m_NormalColor,
                                            "hoveredColor", &UIButtonComponent::m_HoveredColor,
                                            "pressedColor", &UIButtonComponent::m_PressedColor,
                                            "disabledColor", &UIButtonComponent::m_DisabledColor,
                                            "interactable", &UIButtonComponent::m_Interactable,
                                            "state", sol::readonly(&UIButtonComponent::m_State));

        // --- UISliderComponent ---
        lua.new_usertype<UISliderComponent>("UISliderComponent",
                                            "value", &UISliderComponent::m_Value,
                                            "minValue", &UISliderComponent::m_MinValue,
                                            "maxValue", &UISliderComponent::m_MaxValue,
                                            "direction", &UISliderComponent::m_Direction,
                                            "backgroundColor", &UISliderComponent::m_BackgroundColor,
                                            "fillColor", &UISliderComponent::m_FillColor,
                                            "handleColor", &UISliderComponent::m_HandleColor,
                                            "interactable", &UISliderComponent::m_Interactable);

        // --- UICheckboxComponent ---
        lua.new_usertype<UICheckboxComponent>("UICheckboxComponent",
                                              "isChecked", &UICheckboxComponent::m_IsChecked,
                                              "uncheckedColor", &UICheckboxComponent::m_UncheckedColor,
                                              "checkedColor", &UICheckboxComponent::m_CheckedColor,
                                              "checkmarkColor", &UICheckboxComponent::m_CheckmarkColor,
                                              "interactable", &UICheckboxComponent::m_Interactable);

        // --- UIProgressBarComponent ---
        lua.new_usertype<UIProgressBarComponent>("UIProgressBarComponent",
                                                 "value", &UIProgressBarComponent::m_Value,
                                                 "minValue", &UIProgressBarComponent::m_MinValue,
                                                 "maxValue", &UIProgressBarComponent::m_MaxValue,
                                                 "fillMethod", &UIProgressBarComponent::m_FillMethod,
                                                 "backgroundColor", &UIProgressBarComponent::m_BackgroundColor,
                                                 "fillColor", &UIProgressBarComponent::m_FillColor);

        // --- UIInputFieldComponent ---
        lua.new_usertype<UIInputFieldComponent>("UIInputFieldComponent",
                                                "text", &UIInputFieldComponent::m_Text,
                                                "placeholder", &UIInputFieldComponent::m_Placeholder,
                                                "fontSize", &UIInputFieldComponent::m_FontSize,
                                                "textColor", &UIInputFieldComponent::m_TextColor,
                                                "placeholderColor", &UIInputFieldComponent::m_PlaceholderColor,
                                                "backgroundColor", &UIInputFieldComponent::m_BackgroundColor,
                                                "characterLimit", &UIInputFieldComponent::m_CharacterLimit,
                                                "interactable", &UIInputFieldComponent::m_Interactable);

        // --- UIScrollViewComponent ---
        lua.new_usertype<UIScrollViewComponent>("UIScrollViewComponent",
                                                "scrollPosition", &UIScrollViewComponent::m_ScrollPosition,
                                                "contentSize", &UIScrollViewComponent::m_ContentSize,
                                                "scrollDirection", &UIScrollViewComponent::m_ScrollDirection,
                                                "scrollSpeed", &UIScrollViewComponent::m_ScrollSpeed,
                                                "showHorizontalScrollbar", &UIScrollViewComponent::m_ShowHorizontalScrollbar,
                                                "showVerticalScrollbar", &UIScrollViewComponent::m_ShowVerticalScrollbar,
                                                "scrollbarColor", &UIScrollViewComponent::m_ScrollbarColor,
                                                "scrollbarTrackColor", &UIScrollViewComponent::m_ScrollbarTrackColor);

        // --- UIDropdownComponent ---
        lua.new_usertype<UIDropdownComponent>("UIDropdownComponent",
                                              "selectedIndex", &UIDropdownComponent::m_SelectedIndex,
                                              "backgroundColor", &UIDropdownComponent::m_BackgroundColor,
                                              "highlightColor", &UIDropdownComponent::m_HighlightColor,
                                              "textColor", &UIDropdownComponent::m_TextColor,
                                              "fontSize", &UIDropdownComponent::m_FontSize,
                                              "itemHeight", &UIDropdownComponent::m_ItemHeight,
                                              "interactable", &UIDropdownComponent::m_Interactable);

        // --- UIGridLayoutComponent ---
        lua.new_usertype<UIGridLayoutComponent>("UIGridLayoutComponent",
                                                "cellSize", &UIGridLayoutComponent::m_CellSize,
                                                "spacing", &UIGridLayoutComponent::m_Spacing,
                                                "padding", &UIGridLayoutComponent::m_Padding,
                                                "startCorner", &UIGridLayoutComponent::m_StartCorner,
                                                "startAxis", &UIGridLayoutComponent::m_StartAxis,
                                                "constraintCount", &UIGridLayoutComponent::m_ConstraintCount);

        // --- UIToggleComponent ---
        lua.new_usertype<UIToggleComponent>("UIToggleComponent",
                                            "isOn", &UIToggleComponent::m_IsOn,
                                            "offColor", &UIToggleComponent::m_OffColor,
                                            "onColor", &UIToggleComponent::m_OnColor,
                                            "knobColor", &UIToggleComponent::m_KnobColor,
                                            "interactable", &UIToggleComponent::m_Interactable);

        // --- ParticleSystemComponent ---
        lua.new_usertype<ParticleSystem>("ParticleSystem",
                                         "playing", &ParticleSystem::Playing,
                                         "looping", &ParticleSystem::Looping,
                                         "duration", &ParticleSystem::Duration,
                                         "playbackSpeed", &ParticleSystem::PlaybackSpeed,
                                         "windInfluence", &ParticleSystem::WindInfluence,
                                         "getAliveCount", &ParticleSystem::GetAliveCount,
                                         "reset", &ParticleSystem::Reset);

        lua.new_usertype<ParticleEmitter>("ParticleEmitter",
                                          "rateOverTime", &ParticleEmitter::RateOverTime,
                                          "initialSpeed", &ParticleEmitter::InitialSpeed,
                                          "speedVariance", &ParticleEmitter::SpeedVariance,
                                          "lifetimeMin", &ParticleEmitter::LifetimeMin,
                                          "lifetimeMax", &ParticleEmitter::LifetimeMax,
                                          "initialSize", &ParticleEmitter::InitialSize,
                                          "sizeVariance", &ParticleEmitter::SizeVariance,
                                          "initialColor", &ParticleEmitter::InitialColor);

        lua.new_usertype<ParticleSystemComponent>("ParticleSystemComponent",
                                                  "system", &ParticleSystemComponent::System);

        // --- LightProbeComponent ---
        lua.new_usertype<LightProbeComponent>("LightProbeComponent",
                                              "influenceRadius", &LightProbeComponent::m_InfluenceRadius,
                                              "intensity", &LightProbeComponent::m_Intensity,
                                              "active", &LightProbeComponent::m_Active);

        // --- LightProbeVolumeComponent ---
        lua.new_usertype<LightProbeVolumeComponent>("LightProbeVolumeComponent",
                                                    "boundsMin", &LightProbeVolumeComponent::m_BoundsMin,
                                                    "boundsMax", &LightProbeVolumeComponent::m_BoundsMax,
                                                    "spacing", &LightProbeVolumeComponent::m_Spacing,
                                                    "intensity", &LightProbeVolumeComponent::m_Intensity,
                                                    "active", &LightProbeVolumeComponent::m_Active,
                                                    "dirty", &LightProbeVolumeComponent::m_Dirty,
                                                    "getTotalProbeCount", &LightProbeVolumeComponent::GetTotalProbeCount);

        // --- WindSettings (scene-level) ---
        lua.new_usertype<WindSettings>("WindSettings",
                                       "enabled", &WindSettings::Enabled,
                                       "direction", &WindSettings::Direction,
                                       "speed", &WindSettings::Speed,
                                       "gustStrength", &WindSettings::GustStrength,
                                       "gustFrequency", &WindSettings::GustFrequency,
                                       "turbulenceIntensity", &WindSettings::TurbulenceIntensity,
                                       "turbulenceScale", &WindSettings::TurbulenceScale,
                                       "gridWorldSize", &WindSettings::GridWorldSize,
                                       "gridResolution", &WindSettings::GridResolution);

        // --- StreamingVolumeComponent ---
        lua.new_usertype<StreamingVolumeComponent>("StreamingVolumeComponent",
                                                   "loadRadius", &StreamingVolumeComponent::LoadRadius,
                                                   "unloadRadius", &StreamingVolumeComponent::UnloadRadius,
                                                   "isLoaded", sol::readonly(&StreamingVolumeComponent::IsLoaded));

        // --- StreamingSettings (scene-level) ---
        lua.new_usertype<StreamingSettings>("StreamingSettings",
                                            "enabled", &StreamingSettings::Enabled,
                                            "defaultLoadRadius", &StreamingSettings::DefaultLoadRadius,
                                            "defaultUnloadRadius", &StreamingSettings::DefaultUnloadRadius,
                                            "maxLoadedRegions", &StreamingSettings::MaxLoadedRegions,
                                            "regionDirectory", &StreamingSettings::RegionDirectory);

        // --- NetworkIdentityComponent ---
        lua.new_usertype<NetworkIdentityComponent>("NetworkIdentityComponent",
                                                   "ownerClientID", &NetworkIdentityComponent::OwnerClientID,
                                                   "authority", &NetworkIdentityComponent::Authority,
                                                   "isReplicated", &NetworkIdentityComponent::IsReplicated);

        // --- NetworkManager (static functions as table) ---
        auto networkTable = lua.create_named_table("Network");
        networkTable.set_function("isServer", &NetworkManager::IsServer);
        networkTable.set_function("isClient", &NetworkManager::IsClient);
        networkTable.set_function("isConnected", &NetworkManager::IsConnected);
        networkTable.set_function("connect", &NetworkManager::Connect);
        networkTable.set_function("disconnect", &NetworkManager::Disconnect);
        networkTable.set_function("startServer", &NetworkManager::StartServer);
        networkTable.set_function("stopServer", &NetworkManager::StopServer);

        // --- Input (raw + action mapping) ---
        auto inputTable = lua.create_named_table("Input");
        inputTable["IsKeyDown"] = [](u16 keycode)
        {
            return Input::IsKeyPressed(keycode);
        };
        inputTable["IsMouseButtonDown"] = [](u16 button)
        {
            return Input::IsMouseButtonPressed(button);
        };
        inputTable["IsActionPressed"] = [](const std::string& name)
        {
            return InputActionManager::IsActionPressed(name);
        };
        inputTable["IsActionJustPressed"] = [](const std::string& name)
        {
            return InputActionManager::IsActionJustPressed(name);
        };
        inputTable["IsActionJustReleased"] = [](const std::string& name)
        {
            return InputActionManager::IsActionJustReleased(name);
        };

        // --- DialogueComponent ---
        lua.new_usertype<DialogueComponent>("DialogueComponent",
                                            "dialogueTree", &DialogueComponent::m_DialogueTree,
                                            "autoTrigger", &DialogueComponent::m_AutoTrigger,
                                            "triggerRadius", &DialogueComponent::m_TriggerRadius,
                                            "hasTriggered", &DialogueComponent::m_HasTriggered,
                                            "triggerOnce", &DialogueComponent::m_TriggerOnce);

        // --- Dialogue system functions ---
        auto dialogueTable = lua.create_named_table("dialogue");
        dialogueTable["start"] = [](Entity* entity)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->StartDialogue(*entity);
        };
        dialogueTable["advance"] = [](Entity* entity)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->AdvanceDialogue(*entity);
        };
        dialogueTable["select_choice"] = [](Entity* entity, i32 index)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->SelectChoice(*entity, index);
        };
        dialogueTable["is_active"] = [](Entity* entity) -> bool
        {
            if (!entity)
                return false;
            return entity->HasComponent<DialogueStateComponent>();
        };
        dialogueTable["end_dialogue"] = [](Entity* entity)
        {
            if (!entity)
                return;
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->EndDialogue(*entity);
        };

        // --- Dialogue variables ---
        auto varsTable = lua.create_named_table("dialogue_vars");
        varsTable["get_bool"] = [](const std::string& key) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;
            return scene->GetDialogueVariables().GetBool(key);
        };
        varsTable["set_bool"] = [](const std::string& key, bool val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetBool(key, val);
        };
        varsTable["get_int"] = [](const std::string& key) -> i32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return 0;
            return scene->GetDialogueVariables().GetInt(key);
        };
        varsTable["set_int"] = [](const std::string& key, i32 val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetInt(key, val);
        };
        varsTable["get_float"] = [](const std::string& key) -> f32
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return 0.0f;
            return scene->GetDialogueVariables().GetFloat(key);
        };
        varsTable["set_float"] = [](const std::string& key, f32 val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetFloat(key, val);
        };
        varsTable["get_string"] = [](const std::string& key) -> std::string
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return "";
            return scene->GetDialogueVariables().GetString(key);
        };
        varsTable["set_string"] = [](const std::string& key, const std::string& val)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().SetString(key, val);
        };
        varsTable["has"] = [](const std::string& key) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return false;
            return scene->GetDialogueVariables().Has(key);
        };
        varsTable["clear"] = []()
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            if (scene)
                scene->GetDialogueVariables().Clear();
        };

        // --- SaveGame ---
        auto saveGameTable = lua.create_named_table("SaveGame");
        saveGameTable["EnumerateSaves"] = [&lua]() -> sol::table
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::EnumerateSaves");
            auto saves = SaveGameManager::EnumerateSaves();
            sol::table result = lua.create_table(static_cast<int>(saves.size()), 0);
            int index = 1;
            for (const auto& info : saves)
            {
                sol::table entry = lua.create_table(0, 3);
                entry["SlotName"] = info.FilePath.stem().string();
                entry["DisplayName"] = info.Metadata.DisplayName;
                entry["TimestampUTC"] = info.Metadata.TimestampUTC;
                result[index++] = entry;
            }
            return result;
        };
        saveGameTable["DeleteSave"] = [](const std::string& slotName)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::DeleteSave");
            return SaveGameManager::DeleteSave(slotName);
        };
        saveGameTable["ValidateSave"] = [](const std::string& slotName)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::ValidateSave");
            return SaveGameManager::ValidateSave(slotName);
        };
        saveGameTable["GetAutoSaveInterval"] = []()
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::GetAutoSaveInterval");
            return SaveGameManager::GetAutoSaveInterval();
        };
        saveGameTable["SetAutoSaveInterval"] = [](f32 interval)
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::SetAutoSaveInterval");
            SaveGameManager::SetAutoSaveInterval(interval);
        };
    }
} // namespace OloEngine
