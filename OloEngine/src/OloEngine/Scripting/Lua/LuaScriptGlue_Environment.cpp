#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_Environment.cpp — water, terrain, buoyancy and the fluid components.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterEnvironmentTypes(sol::state& lua)
    {
        // --- WaterComponent ---
        // Exposes the gameplay-relevant subset; texture / tessellation / SSR
        // setup is editor-only and intentionally not scripted here.
        lua.new_usertype<WaterComponent>("WaterComponent",
                                         "enabled", &WaterComponent::m_Enabled,
                                         "worldSizeX", sol::property([](const WaterComponent& w)
                                                                     { return w.m_WorldSizeX; }, [](WaterComponent& w, f32 v)
                                                                     { if (std::isfinite(v) && v > 0.0f) w.m_WorldSizeX = v; }),
                                         "worldSizeZ", sol::property([](const WaterComponent& w)
                                                                     { return w.m_WorldSizeZ; }, [](WaterComponent& w, f32 v)
                                                                     { if (std::isfinite(v) && v > 0.0f) w.m_WorldSizeZ = v; }),
                                         "waveAmplitude", sol::property([](const WaterComponent& w)
                                                                        { return w.m_WaveAmplitude; }, [](WaterComponent& w, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) w.m_WaveAmplitude = v; }),
                                         "waveFrequency", sol::property([](const WaterComponent& w)
                                                                        { return w.m_WaveFrequency; }, [](WaterComponent& w, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) w.m_WaveFrequency = v; }),
                                         "waveSpeed", sol::property([](const WaterComponent& w)
                                                                    { return w.m_WaveSpeed; }, [](WaterComponent& w, f32 v)
                                                                    { if (std::isfinite(v)) w.m_WaveSpeed = v; }),
                                         "waterColor", sol::property([](const WaterComponent& w)
                                                                     { return w.m_WaterColor; }, [](WaterComponent& w, const glm::vec3& v)
                                                                     { if (IsFiniteVec3(v)) w.m_WaterColor = v; }),
                                         "deepColor", sol::property([](const WaterComponent& w)
                                                                    { return w.m_DeepColor; }, [](WaterComponent& w, const glm::vec3& v)
                                                                    { if (IsFiniteVec3(v)) w.m_DeepColor = v; }),
                                         "transparency", sol::property([](const WaterComponent& w)
                                                                       { return w.m_Transparency; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f && v <= 1.0f) w.m_Transparency = v; }),
                                         "reflectivity", sol::property([](const WaterComponent& w)
                                                                       { return w.m_Reflectivity; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f && v <= 1.0f) w.m_Reflectivity = v; }),
                                         // Surface detail (issue #882). These are the knobs a
                                         // sea state is legible through in a still frame —
                                         // whitecaps appearing lower down the wave, the
                                         // specular track breaking up, the fine normal detail
                                         // coming forward — so a script that couples the sea
                                         // to the wind needs them alongside waveAmplitude.
                                         "specularIntensity", sol::property([](const WaterComponent& w)
                                                                            { return w.m_SpecularIntensity; }, [](WaterComponent& w, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f && v <= 10.0f) w.m_SpecularIntensity = v; }),
                                         "noiseIntensity", sol::property([](const WaterComponent& w)
                                                                         { return w.m_NoiseIntensity; }, [](WaterComponent& w, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f && v <= 1.0f) w.m_NoiseIntensity = v; }),
                                         "foamHeightStart", sol::property([](const WaterComponent& w)
                                                                          { return w.m_FoamHeightStart; }, [](WaterComponent& w, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f && v <= 10.0f) w.m_FoamHeightStart = v; }),
                                         "foamFadeDistance", sol::property([](const WaterComponent& w)
                                                                           { return w.m_FoamFadeDistance; }, [](WaterComponent& w, f32 v)
                                                                           { if (std::isfinite(v) && v > 0.0f && v <= 10.0f) w.m_FoamFadeDistance = v; }),
                                         "foamBrightness", sol::property([](const WaterComponent& w)
                                                                         { return w.m_FoamBrightness; }, [](WaterComponent& w, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f && v <= 10.0f) w.m_FoamBrightness = v; }),
                                         "foamCoverage", sol::property([](const WaterComponent& w)
                                                                       { return w.m_FoamCoverage; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f && v <= 1.0f) w.m_FoamCoverage = v; }),
                                         // Boat / actor wake foam (issue #967). Exposed because
                                         // DriftWeatherDirector.lua already owns this component's sea-state
                                         // fields at runtime, and a wake that cannot be eased with the rest
                                         // of them would pop the moment the weather changed.
                                         "wakeFoamEnabled", &WaterComponent::m_WakeFoamEnabled,
                                         "wakeFoamIntensity", sol::property([](const WaterComponent& w)
                                                                            { return w.m_WakeFoamIntensity; }, [](WaterComponent& w, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f && v <= 4.0f) w.m_WakeFoamIntensity = v; }),
                                         "wakeFoamHalfLife", sol::property([](const WaterComponent& w)
                                                                           { return w.m_WakeFoamHalfLife; }, [](WaterComponent& w, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.05f && v <= 120.0f) w.m_WakeFoamHalfLife = v; }),
                                         "wakeFoamFadeStart", sol::property([](const WaterComponent& w)
                                                                            { return w.m_WakeFoamFadeStart; }, [](WaterComponent& w, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f && v <= 2000.0f) w.m_WakeFoamFadeStart = v; }),
                                         "wakeFoamFadeEnd", sol::property([](const WaterComponent& w)
                                                                          { return w.m_WakeFoamFadeEnd; }, [](WaterComponent& w, f32 v)
                                                                          { if (std::isfinite(v) && v >= 1.0f && v <= 4000.0f) w.m_WakeFoamFadeEnd = v; }),
                                         // Advected foam and crest spray (issue #1034). Exposed for the
                                         // same reason as the wake fields: DriftWeatherDirector.lua owns
                                         // the sea state, and whitecaps that could not be eased with the
                                         // rest of it would step when the weather changed. The deposit
                                         // THRESHOLDS are the sea-state knob; the field's half-life,
                                         // drift fraction and particle geometry are not weather and stay
                                         // authored in the component.
                                         "foamAdvectionEnabled", &WaterComponent::m_FoamAdvectionEnabled,
                                         "foamAdvectionIntensity", sol::property([](const WaterComponent& w)
                                                                                 { return w.m_FoamAdvectionIntensity; }, [](WaterComponent& w, f32 v)
                                                                                 { if (std::isfinite(v) && v >= 0.0f && v <= 4.0f) w.m_FoamAdvectionIntensity = v; }),
                                         "foamAdvectionThreshold", sol::property([](const WaterComponent& w)
                                                                                 { return w.m_FoamAdvectionThreshold; }, [](WaterComponent& w, f32 v)
                                                                                 { if (std::isfinite(v) && v >= 0.0f && v <= 0.99f) w.m_FoamAdvectionThreshold = v; }),
                                         "sprayEnabled", &WaterComponent::m_SprayEnabled,
                                         "sprayThreshold", sol::property([](const WaterComponent& w)
                                                                         { return w.m_SprayThreshold; }, [](WaterComponent& w, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f && v <= 0.99f) w.m_SprayThreshold = v; }),
                                         "sprayRate", sol::property([](const WaterComponent& w)
                                                                    { return w.m_SprayRate; }, [](WaterComponent& w, f32 v)
                                                                    { if (std::isfinite(v) && v >= 0.0f && v <= 200.0f) w.m_SprayRate = v; }),
                                         // Rain-impact ripples (issue #1034). Exposed for the same
                                         // reason the wake fields above are: DriftWeatherDirector.lua
                                         // drives the precipitation this reacts to, and a stipple it
                                         // could turn the rain on for but not ease in would pop.
                                         // The fade distances are deliberately NOT exposed — they are
                                         // a sampling limit (the 0.55 m cell grid), not weather.
                                         "rainRipplesEnabled", &WaterComponent::m_RainRipplesEnabled,
                                         "rainRippleStrength", sol::property([](const WaterComponent& w)
                                                                             { return w.m_RainRippleStrength; }, [](WaterComponent& w, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f && v <= 4.0f) w.m_RainRippleStrength = v; }),
                                         "underwaterFogColor", sol::property([](const WaterComponent& w)
                                                                             { return w.m_UnderwaterFogColor; }, [](WaterComponent& w, const glm::vec3& v)
                                                                             { if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) w.m_UnderwaterFogColor = glm::clamp(v, glm::vec3(0.0f), glm::vec3(1.0f)); }),
                                         "underwaterFogDensity", sol::property([](const WaterComponent& w)
                                                                               { return w.m_UnderwaterFogDensity; }, [](WaterComponent& w, f32 v)
                                                                               { if (std::isfinite(v) && v >= 0.0f && v <= 10.0f) w.m_UnderwaterFogDensity = v; }),
                                         "underwaterRefractionStrength", sol::property([](const WaterComponent& w)
                                                                                       { return w.m_UnderwaterRefractionStrength; }, [](WaterComponent& w, f32 v)
                                                                                       { if (std::isfinite(v) && v >= 0.0f && v <= 0.1f) w.m_UnderwaterRefractionStrength = v; }),
                                         "underwaterRefractionScale", sol::property([](const WaterComponent& w)
                                                                                    { return w.m_UnderwaterRefractionScale; }, [](WaterComponent& w, f32 v)
                                                                                    { if (std::isfinite(v) && v >= 0.0f && v <= 200.0f) w.m_UnderwaterRefractionScale = v; }),
                                         "underwaterRefractionSpeed", sol::property([](const WaterComponent& w)
                                                                                    { return w.m_UnderwaterRefractionSpeed; }, [](WaterComponent& w, f32 v)
                                                                                    { if (std::isfinite(v) && v >= 0.0f && v <= 50.0f) w.m_UnderwaterRefractionSpeed = v; }),
                                         "underwaterChromaticStrength", sol::property([](const WaterComponent& w)
                                                                                      { return w.m_UnderwaterChromaticStrength; }, [](WaterComponent& w, f32 v)
                                                                                      { if (std::isfinite(v) && v >= 0.0f && v <= 1.0f) w.m_UnderwaterChromaticStrength = v; }),
                                         "causticsIntensity", sol::property([](const WaterComponent& w)
                                                                            { return w.m_CausticsIntensity; }, [](WaterComponent& w, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f && v <= 10.0f) w.m_CausticsIntensity = v; }),
                                         "causticsScale", sol::property([](const WaterComponent& w)
                                                                        { return w.m_CausticsScale; }, [](WaterComponent& w, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.001f && v <= 10.0f) w.m_CausticsScale = v; }),
                                         "causticsSpeed", sol::property([](const WaterComponent& w)
                                                                        { return w.m_CausticsSpeed; }, [](WaterComponent& w, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f && v <= 50.0f) w.m_CausticsSpeed = v; }),
                                         "causticsMaxDepth", sol::property([](const WaterComponent& w)
                                                                           { return w.m_CausticsMaxDepth; }, [](WaterComponent& w, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.1f && v <= 1000.0f) w.m_CausticsMaxDepth = v; }),
                                         "causticsColor", sol::property([](const WaterComponent& w)
                                                                        { return w.m_CausticsColor; }, [](WaterComponent& w, const glm::vec3& v)
                                                                        { if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) w.m_CausticsColor = glm::clamp(v, glm::vec3(0.0f), glm::vec3(1.0f)); }),
                                         "godRayIntensity", sol::property([](const WaterComponent& w)
                                                                          { return w.m_GodRayIntensity; }, [](WaterComponent& w, f32 v)
                                                                          { if (std::isfinite(v) && v >= 0.0f && v <= 10.0f) w.m_GodRayIntensity = v; }),
                                         "godRayDecay", sol::property([](const WaterComponent& w)
                                                                      { return w.m_GodRayDecay; }, [](WaterComponent& w, f32 v)
                                                                      { if (std::isfinite(v) && v >= 0.0f && v <= 0.999f) w.m_GodRayDecay = v; }),
                                         "godRayDensity", sol::property([](const WaterComponent& w)
                                                                        { return w.m_GodRayDensity; }, [](WaterComponent& w, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f && v <= 2.0f) w.m_GodRayDensity = v; }),
                                         "godRayWeight", sol::property([](const WaterComponent& w)
                                                                       { return w.m_GodRayWeight; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f && v <= 2.0f) w.m_GodRayWeight = v; }),
                                         "godRayColor", sol::property([](const WaterComponent& w)
                                                                      { return w.m_GodRayColor; }, [](WaterComponent& w, const glm::vec3& v)
                                                                      { if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) w.m_GodRayColor = glm::clamp(v, glm::vec3(0.0f), glm::vec3(1.0f)); }),
                                         "godRaySamples", sol::property([](const WaterComponent& w)
                                                                        { return w.m_GodRaySamples; }, [](WaterComponent& w, u32 v)
                                                                        { w.m_GodRaySamples = std::clamp(v, 1u, 256u); }),
                                         "godRayDappleFloor", sol::property([](const WaterComponent& w)
                                                                            { return w.m_GodRayDappleFloor; }, [](WaterComponent& w, f32 v)
                                                                            { if (std::isfinite(v) && v >= 0.0f && v <= 1.0f) w.m_GodRayDappleFloor = v; }),
                                         "godRaySunFalloff", sol::property([](const WaterComponent& w)
                                                                           { return w.m_GodRaySunFalloff; }, [](WaterComponent& w, f32 v)
                                                                           { if (std::isfinite(v) && v >= 1.0f && v <= 64.0f) w.m_GodRaySunFalloff = v; }),
                                         "renderFromBelow", &WaterComponent::m_RenderFromBelow,
                                         // FFT ocean (water-ocean.md §1)
                                         "useFFT", &WaterComponent::m_UseFFT,
                                         "fftPatchSize", sol::property([](const WaterComponent& w)
                                                                       { return w.m_FFTPatchSize; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 1.0f && v <= 5000.0f) w.m_FFTPatchSize = v; }),
                                         "fftWindSpeed", sol::property([](const WaterComponent& w)
                                                                       { return w.m_FFTWindSpeed; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.1f && v <= 100.0f) w.m_FFTWindSpeed = v; }),
                                         "fftAmplitude", sol::property([](const WaterComponent& w)
                                                                       { return w.m_FFTAmplitude; }, [](WaterComponent& w, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f && v <= 100.0f) w.m_FFTAmplitude = v; }),
                                         "fftChoppiness", sol::property([](const WaterComponent& w)
                                                                        { return w.m_FFTChoppiness; }, [](WaterComponent& w, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f && v <= 5.0f) w.m_FFTChoppiness = v; }),
                                         "fftHeightScale", sol::property([](const WaterComponent& w)
                                                                         { return w.m_FFTHeightScale; }, [](WaterComponent& w, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f && v <= 20.0f) w.m_FFTHeightScale = v; }),
                                         "fftUseGpuCompute", &WaterComponent::m_FFTUseGpuCompute,
                                         "fftCascades", &WaterComponent::m_FFTCascades);

        // --- TerrainComponent ---
        // Exposes the scalar procedural-generation params so gameplay scripts can drive
        // procedural worlds at runtime (pick a seed per run, regenerate on a level
        // transition, scale heightScale from game state, …). Mirrors the C#/OLO_PROPERTY
        // surface. Setting a param only takes effect once regenerate() is called — it
        // drops the cached terrain so the next tick rebuilds the height field from the new
        // params (reusing the existing Scene::ProcessScene3DSharedLogic rebuild path).
        // Nested TerrainHeightShaping scalars are flattened onto the component here too.
        // Setters validate finiteness / sane ranges (matching the WaterComponent style).
        lua.new_usertype<TerrainComponent>("TerrainComponent",
                                           "proceduralEnabled", &TerrainComponent::m_ProceduralEnabled,
                                           "seed", &TerrainComponent::m_ProceduralSeed,
                                           "resolution", sol::property([](const TerrainComponent& t)
                                                                       { return t.m_ProceduralResolution; }, [](TerrainComponent& t, u32 v)
                                                                       { if (v >= 2u && v <= 8192u) t.m_ProceduralResolution = v; }),
                                           "octaves", sol::property([](const TerrainComponent& t)
                                                                    { return t.m_ProceduralOctaves; }, [](TerrainComponent& t, u32 v)
                                                                    { if (v >= 1u && v <= 20u) t.m_ProceduralOctaves = v; }),
                                           "frequency", sol::property([](const TerrainComponent& t)
                                                                      { return t.m_ProceduralFrequency; }, [](TerrainComponent& t, f32 v)
                                                                      { if (std::isfinite(v) && v > 0.0f) t.m_ProceduralFrequency = v; }),
                                           "lacunarity", sol::property([](const TerrainComponent& t)
                                                                       { return t.m_ProceduralLacunarity; }, [](TerrainComponent& t, f32 v)
                                                                       { if (std::isfinite(v) && v > 0.0f) t.m_ProceduralLacunarity = v; }),
                                           "persistence", sol::property([](const TerrainComponent& t)
                                                                        { return t.m_ProceduralPersistence; }, [](TerrainComponent& t, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) t.m_ProceduralPersistence = v; }),
                                           "worldSizeX", sol::property([](const TerrainComponent& t)
                                                                       { return t.m_WorldSizeX; }, [](TerrainComponent& t, f32 v)
                                                                       { if (std::isfinite(v) && v > 0.0f) t.m_WorldSizeX = v; }),
                                           "worldSizeZ", sol::property([](const TerrainComponent& t)
                                                                       { return t.m_WorldSizeZ; }, [](TerrainComponent& t, f32 v)
                                                                       { if (std::isfinite(v) && v > 0.0f) t.m_WorldSizeZ = v; }),
                                           "heightScale", sol::property([](const TerrainComponent& t)
                                                                        { return t.m_HeightScale; }, [](TerrainComponent& t, f32 v)
                                                                        { if (std::isfinite(v)) t.m_HeightScale = v; }),
                                           "autoMaterial", &TerrainComponent::m_AutoMaterial,
                                           "splatmapGenResolution", sol::property([](const TerrainComponent& t)
                                                                                  { return t.m_SplatmapGenResolution; }, [](TerrainComponent& t, u32 v)
                                                                                  { if (v >= 2u && v <= 8192u) t.m_SplatmapGenResolution = v; }),
                                           "ridgeBlend", sol::property([](const TerrainComponent& t)
                                                                       { return t.m_HeightShaping.RidgeBlend; }, [](TerrainComponent& t, f32 v)
                                                                       { if (std::isfinite(v)) t.m_HeightShaping.RidgeBlend = glm::clamp(v, 0.0f, 1.0f); }),
                                           "warpStrength", sol::property([](const TerrainComponent& t)
                                                                         { return t.m_HeightShaping.WarpStrength; }, [](TerrainComponent& t, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) t.m_HeightShaping.WarpStrength = v; }),
                                           "warpFrequency", sol::property([](const TerrainComponent& t)
                                                                          { return t.m_HeightShaping.WarpFrequency; }, [](TerrainComponent& t, f32 v)
                                                                          { if (std::isfinite(v) && v > 0.0f) t.m_HeightShaping.WarpFrequency = v; }),
                                           "terraceSteps", sol::property([](const TerrainComponent& t)
                                                                         { return t.m_HeightShaping.TerraceSteps; }, [](TerrainComponent& t, u32 v)
                                                                         { if (v <= 256u) t.m_HeightShaping.TerraceSteps = v; }),
                                           "terraceSharpness", sol::property([](const TerrainComponent& t)
                                                                             { return t.m_HeightShaping.TerraceSharpness; }, [](TerrainComponent& t, f32 v)
                                                                             { if (std::isfinite(v)) t.m_HeightShaping.TerraceSharpness = glm::clamp(v, 0.0f, 0.999f); }),
                                           "heightExponent", sol::property([](const TerrainComponent& t)
                                                                           { return t.m_HeightShaping.HeightExponent; }, [](TerrainComponent& t, f32 v)
                                                                           { if (std::isfinite(v) && v > 0.0f) t.m_HeightShaping.HeightExponent = v; }),
                                           "islandFalloff", sol::property([](const TerrainComponent& t)
                                                                          { return t.m_HeightShaping.IslandFalloff; }, [](TerrainComponent& t, f32 v)
                                                                          { if (std::isfinite(v)) t.m_HeightShaping.IslandFalloff = glm::clamp(v, 0.0f, 1.0f); }),
                                           "islandFalloffRadius", sol::property([](const TerrainComponent& t)
                                                                                { return t.m_HeightShaping.IslandFalloffRadius; }, [](TerrainComponent& t, f32 v)
                                                                                { if (std::isfinite(v)) t.m_HeightShaping.IslandFalloffRadius = glm::clamp(v, 0.0f, 0.5f); }),
                                           "erosionIterations", sol::property([](const TerrainComponent& t)
                                                                              { return t.m_ProceduralErosionIterations; }, [](TerrainComponent& t, i32 v)
                                                                              { t.m_ProceduralErosionIterations = std::clamp(v, 0, 64); }),
                                           // Virtual texturing (issue #715). Only the toggle and the
                                           // per-frame bake budget: those are the two a gameplay
                                           // script has any business changing at runtime (a quality
                                           // setting, and a frame-time knob). The sizing fields
                                           // reallocate ~38 MB of GPU memory when they change, so
                                           // they stay authoring-time — the editor and the scene
                                           // file own them.
                                           "virtualTextureEnabled", &TerrainComponent::m_VirtualTextureEnabled,
                                           "vtMaxTileBakesPerFrame", sol::property([](const TerrainComponent& t)
                                                                                   { return t.m_VTMaxTileBakesPerFrame; }, [](TerrainComponent& t, u32 v)
                                                                                   { if (v >= 1u && v <= 64u) t.m_VTMaxTileBakesPerFrame = v; }),
                                           "regenerate", &TerrainComponent::Regenerate);

        // --- BuoyancyComponent ---
        // Lets gameplay scripts toggle / tune how a floating body responds to the
        // wave field (Physics3D/BuoyancySystem). All setters validate finiteness.
        lua.new_usertype<BuoyancyComponent>("BuoyancyComponent",
                                            "enabled", &BuoyancyComponent::m_Enabled,
                                            "probeExtents", sol::property([](const BuoyancyComponent& b)
                                                                          { return b.m_ProbeExtents; }, [](BuoyancyComponent& b, const glm::vec3& v)
                                                                          { if (IsFiniteVec3(v)) b.m_ProbeExtents = glm::clamp(v, glm::vec3(0.01f), glm::vec3(1000.0f)); }),
                                            "fluidDensity", sol::property([](const BuoyancyComponent& b)
                                                                          { return b.m_FluidDensity; }, [](BuoyancyComponent& b, f32 v)
                                                                          { if (std::isfinite(v) && v > 0.0f) b.m_FluidDensity = v; }),
                                            "buoyancyScale", sol::property([](const BuoyancyComponent& b)
                                                                           { return b.m_BuoyancyScale; }, [](BuoyancyComponent& b, f32 v)
                                                                           { if (std::isfinite(v) && v >= 0.0f) b.m_BuoyancyScale = v; }),
                                            "linearDrag", sol::property([](const BuoyancyComponent& b)
                                                                        { return b.m_LinearDrag; }, [](BuoyancyComponent& b, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) b.m_LinearDrag = v; }),
                                            "angularDrag", sol::property([](const BuoyancyComponent& b)
                                                                         { return b.m_AngularDrag; }, [](BuoyancyComponent& b, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) b.m_AngularDrag = v; }),
                                            "submergenceRamp", sol::property([](const BuoyancyComponent& b)
                                                                             { return b.m_SubmergenceRamp; }, [](BuoyancyComponent& b, f32 v)
                                                                             { if (std::isfinite(v) && v > 0.0f) b.m_SubmergenceRamp = v; }));

        // --- Fluid components (issue #630) ---
        // Gameplay-facing knobs for the PBF fluid: toggle domains, drive
        // emitters (cutscene faucets), and arm kill volumes. Setters validate
        // finiteness and clamp to the component's serialized ranges.
        lua.new_usertype<FluidComponent>("FluidComponent",
                                         "enabled", &FluidComponent::m_Enabled,
                                         "prefillFraction", sol::property([](const FluidComponent& f)
                                                                          { return f.m_PrefillFraction; }, [](FluidComponent& f, f32 v)
                                                                          { if (std::isfinite(v)) f.m_PrefillFraction = std::clamp(v, 0.0f, 1.0f); }),
                                         "domainHalfExtents", sol::property([](const FluidComponent& f)
                                                                            { return f.m_DomainHalfExtents; }, [](FluidComponent& f, const glm::vec3& v)
                                                                            { if (IsFiniteVec3(v)) f.m_DomainHalfExtents = glm::clamp(v, glm::vec3(0.25f), glm::vec3(256.0f)); }));

        lua.new_usertype<FluidEmitterComponent>("FluidEmitterComponent",
                                                "enabled", &FluidEmitterComponent::m_Enabled,
                                                "rate", sol::property([](const FluidEmitterComponent& e)
                                                                      { return e.m_Rate; }, [](FluidEmitterComponent& e, f32 v)
                                                                      { if (std::isfinite(v)) e.m_Rate = std::clamp(v, 0.0f, 200000.0f); }),
                                                "speed", sol::property([](const FluidEmitterComponent& e)
                                                                       { return e.m_Speed; }, [](FluidEmitterComponent& e, f32 v)
                                                                       { if (std::isfinite(v)) e.m_Speed = std::clamp(v, 0.0f, 100.0f); }),
                                                "spreadRadius", sol::property([](const FluidEmitterComponent& e)
                                                                              { return e.m_SpreadRadius; }, [](FluidEmitterComponent& e, f32 v)
                                                                              { if (std::isfinite(v)) e.m_SpreadRadius = std::clamp(v, 0.0f, 10.0f); }));

        lua.new_usertype<FluidKillVolumeComponent>("FluidKillVolumeComponent",
                                                   "enabled", &FluidKillVolumeComponent::m_Enabled,
                                                   "halfExtents", sol::property([](const FluidKillVolumeComponent& k)
                                                                                { return k.m_HalfExtents; }, [](FluidKillVolumeComponent& k, const glm::vec3& v)
                                                                                { if (IsFiniteVec3(v)) k.m_HalfExtents = glm::clamp(v, glm::vec3(0.01f), glm::vec3(256.0f)); }));
    }
} // namespace OloEngine
