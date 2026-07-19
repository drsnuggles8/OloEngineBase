#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>

// =============================================================================
// Octahedral impostor mapping — CPU-side mirror of the GLSL used by the
// impostor bake + card shaders (Impostor_Bake.glsl / Impostor_Card.glsl).
//
// All directions are OBJECT SPACE with +Y up (the axis the octahedron is
// folded around) — matching foliage/tree local space where the trunk grows
// +Y. Kept header-only + free-function so the atlas-layout math is unit
// testable with no GL context (ImpostorOctahedralTest).
//
// References: Ryan Brucks "Octahedral Impostors" (UE), the Godot-Octahedral-
// Impostors port, Narkowicz octahedron encoding. See issue #433 / the
// scratch research report.
// =============================================================================

namespace OloEngine::Impostor
{
    // Sign that never returns 0 — sign(0) == 0 would zero a folded octahedron
    // coordinate for an axis-aligned view direction (a head-on +X/+Z look, where
    // a component is exactly 0), corrupting the encode. Narkowicz's octEncode
    // uses exactly this "sign-not-zero".
    [[nodiscard]] inline f32 SignNotZero(f32 v)
    {
        return (v >= 0.0f) ? 1.0f : -1.0f;
    }

    // ── Direction ↔ octahedral square, both in [-1, 1]^2 ──────────────────

    // Full-sphere: any unit direction -> point on the folded octahedron square.
    [[nodiscard]] inline glm::vec2 DirectionToOctaSphere(glm::vec3 dir)
    {
        dir = glm::normalize(dir);
        const f32 sum = std::abs(dir.x) + std::abs(dir.y) + std::abs(dir.z); // L1 norm
        glm::vec3 oct = (sum != 0.0f) ? dir / sum : glm::vec3(0.0f, 1.0f, 0.0f);
        if (oct.y < 0.0f)
        {
            const glm::vec2 a = glm::abs(glm::vec2(oct.x, oct.z));
            const f32 sx = SignNotZero(oct.x);
            const f32 sz = SignNotZero(oct.z);
            oct.x = sx * (1.0f - a.y);
            oct.z = sz * (1.0f - a.x);
        }
        return glm::vec2(oct.x, oct.z);
    }

    // Full-sphere inverse: octahedron-square coord in [-1,1] -> unit direction.
    [[nodiscard]] inline glm::vec3 OctaSphereToDirection(glm::vec2 coord)
    {
        glm::vec3 p(coord.x, 0.0f, coord.y);
        const glm::vec2 a = glm::abs(glm::vec2(p.x, p.z));
        p.y = 1.0f - a.x - a.y;
        if (p.y < 0.0f)
        {
            const f32 sx = (p.x >= 0.0f) ? 1.0f : -1.0f;
            const f32 sz = (p.z >= 0.0f) ? 1.0f : -1.0f;
            p.x = sx * (1.0f - a.y);
            p.z = sz * (1.0f - a.x);
        }
        return glm::normalize(p);
    }

    // Hemi-octahedron (upper hemisphere only, y >= 0). Spends the whole square
    // on the visible hemisphere -> ~2x angular resolution for ground foliage /
    // trees never viewed from below. Returns coord in [-1, 1]^2.
    [[nodiscard]] inline glm::vec2 DirectionToOctaHemi(glm::vec3 dir)
    {
        dir.y = glm::max(dir.y, 0.001f);
        dir = glm::normalize(dir);
        const glm::vec3 octant = glm::sign(dir);
        const f32 sum = glm::dot(dir, octant);
        const glm::vec3 oct = (sum != 0.0f) ? dir / sum : glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::vec2(oct.x + oct.z, oct.z - oct.x);
    }

    // Hemi-octahedron inverse: square coord in [-1,1] -> upper-hemisphere dir.
    [[nodiscard]] inline glm::vec3 OctaHemiToDirection(glm::vec2 coord)
    {
        // coord.x = x+z, coord.y = z-x  =>  x = (coord.x - coord.y)/2, z = (coord.x + coord.y)/2
        glm::vec3 p(0.5f * (coord.x - coord.y), 0.0f, 0.5f * (coord.x + coord.y));
        const glm::vec2 a = glm::abs(glm::vec2(p.x, p.z));
        p.y = 1.0f - a.x - a.y;
        return glm::normalize(p);
    }

    // ── Unified layout wrappers (pick hemi vs full by a bool) ─────────────

    [[nodiscard]] inline glm::vec2 DirectionToOcta(glm::vec3 dir, bool hemi)
    {
        return hemi ? DirectionToOctaHemi(dir) : DirectionToOctaSphere(dir);
    }

    [[nodiscard]] inline glm::vec3 OctaToDirection(glm::vec2 coord, bool hemi)
    {
        return hemi ? OctaHemiToDirection(coord) : OctaSphereToDirection(coord);
    }

    // Map a view direction to continuous grid coordinates in [0, framesPerAxis-1].
    // framesPerAxis (N) is the vertex count per axis; there are N*N captured
    // frames and (N-1)^2 cells.
    [[nodiscard]] inline glm::vec2 DirectionToGrid(glm::vec3 dir, u32 framesPerAxis, bool hemi)
    {
        const glm::vec2 oct = DirectionToOcta(dir, hemi);
        const glm::vec2 uv01 = glm::clamp(oct * 0.5f + 0.5f, glm::vec2(0.0f), glm::vec2(1.0f));
        return uv01 * static_cast<f32>(framesPerAxis - 1u);
    }

    // The direction a given integer frame (tile) was captured from.
    [[nodiscard]] inline glm::vec3 FrameToDirection(glm::ivec2 frame, u32 framesPerAxis, bool hemi)
    {
        const f32 denom = static_cast<f32>(framesPerAxis - 1u);
        const glm::vec2 uv01(static_cast<f32>(frame.x) / denom, static_cast<f32>(frame.y) / denom);
        const glm::vec2 coord = uv01 * 2.0f - 1.0f;
        return OctaToDirection(coord, hemi);
    }

    // ── 3-tile barycentric blend across the surrounding octahedral cell ───

    struct TileBlend
    {
        std::array<glm::ivec2, 3> Frames{}; // integer tile coordinates
        std::array<f32, 3> Weights{};       // barycentric weights, sum == 1
    };

    // Godot "quadBlendWeights": split the unit cell along its diagonal and
    // return the 3 enclosing frames + barycentric weights for the given
    // continuous grid coordinate. Blending the 3 frames removes slice popping.
    [[nodiscard]] inline TileBlend ComputeTileBlend(glm::vec2 grid, u32 framesPerAxis)
    {
        const f32 maxIndex = static_cast<f32>(framesPerAxis - 1u);
        const glm::vec2 gridFloor = glm::min(glm::floor(grid), glm::vec2(maxIndex));
        const glm::vec2 f = grid - gridFloor;

        TileBlend out;
        out.Weights[0] = glm::min(1.0f - f.x, 1.0f - f.y);
        out.Weights[1] = std::abs(f.x - f.y);
        out.Weights[2] = glm::min(f.x, f.y);

        const glm::ivec2 base(gridFloor);
        const glm::ivec2 diagonal = (f.x > f.y) ? glm::ivec2(1, 0) : glm::ivec2(0, 1);
        const glm::ivec2 maxFrame(static_cast<i32>(maxIndex));

        out.Frames[0] = base;
        out.Frames[1] = glm::clamp(base + diagonal, glm::ivec2(0), maxFrame);
        out.Frames[2] = glm::clamp(base + glm::ivec2(1, 1), glm::ivec2(0), maxFrame);
        return out;
    }

    // ── LOD cross-fade weight ─────────────────────────────────────────────

    // Fraction of the impostor to show at a given camera distance. 0 below the
    // start distance (pure mesh/billboard), ramps to 1 across the transition
    // band [start, start + band]. Mesh visibility is (1 - this).
    [[nodiscard]] inline f32 ImpostorFade(f32 distance, f32 startDistance, f32 band)
    {
        if (band <= 0.0f)
            return (distance >= startDistance) ? 1.0f : 0.0f;
        const f32 t = (distance - startDistance) / band;
        return glm::clamp(t, 0.0f, 1.0f);
    }
} // namespace OloEngine::Impostor
