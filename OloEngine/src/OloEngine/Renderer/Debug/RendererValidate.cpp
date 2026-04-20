// =============================================================================
// RendererValidate.cpp
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RendererValidate.h"

#include "OloEngine/Core/Log.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::RendererValidate
{
    namespace
    {
        bool IsFloatColorFormat(FramebufferTextureFormat f)
        {
            // Only RGBA float formats are supported here. ReadFloatAttachmentStats
            // calls glGetTextureImage with GL_RGBA which would be invalid for
            // RG/RGB textures (base format mismatch). Callers wanting to
            // validate RG/RGB attachments should extend both this predicate
            // and the readback to thread the component count through.
            return f == FramebufferTextureFormat::RGBA16F || f == FramebufferTextureFormat::RGBA32F;
        }
    } // namespace

    AttachmentStats ReadFloatAttachmentStats(const Ref<Framebuffer>& fb, u32 attachmentIndex)
    {
        OLO_PROFILE_FUNCTION();

        AttachmentStats stats{};
        if (!fb)
            return stats;

        const auto& spec = fb->GetSpecification();
        if (attachmentIndex >= spec.Attachments.Attachments.size())
        {
            OLO_CORE_WARN("RendererValidate: attachment index {} out of range (have {})",
                          attachmentIndex, spec.Attachments.Attachments.size());
            return stats;
        }
        const auto fmt = spec.Attachments.Attachments[attachmentIndex].TextureFormat;
        if (!IsFloatColorFormat(fmt))
        {
            OLO_CORE_WARN("RendererValidate: attachment {} is not a float format, skipping readback", attachmentIndex);
            return stats;
        }

        const u32 width = spec.Width;
        const u32 height = spec.Height;
        // Defensive overflow guard: width/height are u32 each, so their
        // product in u32 can wrap for pathological framebuffer sizes
        // (e.g. >64K on either side). Compute in u64 first and bound both:
        //   1) pixel count representable by AttachmentStats::m_PixelCount (u32)
        //   2) byte count representable by GLsizei for glGetTextureImage.
        const u64 pixelCount64 = static_cast<u64>(width) * static_cast<u64>(height);
        constexpr u64 bytesPerPixel = 4ull * sizeof(f32); // RGBA32F readback
        const auto maxBytes = static_cast<u64>(std::numeric_limits<GLsizei>::max());
        const auto maxPixelsByGlsizei = maxBytes / bytesPerPixel;
        const auto maxPixelsByStats = static_cast<u64>(std::numeric_limits<u32>::max());
        const auto kMaxPixels = std::min(maxPixelsByGlsizei, maxPixelsByStats);
        if (pixelCount64 > kMaxPixels)
        {
            OLO_CORE_WARN("RendererValidate: attachment {} is {}x{} (>{} pixels); skipping readback to avoid overflow",
                          attachmentIndex, width, height, kMaxPixels);
            return stats;
        }
        const u32 pixelCount = static_cast<u32>(pixelCount64);
        if (pixelCount == 0)
            return stats;

        std::vector<f32> pixels(static_cast<std::size_t>(pixelCount) * 4);
        const auto byteSize64 = pixelCount64 * bytesPerPixel;
        const auto byteSize = static_cast<GLsizei>(byteSize64);
        const GLuint tex = static_cast<GLuint>(fb->GetColorAttachmentRendererID(attachmentIndex));
        // Drain any stale GL error so we can distinguish a failed readback
        // from a caller-induced error. Use glGetTextureImage (GL 4.5+ DSA)
        // to avoid binding state churn.
        while (::glGetError() != GL_NO_ERROR)
        {
        }
        ::glGetTextureImage(tex, 0, GL_RGBA, GL_FLOAT, byteSize, pixels.data());
        if (const GLenum err = ::glGetError(); err != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("RendererValidate: glGetTextureImage failed (GL error 0x{:x}) for attachment {}; marking readback as failed",
                           static_cast<u32>(err), attachmentIndex);
            stats.m_ReadbackFailed = true;
            return stats;
        }

        // Accumulate into doubles to avoid catastrophic cancellation on large FBs.
        stats.m_MinR = stats.m_MinG = stats.m_MinB = stats.m_MinA = std::numeric_limits<f32>::infinity();
        stats.m_MaxR = stats.m_MaxG = stats.m_MaxB = stats.m_MaxA = -std::numeric_limits<f32>::infinity();
        f64 sumR = 0.0, sumG = 0.0, sumB = 0.0, sumA = 0.0;

        for (u32 p = 0; p < pixelCount; ++p)
        {
            const f32 r = pixels[p * 4 + 0];
            const f32 g = pixels[p * 4 + 1];
            const f32 b = pixels[p * 4 + 2];
            const f32 a = pixels[p * 4 + 3];

            // Use std::isnan / std::isinf directly rather than the self-
            // inequality trick; clearer intent and follows the engine's
            // float-comparison style guide.
            const bool nanPixel = std::isnan(r) || std::isnan(g) || std::isnan(b) || std::isnan(a);
            const bool infPixel = std::isinf(r) || std::isinf(g) || std::isinf(b) || std::isinf(a);

            if (nanPixel)
            {
                ++stats.m_NanCount;
                continue;
            }
            if (infPixel)
            {
                ++stats.m_InfCount;
                continue;
            }

            stats.m_MinR = std::min(stats.m_MinR, r);
            stats.m_MinG = std::min(stats.m_MinG, g);
            stats.m_MinB = std::min(stats.m_MinB, b);
            stats.m_MinA = std::min(stats.m_MinA, a);
            stats.m_MaxR = std::max(stats.m_MaxR, r);
            stats.m_MaxG = std::max(stats.m_MaxG, g);
            stats.m_MaxB = std::max(stats.m_MaxB, b);
            stats.m_MaxA = std::max(stats.m_MaxA, a);
            sumR += r;
            sumG += g;
            sumB += b;
            sumA += a;
        }

        stats.m_PixelCount = pixelCount;
        const u32 valid = pixelCount - stats.m_NanCount - stats.m_InfCount;
        const f64 denom = valid > 0 ? static_cast<f64>(valid) : 1.0;
        stats.m_AvgR = sumR / denom;
        stats.m_AvgG = sumG / denom;
        stats.m_AvgB = sumB / denom;
        stats.m_AvgA = sumA / denom;
        return stats;
    }

    bool ValidateFramebuffer(const Ref<Framebuffer>& fb, std::string_view passName,
                             u32 attachmentIndex, bool assertOnFailure)
    {
#ifndef OLO_DEBUG
        (void)fb;
        (void)passName;
        (void)attachmentIndex;
        (void)assertOnFailure;
        return true;
#else
        OLO_PROFILE_FUNCTION();

        const AttachmentStats stats = ReadFloatAttachmentStats(fb, attachmentIndex);
        if (stats.m_ReadbackFailed)
        {
            // Hard failure: we could not read the attachment, so we cannot
            // validate it. Distinct from empty/unsupported (m_PixelCount == 0).
            OLO_CORE_ERROR("[{}] attachment {}: readback failed; validation could not run",
                           passName, attachmentIndex);
            if (assertOnFailure)
            {
                OLO_CORE_ASSERT(false, "Pass output readback failed");
            }
            return false;
        }
        if (stats.m_PixelCount == 0)
            return true; // unsupported / empty — treat as "no opinion"

        bool ok = true;
        FramebufferTextureFormat format = FramebufferTextureFormat::None;
        if (fb)
        {
            const auto& spec = fb->GetSpecification();
            if (attachmentIndex < spec.Attachments.Attachments.size())
            {
                format = spec.Attachments.Attachments[attachmentIndex].TextureFormat;
            }
        }

        if (stats.m_NanCount > 0)
        {
            OLO_CORE_ERROR("[{}] attachment {}: {} NaN pixels detected", passName,
                           attachmentIndex, stats.m_NanCount);
            ok = false;
        }
        if (stats.m_InfCount > 0)
        {
            OLO_CORE_ERROR("[{}] attachment {}: {} Inf pixels detected", passName,
                           attachmentIndex, stats.m_InfCount);
            ok = false;
        }
        // Numeric-range check is format-dependent:
        // - RGBA16F: clamp to fp16 finite range
        // - RGBA32F: no fp16 clamp (NaN/Inf checks above already catch invalid values)
        if (format == FramebufferTextureFormat::RGBA16F)
        {
            constexpr f32 kFp16Max = 65504.0f;
            constexpr f32 kFp16Min = -65504.0f;

            const f32 maxChannel = std::max({ stats.m_MaxR, stats.m_MaxG, stats.m_MaxB, stats.m_MaxA });
            if (maxChannel > kFp16Max)
            {
                OLO_CORE_ERROR("[{}] attachment {}: max RGBA channel {:.2f} exceeds fp16 max ({:.0f})",
                               passName, attachmentIndex, maxChannel, kFp16Max);
                ok = false;
            }
            const f32 minChannel = std::min({ stats.m_MinR, stats.m_MinG, stats.m_MinB, stats.m_MinA });
            if (minChannel < kFp16Min)
            {
                OLO_CORE_ERROR("[{}] attachment {}: min RGBA channel {:.2f} below fp16 min ({:.0f})",
                               passName, attachmentIndex, minChannel, kFp16Min);
                ok = false;
            }
        }

        if (!ok && assertOnFailure)
        {
            OLO_CORE_ASSERT(false, "Pass output validation failed");
        }
        return ok;
#endif
    }
} // namespace OloEngine::RendererValidate
