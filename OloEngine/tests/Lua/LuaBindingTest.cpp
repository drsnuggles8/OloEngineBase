#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Particle/ParticleEmitter.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Lua Binding Test Fixture
// =============================================================================
// Mirrors the bindings from LuaScriptGlue::RegisterAllTypes() for
// component property round-trip verification. No rendering or Mono required.

class LuaBindingTest : public ::testing::Test
{
  protected:
    sol::state lua;

    void SetUp() override
    {
        lua.open_libraries(sol::lib::base, sol::lib::math);

        // --- GLM vector types ---
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

        // --- TransformComponent ---
        lua.new_usertype<TransformComponent>("TransformComponent",
                                             "translation", &TransformComponent::Translation,
                                             "scale", &TransformComponent::Scale,
                                             "rotation", sol::property(&TransformComponent::GetRotationEuler, &TransformComponent::SetRotationEuler));

        // --- Rigidbody2DComponent ---
        lua.new_usertype<Rigidbody2DComponent>("Rigidbody2DComponent",
                                               "type", &Rigidbody2DComponent::Type,
                                               "fixedRotation", &Rigidbody2DComponent::FixedRotation,
                                               "linearVelocity", &Rigidbody2DComponent::LinearVelocity,
                                               "angularVelocity", &Rigidbody2DComponent::AngularVelocity);

        // --- BoxCollider2DComponent ---
        lua.new_usertype<BoxCollider2DComponent>("BoxCollider2DComponent",
                                                 "offset", &BoxCollider2DComponent::Offset,
                                                 "size", &BoxCollider2DComponent::Size,
                                                 "density", &BoxCollider2DComponent::Density,
                                                 "friction", &BoxCollider2DComponent::Friction,
                                                 "restitution", &BoxCollider2DComponent::Restitution,
                                                 "restitutionThreshold", &BoxCollider2DComponent::RestitutionThreshold);

        // --- CircleCollider2DComponent ---
        lua.new_usertype<CircleCollider2DComponent>("CircleCollider2DComponent",
                                                    "offset", &CircleCollider2DComponent::Offset,
                                                    "radius", &CircleCollider2DComponent::Radius,
                                                    "density", &CircleCollider2DComponent::Density,
                                                    "friction", &CircleCollider2DComponent::Friction,
                                                    "restitution", &CircleCollider2DComponent::Restitution,
                                                    "restitutionThreshold", &CircleCollider2DComponent::RestitutionThreshold);

        // --- SceneCamera ---
        lua.new_usertype<SceneCamera>("SceneCamera",
                                      "projectionType", sol::property(&SceneCamera::GetProjectionType, &SceneCamera::SetProjectionType),
                                      "perspectiveFOV", sol::property(&SceneCamera::GetPerspectiveVerticalFOV, &SceneCamera::SetPerspectiveVerticalFOV),
                                      "perspectiveNearClip", sol::property(&SceneCamera::GetPerspectiveNearClip, &SceneCamera::SetPerspectiveNearClip),
                                      "perspectiveFarClip", sol::property(&SceneCamera::GetPerspectiveFarClip, &SceneCamera::SetPerspectiveFarClip),
                                      "orthographicSize", sol::property(&SceneCamera::GetOrthographicSize, &SceneCamera::SetOrthographicSize),
                                      "orthographicNearClip", sol::property(&SceneCamera::GetOrthographicNearClip, &SceneCamera::SetOrthographicNearClip),
                                      "orthographicFarClip", sol::property(&SceneCamera::GetOrthographicFarClip, &SceneCamera::SetOrthographicFarClip));

        // --- CameraComponent ---
        lua.new_usertype<CameraComponent>("CameraComponent",
                                          "camera", &CameraComponent::Camera,
                                          "primary", &CameraComponent::Primary,
                                          "fixedAspectRatio", &CameraComponent::FixedAspectRatio);

        // --- SpriteRendererComponent ---
        lua.new_usertype<SpriteRendererComponent>("SpriteRendererComponent",
                                                  "color", &SpriteRendererComponent::Color,
                                                  "tilingFactor", &SpriteRendererComponent::TilingFactor);

        // --- CircleRendererComponent ---
        lua.new_usertype<CircleRendererComponent>("CircleRendererComponent",
                                                  "color", &CircleRendererComponent::Color,
                                                  "thickness", &CircleRendererComponent::Thickness,
                                                  "fade", &CircleRendererComponent::Fade);

        // --- TextComponent ---
        lua.new_usertype<TextComponent>("TextComponent",
                                        "text", &TextComponent::TextString,
                                        "color", &TextComponent::Color,
                                        "kerning", &TextComponent::Kerning,
                                        "lineSpacing", &TextComponent::LineSpacing,
                                        "maxWidth", &TextComponent::MaxWidth,
                                        "dropShadow", &TextComponent::DropShadow,
                                        "shadowDistance", &TextComponent::ShadowDistance,
                                        "shadowColor", &TextComponent::ShadowColor);

        // --- MeshComponent ---
        lua.new_usertype<MeshComponent>("MeshComponent",
                                        "primitive", &MeshComponent::m_Primitive);

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

        // --- ParticleSystem ---
        lua.new_usertype<ParticleSystem>("ParticleSystem",
                                         "playing", &ParticleSystem::Playing,
                                         "looping", &ParticleSystem::Looping,
                                         "duration", &ParticleSystem::Duration,
                                         "playbackSpeed", &ParticleSystem::PlaybackSpeed,
                                         "windInfluence", &ParticleSystem::WindInfluence);

        // --- ParticleEmitter ---
        lua.new_usertype<ParticleEmitter>("ParticleEmitter",
                                          "rateOverTime", &ParticleEmitter::RateOverTime,
                                          "initialSpeed", &ParticleEmitter::InitialSpeed,
                                          "speedVariance", &ParticleEmitter::SpeedVariance,
                                          "lifetimeMin", &ParticleEmitter::LifetimeMin,
                                          "lifetimeMax", &ParticleEmitter::LifetimeMax,
                                          "initialSize", &ParticleEmitter::InitialSize,
                                          "sizeVariance", &ParticleEmitter::SizeVariance,
                                          "initialColor", &ParticleEmitter::InitialColor);

        // --- ParticleSystemComponent ---
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

        // --- glm::ivec3 for Resolution access ---
        lua.new_usertype<glm::ivec3>("ivec3",
                                     sol::constructors<glm::ivec3(), glm::ivec3(int), glm::ivec3(int, int, int)>(),
                                     "x", &glm::ivec3::x,
                                     "y", &glm::ivec3::y,
                                     "z", &glm::ivec3::z);

        // --- UIWorldAnchorComponent ---
        lua.new_usertype<UIWorldAnchorComponent>("UIWorldAnchorComponent",
                                                 "targetEntity", sol::property([](const UIWorldAnchorComponent& c)
                                                                               { return static_cast<u64>(c.m_TargetEntity); }, [](UIWorldAnchorComponent& c, u64 id)
                                                                               { c.m_TargetEntity = UUID(id); }),
                                                 "worldOffset", &UIWorldAnchorComponent::m_WorldOffset);

        // --- NameplateComponent ---
        lua.new_usertype<NameplateComponent>("NameplateComponent",
                                             "enabled", &NameplateComponent::m_Enabled,
                                             "showHealthBar", &NameplateComponent::m_ShowHealthBar,
                                             "showManaBar", &NameplateComponent::m_ShowManaBar,
                                             "worldOffset", &NameplateComponent::m_WorldOffset,
                                             "barSize", &NameplateComponent::m_BarSize,
                                             "healthBarColor", &NameplateComponent::m_HealthBarColor,
                                             "manaBarColor", &NameplateComponent::m_ManaBarColor,
                                             "barBackgroundColor", &NameplateComponent::m_BarBackgroundColor,
                                             "manaBarGap", &NameplateComponent::m_ManaBarGap);

        // --- IKTargetComponent ---
        lua.new_usertype<IKTargetComponent>("IKTargetComponent",
                                            "aimIKEnabled", &IKTargetComponent::AimIKEnabled,
                                            "aimBoneIndex", &IKTargetComponent::AimBoneIndex,
                                            "aimTarget", &IKTargetComponent::AimTarget,
                                            "aimAxis", &IKTargetComponent::AimAxis,
                                            "aimOffset", &IKTargetComponent::AimOffset,
                                            "aimPoleVector", &IKTargetComponent::AimPoleVector,
                                            "aimChainLength", &IKTargetComponent::AimChainLength,
                                            "aimChainFactor", &IKTargetComponent::AimChainFactor,
                                            "aimWeight", &IKTargetComponent::AimWeight,
                                            "aimTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                             { return static_cast<u64>(c.AimTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                             { c.AimTargetEntity = UUID(id); }),
                                            "limbIKEnabled", &IKTargetComponent::LimbIKEnabled,
                                            "limbBoneIndex", &IKTargetComponent::LimbBoneIndex,
                                            "limbTarget", &IKTargetComponent::LimbTarget,
                                            "limbChainLength", &IKTargetComponent::LimbChainLength,
                                            "limbWeight", &IKTargetComponent::LimbWeight,
                                            "limbTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                              { return static_cast<u64>(c.LimbTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                              { c.LimbTargetEntity = UUID(id); }));

        // --- WindSettings ---
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

        // --- StreamingSettings ---
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

        // --- AudioSourceComponent (with spatial properties) ---
        lua.new_usertype<AudioSourceComponent>("AudioSourceComponent",
                                               "volume", sol::property([](const AudioSourceComponent& c)
                                                                       { return c.Config.VolumeMultiplier; }, [](AudioSourceComponent& c, f32 v)
                                                                       {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.0f, 2.0f);
                    c.Config.VolumeMultiplier = v; }),
                                               "pitch", sol::property([](const AudioSourceComponent& c)
                                                                      { return c.Config.PitchMultiplier; }, [](AudioSourceComponent& c, f32 v)
                                                                      {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.1f, 3.0f);
                    c.Config.PitchMultiplier = v; }),
                                               "playOnAwake", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.Config.PlayOnAwake; }, [](AudioSourceComponent& c, bool v)
                                                                            { c.Config.PlayOnAwake = v; }),
                                               "looping", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.Looping; }, [](AudioSourceComponent& c, bool v)
                                                                        { c.Config.Looping = v; }),
                                               "spatialization", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.Config.Spatialization; }, [](AudioSourceComponent& c, bool v)
                                                                               { c.Config.Spatialization = v; }),
                                               "useEventSystem", &AudioSourceComponent::UseEventSystem,
                                               "startEvent", sol::property([](const AudioSourceComponent& c)
                                                                           { return c.StartEvent; }, [](AudioSourceComponent& c, const std::string& v)
                                                                           { c.StartEvent = v; }),
                                               // Spatial audio properties
                                               "attenuationModel", sol::property([](const AudioSourceComponent& c)
                                                                                 { return static_cast<int>(c.Config.AttenuationModel); }, [](AudioSourceComponent& c, int v)
                                                                                 { c.Config.AttenuationModel = static_cast<AttenuationModelType>(v); }),
                                               "rollOff", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.RollOff; }, [](AudioSourceComponent& c, f32 v)
                                                                        { if (!std::isfinite(v)) v = 1.0f; c.Config.RollOff = v; }),
                                               "minGain", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.MinGain; }, [](AudioSourceComponent& c, f32 v)
                                                                        { if (!std::isfinite(v)) v = 0.0f; c.Config.MinGain = v; }),
                                               "maxGain", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.Config.MaxGain; }, [](AudioSourceComponent& c, f32 v)
                                                                        { if (!std::isfinite(v)) v = 1.0f; c.Config.MaxGain = v; }),
                                               "minDistance", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.Config.MinDistance; }, [](AudioSourceComponent& c, f32 v)
                                                                            { if (!std::isfinite(v)) v = 0.3f; c.Config.MinDistance = v; }),
                                               "maxDistance", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.Config.MaxDistance; }, [](AudioSourceComponent& c, f32 v)
                                                                            { if (!std::isfinite(v)) v = 1000.0f; c.Config.MaxDistance = v; }),
                                               "coneInnerAngle", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.Config.ConeInnerAngle; }, [](AudioSourceComponent& c, f32 v)
                                                                               { c.Config.ConeInnerAngle = v; }),
                                               "coneOuterAngle", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.Config.ConeOuterAngle; }, [](AudioSourceComponent& c, f32 v)
                                                                               { c.Config.ConeOuterAngle = v; }),
                                               "coneOuterGain", sol::property([](const AudioSourceComponent& c)
                                                                              { return c.Config.ConeOuterGain; }, [](AudioSourceComponent& c, f32 v)
                                                                              { c.Config.ConeOuterGain = v; }),
                                               "dopplerFactor", sol::property([](const AudioSourceComponent& c)
                                                                              { return c.Config.DopplerFactor; }, [](AudioSourceComponent& c, f32 v)
                                                                              { if (!std::isfinite(v)) v = 1.0f; c.Config.DopplerFactor = v; }));

        // --- AudioListenerComponent ---
        lua.new_usertype<AudioListenerComponent>("AudioListenerComponent",
                                                 "active", &AudioListenerComponent::Active);

        // --- DialogueComponent ---
        lua.new_usertype<DialogueComponent>("DialogueComponent",
                                            "dialogueTree", &DialogueComponent::m_DialogueTree,
                                            "autoTrigger", &DialogueComponent::m_AutoTrigger,
                                            "triggerRadius", &DialogueComponent::m_TriggerRadius,
                                            "hasTriggered", &DialogueComponent::m_HasTriggered,
                                            "triggerOnce", &DialogueComponent::m_TriggerOnce);

        // --- NavAgentComponent ---
        lua.new_usertype<NavAgentComponent>("NavAgentComponent",
                                            "radius", &NavAgentComponent::m_Radius,
                                            "height", &NavAgentComponent::m_Height,
                                            "maxSpeed", &NavAgentComponent::m_MaxSpeed,
                                            "acceleration", &NavAgentComponent::m_Acceleration,
                                            "stoppingDistance", &NavAgentComponent::m_StoppingDistance,
                                            "avoidancePriority", &NavAgentComponent::m_AvoidancePriority,
                                            "hasTarget", sol::readonly(&NavAgentComponent::m_HasTarget),
                                            "hasPath", sol::readonly(&NavAgentComponent::m_HasPath),
                                            "lockYAxis", &NavAgentComponent::m_LockYAxis);

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

        // --- QuestGiverComponent ---
        lua.new_usertype<QuestGiverComponent>("QuestGiverComponent",
                                              "questMarkerIcon", &QuestGiverComponent::QuestMarkerIcon,
                                              "offeredQuestIDs", &QuestGiverComponent::OfferedQuestIDs,
                                              "turnInQuestIDs", &QuestGiverComponent::TurnInQuestIDs);
    }
};

// =============================================================================
// GLM vector constructors and access
// =============================================================================

TEST_F(LuaBindingTest, Vec2_ConstructAndAccess)
{
    auto result = lua.script("local v = vec2.new(4.0, 5.0); return v.x, v.y");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 4.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 5.0f);
}

TEST_F(LuaBindingTest, Vec3_ConstructAndAccess)
{
    auto result = lua.script("local v = vec3.new(1.0, 2.0, 3.0); return v.x, v.y, v.z");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
}

TEST_F(LuaBindingTest, Vec4_ConstructAndAccess)
{
    auto result = lua.script("local v = vec4.new(1.0, 2.0, 3.0, 4.0); return v.x, v.y, v.z, v.w");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(3), 4.0f);
}

// =============================================================================
// TransformComponent
// =============================================================================

TEST_F(LuaBindingTest, TransformComponent_TranslationRoundTrip)
{
    TransformComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.translation = vec3.new(1.0, 2.0, 3.0)");
    EXPECT_FLOAT_EQ(tc.Translation.x, 1.0f);
    EXPECT_FLOAT_EQ(tc.Translation.y, 2.0f);
    EXPECT_FLOAT_EQ(tc.Translation.z, 3.0f);

    auto result = lua.script("return tc.translation.x, tc.translation.y, tc.translation.z");
    EXPECT_FLOAT_EQ(result.get<f32>(0), 1.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(1), 2.0f);
    EXPECT_FLOAT_EQ(result.get<f32>(2), 3.0f);
}

TEST_F(LuaBindingTest, TransformComponent_ScaleRoundTrip)
{
    TransformComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.scale = vec3.new(2.0, 3.0, 4.0)");
    EXPECT_FLOAT_EQ(tc.Scale.x, 2.0f);
    EXPECT_FLOAT_EQ(tc.Scale.y, 3.0f);
    EXPECT_FLOAT_EQ(tc.Scale.z, 4.0f);
}

TEST_F(LuaBindingTest, TransformComponent_RotationRoundTrip)
{
    TransformComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.rotation = vec3.new(0.1, 0.2, 0.3)");
    auto euler = tc.GetRotationEuler();
    EXPECT_NEAR(euler.x, 0.1f, 1e-5f);
    EXPECT_NEAR(euler.y, 0.2f, 1e-5f);
    EXPECT_NEAR(euler.z, 0.3f, 1e-5f);
}

// =============================================================================
// Rigidbody2DComponent
// =============================================================================

TEST_F(LuaBindingTest, Rigidbody2D_PropertyRoundTrip)
{
    Rigidbody2DComponent rb;
    lua["rb"] = &rb;

    lua.script("rb.fixedRotation = true");
    EXPECT_TRUE(rb.FixedRotation);

    lua.script("rb.angularVelocity = 5.0");
    EXPECT_FLOAT_EQ(rb.AngularVelocity, 5.0f);

    lua.script("rb.linearVelocity = vec2.new(1.0, 2.0)");
    EXPECT_FLOAT_EQ(rb.LinearVelocity.x, 1.0f);
    EXPECT_FLOAT_EQ(rb.LinearVelocity.y, 2.0f);
}

// =============================================================================
// BoxCollider2DComponent
// =============================================================================

TEST_F(LuaBindingTest, BoxCollider2D_PropertyRoundTrip)
{
    BoxCollider2DComponent bc;
    lua["bc"] = &bc;

    lua.script("bc.offset = vec2.new(0.5, 0.5)");
    EXPECT_FLOAT_EQ(bc.Offset.x, 0.5f);
    EXPECT_FLOAT_EQ(bc.Offset.y, 0.5f);

    lua.script("bc.size = vec2.new(1.0, 2.0)");
    EXPECT_FLOAT_EQ(bc.Size.x, 1.0f);
    EXPECT_FLOAT_EQ(bc.Size.y, 2.0f);

    lua.script("bc.density = 2.0; bc.friction = 0.3; bc.restitution = 0.7; bc.restitutionThreshold = 1.0");
    EXPECT_FLOAT_EQ(bc.Density, 2.0f);
    EXPECT_FLOAT_EQ(bc.Friction, 0.3f);
    EXPECT_FLOAT_EQ(bc.Restitution, 0.7f);
    EXPECT_FLOAT_EQ(bc.RestitutionThreshold, 1.0f);
}

// =============================================================================
// CircleCollider2DComponent
// =============================================================================

TEST_F(LuaBindingTest, CircleCollider2D_PropertyRoundTrip)
{
    CircleCollider2DComponent cc;
    lua["cc"] = &cc;

    lua.script("cc.radius = 2.0; cc.offset = vec2.new(0.1, 0.2)");
    EXPECT_FLOAT_EQ(cc.Radius, 2.0f);
    EXPECT_FLOAT_EQ(cc.Offset.x, 0.1f);
    EXPECT_FLOAT_EQ(cc.Offset.y, 0.2f);

    lua.script("cc.density = 1.5; cc.friction = 0.8");
    EXPECT_FLOAT_EQ(cc.Density, 1.5f);
    EXPECT_FLOAT_EQ(cc.Friction, 0.8f);
}

// =============================================================================
// CameraComponent + SceneCamera
// =============================================================================

TEST_F(LuaBindingTest, CameraComponent_PrimaryRoundTrip)
{
    CameraComponent cam;
    lua["cam"] = &cam;

    lua.script("cam.primary = false");
    EXPECT_FALSE(cam.Primary);

    lua.script("cam.primary = true");
    EXPECT_TRUE(cam.Primary);
}

TEST_F(LuaBindingTest, CameraComponent_SceneCameraProperties)
{
    CameraComponent cam;
    lua["cam"] = &cam;

    lua.script("cam.camera.perspectiveFOV = 1.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveVerticalFOV(), 1.0f);

    lua.script("cam.camera.orthographicSize = 20.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetOrthographicSize(), 20.0f);

    lua.script("cam.camera.perspectiveNearClip = 0.1; cam.camera.perspectiveFarClip = 500.0");
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveNearClip(), 0.1f);
    EXPECT_FLOAT_EQ(cam.Camera.GetPerspectiveFarClip(), 500.0f);
}

// =============================================================================
// SpriteRendererComponent
// =============================================================================

TEST_F(LuaBindingTest, SpriteRenderer_PropertyRoundTrip)
{
    SpriteRendererComponent sr;
    lua["sr"] = &sr;

    lua.script("sr.color = vec4.new(0.5, 0.6, 0.7, 0.8)");
    EXPECT_FLOAT_EQ(sr.Color.r, 0.5f);
    EXPECT_FLOAT_EQ(sr.Color.g, 0.6f);
    EXPECT_FLOAT_EQ(sr.Color.b, 0.7f);
    EXPECT_FLOAT_EQ(sr.Color.a, 0.8f);

    lua.script("sr.tilingFactor = 3.0");
    EXPECT_FLOAT_EQ(sr.TilingFactor, 3.0f);
}

// =============================================================================
// CircleRendererComponent
// =============================================================================

TEST_F(LuaBindingTest, CircleRenderer_PropertyRoundTrip)
{
    CircleRendererComponent cr;
    lua["cr"] = &cr;

    lua.script("cr.color = vec4.new(1.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(cr.Color.r, 1.0f);
    EXPECT_FLOAT_EQ(cr.Color.g, 0.0f);

    lua.script("cr.thickness = 0.5; cr.fade = 0.01");
    EXPECT_FLOAT_EQ(cr.Thickness, 0.5f);
    EXPECT_FLOAT_EQ(cr.Fade, 0.01f);
}

// =============================================================================
// TextComponent
// =============================================================================

TEST_F(LuaBindingTest, TextComponent_StringRoundTrip)
{
    TextComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.text = 'Hello, World!'");
    EXPECT_EQ(tc.TextString, "Hello, World!");

    auto result = lua.script("return tc.text");
    EXPECT_EQ(result.get<std::string>(), "Hello, World!");
}

TEST_F(LuaBindingTest, TextComponent_PropertyRoundTrip)
{
    TextComponent tc;
    lua["tc"] = &tc;

    lua.script("tc.color = vec4.new(0.0, 1.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(tc.Color.g, 1.0f);

    lua.script("tc.kerning = 1.5; tc.lineSpacing = 2.0; tc.maxWidth = 300.0");
    EXPECT_FLOAT_EQ(tc.Kerning, 1.5f);
    EXPECT_FLOAT_EQ(tc.LineSpacing, 2.0f);
    EXPECT_FLOAT_EQ(tc.MaxWidth, 300.0f);

    lua.script("tc.dropShadow = true; tc.shadowDistance = 0.05");
    EXPECT_TRUE(tc.DropShadow);
    EXPECT_FLOAT_EQ(tc.ShadowDistance, 0.05f);
}

// =============================================================================
// MeshComponent
// =============================================================================

TEST_F(LuaBindingTest, MeshComponent_PrimitiveRoundTrip)
{
    MeshComponent mc;
    lua["mc"] = &mc;
    mc.m_Primitive = MeshPrimitive::Cube;

    auto result = lua.script("return mc.primitive");
    EXPECT_EQ(result.get<MeshPrimitive>(), MeshPrimitive::Cube);
}

// =============================================================================
// UI Components
// =============================================================================

TEST_F(LuaBindingTest, UICanvasComponent_PropertyRoundTrip)
{
    UICanvasComponent canvas;
    lua["c"] = &canvas;

    lua.script("c.sortOrder = 5");
    EXPECT_EQ(canvas.m_SortOrder, 5);

    lua.script("c.referenceResolution = vec2.new(1280.0, 720.0)");
    EXPECT_FLOAT_EQ(canvas.m_ReferenceResolution.x, 1280.0f);
    EXPECT_FLOAT_EQ(canvas.m_ReferenceResolution.y, 720.0f);
}

TEST_F(LuaBindingTest, UIRectTransformComponent_PropertyRoundTrip)
{
    UIRectTransformComponent rect;
    lua["r"] = &rect;

    lua.script("r.anchorMin = vec2.new(0.0, 0.0); r.anchorMax = vec2.new(1.0, 1.0)");
    EXPECT_FLOAT_EQ(rect.m_AnchorMin.x, 0.0f);
    EXPECT_FLOAT_EQ(rect.m_AnchorMax.x, 1.0f);
    EXPECT_FLOAT_EQ(rect.m_AnchorMax.y, 1.0f);

    lua.script("r.anchoredPosition = vec2.new(10.0, 20.0)");
    EXPECT_FLOAT_EQ(rect.m_AnchoredPosition.x, 10.0f);

    lua.script("r.sizeDelta = vec2.new(200.0, 50.0)");
    EXPECT_FLOAT_EQ(rect.m_SizeDelta.x, 200.0f);
    EXPECT_FLOAT_EQ(rect.m_SizeDelta.y, 50.0f);

    lua.script("r.pivot = vec2.new(0.0, 1.0)");
    EXPECT_FLOAT_EQ(rect.m_Pivot.x, 0.0f);
    EXPECT_FLOAT_EQ(rect.m_Pivot.y, 1.0f);

    lua.script("r.rotation = 45.0; r.scale = vec2.new(2.0, 2.0)");
    EXPECT_FLOAT_EQ(rect.m_Rotation, 45.0f);
    EXPECT_FLOAT_EQ(rect.m_Scale.x, 2.0f);
}

TEST_F(LuaBindingTest, UIImageComponent_PropertyRoundTrip)
{
    UIImageComponent img;
    lua["i"] = &img;

    lua.script("i.color = vec4.new(1.0, 0.0, 0.0, 0.5)");
    EXPECT_FLOAT_EQ(img.m_Color.r, 1.0f);
    EXPECT_FLOAT_EQ(img.m_Color.a, 0.5f);

    lua.script("i.borderInsets = vec4.new(1.0, 2.0, 3.0, 4.0)");
    EXPECT_FLOAT_EQ(img.m_BorderInsets.x, 1.0f);
    EXPECT_FLOAT_EQ(img.m_BorderInsets.w, 4.0f);
}

TEST_F(LuaBindingTest, UIPanelComponent_PropertyRoundTrip)
{
    UIPanelComponent panel;
    lua["p"] = &panel;

    lua.script("p.backgroundColor = vec4.new(0.1, 0.2, 0.3, 0.9)");
    EXPECT_FLOAT_EQ(panel.m_BackgroundColor.r, 0.1f);
    EXPECT_FLOAT_EQ(panel.m_BackgroundColor.a, 0.9f);
}

TEST_F(LuaBindingTest, UITextComponent_PropertyRoundTrip)
{
    UITextComponent text;
    lua["t"] = &text;

    lua.script("t.text = 'UI Label'; t.fontSize = 24.0");
    EXPECT_EQ(text.m_Text, "UI Label");
    EXPECT_FLOAT_EQ(text.m_FontSize, 24.0f);

    lua.script("t.color = vec4.new(0.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(text.m_Color.r, 0.0f);

    lua.script("t.kerning = 0.5; t.lineSpacing = 1.2");
    EXPECT_FLOAT_EQ(text.m_Kerning, 0.5f);
    EXPECT_FLOAT_EQ(text.m_LineSpacing, 1.2f);
}

TEST_F(LuaBindingTest, UIButtonComponent_PropertyRoundTrip)
{
    UIButtonComponent btn;
    lua["b"] = &btn;

    lua.script("b.interactable = false");
    EXPECT_FALSE(btn.m_Interactable);

    lua.script("b.normalColor = vec4.new(0.5, 0.5, 0.5, 1.0)");
    EXPECT_FLOAT_EQ(btn.m_NormalColor.r, 0.5f);

    lua.script("b.hoveredColor = vec4.new(0.6, 0.6, 0.6, 1.0)");
    EXPECT_FLOAT_EQ(btn.m_HoveredColor.r, 0.6f);

    // state is readonly
    auto result = lua.script("return b.state");
    EXPECT_EQ(result.get<UIButtonState>(), UIButtonState::Normal);
}

TEST_F(LuaBindingTest, UISliderComponent_PropertyRoundTrip)
{
    UISliderComponent slider;
    lua["s"] = &slider;

    lua.script("s.value = 0.5; s.minValue = 0.0; s.maxValue = 100.0");
    EXPECT_FLOAT_EQ(slider.m_Value, 0.5f);
    EXPECT_FLOAT_EQ(slider.m_MinValue, 0.0f);
    EXPECT_FLOAT_EQ(slider.m_MaxValue, 100.0f);

    lua.script("s.interactable = false");
    EXPECT_FALSE(slider.m_Interactable);

    lua.script("s.fillColor = vec4.new(0.0, 1.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(slider.m_FillColor.g, 1.0f);
}

TEST_F(LuaBindingTest, UICheckboxComponent_PropertyRoundTrip)
{
    UICheckboxComponent cb;
    lua["c"] = &cb;

    lua.script("c.isChecked = true");
    EXPECT_TRUE(cb.m_IsChecked);

    lua.script("c.interactable = false");
    EXPECT_FALSE(cb.m_Interactable);

    lua.script("c.checkedColor = vec4.new(0.0, 0.8, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(cb.m_CheckedColor.g, 0.8f);
}

TEST_F(LuaBindingTest, UIProgressBarComponent_PropertyRoundTrip)
{
    UIProgressBarComponent pb;
    lua["p"] = &pb;

    lua.script("p.value = 75.0; p.minValue = 0.0; p.maxValue = 100.0");
    EXPECT_FLOAT_EQ(pb.m_Value, 75.0f);
    EXPECT_FLOAT_EQ(pb.m_MinValue, 0.0f);
    EXPECT_FLOAT_EQ(pb.m_MaxValue, 100.0f);

    lua.script("p.fillColor = vec4.new(0.2, 0.8, 0.2, 1.0)");
    EXPECT_FLOAT_EQ(pb.m_FillColor.g, 0.8f);
}

TEST_F(LuaBindingTest, UIInputFieldComponent_PropertyRoundTrip)
{
    UIInputFieldComponent input;
    lua["i"] = &input;

    lua.script("i.text = 'typed text'; i.placeholder = 'Enter...'");
    EXPECT_EQ(input.m_Text, "typed text");
    EXPECT_EQ(input.m_Placeholder, "Enter...");

    lua.script("i.fontSize = 16.0; i.characterLimit = 50; i.interactable = false");
    EXPECT_FLOAT_EQ(input.m_FontSize, 16.0f);
    EXPECT_EQ(input.m_CharacterLimit, 50);
    EXPECT_FALSE(input.m_Interactable);
}

TEST_F(LuaBindingTest, UIScrollViewComponent_PropertyRoundTrip)
{
    UIScrollViewComponent sv;
    lua["s"] = &sv;

    lua.script("s.scrollPosition = vec2.new(10.0, 20.0)");
    EXPECT_FLOAT_EQ(sv.m_ScrollPosition.x, 10.0f);
    EXPECT_FLOAT_EQ(sv.m_ScrollPosition.y, 20.0f);

    lua.script("s.contentSize = vec2.new(800.0, 2000.0)");
    EXPECT_FLOAT_EQ(sv.m_ContentSize.x, 800.0f);

    lua.script("s.scrollSpeed = 2.5");
    EXPECT_FLOAT_EQ(sv.m_ScrollSpeed, 2.5f);

    lua.script("s.showHorizontalScrollbar = false; s.showVerticalScrollbar = true");
    EXPECT_FALSE(sv.m_ShowHorizontalScrollbar);
    EXPECT_TRUE(sv.m_ShowVerticalScrollbar);
}

TEST_F(LuaBindingTest, UIDropdownComponent_PropertyRoundTrip)
{
    UIDropdownComponent dd;
    lua["d"] = &dd;

    lua.script("d.selectedIndex = 3; d.fontSize = 18.0; d.itemHeight = 30.0");
    EXPECT_EQ(dd.m_SelectedIndex, 3);
    EXPECT_FLOAT_EQ(dd.m_FontSize, 18.0f);
    EXPECT_FLOAT_EQ(dd.m_ItemHeight, 30.0f);

    lua.script("d.interactable = false");
    EXPECT_FALSE(dd.m_Interactable);
}

TEST_F(LuaBindingTest, UIGridLayoutComponent_PropertyRoundTrip)
{
    UIGridLayoutComponent grid;
    lua["g"] = &grid;

    lua.script("g.cellSize = vec2.new(64.0, 64.0); g.spacing = vec2.new(4.0, 4.0)");
    EXPECT_FLOAT_EQ(grid.m_CellSize.x, 64.0f);
    EXPECT_FLOAT_EQ(grid.m_Spacing.x, 4.0f);

    lua.script("g.constraintCount = 4");
    EXPECT_EQ(grid.m_ConstraintCount, 4);
}

TEST_F(LuaBindingTest, UIToggleComponent_PropertyRoundTrip)
{
    UIToggleComponent toggle;
    lua["t"] = &toggle;

    lua.script("t.isOn = true; t.interactable = false");
    EXPECT_TRUE(toggle.m_IsOn);
    EXPECT_FALSE(toggle.m_Interactable);

    lua.script("t.onColor = vec4.new(0.0, 1.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(toggle.m_OnColor.g, 1.0f);

    lua.script("t.knobColor = vec4.new(1.0, 1.0, 1.0, 1.0)");
    EXPECT_FLOAT_EQ(toggle.m_KnobColor.r, 1.0f);
}

// =============================================================================
// ParticleSystem + ParticleEmitter
// =============================================================================

TEST_F(LuaBindingTest, ParticleSystem_PropertyRoundTrip)
{
    ParticleSystem ps;
    lua["ps"] = &ps;

    lua.script("ps.playing = false; ps.looping = false; ps.duration = 10.0");
    EXPECT_FALSE(ps.Playing);
    EXPECT_FALSE(ps.Looping);
    EXPECT_FLOAT_EQ(ps.Duration, 10.0f);

    lua.script("ps.playbackSpeed = 2.0; ps.windInfluence = 0.5");
    EXPECT_FLOAT_EQ(ps.PlaybackSpeed, 2.0f);
    EXPECT_FLOAT_EQ(ps.WindInfluence, 0.5f);
}

TEST_F(LuaBindingTest, ParticleEmitter_PropertyRoundTrip)
{
    ParticleEmitter emitter;
    lua["e"] = &emitter;

    lua.script("e.rateOverTime = 50.0; e.initialSpeed = 10.0; e.speedVariance = 2.0");
    EXPECT_FLOAT_EQ(emitter.RateOverTime, 50.0f);
    EXPECT_FLOAT_EQ(emitter.InitialSpeed, 10.0f);
    EXPECT_FLOAT_EQ(emitter.SpeedVariance, 2.0f);

    lua.script("e.lifetimeMin = 0.5; e.lifetimeMax = 3.0");
    EXPECT_FLOAT_EQ(emitter.LifetimeMin, 0.5f);
    EXPECT_FLOAT_EQ(emitter.LifetimeMax, 3.0f);

    lua.script("e.initialSize = 2.0; e.sizeVariance = 0.5");
    EXPECT_FLOAT_EQ(emitter.InitialSize, 2.0f);
    EXPECT_FLOAT_EQ(emitter.SizeVariance, 0.5f);

    lua.script("e.initialColor = vec4.new(1.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(emitter.InitialColor.r, 1.0f);
    EXPECT_FLOAT_EQ(emitter.InitialColor.g, 0.0f);
}

TEST_F(LuaBindingTest, ParticleSystemComponent_SystemAccess)
{
    ParticleSystemComponent psc;
    lua["psc"] = &psc;

    lua.script("psc.system.duration = 7.0; psc.system.looping = false");
    EXPECT_FLOAT_EQ(psc.System.Duration, 7.0f);
    EXPECT_FALSE(psc.System.Looping);
}

// =============================================================================
// LightProbeComponent + LightProbeVolumeComponent
// =============================================================================

TEST_F(LuaBindingTest, LightProbeComponent_PropertyRoundTrip)
{
    LightProbeComponent lp;
    lua["lp"] = &lp;

    lua.script("lp.influenceRadius = 25.0; lp.intensity = 0.8; lp.active = false");
    EXPECT_FLOAT_EQ(lp.m_InfluenceRadius, 25.0f);
    EXPECT_FLOAT_EQ(lp.m_Intensity, 0.8f);
    EXPECT_FALSE(lp.m_Active);
}

TEST_F(LuaBindingTest, LightProbeVolumeComponent_PropertyRoundTrip)
{
    LightProbeVolumeComponent lpv;
    lua["lpv"] = &lpv;

    lua.script("lpv.boundsMin = vec3.new(-10.0, -10.0, -10.0)");
    EXPECT_FLOAT_EQ(lpv.m_BoundsMin.x, -10.0f);

    lua.script("lpv.boundsMax = vec3.new(10.0, 10.0, 10.0)");
    EXPECT_FLOAT_EQ(lpv.m_BoundsMax.x, 10.0f);

    lua.script("lpv.spacing = 5.0");
    EXPECT_FLOAT_EQ(lpv.m_Spacing, 5.0f);

    lua.script("lpv.intensity = 1.5; lpv.active = false; lpv.dirty = true");
    EXPECT_FLOAT_EQ(lpv.m_Intensity, 1.5f);
    EXPECT_FALSE(lpv.m_Active);
    EXPECT_TRUE(lpv.m_Dirty);

    auto result = lua.script("return lpv:getTotalProbeCount()");
    EXPECT_GE(result.get<i32>(), 0);
}

// =============================================================================
// UIWorldAnchorComponent
// =============================================================================

TEST_F(LuaBindingTest, UIWorldAnchorComponent_PropertyRoundTrip)
{
    UIWorldAnchorComponent anchor;
    lua["a"] = &anchor;

    lua.script("a.targetEntity = 42");
    EXPECT_EQ(static_cast<u64>(anchor.m_TargetEntity), 42u);

    lua.script("a.worldOffset = vec3.new(0.0, 5.0, 0.0)");
    EXPECT_FLOAT_EQ(anchor.m_WorldOffset.y, 5.0f);

    auto result = lua.script("return a.targetEntity");
    EXPECT_EQ(result.get<u64>(), 42u);
}

// =============================================================================
// NameplateComponent
// =============================================================================

TEST_F(LuaBindingTest, NameplateComponent_PropertyRoundTrip)
{
    NameplateComponent np;
    lua["np"] = &np;

    lua.script("np.enabled = false; np.showHealthBar = false; np.showManaBar = true");
    EXPECT_FALSE(np.m_Enabled);
    EXPECT_FALSE(np.m_ShowHealthBar);
    EXPECT_TRUE(np.m_ShowManaBar);

    lua.script("np.worldOffset = vec3.new(0.0, 3.0, 0.0)");
    EXPECT_FLOAT_EQ(np.m_WorldOffset.y, 3.0f);

    lua.script("np.barSize = vec2.new(200.0, 16.0)");
    EXPECT_FLOAT_EQ(np.m_BarSize.x, 200.0f);

    lua.script("np.manaBarGap = 4.0");
    EXPECT_FLOAT_EQ(np.m_ManaBarGap, 4.0f);

    lua.script("np.healthBarColor = vec4.new(1.0, 0.0, 0.0, 1.0)");
    EXPECT_FLOAT_EQ(np.m_HealthBarColor.r, 1.0f);
}

// =============================================================================
// IKTargetComponent
// =============================================================================

TEST_F(LuaBindingTest, IKTargetComponent_AimProperties)
{
    IKTargetComponent ik;
    lua["ik"] = &ik;

    lua.script("ik.aimIKEnabled = true; ik.aimBoneIndex = 5");
    EXPECT_TRUE(ik.AimIKEnabled);
    EXPECT_EQ(ik.AimBoneIndex, 5);

    lua.script("ik.aimTarget = vec3.new(10.0, 5.0, 0.0)");
    EXPECT_FLOAT_EQ(ik.AimTarget.x, 10.0f);
    EXPECT_FLOAT_EQ(ik.AimTarget.y, 5.0f);

    lua.script("ik.aimWeight = 0.8; ik.aimChainLength = 3; ik.aimChainFactor = 0.7");
    EXPECT_FLOAT_EQ(ik.AimWeight, 0.8f);
    EXPECT_EQ(ik.AimChainLength, 3);
    EXPECT_FLOAT_EQ(ik.AimChainFactor, 0.7f);

    lua.script("ik.aimTargetEntity = 99");
    EXPECT_EQ(static_cast<u64>(ik.AimTargetEntity), 99u);
}

TEST_F(LuaBindingTest, IKTargetComponent_LimbProperties)
{
    IKTargetComponent ik;
    lua["ik"] = &ik;

    lua.script("ik.limbIKEnabled = true; ik.limbBoneIndex = 12");
    EXPECT_TRUE(ik.LimbIKEnabled);
    EXPECT_EQ(ik.LimbBoneIndex, 12);

    lua.script("ik.limbTarget = vec3.new(1.0, 0.0, 0.0)");
    EXPECT_FLOAT_EQ(ik.LimbTarget.x, 1.0f);

    lua.script("ik.limbChainLength = 4; ik.limbWeight = 0.9");
    EXPECT_EQ(ik.LimbChainLength, 4);
    EXPECT_FLOAT_EQ(ik.LimbWeight, 0.9f);

    lua.script("ik.limbTargetEntity = 200");
    EXPECT_EQ(static_cast<u64>(ik.LimbTargetEntity), 200u);
}

// =============================================================================
// WindSettings
// =============================================================================

TEST_F(LuaBindingTest, WindSettings_PropertyRoundTrip)
{
    WindSettings ws;
    lua["ws"] = &ws;

    lua.script("ws.enabled = true; ws.speed = 5.0; ws.gustStrength = 2.0");
    EXPECT_TRUE(ws.Enabled);
    EXPECT_FLOAT_EQ(ws.Speed, 5.0f);
    EXPECT_FLOAT_EQ(ws.GustStrength, 2.0f);

    lua.script("ws.gustFrequency = 0.5; ws.turbulenceIntensity = 0.3; ws.turbulenceScale = 10.0");
    EXPECT_FLOAT_EQ(ws.GustFrequency, 0.5f);
    EXPECT_FLOAT_EQ(ws.TurbulenceIntensity, 0.3f);
    EXPECT_FLOAT_EQ(ws.TurbulenceScale, 10.0f);

    lua.script("ws.direction = vec3.new(1.0, 0.0, 0.0)");
    EXPECT_FLOAT_EQ(ws.Direction.x, 1.0f);

    lua.script("ws.gridWorldSize = 500.0; ws.gridResolution = 64");
    EXPECT_FLOAT_EQ(ws.GridWorldSize, 500.0f);
    EXPECT_EQ(ws.GridResolution, 64);
}

// =============================================================================
// StreamingVolumeComponent + StreamingSettings
// =============================================================================

TEST_F(LuaBindingTest, StreamingVolumeComponent_PropertyRoundTrip)
{
    StreamingVolumeComponent sv;
    lua["sv"] = &sv;

    lua.script("sv.loadRadius = 300.0; sv.unloadRadius = 400.0");
    EXPECT_FLOAT_EQ(sv.LoadRadius, 300.0f);
    EXPECT_FLOAT_EQ(sv.UnloadRadius, 400.0f);

    // isLoaded is readonly
    auto result = lua.script("return sv.isLoaded");
    EXPECT_FALSE(result.get<bool>());
}

TEST_F(LuaBindingTest, StreamingSettings_PropertyRoundTrip)
{
    StreamingSettings ss;
    lua["ss"] = &ss;

    lua.script("ss.enabled = true; ss.defaultLoadRadius = 150.0; ss.defaultUnloadRadius = 200.0");
    EXPECT_TRUE(ss.Enabled);
    EXPECT_FLOAT_EQ(ss.DefaultLoadRadius, 150.0f);
    EXPECT_FLOAT_EQ(ss.DefaultUnloadRadius, 200.0f);

    lua.script("ss.maxLoadedRegions = 8");
    EXPECT_EQ(ss.MaxLoadedRegions, 8u);

    lua.script("ss.regionDirectory = 'Regions/World1'");
    EXPECT_EQ(ss.RegionDirectory, "Regions/World1");
}

// =============================================================================
// NetworkIdentityComponent
// =============================================================================

TEST_F(LuaBindingTest, NetworkIdentityComponent_PropertyRoundTrip)
{
    NetworkIdentityComponent net;
    lua["n"] = &net;

    lua.script("n.ownerClientID = 42; n.isReplicated = false");
    EXPECT_EQ(net.OwnerClientID, 42u);
    EXPECT_FALSE(net.IsReplicated);
}

// =============================================================================
// AudioSourceComponent (including spatial properties)
// =============================================================================

TEST_F(LuaBindingTest, AudioSourceComponent_BasicProperties)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    lua.script("a.volume = 0.5");
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 0.5f);

    lua.script("a.pitch = 1.5");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 1.5f);

    lua.script("a.playOnAwake = false; a.looping = true; a.spatialization = true");
    EXPECT_FALSE(audio.Config.PlayOnAwake);
    EXPECT_TRUE(audio.Config.Looping);
    EXPECT_TRUE(audio.Config.Spatialization);

    lua.script("a.useEventSystem = true");
    EXPECT_TRUE(audio.UseEventSystem);

    lua.script("a.startEvent = 'PlayFootsteps'");
    EXPECT_EQ(audio.StartEvent, "PlayFootsteps");
}

TEST_F(LuaBindingTest, AudioSourceComponent_VolumeClamping)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    lua.script("a.volume = 5.0");
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 2.0f); // clamped to max

    lua.script("a.volume = -1.0");
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 0.0f); // clamped to min
}

TEST_F(LuaBindingTest, AudioSourceComponent_PitchClamping)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    lua.script("a.pitch = 10.0");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 3.0f); // clamped to max

    lua.script("a.pitch = 0.01");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 0.1f); // clamped to min
}

TEST_F(LuaBindingTest, AudioSourceComponent_SpatialProperties)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    // AttenuationModel (enum as int)
    lua.script("a.attenuationModel = 2"); // Linear
    EXPECT_EQ(audio.Config.AttenuationModel, AttenuationModelType::Linear);

    auto result = lua.script("return a.attenuationModel");
    EXPECT_EQ(result.get<int>(), 2);

    // RollOff
    lua.script("a.rollOff = 2.5");
    EXPECT_FLOAT_EQ(audio.Config.RollOff, 2.5f);

    // Gain range
    lua.script("a.minGain = 0.1; a.maxGain = 0.9");
    EXPECT_FLOAT_EQ(audio.Config.MinGain, 0.1f);
    EXPECT_FLOAT_EQ(audio.Config.MaxGain, 0.9f);

    // Distance range
    lua.script("a.minDistance = 1.0; a.maxDistance = 500.0");
    EXPECT_FLOAT_EQ(audio.Config.MinDistance, 1.0f);
    EXPECT_FLOAT_EQ(audio.Config.MaxDistance, 500.0f);

    // Cone angles
    lua.script("a.coneInnerAngle = 1.57; a.coneOuterAngle = 3.14; a.coneOuterGain = 0.2");
    EXPECT_NEAR(audio.Config.ConeInnerAngle, 1.57f, 1e-5f);
    EXPECT_NEAR(audio.Config.ConeOuterAngle, 3.14f, 1e-5f);
    EXPECT_FLOAT_EQ(audio.Config.ConeOuterGain, 0.2f);

    // Doppler
    lua.script("a.dopplerFactor = 2.0");
    EXPECT_FLOAT_EQ(audio.Config.DopplerFactor, 2.0f);
}

TEST_F(LuaBindingTest, AudioSourceComponent_NaNSafety)
{
    AudioSourceComponent audio;
    lua["a"] = &audio;

    // NaN should reset to defaults
    lua.script("a.volume = 0/0"); // NaN
    EXPECT_FLOAT_EQ(audio.Config.VolumeMultiplier, 1.0f);

    lua.script("a.pitch = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.PitchMultiplier, 1.0f);

    lua.script("a.rollOff = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.RollOff, 1.0f);

    lua.script("a.dopplerFactor = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.DopplerFactor, 1.0f);

    lua.script("a.minDistance = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.MinDistance, 0.3f);

    lua.script("a.maxDistance = 0/0");
    EXPECT_FLOAT_EQ(audio.Config.MaxDistance, 1000.0f);
}

// --- AudioListenerComponent ---

TEST_F(LuaBindingTest, AudioListenerComponent_PropertyRoundTrip)
{
    AudioListenerComponent al;
    lua["al"] = &al;

    lua.script("al.active = false");
    EXPECT_FALSE(al.Active);

    lua.script("al.active = true");
    EXPECT_TRUE(al.Active);
}

// =============================================================================
// DialogueComponent
// =============================================================================

TEST_F(LuaBindingTest, DialogueComponent_PropertyRoundTrip)
{
    DialogueComponent dc;
    lua["dc"] = &dc;

    lua.script("dc.autoTrigger = true; dc.triggerRadius = 5.0; dc.triggerOnce = false");
    EXPECT_TRUE(dc.m_AutoTrigger);
    EXPECT_FLOAT_EQ(dc.m_TriggerRadius, 5.0f);
    EXPECT_FALSE(dc.m_TriggerOnce);

    lua.script("dc.hasTriggered = true");
    EXPECT_TRUE(dc.m_HasTriggered);
}

// =============================================================================
// NavAgentComponent
// =============================================================================

TEST_F(LuaBindingTest, NavAgentComponent_PropertyRoundTrip)
{
    NavAgentComponent nav;
    lua["n"] = &nav;

    lua.script("n.radius = 1.0; n.height = 3.0; n.maxSpeed = 7.0; n.acceleration = 12.0");
    EXPECT_FLOAT_EQ(nav.m_Radius, 1.0f);
    EXPECT_FLOAT_EQ(nav.m_Height, 3.0f);
    EXPECT_FLOAT_EQ(nav.m_MaxSpeed, 7.0f);
    EXPECT_FLOAT_EQ(nav.m_Acceleration, 12.0f);

    lua.script("n.stoppingDistance = 0.5; n.avoidancePriority = 10; n.lockYAxis = true");
    EXPECT_FLOAT_EQ(nav.m_StoppingDistance, 0.5f);
    EXPECT_EQ(nav.m_AvoidancePriority, 10);
    EXPECT_TRUE(nav.m_LockYAxis);

    // hasTarget and hasPath are readonly
    auto r1 = lua.script("return n.hasTarget");
    EXPECT_FALSE(r1.get<bool>());
    auto r2 = lua.script("return n.hasPath");
    EXPECT_FALSE(r2.get<bool>());
}

// =============================================================================
// ItemPickupComponent + ItemContainerComponent
// =============================================================================

TEST_F(LuaBindingTest, ItemPickupComponent_PropertyRoundTrip)
{
    ItemPickupComponent ip;
    lua["ip"] = &ip;

    lua.script("ip.pickupRadius = 3.0; ip.autoPickup = true; ip.despawnTimer = 30.0");
    EXPECT_FLOAT_EQ(ip.PickupRadius, 3.0f);
    EXPECT_TRUE(ip.AutoPickup);
    EXPECT_FLOAT_EQ(ip.DespawnTimer, 30.0f);
}

TEST_F(LuaBindingTest, ItemContainerComponent_PropertyRoundTrip)
{
    ItemContainerComponent ic;
    lua["ic"] = &ic;

    lua.script("ic.isShop = true; ic.lootTableID = 'dungeon_chest'; ic.hasBeenLooted = true");
    EXPECT_TRUE(ic.IsShop);
    EXPECT_EQ(ic.LootTableID, "dungeon_chest");
    EXPECT_TRUE(ic.HasBeenLooted);
}

// =============================================================================
// QuestGiverComponent
// =============================================================================

TEST_F(LuaBindingTest, QuestGiverComponent_PropertyRoundTrip)
{
    QuestGiverComponent qg;
    lua["qg"] = &qg;

    lua.script("qg.questMarkerIcon = '!'");
    EXPECT_EQ(qg.QuestMarkerIcon, "!");
}
