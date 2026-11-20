#pragma once

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

// =============================================================================
// The constants of the terrain surface shading that are not per-layer, and the
// CPU mirror of the math they feed (issue #1721).
//
// THE ONE PLACE these numbers live. The shaders do not repeat them: they arrive
// in the terrain UBO (ShaderBindingLayout::UBOStructures::TerrainUBO::LayerScale
// and ::MacroParams, GLSL twins u_TerrainLayerScale / u_TerrainMacroParams in
// include/TerrainParamsBlock.glsl), packed by the two functions at the bottom of
// this file. The CPU functions here are what the contract tests evaluate; the
// GLSL twins are in include/TerrainLayerBlend.glsl.
// =============================================================================
namespace OloEngine::TerrainSurfaceShading
{
    // ── Two-scale layer sampling ────────────────────────────────────────────
    //
    // A tiled layer repeats visibly once a tile is a few pixels wide. Each
    // layer is therefore sampled at its own tiling and at a second, much lower
    // one, and the two are blended by view distance: only the near scale inside
    // kNearDistance, only the far scale beyond kFarDistance.
    //
    // The ratio is deliberately not 1/N for an integer N: an integer divisor
    // makes the two grids share seams every N tiles, which reads as a larger
    // grid instead of hiding the small one.
    inline constexpr f32 kFarTilingRatio = 0.137f;
    inline constexpr f32 kNearDistance = 40.0f; // metres: fully near-scale inside
    inline constexpr f32 kFarDistance = 160.0f; // metres: fully far-scale beyond

    // 0 = near scale only, 1 = far scale only; a smoothstep between the two
    // distances. GLSL twin: oloTerrainFarScaleBlend.
    [[nodiscard]] inline f32 FarScaleBlend(const f32 viewDistance)
    {
        const f32 t = std::clamp((viewDistance - kNearDistance) / (kFarDistance - kNearDistance), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // ── Macro map ───────────────────────────────────────────────────────────
    //
    // One RGBA8 texture over the terrain's own [0,1] UV. RGB tints the blended
    // albedo, with byte 128 neutral so the map can brighten as well as darken;
    // alpha is dryness (255 = dry, so an opaque image changes nothing), and
    // wetness darkens the albedo and smooths the surface.
    inline constexpr f32 kMacroTintScale = 255.0f / 128.0f;
    inline constexpr f32 kMacroWetAlbedoFactor = 0.55f;
    inline constexpr f32 kMacroWetRoughness = 0.12f;
    inline constexpr u8 kMacroNeutralTint = 128;
    inline constexpr u8 kMacroDryAlpha = 255;

    struct MacroResult
    {
        glm::vec3 Albedo{ 0.0f };
        f32 Roughness = 0.0f;
    };

    // `macro` is the sampled texel in [0,1]. GLSL twin: oloTerrainApplyMacro.
    [[nodiscard]] inline MacroResult ApplyMacro(const glm::vec3& albedo, const f32 roughness, const glm::vec4& macro)
    {
        const f32 wet = 1.0f - macro.a;
        MacroResult result;
        result.Albedo = albedo * (glm::vec3(macro) * kMacroTintScale) * glm::mix(1.0f, kMacroWetAlbedoFactor, wet);
        result.Roughness = glm::mix(roughness, kMacroWetRoughness, wet);
        return result;
    }

    // ── UBO packing ─────────────────────────────────────────────────────────

    // u_TerrainLayerScale: x = far tiling ratio (0 = single scale), y = near
    // distance, z = far distance, w = unused.
    //
    // `twoScale` is false for a terrain that shades through the virtual
    // texture: a baked tile has no view distance, and the per-pixel fallback an
    // unready sector takes has to keep matching the tiles around it.
    [[nodiscard]] inline glm::vec4 PackLayerScale(const bool twoScale)
    {
        return twoScale ? glm::vec4(kFarTilingRatio, kNearDistance, kFarDistance, 0.0f) : glm::vec4(0.0f);
    }

    // u_TerrainMacroParams: x = macro map bound (0/1), y = tint scale, z = wet
    // albedo factor, w = wet roughness. All zero with no map, and the shader
    // then skips the macro step outright, so the surface is what it was.
    [[nodiscard]] inline glm::vec4 PackMacroParams(const bool macroMapBound)
    {
        return macroMapBound ? glm::vec4(1.0f, kMacroTintScale, kMacroWetAlbedoFactor, kMacroWetRoughness)
                             : glm::vec4(0.0f);
    }
} // namespace OloEngine::TerrainSurfaceShading
