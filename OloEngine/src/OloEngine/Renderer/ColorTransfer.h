#pragma once

#include "OloEngine/Core/Base.h"

#include <cmath>

namespace OloEngine::ColorTransfer
{
    // The sRGB transfer function, both directions (IEC 61966-2-1): what a GPU
    // applies when it samples or writes an sRGB format. One definition, so the
    // texture cook, the alpha-coverage mips, the reference tracer and the MCP
    // texture probe cannot decode an 8-bit texel differently.
    [[nodiscard]] inline f32 SrgbToLinear(f32 encoded) noexcept
    {
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] inline f32 LinearToSrgb(f32 linear) noexcept
    {
        return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }
} // namespace OloEngine::ColorTransfer
