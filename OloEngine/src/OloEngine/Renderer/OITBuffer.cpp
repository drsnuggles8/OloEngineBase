#include "OloEnginePCH.h"
#include "OloEngine/Renderer/OITBuffer.h"

#include "OloEngine/Core/Log.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        FramebufferSpecification BuildSpec(u32 width, u32 height)
        {
            FramebufferSpecification spec;
            spec.Width = width;
            spec.Height = height;
            spec.Samples = 1; // WB-OIT accumulation doesn't need MSAA
            spec.Attachments = FramebufferAttachmentSpecification{
                FramebufferTextureSpecification{ FramebufferTextureFormat::RGBA16F },           // Accum
                FramebufferTextureSpecification{ FramebufferTextureFormat::RG16F },             // Revealage (uses R channel only; RG16F chosen for broad driver support)
                FramebufferTextureSpecification{ FramebufferTextureFormat::DEPTH_COMPONENT32F } // Depth (blitted from scene)
            };
            return spec;
        }

        constexpr u32 kMaxOITSize = 8192;
    } // namespace

    Ref<OITBuffer> OITBuffer::Create(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            width = std::max<u32>(width, 1u);
            height = std::max<u32>(height, 1u);
        }
        if (width > kMaxOITSize || height > kMaxOITSize)
        {
            OLO_CORE_WARN("OITBuffer::Create: size ({}x{}) exceeds max {}x{}, clamping.",
                          width, height, kMaxOITSize, kMaxOITSize);
            width = std::min(width, kMaxOITSize);
            height = std::min(height, kMaxOITSize);
        }
        return Ref<OITBuffer>(new OITBuffer(width, height));
    }

    OITBuffer::OITBuffer(u32 width, u32 height)
        : m_Width(width), m_Height(height)
    {
        Recreate();
    }

    void OITBuffer::Recreate()
    {
        m_Framebuffer = Framebuffer::Create(BuildSpec(m_Width, m_Height));
    }

    void OITBuffer::Resize(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        width = std::min(width, kMaxOITSize);
        height = std::min(height, kMaxOITSize);
        if (width == m_Width && height == m_Height)
            return;
        m_Width = width;
        m_Height = height;
        if (m_Framebuffer)
            m_Framebuffer->Resize(m_Width, m_Height);
        else
            Recreate();
    }

    u32 OITBuffer::GetAccumAttachmentID() const
    {
        return m_Framebuffer ? m_Framebuffer->GetColorAttachmentRendererID(Accum) : 0u;
    }

    u32 OITBuffer::GetRevealageAttachmentID() const
    {
        return m_Framebuffer ? m_Framebuffer->GetColorAttachmentRendererID(Revealage) : 0u;
    }

    void OITBuffer::ClearForFrame()
    {
        if (!m_Framebuffer)
            return;
        // Idempotent per frame — OITResolvePass resets the flag after composite.
        // Without this guard, whichever transparent pass runs last (e.g. particles
        // after water) would wipe prior accumulation, leaving OITResolve to
        // composite an empty buffer (water/decals appear to vanish).
        if (m_ClearedThisFrame)
            return;
        m_ClearedThisFrame = true;
        // Use DSA so this does NOT depend on the currently-bound draw FB —
        // callers invoke ClearForFrame() before binding the OIT FB, and the
        // previous (non-DSA) path ended up clearing the scene framebuffer's
        // buffers instead (black viewport + GL_INVALID drawbuffer warnings
        // because the scene FB's RED_INTEGER / GBuffer attachments don't
        // match the float clear).
        const u32 fbo = m_Framebuffer->GetRendererID();
        const glm::vec4 accumClear(0.0f, 0.0f, 0.0f, 0.0f);
        // Accum: (0,0,0,0) — no contribution yet.
        glClearNamedFramebufferfv(fbo, GL_COLOR, static_cast<GLint>(Accum), glm::value_ptr(accumClear));
        // Revealage: 1.0 in R (fully revealed, product starts at 1).
        const glm::vec4 revealClear(1.0f, 0.0f, 0.0f, 0.0f);
        glClearNamedFramebufferfv(fbo, GL_COLOR, static_cast<GLint>(Revealage), glm::value_ptr(revealClear));
        // Depth: far plane. Without this the attachment carries driver-default
        // content (often 0 = near plane), which causes every subsequent
        // water/decal/particle fragment to fail GL_LEQUAL and get discarded
        // — water appears to vanish in Deferred+OIT because nothing ever
        // writes into the accumulation buffer.
        const GLfloat depthClear = 1.0f;
        glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &depthClear);
    }
} // namespace OloEngine
