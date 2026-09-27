#pragma once

// The weighted-blended OIT target state (McGuire & Bavoil 2013), in one place.
//
// Every OIT contributor draws into OITBuffer with two DIFFERENT per-attachment
// blend functions: the accumulation target sums (One, One) and the revealage
// target multiplies (Zero, OneMinusSrcColor). Both targets must also be fully
// writable. Two kinds of ambient state broke that, silently (#1417):
//
//   * A global SetBlendFunc -- a replayed decal packet applying its own render
//     state, or the particle blend-mode helper -- rewrites BOTH attachments to
//     one function. The accumulation overflows RGBA16F and the revealage is
//     never attenuated, so OITResolve discards the pixel.
//   * A per-attachment write mask an earlier draw left narrowed masks the
//     revealage target, so a contributor's draw never attenuates it. (The same
//     mask used to drop OITPreparePass's clear of that target as well; the GL
//     backend now lifts masks around every colour clear.)
//
// So contributors state the whole thing after anything that may have changed
// it.

#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    inline constexpr u32 kOITAccumAttachment = 0;
    inline constexpr u32 kOITRevealageAttachment = 1;

    /// The accumulation target's format. RGBA32F, not RGBA16F (issue #1468): every
    /// layer adds alpha^2 * weight, the weight saturates at 3e3 within ~11 m of the
    /// camera, and 16-bit float tops out at 65504, so about 22 overlapping opaque-ish
    /// layers (or ONE layer of HDR colour above ~27) summed to inf and the resolve's
    /// inf / inf composited black. 32-bit float neither overflows for any realistic
    /// stack nor flushes the weight's 1e-2 floor to zero, and a uniform weight scale
    /// cannot fix both ends: fp16 spans ~12 decades and alpha^2 * weight uses ~11.5.
    inline constexpr RGResourceFormat kOITAccumFormat = RGResourceFormat::RGBA32Float;

    /// Both OIT targets writable on every channel.
    inline void ApplyWeightedBlendedOITWriteMasks(RendererAPI& api)
    {
        api.SetColorMaskForAttachment(kOITAccumAttachment, true, true, true, true);
        api.SetColorMaskForAttachment(kOITRevealageAttachment, true, true, true, true);
    }

    /// The full WB-OIT draw state: write masks plus the per-attachment
    /// accumulate / multiply blend. Re-state it after anything that may have
    /// set a global blend function or narrowed a write mask.
    inline void ApplyWeightedBlendedOITBlend(RendererAPI& api)
    {
        ApplyWeightedBlendedOITWriteMasks(api);
        api.SetBlendStateForAttachment(kOITAccumAttachment, true);
        api.SetBlendStateForAttachment(kOITRevealageAttachment, true);
        api.SetBlendFuncForAttachment(kOITAccumAttachment, RHI::BlendFactor::One, RHI::BlendFactor::One);
        api.SetBlendFuncForAttachment(kOITRevealageAttachment, RHI::BlendFactor::Zero,
                                      RHI::BlendFactor::OneMinusSrcColor);
    }
} // namespace OloEngine
