#include "LuaScriptGlue.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"

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
        inputTable["GetActionAxisValue"] = [](const std::string& name)
        {
            return InputActionManager::GetActionAxisValue(name);
        };

        // --- Gamepad functions (raw access) ---
        {
            OLO_PROFILE_SCOPE("LuaScriptGlue::RegisterGamepad");
            auto gamepadTable = lua.create_named_table("Gamepad");
            gamepadTable["IsButtonPressed"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonPressed(static_cast<GamepadButton>(button));
            };
            gamepadTable["IsButtonJustPressed"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonJustPressed(static_cast<GamepadButton>(button));
            };
            gamepadTable["IsButtonJustReleased"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonJustReleased(static_cast<GamepadButton>(button));
            };
            gamepadTable["GetAxis"] = [](u8 axis, i32 index) -> f32
            {
                if (axis >= Gamepad::AxisCount)
                {
                    return 0.0f;
                }
                auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetAxis(static_cast<GamepadAxis>(axis)) : 0.0f;
            };
            gamepadTable["GetLeftStick"] = [](i32 index) -> glm::vec2
            {
                auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetLeftStickDeadzone() : glm::vec2(0.0f);
            };
            gamepadTable["GetRightStick"] = [](i32 index) -> glm::vec2
            {
                auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetRightStickDeadzone() : glm::vec2(0.0f);
            };
            gamepadTable["IsConnected"] = [](i32 index) -> bool
            {
                auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsConnected();
            };
            gamepadTable["GetConnectedCount"] = []() -> i32
            {
                return GamepadManager::GetConnectedCount();
            };

            // --- Gamepad button/axis enum constants ---
            auto gpButtonTable = lua.create_named_table("GamepadButton");
            gpButtonTable["South"] = static_cast<u8>(GamepadButton::South);
            gpButtonTable["East"] = static_cast<u8>(GamepadButton::East);
            gpButtonTable["West"] = static_cast<u8>(GamepadButton::West);
            gpButtonTable["North"] = static_cast<u8>(GamepadButton::North);
            gpButtonTable["LeftBumper"] = static_cast<u8>(GamepadButton::LeftBumper);
            gpButtonTable["RightBumper"] = static_cast<u8>(GamepadButton::RightBumper);
            gpButtonTable["Back"] = static_cast<u8>(GamepadButton::Back);
            gpButtonTable["Start"] = static_cast<u8>(GamepadButton::Start);
            gpButtonTable["Guide"] = static_cast<u8>(GamepadButton::Guide);
            gpButtonTable["LeftThumb"] = static_cast<u8>(GamepadButton::LeftThumb);
            gpButtonTable["RightThumb"] = static_cast<u8>(GamepadButton::RightThumb);
            gpButtonTable["DPadUp"] = static_cast<u8>(GamepadButton::DPadUp);
            gpButtonTable["DPadRight"] = static_cast<u8>(GamepadButton::DPadRight);
            gpButtonTable["DPadDown"] = static_cast<u8>(GamepadButton::DPadDown);
            gpButtonTable["DPadLeft"] = static_cast<u8>(GamepadButton::DPadLeft);

            auto gpAxisTable = lua.create_named_table("GamepadAxis");
            gpAxisTable["LeftX"] = static_cast<u8>(GamepadAxis::LeftX);
            gpAxisTable["LeftY"] = static_cast<u8>(GamepadAxis::LeftY);
            gpAxisTable["RightX"] = static_cast<u8>(GamepadAxis::RightX);
            gpAxisTable["RightY"] = static_cast<u8>(GamepadAxis::RightY);
            gpAxisTable["LeftTrigger"] = static_cast<u8>(GamepadAxis::LeftTrigger);
            gpAxisTable["RightTrigger"] = static_cast<u8>(GamepadAxis::RightTrigger);
        }

        // --- DialogueComponent ---
        lua.new_usertype<DialogueComponent>("DialogueComponent",
                                            "dialogueTree", &DialogueComponent::m_DialogueTree,
                                            "autoTrigger", &DialogueComponent::m_AutoTrigger,
                                            "triggerRadius", &DialogueComponent::m_TriggerRadius,
                                            "hasTriggered", &DialogueComponent::m_HasTriggered,
                                            "triggerOnce", &DialogueComponent::m_TriggerOnce);

        // --- AnimationGraphComponent ---
        lua.new_usertype<AnimationGraphComponent>("AnimationGraphComponent", "SetFloat", [](AnimationGraphComponent& comp, const std::string& name, f32 value)
                                                  { comp.Parameters.SetFloat(name, value); }, "SetBool", [](AnimationGraphComponent& comp, const std::string& name, bool value)
                                                  { comp.Parameters.SetBool(name, value); }, "SetInt", [](AnimationGraphComponent& comp, const std::string& name, i32 value)
                                                  { comp.Parameters.SetInt(name, value); }, "SetTrigger", [](AnimationGraphComponent& comp, const std::string& name)
                                                  { comp.Parameters.SetTrigger(name); }, "GetFloat", [](const AnimationGraphComponent& comp, const std::string& name) -> f32
                                                  { return comp.Parameters.GetFloat(name); }, "GetBool", [](const AnimationGraphComponent& comp, const std::string& name) -> bool
                                                  { return comp.Parameters.GetBool(name); }, "GetInt", [](const AnimationGraphComponent& comp, const std::string& name) -> i32
                                                  { return comp.Parameters.GetInt(name); }, "GetCurrentState", [](const AnimationGraphComponent& comp, sol::optional<i32> layerIndex) -> std::string
                                                  {
                                                       if (!comp.RuntimeGraph)
                                                           return "";
                                                       return std::string(comp.RuntimeGraph->GetCurrentStateName(layerIndex.value_or(0))); });

        // --- MorphTargetComponent ---
        lua.new_usertype<MorphTargetComponent>("MorphTargetComponent", "SetWeight", [](MorphTargetComponent& comp, const std::string& name, f32 weight)
                                               { comp.SetWeight(name, weight); }, "GetWeight", [](const MorphTargetComponent& comp, const std::string& name) -> f32
                                               { return comp.GetWeight(name); }, "ResetAll", &MorphTargetComponent::ResetAllWeights, "HasActiveWeights", &MorphTargetComponent::HasActiveWeights, "GetTargetCount", [](const MorphTargetComponent& comp) -> u32
                                               { return comp.MorphTargets ? comp.MorphTargets->GetTargetCount() : 0; }, "ApplyExpression", [](MorphTargetComponent& comp, const std::string& name, sol::optional<f32> blend)
                                               { FacialExpressionLibrary::ApplyExpression(comp, name, blend.value_or(1.0f)); });

        // --- MaterialComponent ---
        lua.new_usertype<MaterialComponent>("MaterialComponent",
                                            "shaderGraphHandle",
                                            sol::property(
                                                [](const MaterialComponent& mc) -> u64
                                                { return static_cast<u64>(mc.m_ShaderGraphHandle); },
                                                [](MaterialComponent& mc, u64 handle)
                                                {
                                                    if (handle != 0)
                                                    {
                                                        if (auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(handle))
                                                        {
                                                            if (auto shader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(handle)))
                                                            {
                                                                mc.m_ShaderGraphHandle = handle;
                                                                mc.m_Material.SetShader(shader);
                                                                return;
                                                            }
                                                        }
                                                        OLO_CORE_WARN("[Lua] Failed to compile ShaderGraph handle {}", handle);
                                                    }
                                                    else
                                                    {
                                                        mc.m_ShaderGraphHandle = 0;
                                                        mc.m_Material.SetShader(nullptr);
                                                    }
                                                }));

        // --- NavAgentComponent ---
        lua.new_usertype<NavAgentComponent>("NavAgentComponent",
                                            "radius", &NavAgentComponent::m_Radius,
                                            "height", &NavAgentComponent::m_Height,
                                            "maxSpeed", &NavAgentComponent::m_MaxSpeed,
                                            "acceleration", &NavAgentComponent::m_Acceleration,
                                            "stoppingDistance", &NavAgentComponent::m_StoppingDistance,
                                            "avoidancePriority", &NavAgentComponent::m_AvoidancePriority,
                                            "targetPosition", sol::property([](const NavAgentComponent& a)
                                                                            { return a.m_TargetPosition; }, [](NavAgentComponent& a, const glm::vec3& pos)
                                                                            {
                                                    a.m_TargetPosition = pos;
                                                    a.m_HasTarget = true;
                                                    a.m_HasPath = false;
                                                    a.m_PathCorners.clear();
                                                    a.m_CurrentCornerIndex = 0; }),
                                            "hasTarget", sol::readonly(&NavAgentComponent::m_HasTarget),
                                            "hasPath", sol::readonly(&NavAgentComponent::m_HasPath),
                                            "clearTarget", [](NavAgentComponent& agent)
                                            {
                                                agent.m_HasTarget = false;
                                                agent.m_HasPath = false;
                                                agent.m_PathCorners.clear();
                                                agent.m_CurrentCornerIndex = 0; });

        // --- NavMeshBoundsComponent ---
        lua.new_usertype<NavMeshBoundsComponent>("NavMeshBoundsComponent",
                                                 "min", &NavMeshBoundsComponent::m_Min,
                                                 "max", &NavMeshBoundsComponent::m_Max);

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
        saveGameTable["Save"] = [](const std::string& slotName, const std::string& displayName) -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::Save");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::Save(*scene, slotName, displayName));
        };
        saveGameTable["Load"] = [](const std::string& slotName) -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::Load");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::Load(*scene, slotName));
        };
        saveGameTable["QuickSave"] = []() -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::QuickSave");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::QuickSave(*scene));
        };
        saveGameTable["QuickLoad"] = []() -> i32
        {
            OLO_PROFILE_SCOPE("Lua::SaveGame::QuickLoad");
            Scene* scene = ScriptEngine::GetSceneContext();
            if (!scene)
                return static_cast<i32>(SaveLoadResult::NoActiveScene);
            return static_cast<i32>(SaveGameManager::QuickLoad(*scene));
        };
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

        // --- BehaviorTreeComponent ---
        OLO_PROFILE_SCOPE("Lua::RegisterAITypes");
        lua.new_usertype<BehaviorTreeComponent>("BehaviorTreeComponent", "SetBlackboardBool", [](BehaviorTreeComponent& comp, const std::string& key, bool value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const BehaviorTreeComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](BehaviorTreeComponent& comp, const std::string& key, i32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const BehaviorTreeComponent& comp, const std::string& key) -> i32
                                                { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](BehaviorTreeComponent& comp, const std::string& key, f32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const BehaviorTreeComponent& comp, const std::string& key) -> f32
                                                { return comp.Blackboard.Get<f32>(key); }, "SetBlackboardString", [](BehaviorTreeComponent& comp, const std::string& key, const std::string& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardString", [](const BehaviorTreeComponent& comp, const std::string& key) -> std::string
                                                { return comp.Blackboard.Get<std::string>(key); }, "SetBlackboardVec3", [](BehaviorTreeComponent& comp, const std::string& key, const glm::vec3& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardVec3", [](const BehaviorTreeComponent& comp, const std::string& key) -> glm::vec3
                                                { return comp.Blackboard.Get<glm::vec3>(key); }, "SetBlackboardUUID", [](BehaviorTreeComponent& comp, const std::string& key, u64 value)
                                                { comp.Blackboard.Set(key, UUID(value)); }, "GetBlackboardUUID", [](const BehaviorTreeComponent& comp, const std::string& key) -> u64
                                                { return static_cast<u64>(comp.Blackboard.Get<UUID>(key)); }, "RemoveBlackboardKey", [](BehaviorTreeComponent& comp, const std::string& key)
                                                { comp.Blackboard.Remove(key); }, "HasBlackboardKey", [](const BehaviorTreeComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Has(key); }, "IsRunning", sol::readonly(&BehaviorTreeComponent::IsRunning));

        // --- StateMachineComponent ---
        lua.new_usertype<StateMachineComponent>("StateMachineComponent", "SetBlackboardBool", [](StateMachineComponent& comp, const std::string& key, bool value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardBool", [](const StateMachineComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Get<bool>(key); }, "SetBlackboardInt", [](StateMachineComponent& comp, const std::string& key, i32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardInt", [](const StateMachineComponent& comp, const std::string& key) -> i32
                                                { return comp.Blackboard.Get<i32>(key); }, "SetBlackboardFloat", [](StateMachineComponent& comp, const std::string& key, f32 value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardFloat", [](const StateMachineComponent& comp, const std::string& key) -> f32
                                                { return comp.Blackboard.Get<f32>(key); }, "SetBlackboardString", [](StateMachineComponent& comp, const std::string& key, const std::string& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardString", [](const StateMachineComponent& comp, const std::string& key) -> std::string
                                                { return comp.Blackboard.Get<std::string>(key); }, "SetBlackboardVec3", [](StateMachineComponent& comp, const std::string& key, const glm::vec3& value)
                                                { comp.Blackboard.Set(key, value); }, "GetBlackboardVec3", [](const StateMachineComponent& comp, const std::string& key) -> glm::vec3
                                                { return comp.Blackboard.Get<glm::vec3>(key); }, "SetBlackboardUUID", [](StateMachineComponent& comp, const std::string& key, u64 value)
                                                { comp.Blackboard.Set(key, UUID(value)); }, "GetBlackboardUUID", [](const StateMachineComponent& comp, const std::string& key) -> u64
                                                { return static_cast<u64>(comp.Blackboard.Get<UUID>(key)); }, "RemoveBlackboardKey", [](StateMachineComponent& comp, const std::string& key)
                                                { comp.Blackboard.Remove(key); }, "HasBlackboardKey", [](const StateMachineComponent& comp, const std::string& key) -> bool
                                                { return comp.Blackboard.Has(key); }, "GetCurrentState", [](const StateMachineComponent& comp) -> std::string
                                                {
                if (comp.RuntimeFSM && comp.RuntimeFSM->IsStarted())
                    return comp.RuntimeFSM->GetCurrentStateID();
                return ""; }, "ForceTransition", [](StateMachineComponent& comp, Entity entity, const std::string& stateId)
                                                {
                if (comp.RuntimeFSM)
                    comp.RuntimeFSM->ForceTransition(stateId, entity, comp.Blackboard); });

        // --- InventoryComponent ---
        lua.new_usertype<InventoryComponent>("InventoryComponent", "currency", &InventoryComponent::Currency, "AddItem", [](InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                             {
                                                 i32 total = count.value_or(1);
                                                 if (total <= 0)
                                                     return false;
                                                 const auto* def = ItemDatabase::Get(itemId);
                                                 if (!def)
                                                     return false;
                                                 i32 maxStack = std::max(def->MaxStackSize, 1);
                                                 i32 remaining = total;
                                                 while (remaining > 0)
                                                 {
                                                     ItemInstance instance;
                                                     instance.InstanceID = UUID();
                                                     instance.ItemDefinitionID = itemId;
                                                     instance.StackCount = std::min(remaining, maxStack);
                                                     if (!comp.PlayerInventory.AddItem(instance))
                                                         return false;
                                                     remaining -= instance.StackCount;
                                                 }
                                                 return true; }, "RemoveItem", [](InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                             { return comp.PlayerInventory.RemoveItemByDefinition(itemId, count.value_or(1)); }, "HasItem", [](const InventoryComponent& comp, const std::string& itemId, sol::optional<i32> count) -> bool
                                             { return comp.PlayerInventory.HasItem(itemId, count.value_or(1)); }, "CountItem", [](const InventoryComponent& comp, const std::string& itemId) -> i32
                                             { return comp.PlayerInventory.CountItem(itemId); }, "GetUsedSlots", [](const InventoryComponent& comp) -> i32
                                             { return comp.PlayerInventory.GetUsedSlots(); }, "GetCapacity", [](const InventoryComponent& comp) -> i32
                                             { return comp.PlayerInventory.GetCapacity(); }, "GetTotalWeight", [](const InventoryComponent& comp) -> f32
                                             { return comp.PlayerInventory.GetTotalWeight(); });

        // --- ItemPickupComponent ---
        lua.new_usertype<ItemPickupComponent>("ItemPickupComponent",
                                              "pickupRadius", &ItemPickupComponent::PickupRadius,
                                              "autoPickup", &ItemPickupComponent::AutoPickup,
                                              "despawnTimer", &ItemPickupComponent::DespawnTimer);

        // --- ItemContainerComponent ---
        lua.new_usertype<ItemContainerComponent>("ItemContainerComponent",
                                                 "isShop", &ItemContainerComponent::IsShop,
                                                 "lootTableID", &ItemContainerComponent::LootTableID,
                                                 "hasBeenLooted", &ItemContainerComponent::HasBeenLooted);

        // --- QuestJournalComponent ---
        lua.new_usertype<QuestJournalComponent>("QuestJournalComponent", "AcceptQuest", [](QuestJournalComponent& comp, const std::string& questId) -> bool
                                                {
                const auto* def = QuestDatabase::Get(questId);
                if (!def) return false;
                return comp.Journal.AcceptQuest(questId, *def); }, "AbandonQuest", [](QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.AbandonQuest(questId); }, "CompleteQuest", [](QuestJournalComponent& comp, const std::string& questId, sol::optional<std::string> branch) -> bool
                                                { return comp.Journal.CompleteQuest(questId, branch.value_or("")); }, "IsQuestActive", [](const QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.IsQuestActive(questId); }, "HasCompletedQuest", [](const QuestJournalComponent& comp, const std::string& questId) -> bool
                                                { return comp.Journal.HasCompletedQuest(questId); }, "IncrementObjective", [](QuestJournalComponent& comp, const std::string& questId, const std::string& objId, sol::optional<i32> amount)
                                                { comp.Journal.IncrementObjective(questId, objId, amount.value_or(1)); }, "NotifyKill", [](QuestJournalComponent& comp, const std::string& targetTag)
                                                { comp.Journal.NotifyKill(targetTag); }, "NotifyCollect", [](QuestJournalComponent& comp, const std::string& itemId, sol::optional<i32> count)
                                                { comp.Journal.NotifyCollect(itemId, count.value_or(1)); }, "NotifyInteract", [](QuestJournalComponent& comp, const std::string& id)
                                                { comp.Journal.NotifyInteract(id); }, "NotifyReachLocation", [](QuestJournalComponent& comp, const std::string& locId)
                                                { comp.Journal.NotifyReachLocation(locId); }, "HasTag", [](const QuestJournalComponent& comp, const std::string& tag) -> bool
                                                { return comp.Journal.HasTag(tag); }, "AddTag", [](QuestJournalComponent& comp, const std::string& tag)
                                                { comp.Journal.AddTag(tag); });

        // --- QuestGiverComponent ---
        lua.new_usertype<QuestGiverComponent>("QuestGiverComponent",
                                              "questMarkerIcon", &QuestGiverComponent::QuestMarkerIcon);
    }
} // namespace OloEngine
