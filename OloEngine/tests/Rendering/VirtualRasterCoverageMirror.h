#pragma once

// =============================================================================
// CPU mirror of OloEditor/assets/shaders/include/VirtualRasterCoverage.glsl —
// the virtual-geometry raster's sub-sample-miss rule (issue #712).
//
// Two consumers, so the mirror is a header rather than a local copy in either:
//   * VirtualRasterCoverageTest.cpp — pins the rule itself, against a
//     brute-force scan, with no GL context (so it runs in CI);
//   * ShaderUnitTests.cpp — dispatches the SHIPPED include through
//     tests/ShaderUnit_VirtualSampleBounds.glsl and compares against this,
//     which is what catches the GLSL and the mirror drifting apart.
//
// Line-for-line with the GLSL. If you change one, change both.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests::VirtualRasterCoverage
{
    // GLSL `ivec2(vec2)` truncates toward zero; so does a C++ float->int
    // conversion. Every value reaching this has already been clamped into int
    // range, exactly as the shader does it.
    inline glm::ivec2 ToIvec2(glm::vec2 v)
    {
        return { static_cast<i32>(v.x), static_cast<i32>(v.y) };
    }

    struct SampleRange
    {
        glm::ivec2 Min{ 0, 0 };
        glm::ivec2 Max{ 0, 0 };
        bool Covers{ false }; // false = reject, the box covers no pixel centre
    };

    // Mirror of OloVirtualSampleRange.
    inline SampleRange SampleRangeFromBox(glm::vec2 bbMin, glm::vec2 bbMax, glm::vec2 viewport)
    {
        SampleRange range;
        range.Min = ToIvec2(glm::clamp(glm::ceil(bbMin - 0.5f), glm::vec2(0.0f), viewport));
        range.Max = ToIvec2(glm::clamp(glm::floor(bbMax - 0.5f), glm::vec2(-1.0f), viewport - 1.0f));
        range.Covers = range.Min.x <= range.Max.x && range.Min.y <= range.Max.y;
        return range;
    }

    // Mirror of OloVirtualTriangleSampleRange.
    inline SampleRange SampleRangeFromTriangle(glm::vec2 s0, glm::vec2 s1, glm::vec2 s2,
                                               glm::vec2 viewport)
    {
        return SampleRangeFromBox(glm::min(s0, glm::min(s1, s2)), glm::max(s0, glm::max(s1, s2)),
                                  viewport);
    }

    // Mirror of OloVirtualSignedArea2. Only its SIGN is contractual: the GPU is
    // free to contract the products into an FMA, so the low bits differ.
    inline f32 SignedArea2(glm::vec2 s0, glm::vec2 s1, glm::vec2 s2)
    {
        return (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
    }
} // namespace OloEngine::Tests::VirtualRasterCoverage
