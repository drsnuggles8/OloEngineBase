// =============================================================================
// RendererValidate.h
//
// Layer-7 "Smoke / Sanity Readback" helpers per
// docs/testing.md §6.3 (L7). Tiny, non-visual invariants that can
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
        // Distinct from `m_PixelCount == 0` (which legitimately means
        // "empty / unsupported attachment, no opinion"). Set when
        // glGetTextureImage itself reports a GL error — callers should
        // treat this as a hard validation failure.
        bool m_ReadbackFailed = false;
        // True iff every valid (non-NaN, non-Inf) pixel is RGB-black across
        // R/G/B (|channel| ≤ 1e-6). Alpha is deliberately excluded so a
        // glClear(0,0,0,1) surface that was never drawn into still trips
        // the heuristic. Only meaningful when m_PixelCount > m_NanCount +
        // m_InfCount.
        bool m_IsEntirelyBlack = false;
    };

    // Reads back the given color attachment of a float-format framebuffer
    // (RGBA16F / RGBA32F) and returns aggregate stats.
    //
    // Return-value contract:
    //   - On success, fields (min/max/avg/NaN/Inf counts, m_PixelCount,
    //     m_IsEntirelyBlack) are populated; m_ReadbackFailed is false.
    //   - On an *unsupported* attachment (non-float format, multisampled,
    //     zero-sized, or out-of-range index) the function returns an
    //     all-zero `AttachmentStats` with m_PixelCount == 0 — callers
    //     should treat this as "no opinion", not a failure.
    //   - On a *hard* readback failure (GL error from glGetTextureImage),
    //     `AttachmentStats::m_ReadbackFailed` is set to true. Callers must
    //     check this flag before using any other field — a failed readback
    //     leaves them in an indeterminate state.
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
