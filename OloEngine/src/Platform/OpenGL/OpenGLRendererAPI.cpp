#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/OpenGL/OpenGLDebug.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <glad/gl.h>

namespace OloEngine
{
    void OpenGLRendererAPI::Init()
    {
        OLO_PROFILE_FUNCTION();

#ifdef OLO_DEBUG
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(OpenGLMessageCallback, nullptr);

        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
#endif

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Disable dithering — legacy feature for 8-bit displays that triggers
        // warnings when integer framebuffer attachments (e.g., entity ID) are bound.
        glDisable(GL_DITHER);

        SetDepthTest(true);
        SetDepthFunc(GL_LESS);
        glEnable(GL_LINE_SMOOTH);

        EnableStencilTest();
        SetStencilFunc(GL_ALWAYS, 1, 0xFF);
        SetStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
    void OpenGLRendererAPI::SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
    {
        OLO_PROFILE_FUNCTION();

        glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
    }

    void OpenGLRendererAPI::SetClearColor(const glm::vec4& color)
    {
        OLO_PROFILE_FUNCTION();

        glClearColor(color.r, color.g, color.b, color.a);
    }

    void OpenGLRendererAPI::Clear()
    {
        OLO_PROFILE_FUNCTION();

        GLbitfield clearFlags = GL_COLOR_BUFFER_BIT;
        if (m_DepthTestEnabled)
        {
            clearFlags |= GL_DEPTH_BUFFER_BIT;
        }
        if (m_StencilTestEnabled)
        {
            clearFlags |= GL_STENCIL_BUFFER_BIT;
        }

        glClear(clearFlags);
    }

    void OpenGLRendererAPI::ClearDepthOnly()
    {
        OLO_PROFILE_FUNCTION();

        // Ensure depth writes are enabled before clearing, otherwise glClear silently no-ops
        if (!m_DepthMaskEnabled)
        {
            glDepthMask(GL_TRUE);
        }

        glClear(GL_DEPTH_BUFFER_BIT);

        if (!m_DepthMaskEnabled)
        {
            glDepthMask(GL_FALSE);
        }
    }

    void OpenGLRendererAPI::ClearColorAndDepth()
    {
        OLO_PROFILE_FUNCTION();

        // Ensure depth writes are enabled before clearing, otherwise glClear silently no-ops
        if (!m_DepthMaskEnabled)
        {
            glDepthMask(GL_TRUE);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (!m_DepthMaskEnabled)
        {
            glDepthMask(GL_FALSE);
        }
    }

    Viewport OpenGLRendererAPI::GetViewport() const
    {
        OLO_PROFILE_FUNCTION();

        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        return {
            static_cast<u32>(std::max(vp[0], 0)),
            static_cast<u32>(std::max(vp[1], 0)),
            static_cast<u32>(std::max(vp[2], 0)),
            static_cast<u32>(std::max(vp[3], 0))
        };
    }

    void OpenGLRendererAPI::DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
    {
        OLO_PROFILE_FUNCTION();

        vertexArray->Bind();
        glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(vertexCount));
    }
    void OpenGLRendererAPI::DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount)
    {
        OLO_PROFILE_FUNCTION();

        vertexArray->Bind();
        const u32 count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr);

        // Update profiler counters
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, count / 3);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, count);
    }
    void OpenGLRendererAPI::DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, const u32 indexCount, const u32 instanceCount)
    {
        OLO_PROFILE_FUNCTION();

        vertexArray->Bind();
        const u32 count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(instanceCount));

        // Update profiler counters
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, (count / 3) * instanceCount);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, count * instanceCount);
    }
    void OpenGLRendererAPI::DrawLines(const Ref<VertexArray>& vertexArray, const u32 vertexCount)
    {
        OLO_PROFILE_FUNCTION();

        vertexArray->Bind();
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertexCount));

        // Update profiler counters
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, vertexCount);
    }

    void OpenGLRendererAPI::DrawIndexedPatches(const Ref<VertexArray>& vertexArray, const u32 indexCount, const u32 patchVertices)
    {
        OLO_PROFILE_FUNCTION();

        if (patchVertices == 0)
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::DrawIndexedPatches - patchVertices must be >= 1");
            return;
        }

        GLint maxPatchVerts = 0;
        glGetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVerts);
        if (patchVertices > static_cast<u32>(maxPatchVerts))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::DrawIndexedPatches - patchVertices {} exceeds GL_MAX_PATCH_VERTICES {}",
                           patchVertices, maxPatchVerts);
            return;
        }

        vertexArray->Bind();
        glPatchParameteri(GL_PATCH_VERTICES, static_cast<GLint>(patchVertices));
        u32 count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
        count = (count / patchVertices) * patchVertices; // Trim to whole patches
        if (count == 0)
        {
            return;
        }
        glDrawElements(GL_PATCHES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, nullptr);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, count);
    }

    void OpenGLRendererAPI::DrawIndexedRaw(const u32 vaoID, const u32 indexCount)
    {
        OLO_PROFILE_FUNCTION();

        glBindVertexArray(vaoID);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, nullptr);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, indexCount / 3);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, indexCount);
    }

    void OpenGLRendererAPI::DrawIndexedPatchesRaw(const u32 vaoID, const u32 indexCount, const u32 patchVertices)
    {
        OLO_PROFILE_FUNCTION();

        if (patchVertices == 0)
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::DrawIndexedPatchesRaw - patchVertices must be >= 1");
            return;
        }

        GLint maxPatchVerts = 0;
        glGetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVerts);
        if (patchVertices > static_cast<u32>(maxPatchVerts))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::DrawIndexedPatchesRaw - patchVertices {} exceeds GL_MAX_PATCH_VERTICES {}",
                           patchVertices, maxPatchVerts);
            return;
        }

        glBindVertexArray(vaoID);
        glPatchParameteri(GL_PATCH_VERTICES, static_cast<GLint>(patchVertices));
        glDrawElements(GL_PATCHES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, nullptr);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, indexCount);
    }

    void OpenGLRendererAPI::SetLineWidth(const f32 width)
    {
        OLO_PROFILE_FUNCTION();

        glLineWidth(width);
    }

    void OpenGLRendererAPI::EnableCulling()
    {
        OLO_PROFILE_FUNCTION();

        glEnable(GL_CULL_FACE);
    }

    void OpenGLRendererAPI::DisableCulling()
    {
        OLO_PROFILE_FUNCTION();

        glDisable(GL_CULL_FACE);
    }

    void OpenGLRendererAPI::SetCullFace(GLenum face)
    {
        OLO_PROFILE_FUNCTION();

        glCullFace(face);
    }

    void OpenGLRendererAPI::FrontCull()
    {
        OLO_PROFILE_FUNCTION();

        glCullFace(GL_FRONT);
    }

    void OpenGLRendererAPI::BackCull()
    {
        OLO_PROFILE_FUNCTION();

        glCullFace(GL_BACK);
    }

    void OpenGLRendererAPI::SetDepthMask(bool value)
    {
        OLO_PROFILE_FUNCTION();

        if (m_DepthMaskEnabled != value)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
        }

        m_DepthMaskEnabled = value;
        glDepthMask(value);
    }
    void OpenGLRendererAPI::SetDepthTest(bool value)
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if the value actually changes
        if (m_DepthTestEnabled != value)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
        }

        m_DepthTestEnabled = value;

        if (value)
        {
            glEnable(GL_DEPTH_TEST);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }
    }
    void OpenGLRendererAPI::SetDepthFunc(GLenum func)
    {
        OLO_PROFILE_FUNCTION();

        glDepthFunc(func);
        // Don't track this as state change - it's just parameter setting
    }

    void OpenGLRendererAPI::SetStencilMask(GLuint mask)
    {
        OLO_PROFILE_FUNCTION();

        glStencilMask(mask);
    }

    void OpenGLRendererAPI::ClearStencil()
    {
        OLO_PROFILE_FUNCTION();

        glClear(GL_STENCIL_BUFFER_BIT);
    }
    void OpenGLRendererAPI::SetBlendState(bool value)
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if the value actually changes
        static bool s_BlendEnabled = false;
        if (s_BlendEnabled != value)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
            s_BlendEnabled = value;
        }

        if (value)
        {
            glEnable(GL_BLEND);
        }
        else
        {
            glDisable(GL_BLEND);
        }
    }
    void OpenGLRendererAPI::SetBlendFunc(GLenum sfactor, GLenum dfactor)
    {
        OLO_PROFILE_FUNCTION();

        glBlendFunc(sfactor, dfactor);
        // Don't track this as state change - it's just parameter setting
    }

    void OpenGLRendererAPI::SetBlendEquation(GLenum mode)
    {
        glBlendEquation(mode);
    }
    void OpenGLRendererAPI::EnableStencilTest()
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if not already enabled
        if (!m_StencilTestEnabled)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
        }

        m_StencilTestEnabled = true;
        glEnable(GL_STENCIL_TEST);
    }
    void OpenGLRendererAPI::DisableStencilTest()
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if currently enabled
        if (m_StencilTestEnabled)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
        }

        m_StencilTestEnabled = false;
        glDisable(GL_STENCIL_TEST);
    }

    bool OpenGLRendererAPI::IsStencilTestEnabled() const
    {
        return m_StencilTestEnabled;
    }
    void OpenGLRendererAPI::SetStencilFunc(GLenum func, GLint ref, GLuint mask)
    {
        OLO_PROFILE_FUNCTION();

        glStencilFunc(func, ref, mask);
        // Don't track this as state change - it's just parameter setting
    }
    void OpenGLRendererAPI::SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
    {
        OLO_PROFILE_FUNCTION();

        glStencilOp(sfail, dpfail, dppass);
        // Don't track this as state change - it's just parameter setting
    }
    void OpenGLRendererAPI::SetPolygonMode(GLenum face, GLenum mode)
    {
        OLO_PROFILE_FUNCTION();

        glPolygonMode(face, mode);
        // Only track as state change if switching to/from wireframe mode
        static GLenum s_LastMode = GL_FILL;
        if (mode != s_LastMode)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
            s_LastMode = mode;
        }
    }
    void OpenGLRendererAPI::EnableScissorTest()
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if not already enabled
        static bool s_ScissorEnabled = false;
        if (!s_ScissorEnabled)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
            s_ScissorEnabled = true;
        }

        glEnable(GL_SCISSOR_TEST);
    }
    void OpenGLRendererAPI::DisableScissorTest()
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if currently enabled
        static bool s_ScissorEnabled = false;
        if (s_ScissorEnabled)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
            s_ScissorEnabled = false;
        }

        glDisable(GL_SCISSOR_TEST);
    }
    void OpenGLRendererAPI::SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        OLO_PROFILE_FUNCTION();

        glScissor(x, y, width, height);
        // Don't track this as state change - it's just parameter setting
    }

    void OpenGLRendererAPI::BindDefaultFramebuffer()
    {
        OLO_PROFILE_FUNCTION();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void OpenGLRendererAPI::BindTexture(u32 slot, u32 textureID)
    {
        OLO_PROFILE_FUNCTION();

        glBindTextureUnit(slot, textureID);
    }

    void OpenGLRendererAPI::BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer, GLenum access, GLenum format)
    {
        OLO_PROFILE_FUNCTION();

        glBindImageTexture(unit, textureID, static_cast<GLint>(mipLevel), layered ? GL_TRUE : GL_FALSE, static_cast<GLint>(layer), access, format);
    }

    void OpenGLRendererAPI::DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ)
    {
        OLO_PROFILE_FUNCTION();

        glDispatchCompute(groupsX, groupsY, groupsZ);
    }

    void OpenGLRendererAPI::DrawElementsIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID)
    {
        OLO_PROFILE_FUNCTION();

        vertexArray->Bind();
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBufferID);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
    }

    void OpenGLRendererAPI::DrawArraysIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID)
    {
        OLO_PROFILE_FUNCTION();

        vertexArray->Bind();
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBufferID);
        glDrawArraysIndirect(GL_TRIANGLES, nullptr);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
    }

    void OpenGLRendererAPI::MemoryBarrier(MemoryBarrierFlags flags)
    {
        OLO_PROFILE_FUNCTION();

        if (flags == MemoryBarrierFlags::None)
            return;

        if (flags == MemoryBarrierFlags::All)
        {
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            return;
        }

        GLbitfield glBarrier = 0;
        const auto bits = static_cast<u32>(flags);
        if (bits & static_cast<u32>(MemoryBarrierFlags::VertexAttribArray))
            glBarrier |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::ElementArray))
            glBarrier |= GL_ELEMENT_ARRAY_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::Uniform))
            glBarrier |= GL_UNIFORM_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::TextureFetch))
            glBarrier |= GL_TEXTURE_FETCH_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::ShaderImageAccess))
            glBarrier |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::Command))
            glBarrier |= GL_COMMAND_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::PixelBuffer))
            glBarrier |= GL_PIXEL_BUFFER_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::TextureUpdate))
            glBarrier |= GL_TEXTURE_UPDATE_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::BufferUpdate))
            glBarrier |= GL_BUFFER_UPDATE_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::Framebuffer))
            glBarrier |= GL_FRAMEBUFFER_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::TransformFeedback))
            glBarrier |= GL_TRANSFORM_FEEDBACK_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::AtomicCounter))
            glBarrier |= GL_ATOMIC_COUNTER_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::ShaderStorage))
            glBarrier |= GL_SHADER_STORAGE_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::ClientMappedBuffer))
            glBarrier |= GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
        if (bits & static_cast<u32>(MemoryBarrierFlags::QueryBuffer))
            glBarrier |= GL_QUERY_BUFFER_BARRIER_BIT;

        glMemoryBarrier(glBarrier);
    }

    void OpenGLRendererAPI::SetPolygonOffset(f32 factor, f32 units)
    {
        OLO_PROFILE_FUNCTION();

        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(factor, units);
    }

    void OpenGLRendererAPI::EnableMultisampling()
    {
        OLO_PROFILE_FUNCTION();

        glEnable(GL_MULTISAMPLE);
    }

    void OpenGLRendererAPI::DisableMultisampling()
    {
        OLO_PROFILE_FUNCTION();

        glDisable(GL_MULTISAMPLE);
    }

    void OpenGLRendererAPI::SetColorMask(bool red, bool green, bool blue, bool alpha)
    {
        OLO_PROFILE_FUNCTION();

        glColorMask(red, green, blue, alpha);
    }

    void OpenGLRendererAPI::SetBlendStateForAttachment(u32 attachment, bool enabled)
    {
        OLO_PROFILE_FUNCTION();

        GLint maxDrawBuffers = 0;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
        if (attachment >= static_cast<u32>(maxDrawBuffers))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::SetBlendStateForAttachment - attachment index {} exceeds GL_MAX_DRAW_BUFFERS {}",
                           attachment, maxDrawBuffers);
            return;
        }

        if (enabled)
        {
            glEnablei(GL_BLEND, attachment);
        }
        else
        {
            glDisablei(GL_BLEND, attachment);
        }
    }

    static GLenum ToGLTextureTarget(RendererAPI::TextureTargetType target)
    {
        switch (target)
        {
            case RendererAPI::TextureTargetType::Texture2D:
                return GL_TEXTURE_2D;
            case RendererAPI::TextureTargetType::TextureCubeMap:
                return GL_TEXTURE_CUBE_MAP;
            default:
                OLO_CORE_ERROR("ToGLTextureTarget: Unknown TextureTargetType");
                return GL_TEXTURE_2D;
        }
    }

    void OpenGLRendererAPI::CopyImageSubData(u32 srcID, TextureTargetType srcTarget, u32 dstID, TextureTargetType dstTarget,
                                             u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        glCopyImageSubData(
            srcID, ToGLTextureTarget(srcTarget), 0, 0, 0, 0,
            dstID, ToGLTextureTarget(dstTarget), 0, 0, 0, 0,
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
    }

    void OpenGLRendererAPI::CopyImageSubDataFull(u32 srcID, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                                 u32 dstID, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                                 u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        glCopyImageSubData(
            srcID, ToGLTextureTarget(srcTarget), srcLevel, 0, 0, srcZ,
            dstID, ToGLTextureTarget(dstTarget), dstLevel, 0, 0, dstZ,
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
    }

    void OpenGLRendererAPI::CopyFramebufferToTexture(u32 textureID, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        glCopyTextureSubImage2D(textureID, 0, 0, 0, 0, 0,
                                static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    }

    void OpenGLRendererAPI::SetDrawBuffers(std::span<const u32> attachments)
    {
        OLO_PROFILE_FUNCTION();

        GLint maxDrawBuffers = 0;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
        u32 maxBuf = static_cast<u32>(maxDrawBuffers);

        if (attachments.size() <= maxBuf && attachments.size() <= 16)
        {
            // Stack-allocated path
            std::array<GLenum, 16> drawBuffers{};
            for (std::size_t i = 0; i < attachments.size(); ++i)
            {
                drawBuffers[i] = GL_COLOR_ATTACHMENT0 + attachments[i];
            }
            glDrawBuffers(static_cast<GLsizei>(attachments.size()), drawBuffers.data());
        }
        else
        {
            u32 count = static_cast<u32>(attachments.size());
            if (count > maxBuf)
            {
                OLO_CORE_WARN("OpenGLRendererAPI::SetDrawBuffers - attachment count {} exceeds GL_MAX_DRAW_BUFFERS {}, clamping",
                              count, maxBuf);
                count = maxBuf;
            }
            std::vector<GLenum> drawBuffers(count);
            for (u32 i = 0; i < count; ++i)
            {
                drawBuffers[i] = GL_COLOR_ATTACHMENT0 + attachments[i];
            }
            glDrawBuffers(static_cast<GLsizei>(count), drawBuffers.data());
        }
    }

    void OpenGLRendererAPI::RestoreAllDrawBuffers(u32 colorAttachmentCount)
    {
        OLO_PROFILE_FUNCTION();

        GLint maxDrawBuffers = 0;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
        u32 maxBuf = static_cast<u32>(maxDrawBuffers);

        if (colorAttachmentCount > maxBuf)
        {
            OLO_CORE_WARN("OpenGLRendererAPI::RestoreAllDrawBuffers - count {} exceeds GL_MAX_DRAW_BUFFERS {}, clamping",
                          colorAttachmentCount, maxBuf);
            colorAttachmentCount = maxBuf;
        }

        if (colorAttachmentCount > 16)
        {
            // Heap-allocated path for >16 attachments
            std::vector<GLenum> allBuffers(colorAttachmentCount);
            for (u32 i = 0; i < colorAttachmentCount; ++i)
            {
                allBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            glDrawBuffers(static_cast<GLsizei>(colorAttachmentCount), allBuffers.data());
            return;
        }

        std::array<GLenum, 16> allBuffers{};
        for (u32 i = 0; i < colorAttachmentCount; ++i)
        {
            allBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
        }
        glDrawBuffers(static_cast<GLsizei>(colorAttachmentCount), allBuffers.data());
    }

    u32 OpenGLRendererAPI::CreateTexture2D(u32 width, u32 height, GLenum internalFormat)
    {
        OLO_PROFILE_FUNCTION();

        u32 textureID = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &textureID);
        glTextureStorage2D(textureID, 1, internalFormat,
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        return textureID;
    }

    u32 OpenGLRendererAPI::CreateTextureCubemap(u32 width, u32 height, GLenum internalFormat)
    {
        OLO_PROFILE_FUNCTION();

        u32 textureID = 0;
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &textureID);
        glTextureStorage2D(textureID, 1, internalFormat,
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        return textureID;
    }

    void OpenGLRendererAPI::SetTextureParameter(u32 textureID, GLenum pname, GLint value)
    {
        OLO_PROFILE_FUNCTION();

        glTextureParameteri(textureID, pname, value);
    }

    void OpenGLRendererAPI::UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                                    GLenum format, GLenum type, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        glTextureSubImage2D(textureID, 0, 0, 0,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                            format, type, data);
    }

    void OpenGLRendererAPI::DeleteTexture(u32 textureID)
    {
        OLO_PROFILE_FUNCTION();

        glDeleteTextures(1, &textureID);
    }
} // namespace OloEngine
