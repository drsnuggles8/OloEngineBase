#pragma once

// =============================================================================
// ShadowAtlasBias.h — the local-light atlas's depth bias and normal offset,
// authored in TEXELS of an entry's tile and converted at the receiver. The
// perspective twin of issue #1119's cascade bias.
//
// TWIN OF calculateAtlasEntryShadow in
// OloEditor/assets/shaders/include/PBRCommon.glsl: the same numbers, the same
// way, so the conversion can be asserted on a machine with no GPU.
//
// WHY. The atlas compared `projCoords.z - 0.005` in the entry's [0,1] depth.
// A spot or cube-face entry is a perspective projection with a 0.1 m near
// plane, whose depth compresses as 1/d: d(z01)/dd = f n / ((f - n) d^2), about
// 0.1 / d^2. So the constant was 0.05 d^2 metres of world depth -- 5 cm a metre
// from the light, 20 cm at two, 1.8 m at six -- and a crate standing on the
// floor six metres under a spot cast no shadow at all. #1119 fixed exactly
// this for the cascades, whose orthographic range made the same constant metres
// deep; the atlas kept it.
//
// HOW. In texels, like the cascades. An entry's texel grows with distance,
// 2 d / (P00 * tile) metres, while its depth compresses, so the depth one texel
// spans falls as 1/d:
//
//     bias01 = texels * |P32| / (P00 * tile * d)
//
// P00, P22 and P32 come out of the entry matrix itself -- a perspective
// composed with a rigid lookAt -- so nothing extra is uploaded:
//
//     row 3 = -(view axis row)                    |row 3.xyz| = 1
//     row 2 = P22 * (view axis row) + (0,0,0,P32)  => P22 = -dot(row2.xyz, row3.xyz)
//                                                    P32 = row2.w + P22 * row3.w
//     |row 0.xyz| = P00
//
// Both survive the camera-relative shift (it only moves the w column, and the
// P32 expression cancels it) and Vulkan's sampling matrices (they flip row 1
// only).
//
// A depth bias alone cannot cover a grazing receiver: a 3x3 kernel sees the
// receiver's own plane a texel away, and at 70 degrees that plane is three
// texels deeper. So the receiver is ALSO moved along its normal by a fixed
// number of texels, measured at the receiver -- the cascades' world-metre
// normal offset, made scale-free.
//
// NO RENDERER HEADERS: a matrix and three floats in, a bias out.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace OloEngine
{
    /// The entry's perspective terms, recovered from its view-projection.
    struct AtlasEntryProjectionTerms
    {
        f32 P00 = 1.0f;
        f32 P32 = 0.0f;
    };

    [[nodiscard]] inline AtlasEntryProjectionTerms AtlasEntryTerms(const glm::mat4& entryMatrix) noexcept
    {
        const glm::vec4 row2{ entryMatrix[0][2], entryMatrix[1][2], entryMatrix[2][2], entryMatrix[3][2] };
        const glm::vec4 row3{ entryMatrix[0][3], entryMatrix[1][3], entryMatrix[2][3], entryMatrix[3][3] };
        const glm::vec3 axis{ row3 };
        const f32 p22 = -glm::dot(glm::vec3(row2), axis) / std::max(glm::dot(axis, axis), 1.0e-12f);
        AtlasEntryProjectionTerms terms;
        terms.P32 = row2.w + (p22 * row3.w);
        terms.P00 = std::max(glm::length(glm::vec3(entryMatrix[0][0], entryMatrix[1][0], entryMatrix[2][0])), 1.0e-8f);
        return terms;
    }

    /// One texel of the entry's tile, in world metres, at a receiver whose clip
    /// w (its distance along the light's axis) is `clipW`.
    [[nodiscard]] inline f32 AtlasEntryTexelWorld(const glm::mat4& entryMatrix, f32 tileTexels, f32 clipW) noexcept
    {
        return 2.0f * std::max(clipW, 0.0f) / (AtlasEntryTerms(entryMatrix).P00 * std::max(tileTexels, 1.0f));
    }

    /// The [0,1] depth `biasTexels` texels of the entry's tile span at a
    /// receiver whose clip w is `clipW` -- what the shader subtracts.
    [[nodiscard]] inline f32 AtlasEntryDepthBias(const glm::mat4& entryMatrix, f32 tileTexels, f32 biasTexels,
                                                 f32 clipW) noexcept
    {
        const AtlasEntryProjectionTerms terms = AtlasEntryTerms(entryMatrix);
        return biasTexels * std::abs(terms.P32) / (terms.P00 * std::max(tileTexels, 1.0f) * std::max(clipW, 1.0e-6f));
    }
} // namespace OloEngine
