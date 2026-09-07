#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_Effects.cpp — particles, probes, nameplates, the IK / animation rigs, wind and streaming.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterEffectsTypes(sol::state& lua)
    {
        // --- ParticleSystemComponent ---
        lua.new_usertype<ParticleSystem>("ParticleSystem",
                                         "playing", &ParticleSystem::Playing,
                                         "looping", &ParticleSystem::Looping,
                                         "duration", sol::property([](const ParticleSystem& ps)
                                                                   { return ps.Duration; }, [](ParticleSystem& ps, f32 v)
                                                                   { if (std::isfinite(v) && v > 0.0f) ps.Duration = v; }),
                                         "playbackSpeed", sol::property([](const ParticleSystem& ps)
                                                                        { return ps.PlaybackSpeed; }, [](ParticleSystem& ps, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) ps.PlaybackSpeed = v; }),
                                         "windInfluence", sol::property([](const ParticleSystem& ps)
                                                                        { return ps.WindInfluence; }, [](ParticleSystem& ps, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) ps.WindInfluence = v; }),
                                         "getAliveCount", &ParticleSystem::GetAliveCount,
                                         "reset", &ParticleSystem::Reset);

        lua.new_usertype<ParticleEmitter>("ParticleEmitter",
                                          "rateOverTime", sol::property([](const ParticleEmitter& e)
                                                                        { return e.RateOverTime; }, [](ParticleEmitter& e, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) e.RateOverTime = v; }),
                                          "initialSpeed", sol::property([](const ParticleEmitter& e)
                                                                        { return e.InitialSpeed; }, [](ParticleEmitter& e, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) e.InitialSpeed = v; }),
                                          "speedVariance", sol::property([](const ParticleEmitter& e)
                                                                         { return e.SpeedVariance; }, [](ParticleEmitter& e, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) e.SpeedVariance = v; }),
                                          "lifetimeMin", sol::property([](const ParticleEmitter& e)
                                                                       { return e.LifetimeMin; }, [](ParticleEmitter& e, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f) { e.LifetimeMin = v; if (e.LifetimeMin > e.LifetimeMax) e.LifetimeMax = e.LifetimeMin; } }),
                                          "lifetimeMax", sol::property([](const ParticleEmitter& e)
                                                                       { return e.LifetimeMax; }, [](ParticleEmitter& e, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f) { e.LifetimeMax = v; if (e.LifetimeMax < e.LifetimeMin) e.LifetimeMin = e.LifetimeMax; } }),
                                          "initialSize", sol::property([](const ParticleEmitter& e)
                                                                       { return e.InitialSize; }, [](ParticleEmitter& e, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f) e.InitialSize = v; }),
                                          "sizeVariance", sol::property([](const ParticleEmitter& e)
                                                                        { return e.SizeVariance; }, [](ParticleEmitter& e, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) e.SizeVariance = v; }),
                                          "initialColor", sol::property([](const ParticleEmitter& e)
                                                                        { return e.InitialColor; }, [](ParticleEmitter& e, const glm::vec4& v)
                                                                        { if (IsFiniteVec4(v)) e.InitialColor = v; }));

        lua.new_usertype<ParticleSystemComponent>("ParticleSystemComponent",
                                                  "system", &ParticleSystemComponent::System);

        // --- LightProbeComponent ---
        lua.new_usertype<LightProbeComponent>("LightProbeComponent",
                                              "influenceRadius", sol::property([](const LightProbeComponent& c)
                                                                               { return c.m_InfluenceRadius; }, [](LightProbeComponent& c, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) c.m_InfluenceRadius = v; }),
                                              "intensity", sol::property([](const LightProbeComponent& c)
                                                                         { return c.m_Intensity; }, [](LightProbeComponent& c, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) c.m_Intensity = v; }),
                                              "active", &LightProbeComponent::m_Active);

        // --- LightProbeVolumeComponent ---
        lua.new_usertype<LightProbeVolumeComponent>("LightProbeVolumeComponent",
                                                    "boundsMin", sol::property([](const LightProbeVolumeComponent& c)
                                                                               { return c.m_BoundsMin; }, [](LightProbeVolumeComponent& c, const glm::vec3& v)
                                                                               { if (IsFiniteVec3(v)) c.m_BoundsMin = v; }),
                                                    "boundsMax", sol::property([](const LightProbeVolumeComponent& c)
                                                                               { return c.m_BoundsMax; }, [](LightProbeVolumeComponent& c, const glm::vec3& v)
                                                                               { if (IsFiniteVec3(v)) c.m_BoundsMax = v; }),
                                                    "spacing", sol::property([](const LightProbeVolumeComponent& c)
                                                                             { return c.m_Spacing; }, [](LightProbeVolumeComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v > 0.0f) c.m_Spacing = v; }),
                                                    "intensity", sol::property([](const LightProbeVolumeComponent& c)
                                                                               { return c.m_Intensity; }, [](LightProbeVolumeComponent& c, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) c.m_Intensity = v; }),
                                                    "active", &LightProbeVolumeComponent::m_Active,
                                                    "dirty", &LightProbeVolumeComponent::m_Dirty,
                                                    "mode", sol::property([](const LightProbeVolumeComponent& c)
                                                                          { return static_cast<int>(c.m_Mode); }, [](LightProbeVolumeComponent& c, int v)
                                                                          { if (v >= 0 && v <= 2) c.m_Mode = static_cast<LightProbeVolumeComponent::Mode>(v); }),
                                                    "raysPerProbe", sol::property([](const LightProbeVolumeComponent& c)
                                                                                  { return c.m_RaysPerProbe; }, [](LightProbeVolumeComponent& c, i32 v)
                                                                                  { if (v >= 1 && v <= 4096) c.m_RaysPerProbe = v; }),
                                                    "hysteresis", sol::property([](const LightProbeVolumeComponent& c)
                                                                                { return c.m_Hysteresis; }, [](LightProbeVolumeComponent& c, f32 v)
                                                                                { if (std::isfinite(v) && v >= 0.0f && v <= 0.98f) c.m_Hysteresis = v; }),
                                                    "probeCaptureBudget", sol::property([](const LightProbeVolumeComponent& c)
                                                                                        { return c.m_ProbeCaptureBudget; }, [](LightProbeVolumeComponent& c, i32 v)
                                                                                        { if (v >= 1 && v <= 64) c.m_ProbeCaptureBudget = v; }),
                                                    "relightBudget", sol::property([](const LightProbeVolumeComponent& c)
                                                                                   { return c.m_RelightBudget; }, [](LightProbeVolumeComponent& c, i32 v)
                                                                                   { if (v >= 0 && v <= 1048576) c.m_RelightBudget = v; }),
                                                    "selfShadowBias", sol::property([](const LightProbeVolumeComponent& c)
                                                                                    { return c.m_SelfShadowBias; }, [](LightProbeVolumeComponent& c, f32 v)
                                                                                    { if (std::isfinite(v) && v >= 0.0f && v <= 4.0f) c.m_SelfShadowBias = v; }),
                                                    "getTotalProbeCount", &LightProbeVolumeComponent::GetTotalProbeCount);

        // --- ReflectionProbeComponent ---
        lua.new_usertype<ReflectionProbeComponent>("ReflectionProbeComponent",
                                                   "influenceRadius", sol::property([](const ReflectionProbeComponent& c)
                                                                                    { return c.m_InfluenceRadius; }, [](ReflectionProbeComponent& c, f32 v)
                                                                                    { if (std::isfinite(v) && v > 0.0f) c.m_InfluenceRadius = v; }),
                                                   "blendDistance", sol::property([](const ReflectionProbeComponent& c)
                                                                                  { return c.m_BlendDistance; }, [](ReflectionProbeComponent& c, f32 v)
                                                                                  { if (std::isfinite(v) && v >= 0.0f) c.m_BlendDistance = v; }),
                                                   "intensity", sol::property([](const ReflectionProbeComponent& c)
                                                                              { return c.m_Intensity; }, [](ReflectionProbeComponent& c, f32 v)
                                                                              { if (std::isfinite(v) && v >= 0.0f) c.m_Intensity = v; }),
                                                   "resolution", sol::property([](const ReflectionProbeComponent& c)
                                                                               { return c.m_Resolution; }, [](ReflectionProbeComponent& c, u32 v)
                                                                               { if (v >= 16 && v <= 2048) c.m_Resolution = v; }),
                                                   "active", &ReflectionProbeComponent::m_Active,
                                                   "needsBake", &ReflectionProbeComponent::m_NeedsBake);

        // --- UIWorldAnchorComponent ---
        lua.new_usertype<UIWorldAnchorComponent>("UIWorldAnchorComponent",
                                                 "targetEntity", sol::property([](const UIWorldAnchorComponent& c)
                                                                               { return static_cast<u64>(c.m_TargetEntity); }, [](UIWorldAnchorComponent& c, u64 id)
                                                                               { c.m_TargetEntity = UUID(id); }),
                                                 "worldOffset", sol::property([](const UIWorldAnchorComponent& c)
                                                                              { return c.m_WorldOffset; }, [](UIWorldAnchorComponent& c, const glm::vec3& v)
                                                                              { if (IsFiniteVec3(v)) c.m_WorldOffset = v; }));

        // --- NameplateComponent ---
        lua.new_usertype<NameplateComponent>("NameplateComponent",
                                             "enabled", &NameplateComponent::m_Enabled,
                                             "showHealthBar", &NameplateComponent::m_ShowHealthBar,
                                             "showManaBar", &NameplateComponent::m_ShowManaBar,
                                             "worldOffset", sol::property([](const NameplateComponent& c)
                                                                          { return c.m_WorldOffset; }, [](NameplateComponent& c, const glm::vec3& v)
                                                                          { if (IsFiniteVec3(v)) c.m_WorldOffset = v; }),
                                             "barSize", sol::property([](const NameplateComponent& c)
                                                                      { return c.m_BarSize; }, [](NameplateComponent& c, const glm::vec2& v)
                                                                      { if (IsFiniteVec2(v) && v.x >= 0.0f && v.y >= 0.0f) c.m_BarSize = v; }),
                                             "healthBarColor", sol::property([](const NameplateComponent& c)
                                                                             { return c.m_HealthBarColor; }, [](NameplateComponent& c, const glm::vec4& v)
                                                                             { if (IsFiniteVec4(v)) c.m_HealthBarColor = v; }),
                                             "manaBarColor", sol::property([](const NameplateComponent& c)
                                                                           { return c.m_ManaBarColor; }, [](NameplateComponent& c, const glm::vec4& v)
                                                                           { if (IsFiniteVec4(v)) c.m_ManaBarColor = v; }),
                                             "barBackgroundColor", sol::property([](const NameplateComponent& c)
                                                                                 { return c.m_BarBackgroundColor; }, [](NameplateComponent& c, const glm::vec4& v)
                                                                                 { if (IsFiniteVec4(v)) c.m_BarBackgroundColor = v; }),
                                             "manaBarGap", sol::property([](const NameplateComponent& c)
                                                                         { return c.m_ManaBarGap; }, [](NameplateComponent& c, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) c.m_ManaBarGap = v; }));

        // --- IKTargetComponent ---
        lua.new_usertype<IKTargetComponent>("IKTargetComponent",
                                            "aimIKEnabled", &IKTargetComponent::AimIKEnabled,
                                            "aimBoneIndex", &IKTargetComponent::AimBoneIndex,
                                            "aimTarget", sol::property([](const IKTargetComponent& c)
                                                                       { return c.AimTarget; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                       { if (IsFiniteVec3(v)) c.AimTarget = v; }),
                                            "aimAxis", sol::property([](const IKTargetComponent& c)
                                                                     { return c.AimAxis; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                     { if (IsFiniteVec3(v)) c.AimAxis = v; }),
                                            "aimOffset", sol::property([](const IKTargetComponent& c)
                                                                       { return c.AimOffset; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                       { if (IsFiniteVec3(v)) c.AimOffset = v; }),
                                            "aimPoleVector", sol::property([](const IKTargetComponent& c)
                                                                           { return c.AimPoleVector; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                           { if (IsFiniteVec3(v)) c.AimPoleVector = v; }),
                                            "aimChainLength", &IKTargetComponent::AimChainLength,
                                            "aimChainFactor", sol::property([](const IKTargetComponent& c)
                                                                            { return c.AimChainFactor; }, [](IKTargetComponent& c, f32 v)
                                                                            { if (std::isfinite(v)) c.AimChainFactor = std::clamp(v, 0.0f, 1.0f); }),
                                            "aimWeight", sol::property([](const IKTargetComponent& c)
                                                                       { return c.AimWeight; }, [](IKTargetComponent& c, f32 v)
                                                                       { if (std::isfinite(v)) c.AimWeight = std::clamp(v, 0.0f, 1.0f); }),
                                            "aimTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                             { return static_cast<u64>(c.AimTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                             { c.AimTargetEntity = UUID(id); }),
                                            "limbIKEnabled", &IKTargetComponent::LimbIKEnabled,
                                            "limbBoneIndex", &IKTargetComponent::LimbBoneIndex,
                                            "limbTarget", sol::property([](const IKTargetComponent& c)
                                                                        { return c.LimbTarget; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                        { if (IsFiniteVec3(v)) c.LimbTarget = v; }),
                                            "limbChainLength", &IKTargetComponent::LimbChainLength,
                                            "limbWeight", sol::property([](const IKTargetComponent& c)
                                                                        { return c.LimbWeight; }, [](IKTargetComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.LimbWeight = std::clamp(v, 0.0f, 1.0f); }),
                                            "limbTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                              { return static_cast<u64>(c.LimbTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                              { c.LimbTargetEntity = UUID(id); }),
                                            "chainIKEnabled", &IKTargetComponent::ChainIKEnabled,
                                            "chainBoneIndex", &IKTargetComponent::ChainBoneIndex,
                                            "chainTarget", sol::property([](const IKTargetComponent& c)
                                                                         { return c.ChainTarget; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                         { if (IsFiniteVec3(v)) c.ChainTarget = v; }),
                                            "chainPoleVector", sol::property([](const IKTargetComponent& c)
                                                                             { return c.ChainPoleVector; }, [](IKTargetComponent& c, const glm::vec3& v)
                                                                             { if (IsFiniteVec3(v)) c.ChainPoleVector = v; }),
                                            "chainLength", sol::property([](const IKTargetComponent& c)
                                                                         { return c.ChainLength; }, [](IKTargetComponent& c, u32 v)
                                                                         { c.ChainLength = std::max(2u, v); }),
                                            "chainIterations", sol::property([](const IKTargetComponent& c)
                                                                             { return c.ChainIterations; }, [](IKTargetComponent& c, u32 v)
                                                                             { c.ChainIterations = std::clamp(v, 1u, 128u); }),
                                            "chainTolerance", sol::property([](const IKTargetComponent& c)
                                                                            { return c.ChainTolerance; }, [](IKTargetComponent& c, f32 v)
                                                                            { if (std::isfinite(v)) c.ChainTolerance = std::clamp(v, 0.0f, 10.0f); }),
                                            "chainWeight", sol::property([](const IKTargetComponent& c)
                                                                         { return c.ChainWeight; }, [](IKTargetComponent& c, f32 v)
                                                                         { if (std::isfinite(v)) c.ChainWeight = std::clamp(v, 0.0f, 1.0f); }),
                                            "chainTargetEntity", sol::property([](const IKTargetComponent& c)
                                                                               { return static_cast<u64>(c.ChainTargetEntity); }, [](IKTargetComponent& c, u64 id)
                                                                               { c.ChainTargetEntity = UUID(id); }));

        // --- SpringBoneComponent ---
        lua.new_usertype<SpringBoneComponent>("SpringBoneComponent",
                                              "enabled", &SpringBoneComponent::Enabled,
                                              "endBoneIndex", &SpringBoneComponent::EndBoneIndex,
                                              "chainLength", &SpringBoneComponent::ChainLength,
                                              "stiffness", sol::property([](const SpringBoneComponent& c)
                                                                         { return c.Stiffness; }, [](SpringBoneComponent& c, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) c.Stiffness = v; }),
                                              "damping", sol::property([](const SpringBoneComponent& c)
                                                                       { return c.Damping; }, [](SpringBoneComponent& c, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f) c.Damping = v; }),
                                              "gravity", sol::property([](const SpringBoneComponent& c)
                                                                       { return c.Gravity; }, [](SpringBoneComponent& c, const glm::vec3& v)
                                                                       { if (IsFiniteVec3(v)) c.Gravity = v; }),
                                              "weight", sol::property([](const SpringBoneComponent& c)
                                                                      { return c.Weight; }, [](SpringBoneComponent& c, f32 v)
                                                                      { if (std::isfinite(v)) c.Weight = std::clamp(v, 0.0f, 1.0f); }));

        // --- LocomotionComponent (issue #631) ---
        // Scripts steer a root-motion character by setting desiredVelocity and
        // useDesiredVelocity; everything else is authored tuning.
        lua.new_usertype<LocomotionComponent>("LocomotionComponent",
                                              "enabled", &LocomotionComponent::Enabled,
                                              "useDesiredVelocity", &LocomotionComponent::UseDesiredVelocity,
                                              "desiredVelocity", sol::property([](const LocomotionComponent& c)
                                                                               { return c.DesiredVelocity; }, [](LocomotionComponent& c, const glm::vec3& v)
                                                                               { if (IsFiniteVec3(v)) c.DesiredVelocity = v; }),
                                              "strideWarp", &LocomotionComponent::StrideWarp,
                                              "walkEnterSpeed", sol::property([](const LocomotionComponent& c)
                                                                              { return c.WalkEnterSpeed; }, [](LocomotionComponent& c, f32 v)
                                                                              { if (std::isfinite(v) && v >= 0.0f) c.WalkEnterSpeed = v; }),
                                              "runEnterSpeed", sol::property([](const LocomotionComponent& c)
                                                                             { return c.RunEnterSpeed; }, [](LocomotionComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) c.RunEnterSpeed = v; }));

        // --- FootIKComponent (issue #631) ---
        // Ground-adaptation knobs + hand IK targets; runtime state stays native.
        lua.new_usertype<FootIKComponent>("FootIKComponent",
                                          "enabled", &FootIKComponent::Enabled,
                                          "leftFootBone", &FootIKComponent::LeftFootBone,
                                          "rightFootBone", &FootIKComponent::RightFootBone,
                                          "footLock", &FootIKComponent::FootLock,
                                          "adjustPelvis", &FootIKComponent::AdjustPelvis,
                                          "alignFootToSlope", &FootIKComponent::AlignFootToSlope,
                                          "weight", sol::property([](const FootIKComponent& c)
                                                                  { return c.Weight; }, [](FootIKComponent& c, f32 v)
                                                                  { if (std::isfinite(v)) c.Weight = std::clamp(v, 0.0f, 1.0f); }),
                                          "leftHandEnabled", &FootIKComponent::LeftHandEnabled,
                                          "leftHandTarget", sol::property([](const FootIKComponent& c)
                                                                          { return c.LeftHandTarget; }, [](FootIKComponent& c, const glm::vec3& v)
                                                                          { if (IsFiniteVec3(v)) c.LeftHandTarget = v; }),
                                          "leftHandTargetEntity", sol::property([](const FootIKComponent& c)
                                                                                { return static_cast<u64>(c.LeftHandTargetEntity); }, [](FootIKComponent& c, u64 v)
                                                                                { c.LeftHandTargetEntity = UUID(v); }),
                                          "rightHandEnabled", &FootIKComponent::RightHandEnabled,
                                          "rightHandTarget", sol::property([](const FootIKComponent& c)
                                                                           { return c.RightHandTarget; }, [](FootIKComponent& c, const glm::vec3& v)
                                                                           { if (IsFiniteVec3(v)) c.RightHandTarget = v; }),
                                          "rightHandTargetEntity", sol::property([](const FootIKComponent& c)
                                                                                 { return static_cast<u64>(c.RightHandTargetEntity); }, [](FootIKComponent& c, u64 v)
                                                                                 { c.RightHandTargetEntity = UUID(v); }),
                                          "handWeight", sol::property([](const FootIKComponent& c)
                                                                      { return c.HandWeight; }, [](FootIKComponent& c, f32 v)
                                                                      { if (std::isfinite(v)) c.HandWeight = std::clamp(v, 0.0f, 1.0f); }));

        // --- RetargetingComponent (issue #631) ---
        // Authored settings only; the baked-clip cache is runtime state. UUID
        // bridged as u64 like the IKTargetComponent target entities.
        lua.new_usertype<RetargetingComponent>("RetargetingComponent",
                                               "enabled", &RetargetingComponent::Enabled,
                                               "sourcePath", &RetargetingComponent::m_SourcePath,
                                               "sourceEntity", sol::property([](const RetargetingComponent& c)
                                                                             { return static_cast<u64>(c.m_SourceEntity); }, [](RetargetingComponent& c, u64 v)
                                                                             { c.m_SourceEntity = UUID(v); }),
                                               "useHumanoidRoles", &RetargetingComponent::UseHumanoidRoles,
                                               "perBoneTranslation", &RetargetingComponent::PerBoneTranslation,
                                               "transferRootTranslation", &RetargetingComponent::TransferRootTranslation,
                                               "rootTranslationScale", sol::property([](const RetargetingComponent& c)
                                                                                     { return c.RootTranslationScale; }, [](RetargetingComponent& c, f32 v)
                                                                                     { if (std::isfinite(v)) c.RootTranslationScale = std::clamp(v, 0.0f, 1000.0f); }));

        // --- NoiseAnimationComponent ---
        lua.new_usertype<NoiseAnimationComponent>("NoiseAnimationComponent",
                                                  "enabled", &NoiseAnimationComponent::Enabled,
                                                  "endBoneIndex", &NoiseAnimationComponent::EndBoneIndex,
                                                  "chainLength", &NoiseAnimationComponent::ChainLength,
                                                  "frequency", sol::property([](const NoiseAnimationComponent& c)
                                                                             { return c.Frequency; }, [](NoiseAnimationComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) c.Frequency = v; }),
                                                  "rotationAmplitude", sol::property([](const NoiseAnimationComponent& c)
                                                                                     { return c.RotationAmplitude; }, [](NoiseAnimationComponent& c, const glm::vec3& v)
                                                                                     { if (IsFiniteVec3(v)) c.RotationAmplitude = v; }),
                                                  "translationAmplitude", sol::property([](const NoiseAnimationComponent& c)
                                                                                        { return c.TranslationAmplitude; }, [](NoiseAnimationComponent& c, const glm::vec3& v)
                                                                                        { if (IsFiniteVec3(v)) c.TranslationAmplitude = v; }),
                                                  "octaves", sol::property([](const NoiseAnimationComponent& c)
                                                                           { return c.Octaves; }, [](NoiseAnimationComponent& c, u32 v)
                                                                           { c.Octaves = std::clamp(v, 1u, 8u); }),
                                                  "lacunarity", sol::property([](const NoiseAnimationComponent& c)
                                                                              { return c.Lacunarity; }, [](NoiseAnimationComponent& c, f32 v)
                                                                              { if (std::isfinite(v)) c.Lacunarity = std::clamp(v, 1.0f, 8.0f); }),
                                                  "gain", sol::property([](const NoiseAnimationComponent& c)
                                                                        { return c.Gain; }, [](NoiseAnimationComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.Gain = std::clamp(v, 0.0f, 1.0f); }),
                                                  "seed", &NoiseAnimationComponent::Seed,
                                                  "weight", sol::property([](const NoiseAnimationComponent& c)
                                                                          { return c.Weight; }, [](NoiseAnimationComponent& c, f32 v)
                                                                          { if (std::isfinite(v)) c.Weight = std::clamp(v, 0.0f, 1.0f); }));

        // --- WindSettings (scene-level) ---
        lua.new_usertype<WindSettings>("WindSettings",
                                       "enabled", &WindSettings::Enabled,
                                       "direction", sol::property([](const WindSettings& w)
                                                                  { return w.Direction; }, [](WindSettings& w, const glm::vec3& v)
                                                                  { if (IsFiniteVec3(v)) w.Direction = v; }),
                                       "speed", sol::property([](const WindSettings& w)
                                                              { return w.Speed; }, [](WindSettings& w, f32 v)
                                                              { if (std::isfinite(v) && v >= 0.0f) w.Speed = v; }),
                                       "gustStrength", sol::property([](const WindSettings& w)
                                                                     { return w.GustStrength; }, [](WindSettings& w, f32 v)
                                                                     { if (std::isfinite(v) && v >= 0.0f) w.GustStrength = v; }),
                                       "gustFrequency", sol::property([](const WindSettings& w)
                                                                      { return w.GustFrequency; }, [](WindSettings& w, f32 v)
                                                                      { if (std::isfinite(v) && v >= 0.0f) w.GustFrequency = v; }),
                                       "turbulenceIntensity", sol::property([](const WindSettings& w)
                                                                            { return w.TurbulenceIntensity; }, [](WindSettings& w, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f) w.TurbulenceIntensity = v; }),
                                       "turbulenceScale", sol::property([](const WindSettings& w)
                                                                        { return w.TurbulenceScale; }, [](WindSettings& w, f32 v)
                                                                        { if (std::isfinite(v) && v > 0.0f) w.TurbulenceScale = v; }),
                                       "gridWorldSize", sol::property([](const WindSettings& w)
                                                                      { return w.GridWorldSize; }, [](WindSettings& w, f32 v)
                                                                      { if (std::isfinite(v) && v > 0.0f) w.GridWorldSize = v; }),
                                       "gridResolution", &WindSettings::GridResolution);

        // --- StreamingVolumeComponent ---
        lua.new_usertype<StreamingVolumeComponent>("StreamingVolumeComponent",
                                                   "loadRadius", sol::property([](const StreamingVolumeComponent& c)
                                                                               { return c.LoadRadius; }, [](StreamingVolumeComponent& c, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) c.LoadRadius = v; }),
                                                   "unloadRadius", sol::property([](const StreamingVolumeComponent& c)
                                                                                 { return c.UnloadRadius; }, [](StreamingVolumeComponent& c, f32 v)
                                                                                 { if (std::isfinite(v) && v >= 0.0f) c.UnloadRadius = v; }),
                                                   "isLoaded", sol::readonly(&StreamingVolumeComponent::IsLoaded));

        // --- StreamingSettings (scene-level) ---
        lua.new_usertype<StreamingSettings>("StreamingSettings",
                                            "enabled", &StreamingSettings::Enabled,
                                            "defaultLoadRadius", sol::property([](const StreamingSettings& s)
                                                                               { return s.DefaultLoadRadius; }, [](StreamingSettings& s, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) s.DefaultLoadRadius = v; }),
                                            "defaultUnloadRadius", sol::property([](const StreamingSettings& s)
                                                                                 { return s.DefaultUnloadRadius; }, [](StreamingSettings& s, f32 v)
                                                                                 { if (std::isfinite(v) && v >= 0.0f) s.DefaultUnloadRadius = v; }),
                                            "maxLoadedRegions", &StreamingSettings::MaxLoadedRegions,
                                            "regionDirectory", &StreamingSettings::RegionDirectory);

        // --- NetworkIdentityComponent ---
        lua.new_usertype<NetworkIdentityComponent>("NetworkIdentityComponent",
                                                   "ownerClientID", &NetworkIdentityComponent::OwnerClientID,
                                                   "authority", sol::property([](const NetworkIdentityComponent& c) -> int
                                                                              { return static_cast<int>(std::to_underlying(c.Authority)); }, [](NetworkIdentityComponent& c, int v)
                                                                              { if (v >= 0 && v <= 2) c.Authority = static_cast<ENetworkAuthority>(v); }),
                                                   "isReplicated", &NetworkIdentityComponent::IsReplicated);
    }
} // namespace OloEngine
