// =============================================================================
// RendererValidate.cpp
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RendererValidate.h"

#include "OloEngine/Core/Log.h"

#include <glad/gl.h>

#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::RendererValidate
{
    namespace
    {
        bool IsFloatColorFormat(FramebufferTextureFormat f)
        {
            return f == FramebufferTextureFormat::RGBA16F || f == FramebufferTextureFormat::RGBA32F || f == FramebufferTextureFormat::RGB16F || f == FramebufferTextureFormat::RGB32F || f == FramebufferTextureFormat::RG16F || f == FramebufferTextureFormat::RG32F;
        }
    } // namespace

    AttachmentStats ReadFloatAttachmentStats(const Ref<Framebuffer>& fb, u32 attachmentIndex)
    {
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
        const u32 pixelCount = width * height;
        if (pixelCount == 0)
            return stats;

        std::vector<f32> pixels(static_cast<std::size_t>(pixelCount) * 4);
        const GLuint tex = static_cast<GLuint>(fb->GetColorAttachmentRendererID(attachmentIndex));
        // Use glGetTextureImage (GL 4.5+ DSA) to avoid binding state churn.
        ::glGetTextureImage(tex, 0, GL_RGBA, GL_FLOAT,
                            static_cast<GLsizei>(pixels.size() * sizeof(f32)), pixels.data());

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

            // NaN is the only float that fails self-equality.
            const bool nanPixel = (r != r) || (g != g) || (b != b) || (a != a);
            // Any channel non-finite and non-NaN → Inf.
            auto IsInf = [](f32 v)
            { return std::isinf(v); };
            const bool infPixel = IsInf(r) || IsInf(g) || IsInf(b) || IsInf(a);

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
        const AttachmentStats stats = ReadFloatAttachmentStats(fb, attachmentIndex);
        if (stats.m_PixelCount == 0)
            return true; // unsupported / empty — treat as "no opinion"

        bool ok = true;
        constexpr f32 kFp16Max = 65504.0f;

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
        const f32 maxChannel = std::max({ stats.m_MaxR, stats.m_MaxG, stats.m_MaxB });
        if (maxChannel > kFp16Max)
        {
            OLO_CORE_ERROR("[{}] attachment {}: max channel {:.2f} exceeds fp16 max ({:.0f})",
                           passName, attachmentIndex, maxChannel, kFp16Max);
            ok = false;
        }

        if (!ok && assertOnFailure)
        {
            OLO_CORE_ASSERT(false, "Pass output validation failed");
        }
        return ok;
#endif
    }
} // namespace OloEngine::RendererValidate
