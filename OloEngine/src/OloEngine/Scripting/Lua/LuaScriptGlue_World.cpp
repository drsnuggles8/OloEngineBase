#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_World.cpp — dialogue, visual scripting, animation graphs, materials, lights, sky and weather, navigation and the player rigs.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterWorldTypes(sol::state& lua)
    {
        // --- DialogueComponent ---
        lua.new_usertype<DialogueComponent>("DialogueComponent",
                                            "dialogueTree", &DialogueComponent::m_DialogueTree,
                                            "autoTrigger", &DialogueComponent::m_AutoTrigger,
                                            "triggerRadius", sol::property([](const DialogueComponent& c)
                                                                           { return c.m_TriggerRadius; }, [](DialogueComponent& c, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) c.m_TriggerRadius = v; }),
                                            "hasTriggered", &DialogueComponent::m_HasTriggered,
                                            "triggerOnce", &DialogueComponent::m_TriggerOnce);

        // --- VisualScriptComponent (issue #634) ---
        // Only the authoring fields are exposed. The live blackboard is reached
        // through the `visual_script` table below, not through the component:
        // the values live on the per-scene VisualScriptSystem, and handing Lua a
        // reference into the component's override map would let a script mutate
        // the AUTHORED defaults at runtime (which then persist into a save).
        lua.new_usertype<VisualScriptComponent>("VisualScriptComponent",
                                                "graph", &VisualScriptComponent::m_Graph,
                                                "enabled", &VisualScriptComponent::m_Enabled);

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

        // --- CinematicComponent ---
        lua.new_usertype<CinematicComponent>("CinematicComponent",
                                             "Play", &CinematicComponent::Play,
                                             "PlayFromStart", &CinematicComponent::PlayFromStart,
                                             "Pause", &CinematicComponent::Pause,
                                             "Stop", &CinematicComponent::Stop,
                                             "loop", &CinematicComponent::Loop,
                                             "playOnStart", &CinematicComponent::PlayOnStart,
                                             "playbackSpeed", sol::property([](const CinematicComponent& c)
                                                                            { return c.PlaybackSpeed; }, [](CinematicComponent& c, f32 v)
                                                                            { if (std::isfinite(v)) c.PlaybackSpeed = v; }), // negative = reverse playback, 0 = hold
                                             "isPlaying", sol::readonly(&CinematicComponent::Playing),
                                             "time", sol::readonly(&CinematicComponent::Time),
                                             "isFinished", sol::readonly(&CinematicComponent::Finished));

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
                                                        if (!Project::GetActive())
                                                        {
                                                            OLO_CORE_WARN("[Lua] Cannot validate ShaderGraph handle {} — no active project", handle);
                                                            return;
                                                        }
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
                                                }),
                                            "albedoColor",
                                            sol::property(
                                                [](const MaterialComponent& mc) -> glm::vec4
                                                { return mc.m_Material.GetBaseColorFactor(); },
                                                [](MaterialComponent& mc, const glm::vec4& color)
                                                {
                                                    if (!IsFiniteVec4(color))
                                                        return;
                                                    mc.m_Material.SetBaseColorFactor(color);
                                                }),
                                            // Versioned PBR closure (issue #975): 0=Legacy, 1=ClosureV2.
                                            // A discriminated value: out-of-range writes are rejected,
                                            // never saturated to a different valid model.
                                            "pbrModel",
                                            sol::property(
                                                [](const MaterialComponent& mc) -> int
                                                { return static_cast<int>(mc.m_Material.GetPBRModel()); },
                                                [](MaterialComponent& mc, int model)
                                                {
                                                    if (model < 0 || model >= kPBRModelCount)
                                                    {
                                                        OLO_CORE_WARN("[Lua] MaterialComponent.pbrModel rejects {} (valid: 0=Legacy, 1=ClosureV2)", model);
                                                        return;
                                                    }
                                                    mc.m_Material.SetPBRModel(static_cast<PBRModel>(model));
                                                }));

        // --- DirectionalLightComponent ---
        lua.new_usertype<DirectionalLightComponent>("DirectionalLightComponent",
                                                    "direction", sol::property([](const DirectionalLightComponent& l)
                                                                               { return l.m_Direction; }, [](DirectionalLightComponent& l, const glm::vec3& v)
                                                                               { if (IsFiniteVec3(v)) l.m_Direction = v; }),
                                                    "color", sol::property([](const DirectionalLightComponent& l)
                                                                           { return l.m_Color; }, [](DirectionalLightComponent& l, const glm::vec3& v)
                                                                           { if (IsFiniteVec3(v)) l.m_Color = v; }),
                                                    "intensity", sol::property([](const DirectionalLightComponent& l)
                                                                               { return l.m_Intensity; }, [](DirectionalLightComponent& l, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) l.m_Intensity = v; }),
                                                    "castShadows", &DirectionalLightComponent::m_CastShadows,
                                                    "rayTracedShadows", &DirectionalLightComponent::m_RayTracedShadows,
                                                    "shadowBias", sol::property([](const DirectionalLightComponent& l)
                                                                                { return l.m_ShadowBias; }, [](DirectionalLightComponent& l, f32 v)
                                                                                { if (std::isfinite(v) && v >= 0.0f) l.m_ShadowBias = v; }),
                                                    "shadowNormalBias", sol::property([](const DirectionalLightComponent& l)
                                                                                      { return l.m_ShadowNormalBias; }, [](DirectionalLightComponent& l, f32 v)
                                                                                      { if (std::isfinite(v) && v >= 0.0f) l.m_ShadowNormalBias = v; }),
                                                    "maxShadowDistance", sol::property([](const DirectionalLightComponent& l)
                                                                                       { return l.m_MaxShadowDistance; }, [](DirectionalLightComponent& l, f32 v)
                                                                                       { if (std::isfinite(v) && v > 0.0f) l.m_MaxShadowDistance = v; }),
                                                    "cascadeSplitLambda", sol::property([](const DirectionalLightComponent& l)
                                                                                        { return l.m_CascadeSplitLambda; }, [](DirectionalLightComponent& l, f32 v)
                                                                                        { if (std::isfinite(v)) l.m_CascadeSplitLambda = std::clamp(v, 0.0f, 1.0f); }),
                                                    "cascadeDebugVisualization", &DirectionalLightComponent::m_CascadeDebugVisualization);

        // --- PointLightComponent ---
        lua.new_usertype<PointLightComponent>("PointLightComponent",
                                              "color", sol::property([](const PointLightComponent& l)
                                                                     { return l.m_Color; }, [](PointLightComponent& l, const glm::vec3& v)
                                                                     { if (IsFiniteVec3(v)) l.m_Color = v; }),
                                              "intensity", sol::property([](const PointLightComponent& l)
                                                                         { return l.m_Intensity; }, [](PointLightComponent& l, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) l.m_Intensity = v; }),
                                              "range", sol::property([](const PointLightComponent& l)
                                                                     { return l.m_Range; }, [](PointLightComponent& l, f32 v)
                                                                     { if (std::isfinite(v) && v >= 0.0f) l.m_Range = v; }),
                                              "attenuation", sol::property([](const PointLightComponent& l)
                                                                           { return l.m_Attenuation; }, [](PointLightComponent& l, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) l.m_Attenuation = v; }),
                                              "castShadows", &PointLightComponent::m_CastShadows,
                                              "rayTracedShadows", &PointLightComponent::m_RayTracedShadows,
                                              "shadowBias", sol::property([](const PointLightComponent& l)
                                                                          { return l.m_ShadowBias; }, [](PointLightComponent& l, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) l.m_ShadowBias = v; }),
                                              "shadowNormalBias", sol::property([](const PointLightComponent& l)
                                                                                { return l.m_ShadowNormalBias; }, [](PointLightComponent& l, f32 v)
                                                                                { if (std::isfinite(v) && v >= 0.0f) l.m_ShadowNormalBias = v; }));

        // --- SpotLightComponent ---
        lua.new_usertype<SpotLightComponent>("SpotLightComponent",
                                             "direction", sol::property([](const SpotLightComponent& l)
                                                                        { return l.m_Direction; }, [](SpotLightComponent& l, const glm::vec3& v)
                                                                        { if (IsFiniteVec3(v)) l.m_Direction = v; }),
                                             "color", sol::property([](const SpotLightComponent& l)
                                                                    { return l.m_Color; }, [](SpotLightComponent& l, const glm::vec3& v)
                                                                    { if (IsFiniteVec3(v)) l.m_Color = v; }),
                                             "intensity", sol::property([](const SpotLightComponent& l)
                                                                        { return l.m_Intensity; }, [](SpotLightComponent& l, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) l.m_Intensity = v; }),
                                             "range", sol::property([](const SpotLightComponent& l)
                                                                    { return l.m_Range; }, [](SpotLightComponent& l, f32 v)
                                                                    { if (std::isfinite(v) && v >= 0.0f) l.m_Range = v; }),
                                             "innerCutoff", sol::property([](const SpotLightComponent& l)
                                                                          { return l.m_InnerCutoff; }, [](SpotLightComponent& l, f32 v)
                                                                          { if (std::isfinite(v)) l.m_InnerCutoff = std::clamp(v, 0.0f, 180.0f); }),
                                             "outerCutoff", sol::property([](const SpotLightComponent& l)
                                                                          { return l.m_OuterCutoff; }, [](SpotLightComponent& l, f32 v)
                                                                          { if (std::isfinite(v)) l.m_OuterCutoff = std::clamp(v, 0.0f, 180.0f); }),
                                             "attenuation", sol::property([](const SpotLightComponent& l)
                                                                          { return l.m_Attenuation; }, [](SpotLightComponent& l, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) l.m_Attenuation = v; }),
                                             "castShadows", &SpotLightComponent::m_CastShadows,
                                             "rayTracedShadows", &SpotLightComponent::m_RayTracedShadows,
                                             "shadowBias", sol::property([](const SpotLightComponent& l)
                                                                         { return l.m_ShadowBias; }, [](SpotLightComponent& l, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) l.m_ShadowBias = v; }),
                                             "shadowNormalBias", sol::property([](const SpotLightComponent& l)
                                                                               { return l.m_ShadowNormalBias; }, [](SpotLightComponent& l, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) l.m_ShadowNormalBias = v; }));

        // --- SphereAreaLightComponent ---
        lua.new_usertype<SphereAreaLightComponent>("SphereAreaLightComponent",
                                                   "color", sol::property([](const SphereAreaLightComponent& l)
                                                                          { return l.m_Color; }, [](SphereAreaLightComponent& l, const glm::vec3& v)
                                                                          { if (IsFiniteVec3(v)) l.m_Color = v; }),
                                                   "intensity", sol::property([](const SphereAreaLightComponent& l)
                                                                              { return l.m_Intensity; }, [](SphereAreaLightComponent& l, f32 v)
                                                                              { if (std::isfinite(v) && v >= 0.0f) l.m_Intensity = v; }),
                                                   "radius", sol::property([](const SphereAreaLightComponent& l)
                                                                           { return l.m_Radius; }, [](SphereAreaLightComponent& l, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) l.m_Radius = v; }),
                                                   "range", sol::property([](const SphereAreaLightComponent& l)
                                                                          { return l.m_Range; }, [](SphereAreaLightComponent& l, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) l.m_Range = v; }),
                                                   "castShadows", &SphereAreaLightComponent::m_CastShadows,
                                                   "rayTracedShadows", &SphereAreaLightComponent::m_RayTracedShadows);

        // --- ProceduralSkyComponent ---
        // Bake hash + cached EnvironmentMap stay internal: script writes mark
        // the component dirty by leaving m_LastBakeHash untouched (LoadAndRenderSkybox
        // detects the change via HashParameters mismatch).
        lua.new_usertype<ProceduralSkyComponent>("ProceduralSkyComponent",
                                                 "sunDirection", sol::property([](const ProceduralSkyComponent& s)
                                                                               { return s.m_SunDirection; }, [](ProceduralSkyComponent& s, const glm::vec3& v)
                                                                               { if (IsFiniteVec3(v)) s.m_SunDirection = v; }),
                                                 "turbidity", sol::property([](const ProceduralSkyComponent& s)
                                                                            { return s.m_Turbidity; }, [](ProceduralSkyComponent& s, f32 v)
                                                                            { if (std::isfinite(v) && v > 0.0f) s.m_Turbidity = v; }),
                                                 "exposure", sol::property([](const ProceduralSkyComponent& s)
                                                                           { return s.m_Exposure; }, [](ProceduralSkyComponent& s, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) s.m_Exposure = v; }),
                                                 "sunIntensity", sol::property([](const ProceduralSkyComponent& s)
                                                                               { return s.m_SunIntensity; }, [](ProceduralSkyComponent& s, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) s.m_SunIntensity = v; }),
                                                 "sunDiskSize", sol::property([](const ProceduralSkyComponent& s)
                                                                              { return s.m_SunDiskSize; }, [](ProceduralSkyComponent& s, f32 v)
                                                                              { if (std::isfinite(v) && v > 0.0f) s.m_SunDiskSize = v; }),
                                                 "showSunDisk", &ProceduralSkyComponent::m_ShowSunDisk,
                                                 "enableSkybox", &ProceduralSkyComponent::m_EnableSkybox,
                                                 "enableIBL", &ProceduralSkyComponent::m_EnableIBL,
                                                 "iblIntensity", sol::property([](const ProceduralSkyComponent& s)
                                                                               { return s.m_IBLIntensity; }, [](ProceduralSkyComponent& s, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f) s.m_IBLIntensity = v; }));

        // --- StarNestSkyComponent ---
        // Raymarched nebula sky (issue #292). Bake hash + cached EnvironmentMap
        // stay internal: a script write leaves m_LastBakeHash untouched, so
        // LoadAndRenderSkybox rebakes on the next tick via the hash mismatch.
        // Finite-guard every float so a bad script value can't poison the bake.
        lua.new_usertype<StarNestSkyComponent>("StarNestSkyComponent",
                                               "offset", sol::property([](const StarNestSkyComponent& s)
                                                                       { return s.m_Offset; }, [](StarNestSkyComponent& s, const glm::vec3& v)
                                                                       { if (IsFiniteVec3(v)) s.m_Offset = v; }),
                                               "rotation1", sol::property([](const StarNestSkyComponent& s)
                                                                          { return s.m_Rotation1; }, [](StarNestSkyComponent& s, f32 v)
                                                                          { if (std::isfinite(v)) s.m_Rotation1 = v; }),
                                               "rotation2", sol::property([](const StarNestSkyComponent& s)
                                                                          { return s.m_Rotation2; }, [](StarNestSkyComponent& s, f32 v)
                                                                          { if (std::isfinite(v)) s.m_Rotation2 = v; }),
                                               "formuparam", sol::property([](const StarNestSkyComponent& s)
                                                                           { return s.m_Formuparam; }, [](StarNestSkyComponent& s, f32 v)
                                                                           { if (std::isfinite(v)) s.m_Formuparam = v; }),
                                               "stepSize", sol::property([](const StarNestSkyComponent& s)
                                                                         { return s.m_StepSize; }, [](StarNestSkyComponent& s, f32 v)
                                                                         { if (std::isfinite(v) && v > 0.0f) s.m_StepSize = v; }),
                                               "tile", sol::property([](const StarNestSkyComponent& s)
                                                                     { return s.m_Tile; }, [](StarNestSkyComponent& s, f32 v)
                                                                     { if (std::isfinite(v) && v > 0.0f) s.m_Tile = v; }),
                                               "brightness", sol::property([](const StarNestSkyComponent& s)
                                                                           { return s.m_Brightness; }, [](StarNestSkyComponent& s, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) s.m_Brightness = v; }),
                                               "darkMatter", sol::property([](const StarNestSkyComponent& s)
                                                                           { return s.m_DarkMatter; }, [](StarNestSkyComponent& s, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) s.m_DarkMatter = v; }),
                                               "distFading", sol::property([](const StarNestSkyComponent& s)
                                                                           { return s.m_DistFading; }, [](StarNestSkyComponent& s, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) s.m_DistFading = v; }),
                                               "saturation", sol::property([](const StarNestSkyComponent& s)
                                                                           { return s.m_Saturation; }, [](StarNestSkyComponent& s, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) s.m_Saturation = v; }),
                                               "intensity", sol::property([](const StarNestSkyComponent& s)
                                                                          { return s.m_Intensity; }, [](StarNestSkyComponent& s, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) s.m_Intensity = v; }),
                                               "iterations", sol::property([](const StarNestSkyComponent& s)
                                                                           { return s.m_Iterations; }, [](StarNestSkyComponent& s, i32 v)
                                                                           { s.m_Iterations = std::clamp(v, 1, kStarNestMaxIterations); }),
                                               "volSteps", sol::property([](const StarNestSkyComponent& s)
                                                                         { return s.m_VolSteps; }, [](StarNestSkyComponent& s, i32 v)
                                                                         { s.m_VolSteps = std::clamp(v, 1, kStarNestMaxVolSteps); }),
                                               "enableSkybox", &StarNestSkyComponent::m_EnableSkybox,
                                               "enableIBL", &StarNestSkyComponent::m_EnableIBL,
                                               "iblIntensity", sol::property([](const StarNestSkyComponent& s)
                                                                             { return s.m_IBLIntensity; }, [](StarNestSkyComponent& s, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) s.m_IBLIntensity = v; }));

        // --- TimeOfDayComponent ---
        // Authored clock/ephemeris inputs plus the read-only derived outputs
        // (sun/moon directions, elevation, is-night) rewritten by
        // TimeOfDaySystem::Apply every frame. Setter clamps mirror the
        // OLO_SERIALIZE(Clamp) ranges in Components.h; timeOfDayHours WRAPS
        // (a clock, not a slider) so "hours + 25" from a script keeps ticking
        // across midnight instead of pinning at 24.
        lua.new_usertype<TimeOfDayComponent>("TimeOfDayComponent",
                                             "enabled", &TimeOfDayComponent::m_Enabled,
                                             "timeOfDayHours", sol::property([](const TimeOfDayComponent& t)
                                                                             { return t.m_TimeOfDayHours; }, [](TimeOfDayComponent& t, f32 v)
                                                                             {
                                                    if (!std::isfinite(v))
                                                        return;
                                                    f32 wrapped = std::fmod(v, 24.0f);
                                                    if (wrapped < 0.0f)
                                                        wrapped += 24.0f;
                                                    if (wrapped >= 24.0f) // float rounding on a tiny negative can land exactly on 24
                                                        wrapped = 0.0f;
                                                    t.m_TimeOfDayHours = wrapped; }),
                                             "dayOfYear", sol::property([](const TimeOfDayComponent& t)
                                                                        { return t.m_DayOfYear; }, [](TimeOfDayComponent& t, i32 v)
                                                                        { t.m_DayOfYear = std::clamp(v, 1, 365); }),
                                             "latitudeDegrees", sol::property([](const TimeOfDayComponent& t)
                                                                              { return t.m_LatitudeDegrees; }, [](TimeOfDayComponent& t, f32 v)
                                                                              { if (std::isfinite(v)) t.m_LatitudeDegrees = std::clamp(v, -90.0f, 90.0f); }),
                                             "dayLengthMinutes", sol::property([](const TimeOfDayComponent& t)
                                                                               { return t.m_DayLengthMinutes; }, [](TimeOfDayComponent& t, f32 v)
                                                                               { if (std::isfinite(v)) t.m_DayLengthMinutes = std::clamp(v, 0.1f, 10080.0f); }),
                                             "timeScale", sol::property([](const TimeOfDayComponent& t)
                                                                        { return t.m_TimeScale; }, [](TimeOfDayComponent& t, f32 v)
                                                                        { if (std::isfinite(v)) t.m_TimeScale = std::clamp(v, 0.0f, 1000.0f); }),
                                             "paused", &TimeOfDayComponent::m_Paused,
                                             "moonPhase", sol::property([](const TimeOfDayComponent& t)
                                                                        { return t.m_MoonPhase; }, [](TimeOfDayComponent& t, f32 v)
                                                                        { if (std::isfinite(v)) t.m_MoonPhase = std::clamp(v, 0.0f, 1.0f); }),
                                             "northOffsetDegrees", sol::property([](const TimeOfDayComponent& t)
                                                                                 { return t.m_NorthOffsetDegrees; }, [](TimeOfDayComponent& t, f32 v)
                                                                                 { if (std::isfinite(v)) t.m_NorthOffsetDegrees = std::clamp(v, -360.0f, 360.0f); }),
                                             "sunDirection", sol::readonly_property([](const TimeOfDayComponent& t)
                                                                                    { return t.m_SunDirection; }),
                                             "moonDirection", sol::readonly_property([](const TimeOfDayComponent& t)
                                                                                     { return t.m_MoonDirection; }),
                                             "sunElevationDegrees", sol::readonly_property([](const TimeOfDayComponent& t)
                                                                                           { return t.m_SunElevationDegrees; }),
                                             "isNight", sol::readonly_property([](const TimeOfDayComponent& t)
                                                                               { return t.m_IsNight; }));

        // --- WeatherStateComponent ---
        // Scripts drive the weather director through the state machine only:
        // targetState is a case-sensitive string ("Clear" / "Overcast" /
        // "Rain" / "Storm" / "Snow" / "FogBank"); the per-state presets and
        // the blended output stay internal (WeatherSystem owns the blend).
        lua.new_usertype<WeatherStateComponent>("WeatherStateComponent",
                                                "enabled", &WeatherStateComponent::m_Enabled,
                                                "targetState", sol::property([](const WeatherStateComponent& w)
                                                                             { return WeatherStateIdToName(w.m_TargetState); }, [](WeatherStateComponent& w, std::string_view name)
                                                                             {
                                                    if (const auto id = WeatherStateIdFromName(name))
                                                        w.m_TargetState = *id;
                                                    else
                                                        OLO_CORE_WARN("[Lua] WeatherStateComponent.targetState: unknown weather state '{}' — ignored (expected Clear/Overcast/Rain/Storm/Snow/FogBank)", name); }),
                                                "currentState", sol::readonly_property([](const WeatherStateComponent& w)
                                                                                       { return WeatherStateIdToName(w.m_CurrentState); }),
                                                "transitionDuration", sol::property([](const WeatherStateComponent& w)
                                                                                    { return w.m_TransitionDuration; }, [](WeatherStateComponent& w, f32 v)
                                                                                    { if (std::isfinite(v)) w.m_TransitionDuration = std::clamp(v, 0.0f, 600.0f); }),
                                                "transitionProgress", sol::readonly_property([](const WeatherStateComponent& w)
                                                                                             { return w.m_TransitionProgress; }),
                                                "wetness", sol::readonly_property([](const WeatherStateComponent& w)
                                                                                  { return w.m_Wetness; }));

        // --- CloudscapeComponent ---
        // Field-shaping and shadow knobs only: the raymarch-quality and IBL
        // settings are authoring-time, not script-facing. Setter clamps mirror
        // the OLO_SERIALIZE(Clamp) ranges in Components.h. Note the weather
        // director overrides coverage/typeBlend/wetness while an enabled
        // WeatherStateComponent is present in the scene.
        lua.new_usertype<CloudscapeComponent>("CloudscapeComponent",
                                              "enabled", &CloudscapeComponent::m_Enabled,
                                              "coverage", sol::property([](const CloudscapeComponent& c)
                                                                        { return c.m_Coverage; }, [](CloudscapeComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.m_Coverage = std::clamp(v, 0.0f, 1.0f); }),
                                              "density", sol::property([](const CloudscapeComponent& c)
                                                                       { return c.m_Density; }, [](CloudscapeComponent& c, f32 v)
                                                                       { if (std::isfinite(v)) c.m_Density = std::clamp(v, 0.0f, 4.0f); }),
                                              "typeBlend", sol::property([](const CloudscapeComponent& c)
                                                                         { return c.m_TypeBlend; }, [](CloudscapeComponent& c, f32 v)
                                                                         { if (std::isfinite(v)) c.m_TypeBlend = std::clamp(v, 0.0f, 1.0f); }),
                                              "erosionStrength", sol::property([](const CloudscapeComponent& c)
                                                                               { return c.m_ErosionStrength; }, [](CloudscapeComponent& c, f32 v)
                                                                               { if (std::isfinite(v)) c.m_ErosionStrength = std::clamp(v, 0.0f, 1.0f); }),
                                              "windAnimationScale", sol::property([](const CloudscapeComponent& c)
                                                                                  { return c.m_WindAnimationScale; }, [](CloudscapeComponent& c, f32 v)
                                                                                  { if (std::isfinite(v)) c.m_WindAnimationScale = std::clamp(v, 0.0f, 8.0f); }),
                                              "castCloudShadows", &CloudscapeComponent::m_CastCloudShadows,
                                              "shadowStrength", sol::property([](const CloudscapeComponent& c)
                                                                              { return c.m_ShadowStrength; }, [](CloudscapeComponent& c, f32 v)
                                                                              { if (std::isfinite(v)) c.m_ShadowStrength = std::clamp(v, 0.0f, 1.0f); }),
                                              "layerBottom", sol::property([](const CloudscapeComponent& c)
                                                                           { return c.m_LayerBottom; }, [](CloudscapeComponent& c, f32 v)
                                                                           { if (std::isfinite(v)) c.m_LayerBottom = std::clamp(v, 0.0f, 20000.0f); }),
                                              "layerTop", sol::property([](const CloudscapeComponent& c)
                                                                        { return c.m_LayerTop; }, [](CloudscapeComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.m_LayerTop = std::clamp(v, 100.0f, 30000.0f); }));

        // --- NavAgentComponent ---
        lua.new_usertype<NavAgentComponent>("NavAgentComponent",
                                            "radius", sol::property([](const NavAgentComponent& a)
                                                                    { return a.m_Radius; }, [](NavAgentComponent& a, f32 v)
                                                                    { if (std::isfinite(v) && v > 0.0f) a.m_Radius = v; }),
                                            "height", sol::property([](const NavAgentComponent& a)
                                                                    { return a.m_Height; }, [](NavAgentComponent& a, f32 v)
                                                                    { if (std::isfinite(v) && v > 0.0f) a.m_Height = v; }),
                                            "maxSpeed", sol::property([](const NavAgentComponent& a)
                                                                      { return a.m_MaxSpeed; }, [](NavAgentComponent& a, f32 v)
                                                                      { if (std::isfinite(v) && v >= 0.0f) a.m_MaxSpeed = v; }),
                                            "acceleration", sol::property([](const NavAgentComponent& a)
                                                                          { return a.m_Acceleration; }, [](NavAgentComponent& a, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) a.m_Acceleration = v; }),
                                            "stoppingDistance", sol::property([](const NavAgentComponent& a)
                                                                              { return a.m_StoppingDistance; }, [](NavAgentComponent& a, f32 v)
                                                                              { if (std::isfinite(v) && v >= 0.0f) a.m_StoppingDistance = v; }),
                                            "avoidancePriority", &NavAgentComponent::m_AvoidancePriority,
                                            "targetPosition", sol::property([](const NavAgentComponent& a)
                                                                            { return a.m_TargetPosition; }, [](NavAgentComponent& a, const glm::vec3& pos)
                                                                            {
                                                    if (!IsFiniteVec3(pos)) return;
                                                    a.m_TargetPosition = pos;
                                                    a.m_HasTarget = true;
                                                    a.m_HasPath = false;
                                                    a.m_TargetUnreachable = false;
                                                    a.m_PathCorners.clear();
                                                    a.m_CurrentCornerIndex = 0; }),
                                            "hasTarget", sol::readonly(&NavAgentComponent::m_HasTarget),
                                            "hasPath", sol::readonly(&NavAgentComponent::m_HasPath),
                                            // Terminal signal: target reachable only partially / not at all. Poll
                                            // this alongside hasPath — a partial path keeps hasPath == true forever.
                                            "targetUnreachable", sol::readonly(&NavAgentComponent::m_TargetUnreachable),
                                            "lockYAxis", &NavAgentComponent::m_LockYAxis,
                                            "clearTarget", [](NavAgentComponent& agent)
                                            {
                                                agent.m_HasTarget = false;
                                                agent.m_HasPath = false;
                                                agent.m_TargetUnreachable = false;
                                                agent.m_PathCorners.clear();
                                                agent.m_CurrentCornerIndex = 0; });

        // --- BoidComponent (issue #731) ---
        // Exposes the steering knobs a script actually drives at runtime: the
        // goal a flock chases and the behaviour weights. m_SteeringForce /
        // m_NeighborCount are read-only — they are recomputed by BoidSteering
        // every tick, so a script write would be silently discarded.
        lua.new_usertype<BoidComponent>("BoidComponent",
                                        "maxSpeed", sol::property([](const BoidComponent& b)
                                                                  { return b.m_MaxSpeed; }, [](BoidComponent& b, f32 v)
                                                                  { if (std::isfinite(v) && v >= 0.0f) b.m_MaxSpeed = v; }),
                                        "maxForce", sol::property([](const BoidComponent& b)
                                                                  { return b.m_MaxForce; }, [](BoidComponent& b, f32 v)
                                                                  { if (std::isfinite(v) && v >= 0.0f) b.m_MaxForce = v; }),
                                        "neighborRadius", sol::property([](const BoidComponent& b)
                                                                        { return b.m_NeighborRadius; }, [](BoidComponent& b, f32 v)
                                                                        { if (std::isfinite(v) && v > 0.0f) b.m_NeighborRadius = v; }),
                                        "separationRadius", sol::property([](const BoidComponent& b)
                                                                          { return b.m_SeparationRadius; }, [](BoidComponent& b, f32 v)
                                                                          { if (std::isfinite(v) && v > 0.0f) b.m_SeparationRadius = v; }),
                                        "separationWeight", sol::property([](const BoidComponent& b)
                                                                          { return b.m_SeparationWeight; }, [](BoidComponent& b, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f) b.m_SeparationWeight = v; }),
                                        "alignmentWeight", sol::property([](const BoidComponent& b)
                                                                         { return b.m_AlignmentWeight; }, [](BoidComponent& b, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) b.m_AlignmentWeight = v; }),
                                        "cohesionWeight", sol::property([](const BoidComponent& b)
                                                                        { return b.m_CohesionWeight; }, [](BoidComponent& b, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) b.m_CohesionWeight = v; }),
                                        "goalWeight", sol::property([](const BoidComponent& b)
                                                                    { return b.m_GoalWeight; }, [](BoidComponent& b, f32 v)
                                                                    { if (std::isfinite(v) && v >= 0.0f) b.m_GoalWeight = v; }),
                                        "goalPosition", sol::property([](const BoidComponent& b)
                                                                      { return b.m_GoalPosition; }, [](BoidComponent& b, const glm::vec3& v)
                                                                      { if (IsFiniteVec3(v)) b.m_GoalPosition = v; }),
                                        "obstacleAvoidWeight", sol::property([](const BoidComponent& b)
                                                                             { return b.m_ObstacleAvoidWeight; }, [](BoidComponent& b, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) b.m_ObstacleAvoidWeight = v; }),
                                        "obstacleAvoidRadius", sol::property([](const BoidComponent& b)
                                                                             { return b.m_ObstacleAvoidRadius; }, [](BoidComponent& b, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) b.m_ObstacleAvoidRadius = v; }),
                                        "velocity", sol::property([](const BoidComponent& b)
                                                                  { return b.m_Velocity; }, [](BoidComponent& b, const glm::vec3& v)
                                                                  { if (IsFiniteVec3(v)) b.m_Velocity = v; }),
                                        "lockYAxis", &BoidComponent::m_LockYAxis,
                                        "faceVelocity", &BoidComponent::m_FaceVelocity,
                                        "neighborCount", sol::readonly(&BoidComponent::m_NeighborCount),
                                        "steeringForce", sol::readonly(&BoidComponent::m_SteeringForce));

        // --- BoidObstacleComponent (issue #731) ---
        lua.new_usertype<BoidObstacleComponent>("BoidObstacleComponent",
                                                "radius", sol::property([](const BoidObstacleComponent& o)
                                                                        { return o.m_Radius; }, [](BoidObstacleComponent& o, f32 v)
                                                                        { if (std::isfinite(v) && v > 0.0f) o.m_Radius = v; }));

        // --- PlayerRigComponent (issue #645) ---
        // The script-facing surface is deliberately the INTENT, not the pose:
        // a script sets moveInput / lookInput / jump and the PlayerRig node
        // turns them into character motion at the fixed step. Driving a player
        // by writing its transform from script fights the character controller;
        // driving it by writing intent does not. Set useDeviceInput = false
        // first, or the keyboard sample at the top of the tick overwrites the
        // script's move/look (jump is OR-ed, so it survives either way).
        lua.new_usertype<PlayerRigComponent>(
            "PlayerRigComponent",
            "useDeviceInput", &PlayerRigComponent::m_UseDeviceInput,
            "captureCursor", &PlayerRigComponent::m_CaptureCursor,
            "moveInput", sol::property([](const PlayerRigComponent& r)
                                       { return r.m_MoveInput; }, [](PlayerRigComponent& r, const glm::vec2& v)
                                       { if (std::isfinite(v.x) && std::isfinite(v.y)) r.m_MoveInput = v; }),
            "lookInput", sol::property([](const PlayerRigComponent& r)
                                       { return r.m_LookInput; }, [](PlayerRigComponent& r, const glm::vec2& v)
                                       { if (std::isfinite(v.x) && std::isfinite(v.y)) r.m_LookInput = v; }),
            "sprint", &PlayerRigComponent::m_SprintInput,
            "jump", &PlayerRigComponent::m_JumpInput,
            "yaw", sol::property([](const PlayerRigComponent& r)
                                 { return r.m_YawDeg; }, [](PlayerRigComponent& r, f32 v)
                                 { if (std::isfinite(v)) r.m_YawDeg = v; }),
            "pitch", sol::property([](const PlayerRigComponent& r)
                                   { return r.m_PitchDeg; }, [](PlayerRigComponent& r, f32 v)
                                   { if (std::isfinite(v)) r.m_PitchDeg = v; }),
            "lookSensitivity", sol::property([](const PlayerRigComponent& r)
                                             { return r.m_LookSensitivity; }, [](PlayerRigComponent& r, f32 v)
                                             { if (std::isfinite(v) && v >= 0.0f) r.m_LookSensitivity = v; }),
            "invertLookY", &PlayerRigComponent::m_InvertLookY,
            "walkSpeed", sol::property([](const PlayerRigComponent& r)
                                       { return r.m_WalkSpeed; }, [](PlayerRigComponent& r, f32 v)
                                       { if (std::isfinite(v) && v >= 0.0f) r.m_WalkSpeed = v; }),
            "sprintMultiplier", sol::property([](const PlayerRigComponent& r)
                                              { return r.m_SprintMultiplier; }, [](PlayerRigComponent& r, f32 v)
                                              { if (std::isfinite(v) && v >= 1.0f) r.m_SprintMultiplier = v; }),
            "airControl", sol::property([](const PlayerRigComponent& r)
                                        { return r.m_AirControl; }, [](PlayerRigComponent& r, f32 v)
                                        { if (std::isfinite(v)) r.m_AirControl = std::clamp(v, 0.0f, 1.0f); }),
            "moveRelativeToLook", &PlayerRigComponent::m_MoveRelativeToLook,
            "yawBodyWithLook", &PlayerRigComponent::m_YawBodyWithLook,
            "faceMoveDirection", &PlayerRigComponent::m_FaceMoveDirection,
            // Rewritten by the PlayerRig node every tick — a script write would
            // be silently discarded, so expose them read-only.
            "planarSpeed", sol::readonly(&PlayerRigComponent::m_PlanarSpeed),
            "grounded", sol::readonly(&PlayerRigComponent::m_Grounded));

        // --- FPS combat components (issue #436) ---
        lua.new_usertype<WeaponComponent>(
            "WeaponComponent",
            "weaponItemID", &WeaponComponent::m_WeaponItemID,
            "muzzleOffset", sol::property([](const WeaponComponent& c)
                                          { return c.m_MuzzleOffset; }, [](WeaponComponent& c, const glm::vec3& value)
                                          { if (Math::IsFinite(value)) c.m_MuzzleOffset = value; }),
            "useDeviceInput", &WeaponComponent::m_UseDeviceInput,
            "fire", &WeaponComponent::m_FireInput,
            "reload", &WeaponComponent::m_ReloadInput);

        lua.new_usertype<PlayerRespawnComponent>(
            "PlayerRespawnComponent",
            "spawnPoint", sol::property([](const PlayerRespawnComponent& c)
                                        { return c.m_SpawnPoint; }, [](PlayerRespawnComponent& c, const glm::vec3& value)
                                        { if (Math::IsFinite(value)) c.m_SpawnPoint = value; }),
            "spawnYaw", sol::property([](const PlayerRespawnComponent& c)
                                      { return c.m_SpawnYawDeg; }, [](PlayerRespawnComponent& c, f32 value)
                                      { c.m_SpawnYawDeg = std::isfinite(value) ? value : 0.0f; }),
            "respawnDelay", sol::property([](const PlayerRespawnComponent& c)
                                          { return c.m_RespawnDelay; }, [](PlayerRespawnComponent& c, f32 value)
                                          { c.m_RespawnDelay = std::isfinite(value) ? std::clamp(value, 0.0f, 3600.0f) : 3.0f; }));

        // --- CameraRigComponent (issue #645) ---
        // m_BoomLength == 0 is first person; anything above it is a
        // third-person boom. Scripts retarget the rig (cutscene, possession)
        // and tweak the feel; the placement itself is the CameraRig node's job.
        lua.new_usertype<CameraRigComponent>(
            "CameraRigComponent",
            "target", sol::property([](const CameraRigComponent& c)
                                    { return static_cast<u64>(c.m_Target); }, [](CameraRigComponent& c, u64 v)
                                    { c.m_Target = UUID(v); }),
            "pivotOffset", sol::property([](const CameraRigComponent& c)
                                         { return c.m_PivotOffset; }, [](CameraRigComponent& c, const glm::vec3& v)
                                         { if (IsFiniteVec3(v)) c.m_PivotOffset = v; }),
            "boomLength", sol::property([](const CameraRigComponent& c)
                                        { return c.m_BoomLength; }, [](CameraRigComponent& c, f32 v)
                                        { if (std::isfinite(v) && v >= 0.0f) c.m_BoomLength = v; }),
            "collisionEnabled", &CameraRigComponent::m_CollisionEnabled,
            "probeRadius", sol::property([](const CameraRigComponent& c)
                                         { return c.m_ProbeRadius; }, [](CameraRigComponent& c, f32 v)
                                         { if (std::isfinite(v) && v >= 0.0f) c.m_ProbeRadius = v; }),
            "minBoomLength", sol::property([](const CameraRigComponent& c)
                                           { return c.m_MinBoomLength; }, [](CameraRigComponent& c, f32 v)
                                           { if (std::isfinite(v) && v >= 0.0f) c.m_MinBoomLength = v; }),
            "boomReturnSpeed", sol::property([](const CameraRigComponent& c)
                                             { return c.m_BoomReturnSpeed; }, [](CameraRigComponent& c, f32 v)
                                             { if (std::isfinite(v) && v >= 0.0f) c.m_BoomReturnSpeed = v; }),
            "positionSmoothTime", sol::property([](const CameraRigComponent& c)
                                                { return c.m_PositionSmoothTime; }, [](CameraRigComponent& c, f32 v)
                                                { if (std::isfinite(v) && v >= 0.0f) c.m_PositionSmoothTime = v; }),
            "headBobAmplitude", sol::property([](const CameraRigComponent& c)
                                              { return c.m_HeadBobAmplitude; }, [](CameraRigComponent& c, f32 v)
                                              { if (std::isfinite(v) && v >= 0.0f) c.m_HeadBobAmplitude = v; }),
            "headBobFrequency", sol::property([](const CameraRigComponent& c)
                                              { return c.m_HeadBobFrequency; }, [](CameraRigComponent& c, f32 v)
                                              { if (std::isfinite(v) && v >= 0.0f) c.m_HeadBobFrequency = v; }),
            // The one field that matters most when the rig is following
            // something that is not a player, and the one that was missing from
            // this table until issue #897.
            "fallbackPitchDeg", sol::property([](const CameraRigComponent& c)
                                              { return c.m_FallbackPitchDeg; }, [](CameraRigComponent& c, f32 v)
                                              { if (std::isfinite(v)) c.m_FallbackPitchDeg = std::clamp(v, -89.9f, 89.9f); }),
            // ForwardConvention as an integer: 0 = Auto (derive it from the
            // target's components), 1 = MinusZ (camera/player), 2 = PlusZ
            // (Jolt vehicle). Anything else is rejected rather than clamped —
            // see the field's note in PlayerRigComponents.h.
            "targetForward", sol::property([](const CameraRigComponent& c)
                                           { return static_cast<int>(c.m_TargetForward); }, [](CameraRigComponent& c, int v)
                                           { if (v >= 0 && v <= 2) c.m_TargetForward = static_cast<ForwardConvention>(v); }),
            "currentBoomLength", sol::readonly(&CameraRigComponent::m_CurrentBoomLength));

        // --- NavMeshBoundsComponent ---
        lua.new_usertype<NavMeshBoundsComponent>("NavMeshBoundsComponent",
                                                 "min", sol::property([](const NavMeshBoundsComponent& c)
                                                                      { return c.m_Min; }, [](NavMeshBoundsComponent& c, const glm::vec3& v)
                                                                      { if (IsFiniteVec3(v)) c.m_Min = v; }),
                                                 "max", sol::property([](const NavMeshBoundsComponent& c)
                                                                      { return c.m_Max; }, [](NavMeshBoundsComponent& c, const glm::vec3& v)
                                                                      { if (IsFiniteVec3(v)) c.m_Max = v; }));

        // --- Visual scripting bridge (issue #634) ---
        // The script -> graph half of AC#6: text scripts trigger graph flow and
        // read/write graph blackboard variables. The graph -> script half is the
        // Script.CallLuaFunction node.
        //
        // Every entry point resolves the system through the live scene and
        // no-ops when there is none — a script running in the editor's edit mode
        // has no VisualScriptSystem, and asserting there would break the panel.
        auto visualScriptTable = lua.create_named_table("visual_script");

        // Queued, never dispatched inline: a Lua OnUpdate runs inside
        // Scene::UpdateScripts' entity-view walk, and a graph reacting to this
        // event may spawn or destroy entities.
        visualScriptTable["send_event"] = [](u64 targetEntityID, const std::string& name, sol::object payload)
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            auto* system = scene ? scene->GetVisualScripts() : nullptr;
            if (!system || name.empty())
                return false;

            // A default-constructed PinValue is type Exec, which every data pin
            // coerces to its own zero — indistinguishable from an authored 0 or
            // "". Nil becomes an explicit empty string instead, and an
            // unsupported type is refused rather than silently becoming one.
            VisualScript::PinValue value = VisualScript::PinValue::MakeString("");
            if (payload.valid() && payload.get_type() != sol::type::lua_nil)
            {
                if (payload.is<bool>())
                    value = VisualScript::PinValue::MakeBool(payload.as<bool>());
                else if (payload.is<f64>())
                {
                    const f64 raw = payload.as<f64>();
                    // Range-checked BEFORE the narrowing cast, not after: converting
                    // a double whose magnitude exceeds FLT_MAX is undefined
                    // behaviour, so inspecting the result is already too late. An
                    // infinity in a graph variable poisons every downstream node
                    // silently, which is why this refuses rather than clamps.
                    if (!VisualScript::IsRepresentableAsFloat(raw))
                        return false;
                    value = VisualScript::PinValue::MakeFloat(static_cast<f32>(raw));
                }
                else if (payload.is<std::string>())
                    value = VisualScript::PinValue::MakeString(payload.as<std::string>());
                else
                    return false;
            }

            // 0 broadcasts, matching Utility.PublishEvent's unwired Target.
            system->QueueCustomEvent(name, std::move(value), UUID(targetEntityID), UUID(0));
            return true;
        };

        visualScriptTable["get_variable"] = [&lua](u64 entityID, const std::string& name) -> sol::object
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            auto* system = scene ? scene->GetVisualScripts() : nullptr;
            if (!system)
                return sol::lua_nil;

            VisualScript::PinValue value;
            if (!system->TryGetVariable(UUID(entityID), name, value))
                return sol::lua_nil;

            sol::state_view view(lua.lua_state());
            switch (value.GetType())
            {
                case VisualScript::PinType::Bool:
                    return sol::make_object(view, value.AsBool());
                case VisualScript::PinType::Int:
                    return sol::make_object(view, value.AsInt());
                case VisualScript::PinType::Float:
                    return sol::make_object(view, static_cast<f64>(value.AsFloat()));
                case VisualScript::PinType::Entity:
                case VisualScript::PinType::Asset:
                    // Signed: entity/asset ids are full-range u64 and Lua's
                    // integer is signed 64-bit. Same bits, and the round trip
                    // back through set_variable is lossless.
                    return sol::make_object(view, static_cast<i64>(static_cast<u64>(value.AsEntity())));
                default:
                    return sol::make_object(view, value.AsString());
            }
        };

        visualScriptTable["set_variable"] = [](u64 entityID, const std::string& name, sol::object newValue) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            auto* system = scene ? scene->GetVisualScripts() : nullptr;
            if (!system)
                return false;

            VisualScript::PinValue value;
            if (newValue.is<bool>())
                value = VisualScript::PinValue::MakeBool(newValue.as<bool>());
            else if (newValue.is<f64>())
            {
                const f64 raw = newValue.as<f64>();
                // Same pre-cast range check as send_event above: a finite Lua number
                // past FLT_MAX is undefined behaviour to narrow, not an infinity.
                if (!VisualScript::IsRepresentableAsFloat(raw))
                    return false;
                value = VisualScript::PinValue::MakeFloat(static_cast<f32>(raw));
            }
            else if (newValue.is<std::string>())
                value = VisualScript::PinValue::MakeString(newValue.as<std::string>());
            else
                return false;

            // The system coerces to the variable's declared type, so a Lua
            // number written into a Bool variable does the obvious thing.
            return system->SetVariable(UUID(entityID), name, value);
        };

        visualScriptTable["is_running"] = [](u64 entityID) -> bool
        {
            Scene* scene = ScriptEngine::GetSceneContext();
            auto* system = scene ? scene->GetVisualScripts() : nullptr;
            return system != nullptr && system->FindInstance(UUID(entityID)) != nullptr;
        };

        // --- Dialogue system functions ---
        auto dialogueTable = lua.create_named_table("dialogue");
        dialogueTable["start"] = [](Entity* entity)
        {
            if (!entity)
                return;
            const Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->StartDialogue(*entity);
        };
        dialogueTable["advance"] = [](Entity* entity)
        {
            if (!entity)
                return;
            const Scene* scene = ScriptEngine::GetSceneContext();
            if (scene && scene->GetDialogueSystem())
                scene->GetDialogueSystem()->AdvanceDialogue(*entity);
        };
        dialogueTable["select_choice"] = [](Entity* entity, i32 index)
        {
            if (!entity)
                return;
            const Scene* scene = ScriptEngine::GetSceneContext();
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
            const Scene* scene = ScriptEngine::GetSceneContext();
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
    }
} // namespace OloEngine
