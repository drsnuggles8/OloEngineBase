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

        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, textureID);
    }

    void OpenGLRendererAPI::DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ)
    {
        OLO_PROFILE_FUNCTION();

        glDispatchCompute(groupsX, groupsY, groupsZ);
    }

    void OpenGLRendererAPI::MemoryBarrier(u32 barrierBits)
    {
        OLO_PROFILE_FUNCTION();

        glMemoryBarrier(static_cast<GLbitfield>(barrierBits));
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
} // namespace OloEngine
