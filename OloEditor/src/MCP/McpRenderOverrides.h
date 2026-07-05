#pragma once

// Pure, engine-light name tables + result shaping for the ephemeral render-override
// MCP tools (issue #316 Part 4 — the rendering A/B inner loop):
//
//   * olo_render_toggle_pass     — flip a post-process / fog feature on or off
//                                  (bloom, SSAO, SSR, SSGI, fog, god-rays, …). The
//                                  core A/B loop: toggle off -> olo_screenshot ->
//                                  toggle on -> olo_screenshot.
//   * olo_render_set_debug_view  — switch the viewport to a raw AO/SSR/SSGI debug
//                                  buffer (the *DebugView flags).
//
// Both mutate ONLY the renderer's session-global settings
// (Renderer3D::GetPostProcessSettings() / GetFogSettings()), never the loaded
// scene's own copy, so the edit is ephemeral (a scene reload restores it) and the
// server stays read-only with respect to the project — the same boundary the
// Tier-0 camera/viewport tools respect.
//
//   * olo_scene_set_time_of_day — set an ephemeral time-of-day (0-24h) that
//                                  drives the procedural sky's sun direction.
//   * olo_scene_set_sun_angle    — set the sun direction directly from a
//                                  yaw (azimuth) / pitch (elevation) pair.
//     Both back the lighting inner loop: move the sun -> olo_screenshot ->
//     move it again. They mutate ONLY a session-global Renderer3D sun-direction
//     override (read by Scene::LoadAndRenderSkybox when baking the procedural
//     sky), never the ProceduralSkyComponent's serialized m_SunDirection — so
//     the change is visible next frame, never saved, and resets on scene reload
//     / play-stop / server-stop / explicit clear. The sun-direction math lives
//     here as pure float functions (NO glm) so it unit-tests without a renderer;
//     the handler converts the result to glm::vec3.
//
// The handler in McpTools.cpp does the renderer-bound work on the main thread
// (resolve the token to the bool field on PostProcessSettings / FogSettings, flip
// it, read back the new value) and hands the gathered facts here to be turned into
// the tool JSON. Keeping the token<->field mapping and the JSON schema in free
// functions with NO renderer / editor / GPU dependencies means this contract is
// unit-tested directly (the MCP test binary compiles the dispatch core but
// deliberately NOT McpTools.cpp), the same split McpShaderReload.h /
// McpRenderExplain.h use. Only nlohmann::json + the standard library here.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::RenderOverrides
{
    using Json = nlohmann::json;

    // A post-process / fog feature olo_render_toggle_pass can flip. Each value maps
    // to exactly one bool field; the enum -> field binding lives in the McpTools.cpp
    // handler so this header stays renderer-free. PostProcess* live on
    // PostProcessSettings; Fog* live on FogSettings.
    enum class Pass
    {
        Bloom,
        SSAO,
        GTAO,
        SSR,
        SSGI,
        FXAA,
        TAA,
        Vignette,
        ChromaticAberration,
        DepthOfField,
        MotionBlur,
        ColorGrading,
        AutoExposure,
        Fog,
        FogScattering,
        FogVolumetric,
        GodRays,
    };

    // Canonical token + human description for each toggleable pass. The token is
    // the stable, user-facing name the agent passes; the description feeds the
    // introspection path (olo_render_toggle_pass with no name) and the unknown-name
    // error. Order is the display order.
    struct PassInfo
    {
        std::string_view Token;
        Pass Id;
        std::string_view Description;
    };

    inline constexpr std::array<PassInfo, 17> kPasses = { {
        { "bloom", Pass::Bloom, "Bloom (HDR bright-pass glow)" },
        { "ssao", Pass::SSAO, "Screen-space ambient occlusion (also selects the SSAO AO technique)" },
        { "gtao", Pass::GTAO, "Ground-truth ambient occlusion (also selects the GTAO AO technique)" },
        { "ssr", Pass::SSR, "Screen-space reflections (deferred path only)" },
        { "ssgi", Pass::SSGI, "Screen-space global illumination (deferred path only)" },
        { "fxaa", Pass::FXAA, "FXAA anti-aliasing" },
        { "taa", Pass::TAA, "Temporal anti-aliasing" },
        { "vignette", Pass::Vignette, "Vignette" },
        { "chromaticaberration", Pass::ChromaticAberration, "Chromatic aberration (alias: ca)" },
        { "depthoffield", Pass::DepthOfField, "Depth of field (alias: dof)" },
        { "motionblur", Pass::MotionBlur, "Motion blur" },
        { "colorgrading", Pass::ColorGrading, "Color grading (alias: colourgrading)" },
        { "autoexposure", Pass::AutoExposure, "Automatic exposure / eye adaptation (alias: eyeadaptation)" },
        { "fog", Pass::Fog, "Fog & atmospheric fog (master enable)" },
        { "fogscattering", Pass::FogScattering, "Atmospheric scattering (alias: scattering); needs fog enabled" },
        { "fogvolumetric", Pass::FogVolumetric, "Volumetric fog ray-marching (alias: volumetric); needs fog enabled" },
        { "godrays", Pass::GodRays, "Volumetric light shafts / god rays (alias: lightshafts); needs fog enabled" },
    } };

    // Short aliases that resolve to a canonical pass. Separators (space / underscore
    // / hyphen / case) are already handled by Normalize(), so this table only needs
    // genuinely different spellings.
    struct PassAlias
    {
        std::string_view Alias;
        Pass Id;
    };

    inline constexpr std::array<PassAlias, 8> kPassAliases = { {
        { "ca", Pass::ChromaticAberration },
        { "dof", Pass::DepthOfField },
        { "colourgrading", Pass::ColorGrading },
        { "eyeadaptation", Pass::AutoExposure },
        { "scattering", Pass::FogScattering },
        { "volumetric", Pass::FogVolumetric },
        { "lightshafts", Pass::GodRays },
        { "godray", Pass::GodRays },
    } };

    // Lowercase a string and drop every non-alphanumeric character so
    // "Depth Of Field", "depth_of_field" and "depth-of-field" all collapse to the
    // same key. Pure; used to make token matching forgiving.
    [[nodiscard]] inline std::string Normalize(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc))
                out.push_back(static_cast<char>(std::tolower(uc)));
        }
        return out;
    }

    // Resolve a token (canonical name or alias, case / separator insensitive) to a
    // Pass. Returns false for an unknown token.
    [[nodiscard]] inline bool ParsePass(std::string_view token, Pass& out)
    {
        const std::string key = Normalize(token);
        if (key.empty())
            return false;
        for (const auto& info : kPasses)
        {
            if (Normalize(info.Token) == key)
            {
                out = info.Id;
                return true;
            }
        }
        for (const auto& alias : kPassAliases)
        {
            if (Normalize(alias.Alias) == key)
            {
                out = alias.Id;
                return true;
            }
        }
        return false;
    }

    // Canonical token for a Pass (the value reported back in the tool JSON).
    [[nodiscard]] inline const char* PassToken(Pass pass)
    {
        for (const auto& info : kPasses)
        {
            if (info.Id == pass)
                return info.Token.data();
        }
        return "unknown";
    }

    // Canonical pass tokens in display order — for the introspection list and the
    // unknown-name error message.
    [[nodiscard]] inline std::vector<std::string> PassTokens()
    {
        std::vector<std::string> tokens;
        tokens.reserve(kPasses.size());
        for (const auto& info : kPasses)
            tokens.emplace_back(info.Token);
        return tokens;
    }

    // ", "-joined token list for error/help text.
    [[nodiscard]] inline std::string JoinTokens(const std::vector<std::string>& tokens)
    {
        std::string out;
        for (const auto& t : tokens)
        {
            if (!out.empty())
                out += ", ";
            out += t;
        }
        return out;
    }

    // Static metadata for every pass: { name, description }. The handler augments
    // each entry with the live "enabled" state for the introspection response.
    [[nodiscard]] inline Json DescribePasses()
    {
        Json arr = Json::array();
        for (const auto& info : kPasses)
            arr.push_back(Json{ { "name", std::string(info.Token) }, { "description", std::string(info.Description) } });
        return arr;
    }

    // Facts gathered by the handler after flipping a pass.
    struct ToggleResult
    {
        std::string Pass;      // canonical token of the affected pass
        bool Enabled = false;  // the new state after the flip
        bool Previous = false; // the state before the flip
        bool Changed = false;  // Enabled != Previous
        std::string Note;      // optional precondition hint (e.g. SSR needs the deferred path); empty if none
    };

    [[nodiscard]] inline Json ToJson(const ToggleResult& r)
    {
        Json j;
        j["pass"] = r.Pass;
        j["enabled"] = r.Enabled;
        j["previous"] = r.Previous;
        j["changed"] = r.Changed;
        if (!r.Note.empty())
            j["note"] = r.Note;
        return j;
    }

    // ---- Debug views -------------------------------------------------------

    // The raw intermediate buffer the viewport can be switched to. Exactly one is
    // shown at a time (or None = the normal composite).
    enum class DebugView
    {
        None,
        SSAO,
        GTAO,
        SSR,
        SSGI,
        Overdraw,
    };

    struct DebugViewInfo
    {
        std::string_view Token;
        DebugView Id;
        std::string_view Description;
    };

    inline constexpr std::array<DebugViewInfo, 6> kDebugViews = { {
        { "none", DebugView::None, "Normal composite (clear all debug views)" },
        { "ssao", DebugView::SSAO, "Raw SSAO occlusion buffer" },
        { "gtao", DebugView::GTAO, "Raw GTAO occlusion buffer" },
        { "ssr", DebugView::SSR, "Raw screen-space reflection buffer" },
        { "ssgi", DebugView::SSGI, "Raw screen-space GI (indirect-diffuse) buffer" },
        { "overdraw", DebugView::Overdraw, "Per-pixel overdraw heatmap (fragment shade count)" },
    } };

    // Resolve a debug-view mode token (case / separator insensitive). "off" is
    // accepted as an alias for "none". Returns false for an unknown token.
    [[nodiscard]] inline bool ParseDebugView(std::string_view token, DebugView& out)
    {
        const std::string key = Normalize(token);
        if (key.empty() || key == "off")
        {
            out = DebugView::None;
            return true;
        }
        for (const auto& info : kDebugViews)
        {
            if (Normalize(info.Token) == key)
            {
                out = info.Id;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] inline const char* DebugViewToken(DebugView view)
    {
        for (const auto& info : kDebugViews)
        {
            if (info.Id == view)
                return info.Token.data();
        }
        return "unknown";
    }

    [[nodiscard]] inline std::vector<std::string> DebugViewModes()
    {
        std::vector<std::string> modes;
        modes.reserve(kDebugViews.size());
        for (const auto& info : kDebugViews)
            modes.emplace_back(info.Token);
        return modes;
    }

    [[nodiscard]] inline Json DescribeDebugViews()
    {
        Json arr = Json::array();
        for (const auto& info : kDebugViews)
            arr.push_back(Json{ { "name", std::string(info.Token) }, { "description", std::string(info.Description) } });
        return arr;
    }

    // Facts gathered by the handler after setting a debug view. The four flag
    // booleans mirror the *DebugView fields on PostProcessSettings so the agent
    // sees the full state, not just the requested mode. PassEnabled tells whether
    // the buffer backing the active mode is actually being produced this frame (the
    // backing pass is enabled), so the agent isn't left wondering why the viewport
    // is black; it is true for mode "none".
    struct DebugViewResult
    {
        std::string Mode; // canonical token of the active view
        bool SSAODebugView = false;
        bool GTAODebugView = false;
        bool SSRDebugView = false;
        bool SSGIDebugView = false;
        bool OverdrawDebugView = false;
        bool PassEnabled = false; // backing pass enabled so the view renders (true for "none")
        std::string Note;         // actionable hint when PassEnabled is false; empty otherwise
    };

    [[nodiscard]] inline Json ToJson(const DebugViewResult& r)
    {
        Json j;
        j["mode"] = r.Mode;
        j["ssaoDebugView"] = r.SSAODebugView;
        j["gtaoDebugView"] = r.GTAODebugView;
        j["ssrDebugView"] = r.SSRDebugView;
        j["ssgiDebugView"] = r.SSGIDebugView;
        j["overdrawDebugView"] = r.OverdrawDebugView;
        j["passEnabled"] = r.PassEnabled;
        if (!r.Note.empty())
            j["note"] = r.Note;
        return j;
    }

    // ---- Sun / time-of-day override ----------------------------------------
    // Pure sun-direction math for olo_scene_set_time_of_day / olo_scene_set_sun_angle.
    // A "toward-sun" unit direction in world space (Y up), kept as plain doubles so
    // this header stays free of glm; the handler converts to glm::vec3 before
    // handing it to Renderer3D::SetSunDirectionOverride.

    struct SunVec3
    {
        double X = 0.0;
        double Y = 1.0;
        double Z = 0.0;
    };

    // Normalise to a unit vector; a (near-)zero vector collapses to straight up so
    // a degenerate input never yields a NaN direction.
    [[nodiscard]] inline SunVec3 NormalizeSun(SunVec3 d)
    {
        const double len = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
        if (len < 1e-9)
            return SunVec3{ 0.0, 1.0, 0.0 };
        return SunVec3{ d.X / len, d.Y / len, d.Z / len };
    }

    // Build a toward-sun unit direction from an azimuth (yaw) and elevation (pitch),
    // both in degrees. Azimuth is measured from +Z toward +X: 0deg -> +Z, 90deg ->
    // +X. Elevation is the angle above the horizon: 0deg on the horizon, 90deg
    // straight up, negative below. This is the shared spherical->cartesian core both
    // tools resolve to.
    [[nodiscard]] inline SunVec3 SunDirectionFromAngles(double azimuthDegrees, double elevationDegrees)
    {
        constexpr double kDeg2Rad = std::numbers::pi / 180.0;
        const double az = azimuthDegrees * kDeg2Rad;
        const double el = elevationDegrees * kDeg2Rad;
        const double cosE = std::cos(el);
        return NormalizeSun(SunVec3{ cosE * std::sin(az), std::sin(el), cosE * std::cos(az) });
    }

    // Map a 24-hour clock time to a toward-sun direction. The sun rises on the east
    // horizon (+X) at 06:00, climbs to overhead (+Y) at noon, and sets on the west
    // horizon (-X) at 18:00; before 06:00 / after 18:00 the elevation is negative
    // (the sun is below the horizon -> night). Elevation follows a smooth
    // 90*sin(pi*(h-6)/12) arc and the azimuth sweeps east->south->west at 15deg/hr,
    // so the mapping is monotonic and continuous across the day.
    [[nodiscard]] inline SunVec3 SunDirectionFromTimeOfDay(double hours)
    {
        const double elevationDegrees = 90.0 * std::sin(std::numbers::pi * (hours - 6.0) / 12.0);
        const double azimuthDegrees = 90.0 + (hours - 6.0) * 15.0;
        return SunDirectionFromAngles(azimuthDegrees, elevationDegrees);
    }

    // Elevation (degrees above the horizon) of a toward-sun direction; expects a
    // roughly-unit vector. Clamped so a slightly-denormalised input can't trip asin.
    [[nodiscard]] inline double SunElevationDegrees(SunVec3 d)
    {
        constexpr double kRad2Deg = 180.0 / std::numbers::pi;
        return std::asin(std::clamp(d.Y, -1.0, 1.0)) * kRad2Deg;
    }

    // Azimuth (degrees, measured from +Z toward +X, normalised to [0, 360)) of a
    // toward-sun direction — the inverse of SunDirectionFromAngles' azimuth.
    [[nodiscard]] inline double SunAzimuthDegrees(SunVec3 d)
    {
        constexpr double kRad2Deg = 180.0 / std::numbers::pi;
        double az = std::atan2(d.X, d.Z) * kRad2Deg;
        if (az < 0.0)
            az += 360.0;
        return az;
    }

    // Facts gathered by the handler after a set / clear / introspect call on the
    // ephemeral sun-direction override.
    struct SunOverrideResult
    {
        bool Active = false;   // an override is in effect after this call
        bool Cleared = false;  // this call cleared a previously-active override
        SunVec3 Direction{};   // toward-sun unit direction (meaningful when Active)
        bool HasHours = false; // Hours is meaningful (the override came from set_time_of_day)
        double Hours = 0.0;    // 0-24 clock time
        std::string Source;    // "timeOfDay" | "sunAngle" | "cleared" | "current"
        std::string Note;      // optional hint (no procedural sky / below-horizon); empty if none
    };

    [[nodiscard]] inline Json ToJson(const SunOverrideResult& r)
    {
        Json j;
        j["active"] = r.Active;
        j["source"] = r.Source;
        if (r.Cleared)
            j["cleared"] = true;
        if (r.Active)
        {
            j["sunDirection"] = Json::array({ r.Direction.X, r.Direction.Y, r.Direction.Z });
            j["elevationDegrees"] = SunElevationDegrees(r.Direction);
            j["azimuthDegrees"] = SunAzimuthDegrees(r.Direction);
            if (r.HasHours)
                j["hours"] = r.Hours;
        }
        if (!r.Note.empty())
            j["note"] = r.Note;
        return j;
    }
} // namespace OloEngine::MCP::RenderOverrides
