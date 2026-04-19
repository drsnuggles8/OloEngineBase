// =============================================================================
// RendererValidate.h
//
// Layer-7 "Smoke / Sanity Readback" helpers per
// docs/renderer-testing-strategy.md §7. Tiny, non-visual invariants that can
// run after every pass in debug builds as a continuous safety net.
//
// Not for property testing — those live in OloEngine/tests/. This is the
// runtime side: engine code can call ValidateFramebuffer() after a pass to
// catch catastrophic failures (NaN storms, Inf values, entire-pass-black).
//
// Intentionally cheap to call but expensive to execute (a readback stalls
// the pipeline). Callers should gate on OLO_DEBUG or a runtime flag.
// =============================================================================

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <string_view>

namespace OloEngine::RendererValidate
{
    // Aggregated stats of a float-format framebuffer attachment. Populated by
    // ValidateFramebuffer. Pure-data type so callers can log / assert on it
    // however they want.
    struct AttachmentStats
    {
        f32 m_MinR = 0.0f, m_MinG = 0.0f, m_MinB = 0.0f, m_MinA = 0.0f;
        f32 m_MaxR = 0.0f, m_MaxG = 0.0f, m_MaxB = 0.0f, m_MaxA = 0.0f;
        f64 m_AvgR = 0.0, m_AvgG = 0.0, m_AvgB = 0.0, m_AvgA = 0.0;
        u32 m_NanCount = 0;
        u32 m_InfCount = 0;
        u32 m_PixelCount = 0;
    };

    // Reads back the given color attachment of a float-format framebuffer
    // (RGBA16F / RGBA32F) and returns aggregate stats. Returns all-zero stats
    // (and logs) if the format is unsupported or the readback fails.
    AttachmentStats ReadFloatAttachmentStats(const Ref<Framebuffer>& fb, u32 attachmentIndex);

    // One-shot sanity check for a pass output. Performs the readback, logs an
    // error (and optionally asserts) on:
    //   - NaN pixels present
    //   - Inf pixels present
    //   - Max value > fp16 maximum (65504) — indicates format overflow
    //
    // Returns true if the pass output is sane. Caller decides whether to
    // abort or continue. Skips work entirely unless OLO_DEBUG is defined.
    bool ValidateFramebuffer(const Ref<Framebuffer>& fb, std::string_view passName,
                             u32 attachmentIndex = 0, bool assertOnFailure = false);
} // namespace OloEngine::RendererValidate
