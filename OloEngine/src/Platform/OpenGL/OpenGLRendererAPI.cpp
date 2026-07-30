#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/OpenGL/OpenGLDebug.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "Platform/OpenGL/OpenGLRHIConversions.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <string_view>

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
        SetDepthFunc(RHI::CompareOp::Less);
        glEnable(GL_LINE_SMOOTH);

        // Validate that the GPU supports enough combined texture units for our highest binding slot.
        // OLO_CORE_VERIFY evaluates the condition in all configurations (including Release)
        // and triggers a debugger break when the check fails.
        {
            GLint maxCombinedUnits = 0;
            glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxCombinedUnits);
            if (static_cast<u32>(maxCombinedUnits) <= ShaderBindingLayout::TEX_PRECIPITATION_NOISE)
            {
                OLO_CORE_ERROR("GPU reports GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS={}, but highest binding slot is {} "
                               "(TEX_PRECIPITATION_NOISE). Renderer cannot function correctly.",
                               maxCombinedUnits, ShaderBindingLayout::TEX_PRECIPITATION_NOISE);
            }
            OLO_CORE_VERIFY(static_cast<u32>(maxCombinedUnits) > ShaderBindingLayout::TEX_PRECIPITATION_NOISE,
                            "GPU texture-unit count ({}) is insufficient for highest binding slot {} "
                            "(TEX_PRECIPITATION_NOISE). Renderer cannot function correctly.",
                            maxCombinedUnits, ShaderBindingLayout::TEX_PRECIPITATION_NOISE);
        }

        EnableStencilTest();
        SetStencilFunc(RHI::CompareOp::Always, 1, 0xFF);
        SetStencilOp(RHI::StencilOp::Keep, RHI::StencilOp::Keep, RHI::StencilOp::Replace);

        // Cache GL_MAX_DRAW_BUFFERS once — SetBlend*ForAttachment are hot paths
        // and glGetIntegerv on every call costs a driver round-trip.
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &m_MaxDrawBuffers);

        // Same reasoning for the tessellation cap — SetPatchVertexCount runs
        // once per terrain/water patch draw.
        glGetIntegerv(GL_MAX_PATCH_VERTICES, &m_MaxPatchVertices);

        // Detect 64-bit shader integer + atomic support once (issue #629). The
        // virtualized-geometry software rasterizer uses a single atomicMin on a
        // packed uint64_t visibility word when BOTH extensions are present, and
        // falls back to the portable two-pass 2x32 scheme otherwise. Core-profile
        // extension enumeration must use glGetStringi (glGetString(GL_EXTENSIONS)
        // returns null in a core context).
        {
            bool hasInt64 = false;
            bool hasAtomicInt64NV = false;
            bool hasAtomicInt64EXT = false;
            GLint extensionCount = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
            for (GLint i = 0; i < extensionCount; ++i)
            {
                const char* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (extension == nullptr)
                    continue;
                std::string_view const name(extension);
                if (name == "GL_ARB_gpu_shader_int64")
                    hasInt64 = true;
                else if (name == "GL_NV_shader_atomic_int64")
                    hasAtomicInt64NV = true;
                else if (name == "GL_EXT_shader_atomic_int64")
                    hasAtomicInt64EXT = true;
            }

            // Two spellings of the same capability. NV is the long-standing NVIDIA one;
            // EXT is the cross-vendor form AMD ships. Probing only NV silently demoted
            // every AMD/Intel GPU that DOES support 64-bit atomics to the slow portable
            // two-pass path. GL_ARB_gpu_shader_int64 is required regardless — it is what
            // provides the uint64_t type the packed visibility buffer is built on.
            m_SupportsInt64Atomics = hasInt64 && (hasAtomicInt64NV || hasAtomicInt64EXT);

            const char* atomicExtension = hasAtomicInt64NV    ? "GL_NV_shader_atomic_int64"
                                          : hasAtomicInt64EXT ? "GL_EXT_shader_atomic_int64"
                                                              : "none";
            OLO_CORE_INFO("GPU 64-bit shader atomics (GL_ARB_gpu_shader_int64: {}, atomic ext: {}): {} "
                          "(virtual-geometry SW rasterizer uses {} visibility path)",
                          hasInt64 ? "yes" : "no", atomicExtension,
                          m_SupportsInt64Atomics ? "supported" : "unsupported",
                          m_SupportsInt64Atomics ? "single-pass 64-bit atomic" : "portable two-pass 2x32");
        }
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

        GLint previousStencilWriteMask = 0;
        const bool clearingStencil = (clearFlags & GL_STENCIL_BUFFER_BIT) != 0;
        bool restoreStencilWriteMask = false;
        if (clearingStencil)
        {
            glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
            if (previousStencilWriteMask == 0)
            {
                // Some passes intentionally lock stencil writes with mask=0.
                // glClear(GL_STENCIL_BUFFER_BIT) with mask=0 triggers debug
                // warning 131076 and clears nothing. Temporarily enable writes.
                glStencilMask(0xFF);
                restoreStencilWriteMask = true;
            }
        }

        // A program left bound by the previous pass would be revalidated
        // against this framebuffer by the driver during the clear (NVIDIA
        // id 131218 vertex-shader recompile) — unbind it for the clear.
        Utils::GLClearProgramGuard programGuard;
        glClear(clearFlags);

        if (restoreStencilWriteMask)
        {
            glStencilMask(static_cast<GLuint>(previousStencilWriteMask));
        }
    }

    void OpenGLRendererAPI::ClearDepthOnly()
    {
        OLO_PROFILE_FUNCTION();

        // Ensure depth writes are enabled before clearing, otherwise glClear silently no-ops
        if (!m_DepthMaskEnabled)
        {
            glDepthMask(GL_TRUE);
        }

        // See Clear(): don't let a stale bound program get revalidated here.
        Utils::GLClearProgramGuard programGuard;
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

        // See Clear(): don't let a stale bound program get revalidated here.
        Utils::GLClearProgramGuard programGuard;
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

    void OpenGLRendererAPI::DrawIndexedRaw(const u32 vaoID, const u32 indexCount, const u32 baseIndex)
    {
        OLO_PROFILE_FUNCTION();

        glBindVertexArray(vaoID);
        // baseIndex is an index count; convert to a byte offset for glDrawElements.
        const void* indexOffset = reinterpret_cast<const void*>(static_cast<uintptr_t>(baseIndex) * sizeof(u32));
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, indexOffset);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, indexCount / 3);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::VerticesRendered, indexCount);
    }

    void OpenGLRendererAPI::DrawIndexedInstancedRaw(const u32 vaoID, const u32 indexCount, const u32 baseIndex, const u32 instanceCount)
    {
        OLO_PROFILE_FUNCTION();

        if (instanceCount == 0 || indexCount == 0)
            return;

        glBindVertexArray(vaoID);
        const void* indexOffset = reinterpret_cast<const void*>(static_cast<uintptr_t>(baseIndex) * sizeof(u32));
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, indexOffset,
                                static_cast<GLsizei>(instanceCount));

        // Keep all "Instanced" frame counters consistent across every path
        // that emits an instanced draw — CommandDispatch::DrawMeshInstanced
        // bumps them for the CPU-batched + GPU-cull paths; this raw entry
        // covers the shadow batcher (ShadowRenderPass) and any future
        // direct caller. Without these, the profiler's "Instanced Draws"
        // tab and the headline counters disagree on the totals.
        auto& profiler = RendererProfiler::GetInstance();
        profiler.IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
        profiler.IncrementCounter(RendererProfiler::MetricType::InstancedDrawCalls, 1);
        profiler.IncrementCounter(RendererProfiler::MetricType::InstancesRendered, instanceCount);
        if (instanceCount > 1)
            profiler.IncrementCounter(RendererProfiler::MetricType::InstancesBatched, instanceCount - 1);
        profiler.IncrementCounter(RendererProfiler::MetricType::TrianglesRendered, (indexCount / 3) * instanceCount);
        profiler.IncrementCounter(RendererProfiler::MetricType::VerticesRendered, indexCount * instanceCount);
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

    void OpenGLRendererAPI::SetCullFace(RHI::CullMode face)
    {
        OLO_PROFILE_FUNCTION();

        glCullFace(Utils::ToGL(face));
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
    void OpenGLRendererAPI::SetDepthFunc(RHI::CompareOp func)
    {
        OLO_PROFILE_FUNCTION();

        glDepthFunc(Utils::ToGL(func));
        // Don't track this as state change - it's just parameter setting
    }

    void OpenGLRendererAPI::SetStencilMask(u32 mask)
    {
        OLO_PROFILE_FUNCTION();

        glStencilMask(static_cast<GLuint>(mask));
    }

    void OpenGLRendererAPI::ClearStencil()
    {
        OLO_PROFILE_FUNCTION();

        GLint previousStencilWriteMask = 0;
        glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
        const bool restoreStencilWriteMask = previousStencilWriteMask == 0;
        if (restoreStencilWriteMask)
        {
            glStencilMask(0xFF);
        }

        // See Clear(): don't let a stale bound program get revalidated here.
        Utils::GLClearProgramGuard programGuard;
        glClear(GL_STENCIL_BUFFER_BIT);

        if (restoreStencilWriteMask)
        {
            glStencilMask(static_cast<GLuint>(previousStencilWriteMask));
        }
    }
    void OpenGLRendererAPI::SetBlendState(bool value)
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if the value actually changes
        if (static bool s_BlendEnabled = false; s_BlendEnabled != value)
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
    void OpenGLRendererAPI::SetBlendFunc(RHI::BlendFactor sfactor, RHI::BlendFactor dfactor)
    {
        OLO_PROFILE_FUNCTION();

        glBlendFunc(Utils::ToGL(sfactor), Utils::ToGL(dfactor));
        // Don't track this as state change - it's just parameter setting
    }

    void OpenGLRendererAPI::SetBlendEquation(RHI::BlendOp mode)
    {
        glBlendEquation(Utils::ToGL(mode));
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
    void OpenGLRendererAPI::SetStencilFunc(RHI::CompareOp func, i32 ref, u32 mask)
    {
        OLO_PROFILE_FUNCTION();

        glStencilFunc(Utils::ToGL(func), static_cast<GLint>(ref), static_cast<GLuint>(mask));
        // Don't track this as state change - it's just parameter setting
    }
    void OpenGLRendererAPI::SetStencilOp(RHI::StencilOp sfail, RHI::StencilOp dpfail, RHI::StencilOp dppass)
    {
        OLO_PROFILE_FUNCTION();

        glStencilOp(Utils::ToGL(sfail), Utils::ToGL(dpfail), Utils::ToGL(dppass));
        // Don't track this as state change - it's just parameter setting
    }
    void OpenGLRendererAPI::SetPolygonMode(RHI::PolygonMode mode)
    {
        OLO_PROFILE_FUNCTION();

        // GL_FRONT_AND_BACK is the only face core-profile glPolygonMode accepts,
        // which is why the neutral signature carries no face parameter at all.
        const GLenum glMode = Utils::ToGL(mode);
        glPolygonMode(GL_FRONT_AND_BACK, glMode);
        // Only track as state change if switching to/from wireframe mode
        static GLenum s_LastMode = GL_FILL;
        if (glMode != s_LastMode)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
            s_LastMode = glMode;
        }
    }
    void OpenGLRendererAPI::EnableScissorTest()
    {
        OLO_PROFILE_FUNCTION();

        // Only track state change if not already enabled
        if (static bool s_ScissorEnabled = false; !s_ScissorEnabled)
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
        if (static bool s_ScissorEnabled = false; s_ScissorEnabled)
        {
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
            s_ScissorEnabled = false;
        }

        glDisable(GL_SCISSOR_TEST);
    }
    void OpenGLRendererAPI::SetScissorBox(i32 x, i32 y, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        glScissor(static_cast<GLint>(x), static_cast<GLint>(y),
                  static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        // Don't track this as state change - it's just parameter setting
    }

    void OpenGLRendererAPI::BindDefaultFramebuffer()
    {
        OLO_PROFILE_FUNCTION();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void OpenGLRendererAPI::BlitFramebufferToDefault(u32 srcFboID, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);

        glBlitNamedFramebuffer(srcFboID, 0,
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    void OpenGLRendererAPI::BindTexture(u32 slot, u32 textureID)
    {
        OLO_PROFILE_FUNCTION();

        glBindTextureUnit(slot, textureID);
    }

    void OpenGLRendererAPI::BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer,
                                             RHI::Access access, RHI::Format format)
    {
        OLO_PROFILE_FUNCTION();

        glBindImageTexture(unit, textureID, static_cast<GLint>(mipLevel), layered ? GL_TRUE : GL_FALSE,
                           static_cast<GLint>(layer), Utils::ToGLImageAccess(access),
                           Utils::ToGLInternalFormat(format));
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

    void OpenGLRendererAPI::DrawElementsIndirectRaw(u32 vaoID, u32 indirectBufferID)
    {
        OLO_PROFILE_FUNCTION();

        if (vaoID == 0 || indirectBufferID == 0)
            return;

        glBindVertexArray(vaoID);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBufferID);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        // The actual instance/triangle counts live on the GPU (the cull
        // compute writes them and we don't sync) — counting one draw call
        // here is the most accurate stat we can record without a CPU readback
        // that would stall the pipeline. The "Instanced Draws" tab still
        // reports per-call mesh handle / instance count via the
        // RendererProfiler hook in CommandDispatch::DrawMeshInstanced.
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
    }

    void OpenGLRendererAPI::MultiDrawElementsIndirectCountRaw(u32 vaoID, u32 indirectBufferID, u32 indirectOffsetBytes,
                                                              u32 parameterBufferID, u32 parameterOffsetBytes,
                                                              u32 maxDrawCount, u32 strideBytes)
    {
        OLO_PROFILE_FUNCTION();

        if (vaoID == 0 || indirectBufferID == 0 || parameterBufferID == 0 || maxDrawCount == 0)
            return;

        glBindVertexArray(vaoID);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBufferID);
        glBindBuffer(GL_PARAMETER_BUFFER, parameterBufferID);
        glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_INT,
                                         reinterpret_cast<const void*>(static_cast<uintptr_t>(indirectOffsetBytes)),
                                         static_cast<GLintptr>(parameterOffsetBytes),
                                         static_cast<GLsizei>(maxDrawCount), static_cast<GLsizei>(strideBytes));
        glBindBuffer(GL_PARAMETER_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        // Like DrawElementsIndirectRaw: the true draw count lives on the GPU
        // (the cull compute wrote it); one call recorded, no readback stall.
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
        const auto bits = std::to_underlying(flags);
        if (bits & std::to_underlying(MemoryBarrierFlags::VertexAttribArray))
            glBarrier |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::ElementArray))
            glBarrier |= GL_ELEMENT_ARRAY_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::Uniform))
            glBarrier |= GL_UNIFORM_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::TextureFetch))
            glBarrier |= GL_TEXTURE_FETCH_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::ShaderImageAccess))
            glBarrier |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::Command))
            glBarrier |= GL_COMMAND_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::PixelBuffer))
            glBarrier |= GL_PIXEL_BUFFER_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::TextureUpdate))
            glBarrier |= GL_TEXTURE_UPDATE_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::BufferUpdate))
            glBarrier |= GL_BUFFER_UPDATE_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::Framebuffer))
            glBarrier |= GL_FRAMEBUFFER_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::TransformFeedback))
            glBarrier |= GL_TRANSFORM_FEEDBACK_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::AtomicCounter))
            glBarrier |= GL_ATOMIC_COUNTER_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::ShaderStorage))
            glBarrier |= GL_SHADER_STORAGE_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::ClientMappedBuffer))
            glBarrier |= GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
        if (bits & std::to_underlying(MemoryBarrierFlags::QueryBuffer))
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

    void OpenGLRendererAPI::SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha)
    {
        OLO_PROFILE_FUNCTION();

        glColorMaski(attachment, red, green, blue, alpha);
    }

    void OpenGLRendererAPI::SetBlendStateForAttachment(u32 attachment, bool enabled)
    {
        OLO_PROFILE_FUNCTION();

        if (attachment >= static_cast<u32>(m_MaxDrawBuffers))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::SetBlendStateForAttachment - attachment index {} exceeds GL_MAX_DRAW_BUFFERS {}",
                           attachment, m_MaxDrawBuffers);
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

    void OpenGLRendererAPI::SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst)
    {
        OLO_PROFILE_FUNCTION();

        if (attachment >= static_cast<u32>(m_MaxDrawBuffers))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::SetBlendFuncForAttachment - attachment index {} exceeds GL_MAX_DRAW_BUFFERS {}",
                           attachment, m_MaxDrawBuffers);
            return;
        }

        glBlendFunci(attachment, Utils::ToGL(src), Utils::ToGL(dst));
    }

    static GLenum ToGLTextureTarget(RendererAPI::TextureTargetType target)
    {
        switch (target)
        {
            case RendererAPI::TextureTargetType::Texture2D:
                return GL_TEXTURE_2D;
            case RendererAPI::TextureTargetType::TextureCubeMap:
                return GL_TEXTURE_CUBE_MAP;
            case RendererAPI::TextureTargetType::Texture2DMultisample:
                return GL_TEXTURE_2D_MULTISAMPLE;
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
        if (u32 maxBuf = static_cast<u32>(maxDrawBuffers); colorAttachmentCount > maxBuf)
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

    u32 OpenGLRendererAPI::CreateTexture2D(u32 width, u32 height, RHI::Format internalFormat)
    {
        OLO_PROFILE_FUNCTION();

        u32 textureID = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &textureID);
        glTextureStorage2D(textureID, 1, Utils::ToGLInternalFormat(internalFormat),
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        return textureID;
    }

    u32 OpenGLRendererAPI::CreateTextureCubemap(u32 width, u32 height, RHI::Format internalFormat)
    {
        OLO_PROFILE_FUNCTION();

        u32 textureID = 0;
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &textureID);
        glTextureStorage2D(textureID, 1, Utils::ToGLInternalFormat(internalFormat),
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        return textureID;
    }

    u32 OpenGLRendererAPI::CreateDepthArrayCompareOffView(u32 srcTextureID, u32 numLayers)
    {
        OLO_PROFILE_FUNCTION();

        // glTextureView needs a *generated but unbound* name (NOT one from
        // glCreateTextures, which already assigns a target). The view aliases
        // the source's immutable DEPTH_COMPONENT32F storage with independent
        // sampler state, so disabling comparison here does not affect the
        // source's sampler2DArrayShadow binding.
        u32 viewID = 0;
        glGenTextures(1, &viewID);
        glTextureView(viewID, GL_TEXTURE_2D_ARRAY, srcTextureID, GL_DEPTH_COMPONENT32F,
                      0, 1, 0, static_cast<GLuint>(numLayers));

        // Raw-depth reads for the PCSS blocker search: comparison OFF, point
        // sampling (interpolating depths across occluder edges would corrupt the
        // average-blocker estimate), white border so out-of-bounds taps read as
        // "far" (no occluder).
        glTextureParameteri(viewID, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glTextureParameteri(viewID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(viewID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(viewID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTextureParameteri(viewID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glTextureParameteri(viewID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
        constexpr std::array<float, 4> borderColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        glTextureParameterfv(viewID, GL_TEXTURE_BORDER_COLOR, borderColor.data());
        return viewID;
    }

    void OpenGLRendererAPI::SetTextureFilter(u32 textureID, RHI::Filter minFilter, RHI::Filter magFilter)
    {
        OLO_PROFILE_FUNCTION();

        glTextureParameteri(textureID, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(Utils::ToGL(minFilter)));
        glTextureParameteri(textureID, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(Utils::ToGL(magFilter)));
    }

    void OpenGLRendererAPI::SetTextureWrap(u32 textureID, RHI::AddressMode wrap)
    {
        OLO_PROFILE_FUNCTION();

        // All three axes. WRAP_R is part of every texture object's sampler state
        // and is simply unused on a 2D target, so setting it there is a no-op
        // rather than an error — which is why one mode for all axes faithfully
        // reproduces every call site the sweep replaced.
        const GLint glWrap = static_cast<GLint>(Utils::ToGL(wrap));
        glTextureParameteri(textureID, GL_TEXTURE_WRAP_S, glWrap);
        glTextureParameteri(textureID, GL_TEXTURE_WRAP_T, glWrap);
        glTextureParameteri(textureID, GL_TEXTURE_WRAP_R, glWrap);
    }

    void OpenGLRendererAPI::UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                                    RHI::Format sourceFormat, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        glTextureSubImage2D(textureID, 0, 0, 0,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                            Utils::ToGLPixelFormat(sourceFormat), Utils::ToGLPixelType(sourceFormat), data);
    }

    bool OpenGLRendererAPI::IsDeviceAvailable() const
    {
        // glad resolves every entry point as a batch inside gladLoadGL, so
        // probing one DSA function pointer answers for all of them. A null
        // pointer means no context has ever been made current in this process,
        // and calling through it segfaults rather than raising a GL error —
        // which is exactly why the check has to happen before the call, not
        // after it via glGetError.
        //
        // Note this is process-wide, not per-thread: glad (built without MX)
        // keeps one global pointer table. That matches the semantics the callers
        // need — "was a device ever brought up" — and is deliberately unchanged
        // from the probe this replaced.
        return glad_glCreateTextures != nullptr;
    }

    void OpenGLRendererAPI::DeleteTexture(u32 textureID)
    {
        OLO_PROFILE_FUNCTION();

        glDeleteTextures(1, &textureID);
    }

    void OpenGLRendererAPI::BeginConditionalRender(u32 queryID)
    {
        OLO_PROFILE_FUNCTION();
        glBeginConditionalRender(queryID, GL_QUERY_BY_REGION_WAIT);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
    }

    void OpenGLRendererAPI::EndConditionalRender()
    {
        OLO_PROFILE_FUNCTION();
        glEndConditionalRender();
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
    }

    u32 OpenGLRendererAPI::GetMaxUniformBlockSize() const
    {
        OLO_PROFILE_FUNCTION();

        GLint size = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &size);
        return static_cast<u32>(size);
    }

    // =========================================================================
    // Phase 2 step 2 (issue #691) — the operations the call-site sweep found
    // the facade had never abstracted. See ADR 0011's "Amendments from Phase 2
    // step 2" for why each has the shape it does.
    // =========================================================================

    // --- Buffer binding points -----------------------------------------------

    void OpenGLRendererAPI::BindUniformBuffer(u32 bindingPoint, u32 bufferID)
    {
        OLO_PROFILE_FUNCTION();

        glBindBufferBase(GL_UNIFORM_BUFFER, bindingPoint, bufferID);
    }

    void OpenGLRendererAPI::BindStorageBuffer(u32 bindingPoint, u32 bufferID)
    {
        OLO_PROFILE_FUNCTION();

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, bufferID);
    }

    // --- Program / VAO / framebuffer binding ----------------------------------

    void OpenGLRendererAPI::BindShaderProgram(u32 programID)
    {
        OLO_PROFILE_FUNCTION();

        glUseProgram(programID);
    }

    void OpenGLRendererAPI::BindVertexArrayRaw(u32 vaoID)
    {
        OLO_PROFILE_FUNCTION();

        glBindVertexArray(vaoID);
    }

    void OpenGLRendererAPI::BindFramebuffer(u32 framebufferID)
    {
        OLO_PROFILE_FUNCTION();

        glBindFramebuffer(GL_FRAMEBUFFER, framebufferID);
    }

    // --- Draws from already-bound geometry -------------------------------------
    //
    // These deliberately do NOT touch RendererProfiler, unlike the
    // DrawIndexedRaw family above. The call sites they replace (CommandDispatch's
    // raw glDrawElements / glDrawArrays, TiledForwardPlus's debug overlay) never
    // incremented it either, and several of them keep their own counters in
    // CommandDispatch::Statistics. Adding increments here would silently change
    // every profiler-derived number the sweep is supposed to leave untouched.

    namespace
    {
        // baseIndex is an index COUNT; glDrawElements wants a byte offset into
        // the bound element buffer, and the stride depends on the index width.
        [[nodiscard]] const void* IndexByteOffset(RHI::IndexType type, u32 baseIndex)
        {
            const uintptr_t stride = (type == RHI::IndexType::UInt16) ? sizeof(u16) : sizeof(u32);
            return reinterpret_cast<const void*>(static_cast<uintptr_t>(baseIndex) * stride);
        }
    } // namespace

    void OpenGLRendererAPI::DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount,
                                             RHI::IndexType indexType, u32 baseIndex)
    {
        OLO_PROFILE_FUNCTION();

        glDrawElements(Utils::ToGL(topology), static_cast<GLsizei>(indexCount), Utils::ToGL(indexType),
                       IndexByteOffset(indexType, baseIndex));
    }

    void OpenGLRendererAPI::DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount,
                                                      RHI::IndexType indexType, u32 baseIndex,
                                                      u32 instanceCount)
    {
        OLO_PROFILE_FUNCTION();

        glDrawElementsInstanced(Utils::ToGL(topology), static_cast<GLsizei>(indexCount),
                                Utils::ToGL(indexType), IndexByteOffset(indexType, baseIndex),
                                static_cast<GLsizei>(instanceCount));
    }

    void OpenGLRendererAPI::DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount)
    {
        OLO_PROFILE_FUNCTION();

        glDrawArrays(Utils::ToGL(topology), static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
    }

    void OpenGLRendererAPI::SetPatchVertexCount(u32 patchVertices)
    {
        OLO_PROFILE_FUNCTION();

        // Validated against the cap cached in Init(). The raw call sites this
        // replaces had no check at all, so an out-of-range value became a
        // silent GL_INVALID_VALUE and the tessellation draw simply did nothing.
        //
        // The `m_MaxPatchVertices > 0` term is load-bearing: a zero cap means
        // Init() never ran (or the query failed), and rejecting on that would
        // turn a missing cache value into "terrain and water silently stop
        // rendering" — a far worse failure than the one this guard prevents.
        // Fail OPEN on an unknown cap; GL will still report a real violation.
        if (patchVertices == 0 || (m_MaxPatchVertices > 0 && patchVertices > static_cast<u32>(m_MaxPatchVertices)))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::SetPatchVertexCount - {} is outside [1, GL_MAX_PATCH_VERTICES={}]",
                           patchVertices, m_MaxPatchVertices);
            return;
        }
        glPatchParameteri(GL_PATCH_VERTICES, static_cast<GLint>(patchVertices));
    }

    // --- Pipeline state the facade was missing -----------------------------------

    void OpenGLRendererAPI::SetFrontFace(RHI::FrontFace face)
    {
        OLO_PROFILE_FUNCTION();

        glFrontFace(Utils::ToGL(face));
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
    }

    void OpenGLRendererAPI::SetBlendFuncSeparate(RHI::BlendFactor srcRGB, RHI::BlendFactor dstRGB,
                                                 RHI::BlendFactor srcAlpha, RHI::BlendFactor dstAlpha)
    {
        OLO_PROFILE_FUNCTION();

        glBlendFuncSeparate(Utils::ToGL(srcRGB), Utils::ToGL(dstRGB),
                            Utils::ToGL(srcAlpha), Utils::ToGL(dstAlpha));
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::StateChanges, 1);
    }

    void OpenGLRendererAPI::SetClearDepth(f32 depth)
    {
        OLO_PROFILE_FUNCTION();

        glClearDepth(static_cast<GLdouble>(depth));
    }

    // --- Named framebuffers --------------------------------------------------------

    u32 OpenGLRendererAPI::CreateFramebuffer()
    {
        OLO_PROFILE_FUNCTION();

        GLuint fbo = 0;
        glCreateFramebuffers(1, &fbo);
        return fbo;
    }

    void OpenGLRendererAPI::DeleteFramebuffer(u32 framebufferID)
    {
        OLO_PROFILE_FUNCTION();

        glDeleteFramebuffers(1, &framebufferID);
    }

    void OpenGLRendererAPI::AttachFramebufferColorTexture(u32 framebufferID, u32 attachmentIndex,
                                                          u32 textureID, u32 mipLevel)
    {
        OLO_PROFILE_FUNCTION();

        glNamedFramebufferTexture(framebufferID, GL_COLOR_ATTACHMENT0 + attachmentIndex, textureID,
                                  static_cast<GLint>(mipLevel));
    }

    void OpenGLRendererAPI::AttachFramebufferDepthTexture(u32 framebufferID, u32 textureID, u32 mipLevel)
    {
        OLO_PROFILE_FUNCTION();

        glNamedFramebufferTexture(framebufferID, GL_DEPTH_ATTACHMENT, textureID, static_cast<GLint>(mipLevel));
    }

    bool OpenGLRendererAPI::IsFramebufferComplete(u32 framebufferID)
    {
        OLO_PROFILE_FUNCTION();

        return glCheckNamedFramebufferStatus(framebufferID, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    void OpenGLRendererAPI::SetFramebufferDrawAttachments(u32 framebufferID, std::span<const u32> attachmentIndices)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = static_cast<u32>(attachmentIndices.size());
        const u32 maxBuf = static_cast<u32>(m_MaxDrawBuffers);
        if (count > maxBuf)
        {
            OLO_CORE_WARN("OpenGLRendererAPI::SetFramebufferDrawAttachments - count {} exceeds "
                          "GL_MAX_DRAW_BUFFERS {}, clamping",
                          count, maxBuf);
            count = maxBuf;
        }

        // 8 covers every framebuffer in the engine (the G-Buffer is the widest
        // at 5); the heap path exists only so a future wider target degrades in
        // performance rather than in correctness.
        if (count <= 8)
        {
            std::array<GLenum, 8> drawBuffers{};
            for (u32 i = 0; i < count; ++i)
            {
                drawBuffers[i] = Utils::ToGLColorAttachment(attachmentIndices[i]);
            }
            glNamedFramebufferDrawBuffers(framebufferID, static_cast<GLsizei>(count), drawBuffers.data());
        }
        else
        {
            std::vector<GLenum> drawBuffers(count);
            for (u32 i = 0; i < count; ++i)
            {
                drawBuffers[i] = Utils::ToGLColorAttachment(attachmentIndices[i]);
            }
            glNamedFramebufferDrawBuffers(framebufferID, static_cast<GLsizei>(count), drawBuffers.data());
        }
    }

    void OpenGLRendererAPI::SetFramebufferReadAttachment(u32 framebufferID, u32 attachmentIndex)
    {
        OLO_PROFILE_FUNCTION();

        glNamedFramebufferReadBuffer(framebufferID, Utils::ToGLColorAttachment(attachmentIndex));
    }

    void OpenGLRendererAPI::ClearFramebufferColorAttachment(u32 framebufferID, u32 attachmentIndex,
                                                            const glm::vec4& color)
    {
        OLO_PROFILE_FUNCTION();

        // The clear-program guard lives HERE, not at the call site. It is an
        // NVIDIA-driver hazard mitigation (the bound program's vertex shader is
        // revalidated against the target at clear time, debug id 131218 — see
        // docs/agent-rules/gl-clear-program-revalidation.md), so it is backend
        // knowledge. Three passes used to construct it themselves, which forced
        // them to include Platform/OpenGL/OpenGLUtilities.h — a backend header in
        // the sweep bucket, which would have left `sweep_glad_includes` able to
        // reach zero while every one of those TUs could still see all of GL
        // transitively. Same placement as ClearDepthOnly()'s existing guard.
        Utils::GLClearProgramGuard programGuard;

        // The third parameter of glClearNamedFramebufferfv with GL_COLOR is a
        // DRAW BUFFER INDEX, not an attachment enum — hence no ToGLColorAttachment.
        glClearNamedFramebufferfv(framebufferID, GL_COLOR, static_cast<GLint>(attachmentIndex), &color.x);
    }

    void OpenGLRendererAPI::ClearFramebufferDepth(u32 framebufferID, f32 depth)
    {
        OLO_PROFILE_FUNCTION();

        Utils::GLClearProgramGuard programGuard;
        glClearNamedFramebufferfv(framebufferID, GL_DEPTH, 0, &depth);
    }

    void OpenGLRendererAPI::BlitFramebuffer(u32 srcFramebufferID, u32 dstFramebufferID,
                                            i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                                            i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                                            RHI::BlitAspect aspect, RHI::Filter filter)
    {
        OLO_PROFILE_FUNCTION();

        glBlitNamedFramebuffer(srcFramebufferID, dstFramebufferID,
                               srcX0, srcY0, srcX1, srcY1,
                               dstX0, dstY0, dstX1, dstY1,
                               Utils::ToGLBlitMask(aspect), Utils::ToGL(filter));
    }

    // --- Raw buffer lifecycle --------------------------------------------------------

    u32 OpenGLRendererAPI::CreateBuffer()
    {
        OLO_PROFILE_FUNCTION();

        GLuint buffer = 0;
        glCreateBuffers(1, &buffer);
        return buffer;
    }

    void OpenGLRendererAPI::DeleteBuffer(u32 bufferID)
    {
        OLO_PROFILE_FUNCTION();

        glDeleteBuffers(1, &bufferID);
    }

    void OpenGLRendererAPI::AllocateBufferStorage(u32 bufferID, u64 sizeBytes, RHI::MemoryResidency residency)
    {
        OLO_PROFILE_FUNCTION();

        glNamedBufferData(bufferID, static_cast<GLsizeiptr>(sizeBytes), nullptr, Utils::ToGL(residency));
    }

    void* OpenGLRendererAPI::AllocatePersistentUploadStorage(u32 bufferID, u64 sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        // Storage flags and map flags must agree or glMapNamedBufferRange fails
        // at map time rather than at allocation time — which is why these are
        // one call rather than two.
        constexpr GLbitfield kFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glNamedBufferStorage(bufferID, static_cast<GLsizeiptr>(sizeBytes), nullptr, kFlags);
        return glMapNamedBufferRange(bufferID, 0, static_cast<GLsizeiptr>(sizeBytes), kFlags);
    }

    void OpenGLRendererAPI::UnmapBuffer(u32 bufferID)
    {
        OLO_PROFILE_FUNCTION();

        glUnmapNamedBuffer(bufferID);
    }

    void OpenGLRendererAPI::UploadBufferSubData(u32 bufferID, u64 offsetBytes, u64 sizeBytes, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        glNamedBufferSubData(bufferID, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(sizeBytes), data);
    }

    void OpenGLRendererAPI::ReadBufferSubData(u32 bufferID, u64 offsetBytes, u64 sizeBytes, void* dest)
    {
        OLO_PROFILE_FUNCTION();

        glGetNamedBufferSubData(bufferID, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(sizeBytes), dest);
    }

    void OpenGLRendererAPI::CopyBufferSubData(u32 srcBufferID, u32 dstBufferID,
                                              u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        glCopyNamedBufferSubData(srcBufferID, dstBufferID,
                                 static_cast<GLintptr>(srcOffsetBytes), static_cast<GLintptr>(dstOffsetBytes),
                                 static_cast<GLsizeiptr>(sizeBytes));
    }

    void OpenGLRendererAPI::ClearBufferUInt(u32 bufferID, u32 value)
    {
        OLO_PROFILE_FUNCTION();

        Utils::GLClearProgramGuard programGuard;
        glClearNamedBufferData(bufferID, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    }

    void OpenGLRendererAPI::ClearBufferFloat(u32 bufferID, f32 value)
    {
        OLO_PROFILE_FUNCTION();

        Utils::GLClearProgramGuard programGuard;
        glClearNamedBufferData(bufferID, GL_R32F, GL_RED, GL_FLOAT, &value);
    }

    // --- Vertex array lifecycle ---------------------------------------------------------

    u32 OpenGLRendererAPI::CreateVertexArray()
    {
        OLO_PROFILE_FUNCTION();

        GLuint vao = 0;
        glCreateVertexArrays(1, &vao);
        return vao;
    }

    void OpenGLRendererAPI::SetVertexArrayIndexBuffer(u32 vaoID, u32 bufferID)
    {
        OLO_PROFILE_FUNCTION();

        glVertexArrayElementBuffer(vaoID, bufferID);
    }

    void OpenGLRendererAPI::DeleteVertexArray(u32 vaoID)
    {
        OLO_PROFILE_FUNCTION();

        glDeleteVertexArrays(1, &vaoID);
    }

    // --- Texture clear / upload / readback ------------------------------------------------

    void OpenGLRendererAPI::ClearTextureFloat(u32 textureID, u32 mipLevel, const glm::vec4& color)
    {
        OLO_PROFILE_FUNCTION();

        Utils::GLClearProgramGuard programGuard;
        glClearTexImage(textureID, static_cast<GLint>(mipLevel), GL_RGBA, GL_FLOAT, &color.x);
    }

    void OpenGLRendererAPI::ClearTextureUInt(u32 textureID, u32 mipLevel, u32 value)
    {
        OLO_PROFILE_FUNCTION();

        Utils::GLClearProgramGuard programGuard;
        glClearTexImage(textureID, static_cast<GLint>(mipLevel), GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    }

    void OpenGLRendererAPI::UploadTextureSubImage2D(u32 textureID, i32 xOffset, i32 yOffset,
                                                    u32 width, u32 height,
                                                    RHI::Format sourceFormat, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        glTextureSubImage2D(textureID, 0, xOffset, yOffset,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                            Utils::ToGLPixelFormat(sourceFormat), Utils::ToGLPixelType(sourceFormat), data);
    }

    void OpenGLRendererAPI::UploadTextureSubImage3D(u32 textureID, i32 xOffset, i32 yOffset, i32 zOffset,
                                                    u32 width, u32 height, u32 depth,
                                                    RHI::Format sourceFormat, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        glTextureSubImage3D(textureID, 0, xOffset, yOffset, zOffset,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height), static_cast<GLsizei>(depth),
                            Utils::ToGLPixelFormat(sourceFormat), Utils::ToGLPixelType(sourceFormat), data);
    }

    namespace
    {
        // GL's error flag is a sticky global, so an error raised by some earlier
        // call would otherwise be attributed to this readback. Drain first, then
        // the post-call check describes THIS call — which is what the bool
        // return promises. (ADR 0011 amendment (7): glGetError does not become a
        // facade entry point, it disappears into these two functions.)
        void DrainGLErrors()
        {
            constexpr u32 kMaxDrain = 32;
            for (u32 i = 0; i < kMaxDrain && glGetError() != GL_NO_ERROR; ++i)
            {
            }
        }
    } // namespace

    bool OpenGLRendererAPI::ReadTextureImage(u32 textureID, u32 mipLevel, RHI::Format destFormat,
                                             sizet destSizeBytes, void* dest)
    {
        OLO_PROFILE_FUNCTION();

        DrainGLErrors();
        glGetTextureImage(textureID, static_cast<GLint>(mipLevel),
                          Utils::ToGLPixelFormat(destFormat), Utils::ToGLPixelType(destFormat),
                          static_cast<GLsizei>(destSizeBytes), dest);
        return glGetError() == GL_NO_ERROR;
    }

    bool OpenGLRendererAPI::ReadTextureSubImage(u32 textureID, u32 mipLevel, i32 x, i32 y, i32 z,
                                                u32 width, u32 height, u32 depth,
                                                RHI::Format destFormat, sizet destSizeBytes, void* dest)
    {
        OLO_PROFILE_FUNCTION();

        DrainGLErrors();
        glGetTextureSubImage(textureID, static_cast<GLint>(mipLevel), x, y, z,
                             static_cast<GLsizei>(width), static_cast<GLsizei>(height), static_cast<GLsizei>(depth),
                             Utils::ToGLPixelFormat(destFormat), Utils::ToGLPixelType(destFormat),
                             static_cast<GLsizei>(destSizeBytes), dest);
        return glGetError() == GL_NO_ERROR;
    }

    void OpenGLRendererAPI::GetTextureDimensions(u32 textureID, u32 mipLevel, u32& outWidth, u32& outHeight)
    {
        OLO_PROFILE_FUNCTION();

        GLint width = 0;
        GLint height = 0;
        glGetTextureLevelParameteriv(textureID, static_cast<GLint>(mipLevel), GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(textureID, static_cast<GLint>(mipLevel), GL_TEXTURE_HEIGHT, &height);
        outWidth = static_cast<u32>(std::max(width, 0));
        outHeight = static_cast<u32>(std::max(height, 0));
    }

    void OpenGLRendererAPI::TextureBarrier()
    {
        OLO_PROFILE_FUNCTION();

        glTextureBarrier();
    }

    // --- Queries ----------------------------------------------------------------------------

    void OpenGLRendererAPI::CreateQueries(RHI::QueryType type, std::span<u32> outQueryIDs)
    {
        OLO_PROFILE_FUNCTION();

        if (outQueryIDs.empty())
        {
            return;
        }
        // glCreateQueries rather than glGenQueries: the DSA form binds the
        // object to its target at creation, so a subsequent glBeginQuery with a
        // mismatched target is an immediate error rather than a latent one.
        glCreateQueries(Utils::ToGL(type), static_cast<GLsizei>(outQueryIDs.size()), outQueryIDs.data());
    }

    void OpenGLRendererAPI::DeleteQueries(std::span<const u32> queryIDs)
    {
        OLO_PROFILE_FUNCTION();

        if (queryIDs.empty())
        {
            return;
        }
        glDeleteQueries(static_cast<GLsizei>(queryIDs.size()), queryIDs.data());
    }

    void OpenGLRendererAPI::BeginQuery(RHI::QueryType type, u32 queryID)
    {
        OLO_PROFILE_FUNCTION();

        glBeginQuery(Utils::ToGL(type), queryID);
    }

    void OpenGLRendererAPI::EndQuery(RHI::QueryType type)
    {
        OLO_PROFILE_FUNCTION();

        glEndQuery(Utils::ToGL(type));
    }

    bool OpenGLRendererAPI::IsQueryResultAvailable(u32 queryID)
    {
        OLO_PROFILE_FUNCTION();

        GLint available = 0;
        glGetQueryObjectiv(queryID, GL_QUERY_RESULT_AVAILABLE, &available);
        return available != 0;
    }

    u32 OpenGLRendererAPI::GetQueryResultU32(u32 queryID)
    {
        OLO_PROFILE_FUNCTION();

        GLuint result = 0;
        glGetQueryObjectuiv(queryID, GL_QUERY_RESULT, &result);
        return result;
    }

    u64 OpenGLRendererAPI::GetQueryResultU64(u32 queryID)
    {
        OLO_PROFILE_FUNCTION();

        GLuint64 result = 0;
        glGetQueryObjectui64v(queryID, GL_QUERY_RESULT, &result);
        return result;
    }

    // --- Fences -------------------------------------------------------------------------------
    //
    // GLsync is an opaque pointer; the facade carries it as a u64 so the same
    // slot can hold a VkFence (a 64-bit handle) without the callers changing.

    u64 OpenGLRendererAPI::CreateFence()
    {
        OLO_PROFILE_FUNCTION();

        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        return reinterpret_cast<u64>(sync);
    }

    RHI::FenceStatus OpenGLRendererAPI::ClientWaitFence(u64 fence, u64 timeoutNanoseconds)
    {
        OLO_PROFILE_FUNCTION();

        if (fence == 0)
        {
            return RHI::FenceStatus::Failed;
        }
        const GLenum result = glClientWaitSync(reinterpret_cast<GLsync>(fence), GL_SYNC_FLUSH_COMMANDS_BIT,
                                               timeoutNanoseconds);
        switch (result)
        {
            case GL_ALREADY_SIGNALED:
                return RHI::FenceStatus::AlreadySignaled;
            case GL_CONDITION_SATISFIED:
                return RHI::FenceStatus::ConditionSatisfied;
            case GL_TIMEOUT_EXPIRED:
                return RHI::FenceStatus::TimeoutExpired;
            default:
                return RHI::FenceStatus::Failed;
        }
    }

    bool OpenGLRendererAPI::IsFenceSignaled(u64 fence)
    {
        OLO_PROFILE_FUNCTION();

        if (fence == 0)
        {
            return false;
        }
        GLint signaled = 0;
        GLsizei length = 0;
        glGetSynciv(reinterpret_cast<GLsync>(fence), GL_SYNC_STATUS, sizeof(signaled), &length, &signaled);
        return signaled == GL_SIGNALED;
    }

    void OpenGLRendererAPI::DestroyFence(u64 fence)
    {
        OLO_PROFILE_FUNCTION();

        if (fence != 0)
        {
            glDeleteSync(reinterpret_cast<GLsync>(fence));
        }
    }

    // --- Debug markers -------------------------------------------------------------------------

    void OpenGLRendererAPI::PushDebugGroup(u32 id, std::string_view label)
    {
        // The capability check belongs here, not at the call site. RGCommandContext
        // used to guard this with `if (GLAD_GL_KHR_debug)` — a glad LOADER symbol,
        // which is not a portable way to ask "does this backend support debug
        // markers" and which kept a backend dependency in a renderer TU. It also
        // does not match `gl[A-Z]`, so the boundary ratchet could never see it
        // (issue #691 Phase 2 step 2; same class of problem as SlugFontProcessor's
        // `glad_glCreateTextures != nullptr` context probe, replaced in step 1).
        if (GLAD_GL_KHR_debug == 0)
        {
            return;
        }
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, id, static_cast<GLsizei>(label.size()), label.data());
    }

    void OpenGLRendererAPI::PopDebugGroup()
    {
        if (GLAD_GL_KHR_debug == 0)
        {
            return;
        }
        glPopDebugGroup();
    }

    // --- Device ----------------------------------------------------------------------------------

    void OpenGLRendererAPI::WaitForDeviceIdle()
    {
        OLO_PROFILE_FUNCTION();

        glFinish();
    }

    u32 OpenGLRendererAPI::GetMaxFramebufferSamples() const
    {
        OLO_PROFILE_FUNCTION();

        GLint samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &samples);
        return static_cast<u32>(std::max(samples, 0));
    }

    u32 OpenGLRendererAPI::GetMaxColorTextureSamples() const
    {
        OLO_PROFILE_FUNCTION();

        GLint samples = 0;
        glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &samples);
        return static_cast<u32>(std::max(samples, 0));
    }

    u32 OpenGLRendererAPI::GetMaxDepthTextureSamples() const
    {
        OLO_PROFILE_FUNCTION();

        GLint samples = 0;
        glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &samples);
        return static_cast<u32>(std::max(samples, 0));
    }

    void OpenGLRendererAPI::SetProgramUniformFloat(u32 programID, std::string_view name, f32 value)
    {
        OLO_PROFILE_FUNCTION();

        // glGetUniformLocation needs a null-terminated name and string_view does
        // not promise one. A stack buffer keeps this allocation-free — the one
        // caller runs per frame.
        std::array<char, 128> nameBuffer{};
        if (name.size() >= nameBuffer.size())
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::SetProgramUniformFloat - uniform name '{}' exceeds {} chars",
                           name, nameBuffer.size() - 1);
            return;
        }
        std::memcpy(nameBuffer.data(), name.data(), name.size());

        const GLint location = glGetUniformLocation(programID, nameBuffer.data());
        if (location == -1)
        {
            // Absent uniform is not an error — the caller uses this to set an
            // optional uniform on shaders that may not declare it.
            return;
        }
        // glProgramUniform1f rather than glUniform1f: it names the program
        // explicitly instead of acting on whatever happens to be bound.
        glProgramUniform1f(programID, location, value);
    }
} // namespace OloEngine
