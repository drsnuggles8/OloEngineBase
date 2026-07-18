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
//   * olo_scene_set_time_of_day — write the scene's TimeOfDayComponent clock
//                                  (hours / day-of-year / latitude / pause),
//                                  the serialized, authoritative sun source.
//   * olo_scene_set_sun_angle    — solve for the time of day whose ephemeris
//                                  sun best matches a requested azimuth /
//                                  elevation pair and write THAT into the
//                                  TimeOfDayComponent.
//     Both back the lighting inner loop: move the sun -> olo_screenshot ->
//     move it again. Since issue #633 they write the scene's serialized
//     TimeOfDayComponent (the ephemeral Renderer3D sun-direction override they
//     used to drive is retired), so they are consented PROJECT writes. The
//     angle->time solver lives here as a pure float function (NO Scene, NO
//     renderer) so it unit-tests headlessly; the component lookup/write stays
//     in the McpToolsRender.cpp handlers.
//
// The handler in McpTools.cpp does the renderer-bound work on the main thread
// (resolve the token to the bool field on PostProcessSettings / FogSettings, flip
// it, read back the new value) and hands the gathered facts here to be turned into
// the tool JSON. Keeping the token<->field mapping and the JSON schema in free
// functions with NO renderer / editor / GPU dependencies means this contract is
// unit-tested directly (the MCP test binary compiles the dispatch core but
// deliberately NOT McpTools.cpp), the same split McpShaderReload.h /
// McpRenderExplain.h use. Only nlohmann::json, the scalar typedefs from
// OloEngine/Core/Base.h, and the standard library here.

#include "OloEngine/Core/Base.h"

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
    // The vg* modes are the virtualized-geometry (Nanite-style) visualisations
    // (issue #607 / #629). They do NOT live on PostProcessSettings — they are the
    // VirtualMeshRegistry's debug mode, the same knob olo_virtual_geometry_set
    // { debugMode } drives. They are listed here because an agent hunting for a
    // debug view looks at olo_render_set_debug_view FIRST; the handler routes
    // them to the one registry write path so the two tools can never disagree
    // about what is currently on.
    enum class DebugView
    {
        None,
        SSAO,
        GTAO,
        SSR,
        SSGI,
        Overdraw,
        VGClusterId,
        VGLod,
        VGOverdraw,
    };

    struct DebugViewInfo
    {
        std::string_view Token;
        DebugView Id;
        std::string_view Description;
    };

    inline constexpr std::array<DebugViewInfo, 9> kDebugViews = { {
        { "none", DebugView::None, "Normal composite (clear all debug views)" },
        { "ssao", DebugView::SSAO, "Raw SSAO occlusion buffer" },
        { "gtao", DebugView::GTAO, "Raw GTAO occlusion buffer" },
        { "ssr", DebugView::SSR, "Raw screen-space reflection buffer" },
        { "ssgi", DebugView::SSGI, "Raw screen-space GI (indirect-diffuse) buffer" },
        { "overdraw", DebugView::Overdraw, "Per-pixel overdraw heatmap (fragment shade count)" },
        { "vgclusterid", DebugView::VGClusterId,
          "Virtual geometry: per-pixel cluster id (hashed colour); capture 'VirtualGeometryDebug'" },
        { "vglod", DebugView::VGLod,
          "Virtual geometry: per-pixel DAG LOD level ramp; capture 'VirtualGeometryDebug'" },
        { "vgoverdraw", DebugView::VGOverdraw,
          "Virtual geometry: per-pixel cluster fragment count heat ramp; capture 'VirtualGeometryDebug'" },
    } };

    // True for the three virtualized-geometry modes, whose state lives on the
    // VirtualMeshRegistry rather than PostProcessSettings.
    [[nodiscard]] inline constexpr bool IsVirtualGeometryView(DebugView view)
    {
        return view == DebugView::VGClusterId || view == DebugView::VGLod || view == DebugView::VGOverdraw;
    }

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

        // The VirtualMeshRegistry's live debug mode ("off" | "clusterid" | "lod" |
        // "overdraw") — the SAME state olo_virtual_geometry_set reports, echoed
        // here so the two tools visibly agree.
        std::string VirtualGeometryDebugMode = "off";
        // Render-graph target to capture for the active view, when the view is
        // written to a buffer rather than the viewport (the vg* modes).
        std::string CaptureTarget;
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
        j["virtualGeometryDebugMode"] = r.VirtualGeometryDebugMode;
        j["passEnabled"] = r.PassEnabled;
        if (!r.CaptureTarget.empty())
            j["captureTarget"] = r.CaptureTarget;
        if (!r.Note.empty())
            j["note"] = r.Note;
        return j;
    }

    // ---- Sun-angle -> time-of-day solver ------------------------------------
    // Pure math for olo_scene_set_sun_angle (issue #633): the ephemeral
    // Renderer3D sun-direction override is retired and the serialized
    // TimeOfDayComponent is the single authoritative sun source, so "aim the
    // sun at azimuth A / elevation E" becomes "solve for the clock time whose
    // ephemeris sun best matches that direction"; the handler writes the
    // solved hours into the component.
    //
    // Conventions (matching Atmosphere/Ephemeris.h, which itself kept the old
    // override tools' frame): +Y up, azimuth in degrees measured from +Z
    // (north) toward +X (east) — 90 = east, 180 = south, 270 = west;
    // elevation in degrees above the horizon (negative = below). The azimuth
    // is honoured ONLY for its east/west side (the morning/afternoon branch of
    // the hour angle): with one knob — the time of day — the solver can hit
    // any *achievable* elevation exactly, but the azimuth then falls out of
    // the ephemeris; matching both would be over-constrained.
    //
    // Model (the same Cooper-declination / hour-angle sphere Ephemeris pins):
    //   sin(elev) = sin(delta)*sin(lat) + cos(delta)*cos(lat)*cos(H)
    // solved for the hour angle H = +/-acos(...), the east (morning) side
    // taking the negative branch; hours = 12 + H / 15deg. When the requested
    // elevation is outside the day's achievable [min, max], the acos operand
    // leaves [-1, 1]: it is clamped (H = 0 -> solar noon, H = pi -> solar
    // midnight), Clamped is set, and AchievedElevationDeg reports the
    // elevation actually reached.

    struct SunAngleSolve
    {
        f32 Hours = 12.0f;               // solved clock time in [0, 24)
        f32 AchievedElevationDeg = 0.0f; // elevation the solved hour actually yields
        bool Clamped = false;            // requested elevation was outside the day's range
    };

    [[nodiscard]] inline SunAngleSolve SolveTimeForSunAngle(f32 elevationDeg, f32 azimuthDeg,
                                                            i32 dayOfYear, f32 latitudeDeg)
    {
        constexpr f64 kDeg2Rad = std::numbers::pi / 180.0;
        constexpr f64 kRad2Deg = 180.0 / std::numbers::pi;

        // Untrusted inputs (agent JSON / scene YAML): non-finite falls back to
        // a sane default, the rest clamps — mirroring EphemerisInputs'
        // sanitizing so the solver can never produce a NaN clock time.
        const f64 elevation = std::isfinite(elevationDeg)
                                  ? std::clamp(static_cast<f64>(elevationDeg), -90.0, 90.0)
                                  : 0.0;
        const f64 azimuth = std::isfinite(azimuthDeg) ? static_cast<f64>(azimuthDeg) : 180.0;
        const f64 latitude = std::isfinite(latitudeDeg)
                                 ? std::clamp(static_cast<f64>(latitudeDeg), -90.0, 90.0) * kDeg2Rad
                                 : 48.0 * kDeg2Rad;
        const i32 day = std::clamp(dayOfYear, 1, 365);

        // Cooper (1969) declination — the same model Ephemeris::SolarDeclination pins.
        const f64 declination = -23.44 * kDeg2Rad *
                                std::cos(2.0 * std::numbers::pi * (static_cast<f64>(day) + 10.0) / 365.0);

        const f64 sinDsinL = std::sin(declination) * std::sin(latitude);
        const f64 cosDcosL = std::cos(declination) * std::cos(latitude);

        SunAngleSolve solve;
        f64 hourAngle = 0.0; // radians; 0 = solar noon, negative = morning

        if (std::abs(cosDcosL) < 1e-9)
        {
            // Degenerate pole case: the elevation is (nearly) constant all
            // day, so noon is as good an answer as any hour.
            solve.Clamped = std::abs(std::sin(elevation * kDeg2Rad) - sinDsinL) > 1e-6;
        }
        else
        {
            const f64 operand = (std::sin(elevation * kDeg2Rad) - sinDsinL) / cosDcosL;
            if (operand >= 1.0)
                hourAngle = 0.0; // above the day's maximum -> solar noon
            else if (operand <= -1.0)
                hourAngle = std::numbers::pi; // below the day's minimum -> solar midnight
            else
                hourAngle = std::acos(operand);
            solve.Clamped = operand > 1.0 || operand < -1.0;

            // East/west branch: an easterly azimuth (sin > 0, i.e. (0, 180)
            // mod 360) is the morning sun -> negative hour angle; westerly is
            // the afternoon. Only the side is honoured — see the note above.
            if (std::sin(azimuth * kDeg2Rad) > 0.0)
                hourAngle = -hourAngle;
        }

        // The elevation actually reached at the solved hour (== the requested
        // one when not clamped, up to fp): cos is even, so the morning /
        // afternoon branch sign does not change it.
        solve.AchievedElevationDeg = static_cast<f32>(
            std::asin(std::clamp(sinDsinL + cosDcosL * std::cos(hourAngle), -1.0, 1.0)) * kRad2Deg);

        f64 hours = 12.0 + (hourAngle * kRad2Deg) / 15.0; // 15 deg of hour angle per hour
        if (hours >= 24.0)
            hours -= 24.0;
        if (hours < 0.0)
            hours += 24.0;
        solve.Hours = static_cast<f32>(hours);
        return solve;
    }
} // namespace OloEngine::MCP::RenderOverrides
