#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/OpenGL/OpenGLDebug.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "Platform/OpenGL/OpenGLRHIConversions.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <string_view>

namespace OloEngine
{
    OpenGLRendererAPI::~OpenGLRendererAPI()
    {
        // Order matters: the neutral heap releases every descriptor through the
        // backend, so it has to run while the backend is still alive and the
        // context still current. Shutting the backend down first would leave
        // every ARB_bindless_texture handle resident, which keeps its texture
        // permanently immutable for whatever remains of the process.
        //
        // BUT THE ORDER WAS RIGHT AND THE TIMING WAS NOT. This destructor runs
        // from the static destructor of `RenderCommand::s_RendererAPI`, i.e. at
        // atexit — long after the window and its GL context are gone. Every
        // `glMakeTextureHandleNonResidentARB` in the release path then executes
        // against a dead context and faults inside the driver: an access
        // violation in nvoglv64 with a stack running through
        // ReleaseDescriptor -> ReleaseSlotLocked -> Shutdown, and no message,
        // because there is nothing left to log through.
        //
        // Releasing residency is a nicety at process exit — the driver reclaims
        // everything when the context dies — whereas crashing is not. So the
        // teardown happens in ShutdownGpuResources(), called while the context is
        // still current, and this destructor does nothing if that already ran.
        // If it did NOT run we deliberately leak rather than touch dead GL.
        // A never-Init()ed instance held no GPU resources, so there is
        // nothing to warn about: RenderCommand's static-init
        // instance is deliberately replaced by RecreateForSelectedBackend()
        // before any use (ADR 0011 amendment (39)), and warning on THAT
        // destruction was pure noise.
        if (!m_GpuResourcesReleased && m_Initialized)
        {
            OLO_CORE_WARN("[RHI/GL] OpenGLRendererAPI destroyed without ShutdownGpuResources(); "
                          "skipping descriptor-heap teardown because the GL context is likely gone. "
                          "Bindless handles leak until process exit, which is harmless — calling GL "
                          "here is not.");
        }
    }

    void OpenGLRendererAPI::ShutdownGpuResources()
    {
        // Call this while the context is STILL CURRENT — see the destructor.
        if (m_GpuResourcesReleased)
        {
            return;
        }
        RHI::DescriptorHeap::Get().Shutdown();
        m_DescriptorHeapBackend.Shutdown();
        // The cubemap face-upload staging PBO: a lazily created GL buffer that
        // glNamedBufferData grows to the largest face the session ever uploaded (tens of
        // MB for a 2048² HDR skybox) and that nothing ever deleted (#839). GL-only, so
        // the Vulkan teardown forensics that found the rest of that leak class could not
        // see it, and GL has no allocator-teardown assertion to complain.
        //
        // Released HERE rather than from Renderer::Shutdown() for a boundary reason:
        // OpenGLTextureCubemap.h includes <glad/gl.h>, so calling it from Renderer.cpp
        // would drag the GL loader into an engine-core translation unit and undo the
        // `sweep_glad_includes == 0` property RHIBoundaryRatchetTest pins. This entry
        // point is already the GL-only teardown that always runs — Renderer::Shutdown()
        // reaches it through RenderCommand::ShutdownGpuResources(), which the Vulkan path
        // inherits as the empty RendererAPI base default — and it runs while the context
        // is still current, which is exactly the deadline a GL delete has.
        OpenGLTextureCubemap::ShutdownSharedResources();
        m_GpuResourcesReleased = true;
    }

    void OpenGLRendererAPI::Init()
    {
        OLO_PROFILE_FUNCTION();

        m_Initialized = true;

#ifdef OLO_DEBUG
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(OpenGLMessageCallback, nullptr);

        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
#endif

        // Through the facade, not a raw glEnable: SetBlendState owns the
        // m_BlendEnabled mirror a per-attachment withdrawal reads, and a raw
        // call here would leave it claiming "disabled" while GL blends
        // (issue #896).
        SetBlendState(true);
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

        // Same check for storage-buffer binding points (issue #1015). The GL
        // 4.6 minimum is 8, Mesa drivers expose 80 (5 graphics stages x 16),
        // NVIDIA 96; the engine's SSBO_* constants are pinned below
        // SSBO_BINDING_LIMIT (80) at compile time. A driver below that turns
        // every shader declaring a high binding into a compile failure and
        // every glBindBufferBase on it into GL_INVALID_VALUE — hundreds of
        // scattered errors whose cause is this one number. Report it ONCE,
        // here, and keep going: a Release nightly must finish and say so, not
        // trap.
        {
            GLint maxStorageBindings = 0;
            glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxStorageBindings);
            if (maxStorageBindings > 0 && static_cast<u32>(maxStorageBindings) < ShaderBindingLayout::SSBO_BINDING_LIMIT)
            {
                OLO_CORE_ERROR("GPU reports GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS={}, below the engine's "
                               "SSBO_BINDING_LIMIT ({}); the highest SSBO_* slot is {} (SSBO_HIGHEST_BINDING). Shaders "
                               "declaring bindings >= {} will fail to compile and their glBindBufferBase calls "
                               "will raise GL_INVALID_VALUE. See ShaderBindingLayout.h, SSBO_BINDING_LIMIT.",
                               maxStorageBindings, ShaderBindingLayout::SSBO_BINDING_LIMIT,
                               ShaderBindingLayout::SSBO_HIGHEST_BINDING, maxStorageBindings);
            }
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

        // ---------------------------------------------------------------------
        // The descriptor heap (issue #691).
        //
        // Sized generously rather than tightly on purpose: a heap that runs out
        // mid-frame falls back to the slot-based path, which is correct but
        // makes an A/B capture compare two different renderers. 4096 persistent
        // slots covers every texture a scene loads with room to spare, and 1024
        // ring slots is ~16x the busiest frame's transient count.
        //
        // POISON DEFAULTS TO ON IN DEBUG. It costs one buffer write per freed
        // slot and turns a use-after-free from "shows the previous tenant" —
        // which LIFO slot reuse hides in steady state, exactly as the transient
        // POOL hides it — into a deterministic black read. That trade is the
        // same one OLO_RG_POISON_TRANSIENTS makes, except that instrument is
        // opt-in because it costs a full clear per resource and this one does
        // not.
        // ---------------------------------------------------------------------
        {
            m_DescriptorHeapBackend.Initialize(kDescriptorHeapSlots);

            RHI::HeapDesc heapDesc;
            heapDesc.ResourceSlotCapacity = kDescriptorHeapPersistentSlots;
            heapDesc.SamplerSlotCapacity = kDescriptorHeapSamplerSlots;
            heapDesc.FrameTransientRingSlots = kDescriptorHeapTransientSlots;
#ifdef OLO_DEBUG
            heapDesc.PoisonOnFree = true;
#else
            heapDesc.PoisonOnFree = false;
#endif

            RHI::DescriptorHeap::Get().Initialize(heapDesc, &m_DescriptorHeapBackend);
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

        const bool restoreColorMasks = LiftAttachmentColorMasksForClear();

        // A program left bound by the previous pass would be revalidated
        // against this framebuffer by the driver during the clear (NVIDIA
        // id 131218 vertex-shader recompile) — unbind it for the clear.
        Utils::GLClearProgramGuard programGuard;
        glClear(clearFlags);

        if (restoreColorMasks)
        {
            RestoreAttachmentColorMasks();
        }

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

        const bool restoreColorMasks = LiftAttachmentColorMasksForClear();

        // See Clear(): don't let a stale bound program get revalidated here.
        Utils::GLClearProgramGuard programGuard;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (restoreColorMasks)
        {
            RestoreAttachmentColorMasks();
        }

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

        if (m_MaxPatchVertices > 0 && patchVertices > static_cast<u32>(m_MaxPatchVertices))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::DrawIndexedPatches - patchVertices {} exceeds GL_MAX_PATCH_VERTICES {}",
                           patchVertices, m_MaxPatchVertices);
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

        if (m_MaxPatchVertices > 0 && patchVertices > static_cast<u32>(m_MaxPatchVertices))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::DrawIndexedPatchesRaw - patchVertices {} exceeds GL_MAX_PATCH_VERTICES {}",
                           patchVertices, m_MaxPatchVertices);
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

        m_BlendEnabled = value;

        if (value)
        {
            glEnable(GL_BLEND);
        }
        else
        {
            glDisable(GL_BLEND);
        }

        // glEnable(GL_BLEND) is glEnablei for EVERY draw buffer, so the call
        // above just flattened any per-attachment opinion a pass is holding.
        // The engine's contract is that an opinion survives until it is
        // explicitly withdrawn, so put them back -- see the declaration of
        // m_AttachmentBlend (issue #896).
        ReassertAttachmentBlendOpinions();
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

    void OpenGLRendererAPI::BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint srcFboID = Utils::ResolveNativeAs(srcFramebuffer, RHI::ResourceKind::Framebuffer);
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

    void OpenGLRendererAPI::DispatchComputeIndirect(RHI::ResourceHandle argsBuffer, u32 offsetBytes)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint argsBufferID = Utils::ResolveNativeAs(argsBuffer, RHI::ResourceKind::Buffer);
        if (argsBufferID == 0)
            return;

        // The caller is responsible for a GL_COMMAND_BARRIER_BIT between the
        // kernel that wrote these arguments and this dispatch — without it the
        // group count read here is whatever was in the buffer before.
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, argsBufferID);
        glDispatchComputeIndirect(static_cast<GLintptr>(offsetBytes));
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    }

    void OpenGLRendererAPI::DrawMeshTasks(u32 groupsX, u32 groupsY, u32 groupsZ)
    {
        // Facade contract first: a zero group count in any dimension is a
        // legal no-op on EVERY backend (the Vulkan twin returns before its
        // own capability guard too) — a caller that computes an empty launch
        // must not trip the gate warning below, which would then mask a
        // later genuine violation behind the warn-once latch.
        if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
            return;

        // SupportsMeshShaders() is false on this backend (GL_NV_mesh_shader
        // is deliberately out of scope — vendor-specific, never promoted), so
        // a call landing here means the capability gate upstream did not
        // route the work away. Loud once, never silent (issue #813; the
        // MultiDrawElementsIndirectCountRaw warn-once shape on the Vulkan
        // twin).
        static bool s_Warned = false;
        if (!s_Warned)
        {
            s_Warned = true;
            OLO_CORE_ERROR("[RHI/OpenGL] mesh-shader draw reached the OpenGL backend — the capability gate "
                           "should have routed this away; draw dropped");
        }
    }

    void OpenGLRendererAPI::DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint indirectBufferID = Utils::ResolveNativeAs(indirectBuffer, RHI::ResourceKind::Buffer);
        vertexArray->Bind();
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBufferID);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
    }

    void OpenGLRendererAPI::DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer,
                                                      RHI::PrimitiveTopology topology)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint indirectBufferID = Utils::ResolveNativeAs(indirectBuffer, RHI::ResourceKind::Buffer);
        if (indirectBufferID == 0)
            return;

        // No glBindVertexArray: the caller's BindVAOIfNeeded already bound it,
        // and binding here would defeat that cache (see the DrawBound* family).
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBufferID);
        glDrawElementsIndirect(Utils::ToGL(topology), GL_UNSIGNED_INT, nullptr);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        // The actual instance/triangle counts live on the GPU (the cull
        // compute writes them and we don't sync) — counting one draw call
        // here is the most accurate stat we can record without a CPU readback
        // that would stall the pipeline. The "Instanced Draws" tab still
        // reports per-call mesh handle / instance count via the
        // RendererProfiler hook in CommandDispatch::DrawMeshInstanced.
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::DrawCalls, 1);
    }

    void OpenGLRendererAPI::MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray,
                                                              RHI::ResourceHandle indirectBuffer,
                                                              u32 indirectOffsetBytes,
                                                              RHI::ResourceHandle parameterBuffer,
                                                              u32 parameterOffsetBytes,
                                                              u32 maxDrawCount, u32 strideBytes)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint vaoID = Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray);
        const GLuint indirectBufferID = Utils::ResolveNativeAs(indirectBuffer, RHI::ResourceKind::Buffer);
        const GLuint parameterBufferID = Utils::ResolveNativeAs(parameterBuffer, RHI::ResourceKind::Buffer);
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

    void OpenGLRendererAPI::DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint indirectBufferID = Utils::ResolveNativeAs(indirectBuffer, RHI::ResourceKind::Buffer);
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

    void OpenGLRendererAPI::IssueBarrierBatch(const MemoryBarrierFlags flags, std::span<const RHI::Barrier> /*barriers*/)
    {
        // ADR 0011 §1.5: on GL the flags bitmask IS the lowering — the
        // per-resource transitions are the explicit-barrier backends'
        // currency and are deliberately ignored here. Delegating keeps the
        // pre-Phase-5 glMemoryBarrier behaviour byte-identical.
        MemoryBarrier(flags);
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

        // glColorMask sets EVERY draw buffer, so it also clears any
        // per-attachment mask a previous draw installed via glColorMaski.
        const AttachmentColorMask mask{ red, green, blue, alpha };
        m_AttachmentColorMasks.fill(mask);
        m_AnyAttachmentColorMaskDisabled = !mask.IsFullyEnabled();
    }

    void OpenGLRendererAPI::SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha)
    {
        OLO_PROFILE_FUNCTION();

        glColorMaski(attachment, red, green, blue, alpha);

        if (attachment < kMaxTrackedDrawBuffers)
        {
            m_AttachmentColorMasks[attachment] = { red, green, blue, alpha };
            m_AnyAttachmentColorMaskDisabled = false;
            for (const AttachmentColorMask& tracked : m_AttachmentColorMasks)
            {
                if (!tracked.IsFullyEnabled())
                {
                    m_AnyAttachmentColorMaskDisabled = true;
                    break;
                }
            }
        }
    }

    bool OpenGLRendererAPI::LiftAttachmentColorMasksForClear()
    {
        if (!m_AnyAttachmentColorMaskDisabled)
        {
            return false;
        }

        // glClear honours the colour write mask, so an attachment a previous
        // draw masked off (the infinite grid keeps itself out of the view-
        // normals attachment; skeleton/joint debug draws mask everything but
        // RT0) would silently keep last frame's contents. That stale data then
        // feeds whatever samples it — GTAO reads the view normals, and a
        // never-cleared sky region there reads as occluded geometry.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        return true;
    }

    void OpenGLRendererAPI::RestoreAttachmentColorMasks()
    {
        const u32 trackedCount =
            m_MaxDrawBuffers > 0 ? std::min(static_cast<u32>(m_MaxDrawBuffers), kMaxTrackedDrawBuffers)
                                 : kMaxTrackedDrawBuffers;
        for (u32 i = 0; i < trackedCount; ++i)
        {
            const AttachmentColorMask& mask = m_AttachmentColorMasks[i];
            glColorMaski(i, mask.R, mask.G, mask.B, mask.A);
        }
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

        if (attachment < kMaxTrackedDrawBuffers)
        {
            m_AttachmentBlend[attachment] =
                enabled ? AttachmentBlendOpinion::Enabled : AttachmentBlendOpinion::Disabled;
            m_AnyAttachmentBlendOpinion = true;
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

    void OpenGLRendererAPI::ResetBlendStateForAttachment(u32 attachment)
    {
        OLO_PROFILE_FUNCTION();

        if (attachment >= static_cast<u32>(m_MaxDrawBuffers))
        {
            OLO_CORE_ERROR("OpenGLRendererAPI::ResetBlendStateForAttachment - attachment index {} exceeds GL_MAX_DRAW_BUFFERS {}",
                           attachment, m_MaxDrawBuffers);
            return;
        }

        if (attachment < kMaxTrackedDrawBuffers)
        {
            m_AttachmentBlend[attachment] = AttachmentBlendOpinion::None;
            m_AnyAttachmentBlendOpinion = false;
            for (const AttachmentBlendOpinion opinion : m_AttachmentBlend)
            {
                if (opinion != AttachmentBlendOpinion::None)
                {
                    m_AnyAttachmentBlendOpinion = true;
                    break;
                }
            }
        }

        // "No opinion" is not a state a GL draw buffer can hold, so withdrawing
        // one means re-issuing the GLOBAL enable at this index. Read from the
        // mirror, because GL will not answer the question — see the
        // m_BlendEnabled declaration for the measurement.
        if (m_BlendEnabled)
        {
            glEnablei(GL_BLEND, attachment);
        }
        else
        {
            glDisablei(GL_BLEND, attachment);
        }
    }

    void OpenGLRendererAPI::ReassertAttachmentBlendOpinions()
    {
        // Nothing to put back in the common case: no pass is mid-flight, so no
        // attachment carries an opinion and the global call stands alone.
        if (!m_AnyAttachmentBlendOpinion)
            return;

        const u32 trackedCount =
            m_MaxDrawBuffers > 0 ? std::min(static_cast<u32>(m_MaxDrawBuffers), kMaxTrackedDrawBuffers)
                                 : kMaxTrackedDrawBuffers;
        for (u32 i = 0; i < trackedCount; ++i)
        {
            switch (m_AttachmentBlend[i])
            {
                case AttachmentBlendOpinion::Enabled:
                    glEnablei(GL_BLEND, i);
                    break;
                case AttachmentBlendOpinion::Disabled:
                    glDisablei(GL_BLEND, i);
                    break;
                case AttachmentBlendOpinion::None:
                    break;
            }
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
            case RendererAPI::TextureTargetType::Texture2DArray:
                return GL_TEXTURE_2D_ARRAY;
            case RendererAPI::TextureTargetType::Texture3D:
                return GL_TEXTURE_3D;
            case RendererAPI::TextureTargetType::TextureCubeMapArray:
                return GL_TEXTURE_CUBE_MAP_ARRAY;
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
        // The (0, 0) special case of the offset-taking form — one body per
        // backend for the copy contract, matching the Vulkan implementation.
        CopyImageSubDataRegion(srcID, srcTarget, srcLevel, 0, 0, srcZ, dstID, dstTarget, dstLevel, 0, 0, dstZ, width,
                               height);
    }

    void OpenGLRendererAPI::CopyImageSubDataRegion(u32 srcID, TextureTargetType srcTarget, i32 srcLevel,
                                                   i32 srcX, i32 srcY, i32 srcZ,
                                                   u32 dstID, TextureTargetType dstTarget, i32 dstLevel,
                                                   i32 dstX, i32 dstY, i32 dstZ,
                                                   u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        // Offsets and dimensions pass through UNSCALED — the block-copy
        // contract on the facade declaration (RendererAPI.h): for a mixed
        // compressed/uncompressed pair GL takes width/height in SOURCE texels
        // and dstX/dstY in DEST texels.
        glCopyImageSubData(
            srcID, ToGLTextureTarget(srcTarget), srcLevel, srcX, srcY, srcZ,
            dstID, ToGLTextureTarget(dstTarget), dstLevel, dstX, dstY, dstZ,
            static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
    }

    void OpenGLRendererAPI::CopyFramebufferToTexture(RHI::ResourceHandle texture, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
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

    void OpenGLRendererAPI::BeginConditionalRender(RHI::ResourceHandle query)
    {
        OLO_PROFILE_FUNCTION();
        glBeginConditionalRender(Utils::ResolveNativeAs(query, RHI::ResourceKind::Query), GL_QUERY_BY_REGION_WAIT);
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
    // Issue #691 — the operations the call-site sweep found
    // the facade had never abstracted. See ADR 0011's "Amendments from the
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

        // PUBLISH WHETHER THIS PROGRAM READS THE HEAP (issue #691).
        //
        // OpenGLShader::Bind() does this from its own m_IsBindlessVariant, but the
        // command layer never goes through it — CommandDispatch binds by handle
        // and lands here, so without this line the flag keeps whatever the last
        // post-process shader set. The binding seam then makes the wrong choice
        // for every command-dispatched draw, in both directions: it skips a bind
        // an unconverted program needed, or writes a bind a converted program
        // ignores in favour of an offset nobody wrote.
        //
        // Found by running the editor with OLO_RHI_BINDLESS=1 and looking at the
        // frame: the sky was black and the terrain was missing, with a completely
        // clean log and 5296 passing tests. No unit test can see it, because the
        // suite never builds a bindless variant at all.
        Shader::SetBoundProgramBindless(Shader::IsProgramBindless(programID));
        Shader::SetBoundProgramMaterialOffsets(Shader::ProgramReadsMaterialOffsets(programID));
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

    void OpenGLRendererAPI::AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                                          RHI::ResourceHandle texture, u32 mipLevel)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);
        // RHI::NullResource resolves to 0, which DETACHES — the same contract the
        // native form had for a literal 0.
        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
        glNamedFramebufferTexture(framebufferID, GL_COLOR_ATTACHMENT0 + attachmentIndex, textureID,
                                  static_cast<GLint>(mipLevel));
    }

    void OpenGLRendererAPI::AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture,
                                                          u32 mipLevel)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);
        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
        glNamedFramebufferTexture(framebufferID, GL_DEPTH_ATTACHMENT, textureID, static_cast<GLint>(mipLevel));
    }

    bool OpenGLRendererAPI::IsFramebufferComplete(RHI::ResourceHandle framebuffer)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);
        // Framebuffer 0 (the default) always reports complete, so a stale handle
        // would answer "yes" for an object that no longer exists. Say no instead:
        // every caller treats false as "fall back / do not use this target".
        if (framebufferID == 0)
        {
            return false;
        }
        return glCheckNamedFramebufferStatus(framebufferID, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    void OpenGLRendererAPI::SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                                          std::span<const u32> attachmentIndices)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);
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

    void OpenGLRendererAPI::RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                                                 u32 colorAttachmentCount)
    {
        OLO_PROFILE_FUNCTION();

        // Build the identity list { 0, 1, ... count-1 } once, here, instead of at
        // the nine call sites that used to open-code it.
        u32 count = colorAttachmentCount;
        const u32 maxBuf = static_cast<u32>(m_MaxDrawBuffers);
        if (count > maxBuf)
        {
            OLO_CORE_WARN("OpenGLRendererAPI::RestoreAllFramebufferDrawAttachments - count {} exceeds "
                          "GL_MAX_DRAW_BUFFERS {}, clamping",
                          count, maxBuf);
            count = maxBuf;
        }

        // The stack path covers every framebuffer in the engine (the G-Buffer is
        // the widest at 5) but must NOT be a silent cap: this helper's whole
        // contract is "restore ALL of them", and quietly truncating is the exact
        // failure the comment on the declaration warns about — a narrower list
        // drops later fragment outputs. Above 16, allocate rather than clip.
        // GL_MAX_DRAW_BUFFERS is the only legitimate ceiling, and it is applied
        // above with a warning.
        static constexpr u32 kStackCapacity = 16;
        if (count <= kStackCapacity)
        {
            std::array<u32, kStackCapacity> attachments{};
            for (u32 i = 0; i < count; ++i)
            {
                attachments[i] = i;
            }
            SetFramebufferDrawAttachments(framebuffer, std::span<const u32>(attachments.data(), count));
            return;
        }

        std::vector<u32> attachments(count);
        for (u32 i = 0; i < count; ++i)
        {
            attachments[i] = i;
        }
        SetFramebufferDrawAttachments(framebuffer, attachments);
    }

    void OpenGLRendererAPI::SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);
        glNamedFramebufferReadBuffer(framebufferID, Utils::ToGLColorAttachment(attachmentIndex));
    }

    void OpenGLRendererAPI::ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                                            const glm::vec4& color)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);

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

    void OpenGLRendererAPI::ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint framebufferID = Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer);
        Utils::GLClearProgramGuard programGuard;
        glClearNamedFramebufferfv(framebufferID, GL_DEPTH, 0, &depth);
    }

    void OpenGLRendererAPI::BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer,
                                            i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                                            i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                                            RHI::BlitAspect aspect, RHI::Filter filter)
    {
        OLO_PROFILE_FUNCTION();

        // RHI::NullResource resolves to 0 = the DEFAULT framebuffer, which is how
        // a blit to the backbuffer is spelled.
        const GLuint srcFramebufferID = Utils::ResolveNativeAs(srcFramebuffer, RHI::ResourceKind::Framebuffer);
        const GLuint dstFramebufferID = Utils::ResolveNativeAs(dstFramebuffer, RHI::ResourceKind::Framebuffer);
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

    void OpenGLRendererAPI::AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes,
                                                  RHI::MemoryResidency residency)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
        glNamedBufferData(bufferID, static_cast<GLsizeiptr>(sizeBytes), nullptr, Utils::ToGL(residency));
    }

    void* OpenGLRendererAPI::AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
        if (bufferID == 0)
        {
            // Mapping buffer 0 is an error and would hand back nullptr anyway;
            // saying so up front keeps the caller's "mapping failed" branch the
            // single place that deals with it.
            return nullptr;
        }

        // Storage flags and map flags must agree or glMapNamedBufferRange fails
        // at map time rather than at allocation time — which is why these are
        // one call rather than two.
        constexpr GLbitfield kFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glNamedBufferStorage(bufferID, static_cast<GLsizeiptr>(sizeBytes), nullptr, kFlags);
        return glMapNamedBufferRange(bufferID, 0, static_cast<GLsizeiptr>(sizeBytes), kFlags);
    }

    void OpenGLRendererAPI::UnmapBuffer(RHI::ResourceHandle buffer)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
        if (bufferID == 0)
        {
            return;
        }
        glUnmapNamedBuffer(bufferID);
    }

    void OpenGLRendererAPI::UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes,
                                                const void* data)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
        glNamedBufferSubData(bufferID, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(sizeBytes), data);
    }

    void OpenGLRendererAPI::ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
        glGetNamedBufferSubData(bufferID, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(sizeBytes), dest);
    }

    void OpenGLRendererAPI::CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer,
                                              u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint srcBufferID = Utils::ResolveNativeAs(srcBuffer, RHI::ResourceKind::Buffer);
        const GLuint dstBufferID = Utils::ResolveNativeAs(dstBuffer, RHI::ResourceKind::Buffer);
        glCopyNamedBufferSubData(srcBufferID, dstBufferID,
                                 static_cast<GLintptr>(srcOffsetBytes), static_cast<GLintptr>(dstOffsetBytes),
                                 static_cast<GLsizeiptr>(sizeBytes));
    }

    void OpenGLRendererAPI::ClearBufferUInt(RHI::ResourceHandle buffer, u32 value, u64 offset, u64 size)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
        Utils::GLClearProgramGuard programGuard;
        if (offset == 0 && size == ~0ull)
        {
            glClearNamedBufferData(bufferID, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
            return;
        }
        glClearNamedBufferSubData(bufferID, GL_R32UI, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size),
                                  GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    }

    void OpenGLRendererAPI::ClearBufferFloat(RHI::ResourceHandle buffer, f32 value)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint bufferID = Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer);
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

    void OpenGLRendererAPI::SetVertexArrayIndexBuffer(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indexBuffer)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint vaoID = Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray);
        const GLuint bufferID = Utils::ResolveNativeAs(indexBuffer, RHI::ResourceKind::Buffer);
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

    void OpenGLRendererAPI::ClearTextureUInt(RHI::ResourceHandle texture, u32 mipLevel, u32 value)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
        Utils::GLClearProgramGuard programGuard;
        glClearTexImage(textureID, static_cast<GLint>(mipLevel), GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
    }

    void OpenGLRendererAPI::UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset,
                                                    u32 width, u32 height,
                                                    RHI::Format sourceFormat, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
        glTextureSubImage2D(textureID, 0, xOffset, yOffset,
                            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                            Utils::ToGLPixelFormat(sourceFormat), Utils::ToGLPixelType(sourceFormat), data);
    }

    void OpenGLRendererAPI::UploadTextureSubImage3D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, i32 zOffset,
                                                    u32 width, u32 height, u32 depth,
                                                    RHI::Format sourceFormat, const void* data)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
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
    } // namespace

    bool OpenGLRendererAPI::ReadTextureImage(u32 textureID, u32 mipLevel, RHI::Format destFormat,
                                             sizet destSizeBytes, void* dest)
    {
        OLO_PROFILE_FUNCTION();

        Utils::DrainGLErrors();
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

        Utils::DrainGLErrors();
        glGetTextureSubImage(textureID, static_cast<GLint>(mipLevel), x, y, z,
                             static_cast<GLsizei>(width), static_cast<GLsizei>(height), static_cast<GLsizei>(depth),
                             Utils::ToGLPixelFormat(destFormat), Utils::ToGLPixelType(destFormat),
                             static_cast<GLsizei>(destSizeBytes), dest);
        return glGetError() == GL_NO_ERROR;
    }

    void OpenGLRendererAPI::GetTextureDimensions(RHI::ResourceHandle texture, u32 mipLevel, u32& outWidth,
                                                 u32& outHeight)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
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

    bool OpenGLRendererAPI::QueryTextureFormat(RHI::ResourceHandle texture, u32 mipLevel,
                                               RHI::TextureFormatInfo& out)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
        if (textureID == 0u)
        {
            return false;
        }

        GLint width = 0;
        GLint internalFormat = 0;
        glGetTextureLevelParameteriv(textureID, static_cast<GLint>(mipLevel), GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(textureID, static_cast<GLint>(mipLevel), GL_TEXTURE_INTERNAL_FORMAT,
                                     &internalFormat);
        if (width <= 0)
        {
            return false; // no storage at this level — GL's own answer
        }

        // Layout, from the same object. GL_TEXTURE_DEPTH at a mip is the array
        // layer count for array targets and the slice count for a 3D texture;
        // it is 1 for a plain 2D one and 6 for a cubemap addressed as layers.
        GLint immutableLevels = 0;
        GLint depthOrLayers = 0;
        GLint textureTarget = 0;
        glGetTextureParameteriv(textureID, GL_TEXTURE_IMMUTABLE_LEVELS, &immutableLevels);
        glGetTextureLevelParameteriv(textureID, static_cast<GLint>(mipLevel), GL_TEXTURE_DEPTH, &depthOrLayers);
        glGetTextureParameteriv(textureID, GL_TEXTURE_TARGET, &textureTarget);

        RHI::TextureShape shape = RHI::TextureShape::Unknown;
        switch (textureTarget)
        {
            case GL_TEXTURE_2D:
                shape = RHI::TextureShape::Texture2D;
                break;
            case GL_TEXTURE_2D_ARRAY:
                shape = RHI::TextureShape::Texture2DArray;
                break;
            case GL_TEXTURE_2D_MULTISAMPLE:
                shape = RHI::TextureShape::Texture2DMultisample;
                break;
            case GL_TEXTURE_CUBE_MAP:
                shape = RHI::TextureShape::TextureCube;
                break;
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                shape = RHI::TextureShape::TextureCubeArray;
                break;
            case GL_TEXTURE_3D:
                shape = RHI::TextureShape::Texture3D;
                break;
            default:
                break;
        }

        RHI::TextureFormatInfo info;
        info.Native = static_cast<u64>(static_cast<u32>(internalFormat));
        const u32 mipLevels = immutableLevels > 0 ? static_cast<u32>(immutableLevels) : 1u;
        const u32 arrayLayers = depthOrLayers > 0 ? static_cast<u32>(depthOrLayers) : 1u;

        // The neutral tokens are the spelling the Vulkan arm produces too, so a
        // GL reading and a Vulkan reading of the same target compare directly.
        switch (internalFormat)
        {
            case GL_RGBA8:
                info = { RHI::Format::RGBA8UNorm, info.Native, "RGBA8", 4, false, false, false };
                break;
            case GL_SRGB8_ALPHA8:
                info = { RHI::Format::RGBA8SRGB, info.Native, "RGBA8_SRGB", 4, false, false, false };
                break;
            case GL_RGB8:
                info = { RHI::Format::RGB8UNorm, info.Native, "RGB8", 3, false, false, false };
                break;
            case GL_SRGB8:
                info = { RHI::Format::Unknown, info.Native, "RGB8_SRGB", 3, false, false, false };
                break;
            case GL_RG8:
                info = { RHI::Format::RG8UNorm, info.Native, "RG8", 2, false, false, false };
                break;
            case GL_R8:
                info = { RHI::Format::R8UNorm, info.Native, "R8", 1, false, false, false };
                break;
            case GL_RGBA16F:
                info = { RHI::Format::RGBA16Float, info.Native, "RGBA16F", 4, false, false, true };
                break;
            case GL_RGBA32F:
                info = { RHI::Format::RGBA32Float, info.Native, "RGBA32F", 4, false, false, true };
                break;
            case GL_RGB16F:
                info = { RHI::Format::Unknown, info.Native, "RGB16F", 3, false, false, true };
                break;
            case GL_RGB32F:
                info = { RHI::Format::RGB32Float, info.Native, "RGB32F", 3, false, false, true };
                break;
            case GL_R11F_G11F_B10F:
                info = { RHI::Format::Unknown, info.Native, "R11F_G11F_B10F", 3, false, false, true };
                break;
            case GL_RG16F:
                info = { RHI::Format::RG16Float, info.Native, "RG16F", 2, false, false, true };
                break;
            case GL_RG32F:
                info = { RHI::Format::RG32Float, info.Native, "RG32F", 2, false, false, true };
                break;
            case GL_R16F:
                info = { RHI::Format::Unknown, info.Native, "R16F", 1, false, false, true };
                break;
            case GL_R32F:
                info = { RHI::Format::R32Float, info.Native, "R32F", 1, false, false, true };
                break;
            case GL_R32I:
                info = { RHI::Format::R32Int, info.Native, "R32I", 1, true, false, false };
                break;
            case GL_R32UI:
                info = { RHI::Format::R32UInt, info.Native, "R32UI", 1, true, false, false };
                break;
            case GL_DEPTH_COMPONENT16:
                info = { RHI::Format::Unknown, info.Native, "D16", 1, false, true, false };
                break;
            case GL_DEPTH_COMPONENT24:
                info = { RHI::Format::Unknown, info.Native, "D24", 1, false, true, false };
                break;
            case GL_DEPTH_COMPONENT32:
                info = { RHI::Format::Unknown, info.Native, "D32", 1, false, true, false };
                break;
            case GL_DEPTH_COMPONENT32F:
                info = { RHI::Format::D32Float, info.Native, "D32F", 1, false, true, true };
                break;
            case GL_DEPTH24_STENCIL8:
                info = { RHI::Format::D24UNormS8UInt, info.Native, "D24S8", 1, false, true, false };
                break;
            case GL_DEPTH32F_STENCIL8:
                info = { RHI::Format::Unknown, info.Native, "D32FS8", 1, false, true, true };
                break;
            default:
                // Undecodable: say so rather than let a caller read with a
                // guessed channel count.
                return false;
        }

        info.MipLevels = mipLevels;
        info.ArrayLayers = arrayLayers;
        info.Shape = shape;
        out = info;
        return true;
    }

    RHI::ResourceHandle OpenGLRendererAPI::CreateMatchingTextureHandle(RHI::ResourceHandle source)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint sourceID = Utils::ResolveNativeAs(source, RHI::ResourceKind::Texture);
        if (sourceID == 0u)
        {
            return {};
        }

        // The source's OWN native description — no neutral vocabulary in the
        // middle, which is the whole point of this entry (RendererAPI.h).
        Utils::DrainGLErrors();
        GLint target = 0;
        GLint samples = 0;
        GLint internalFormat = 0;
        GLint width = 0;
        GLint height = 0;
        GLint depthOrLayers = 0;
        GLint levels = 0;
        glGetTextureParameteriv(sourceID, GL_TEXTURE_TARGET, &target);
        glGetTextureParameteriv(sourceID, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
        glGetTextureLevelParameteriv(sourceID, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        glGetTextureLevelParameteriv(sourceID, 0, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(sourceID, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(sourceID, 0, GL_TEXTURE_DEPTH, &depthOrLayers);
        glGetTextureLevelParameteriv(sourceID, 0, GL_TEXTURE_SAMPLES, &samples);
        if (glGetError() != GL_NO_ERROR || width <= 0 || height <= 0)
        {
            return {};
        }
        if (samples > 1)
        {
            // A multisample copy destination would have to match sample count
            // too, and every consumer of the clone reads it as a plain image.
            // Refuse rather than silently resolving.
            OLO_CORE_WARN("[RHI/GL] CreateMatchingTextureHandle: source is multisampled ({} samples) — refused",
                          samples);
            return {};
        }

        const auto mipLevels = static_cast<GLsizei>(levels > 0 ? levels : 1);
        const auto layers = static_cast<GLsizei>(depthOrLayers > 0 ? depthOrLayers : 1);

        GLuint clone = 0;
        glCreateTextures(static_cast<GLenum>(target), 1, &clone);
        switch (target)
        {
            case GL_TEXTURE_2D:
            case GL_TEXTURE_CUBE_MAP:
                glTextureStorage2D(clone, mipLevels, static_cast<GLenum>(internalFormat),
                                   static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                break;
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_3D:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                glTextureStorage3D(clone, mipLevels, static_cast<GLenum>(internalFormat),
                                   static_cast<GLsizei>(width), static_cast<GLsizei>(height), layers);
                break;
            default:
                glDeleteTextures(1, &clone);
                OLO_CORE_WARN("[RHI/GL] CreateMatchingTextureHandle: unsupported target 0x{:X}",
                              static_cast<u32>(target));
                return {};
        }

        // NEAREST filters: an INTEGER-format texture (the R32I entity-id
        // buffer) is texture-INcomplete under the default LINEAR filters
        // (GL 4.6 §8.17), and glCopyImageSubData mandates INVALID_OPERATION on
        // an incomplete texture (§18.3.2) — NVIDIA is lenient, other drivers
        // are not. Harmless for every other format; a clone is readback-only
        // and never shader-sampled.
        glTextureParameteri(clone, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(clone, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        if (glGetError() != GL_NO_ERROR)
        {
            glDeleteTextures(1, &clone);
            return {};
        }

        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture, clone, RHI::Backend::OpenGL);
    }

    // --- Queries ----------------------------------------------------------------------------

    void OpenGLRendererAPI::CreateQueries(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries)
    {
        OLO_PROFILE_FUNCTION();

        if (outQueries.empty())
        {
            return;
        }
        // The out-span NEVER reaches the driver. Handing glCreateQueries a
        // `std::span<RHI::ResourceHandle>::data()` still compiles — the
        // parameter is a GLuint* and the pointer converts — and GL would write
        // 4-byte names over the low half of each 8-byte handle, leaving every
        // Generation intact and every Index garbage. Silent, and no test would
        // see it (ADR 0011 / §4's `.data()` trap, which was written about this
        // exact function).
        std::vector<GLuint> nativeNames(outQueries.size(), 0u);

        // glCreateQueries rather than glGenQueries: the DSA form binds the
        // object to its target at creation, so a subsequent glBeginQuery with a
        // mismatched target is an immediate error rather than a latent one.
        glCreateQueries(Utils::ToGL(type), static_cast<GLsizei>(nativeNames.size()), nativeNames.data());

        auto& registry = RHI::ResourceRegistry::Get();
        for (sizet i = 0; i < outQueries.size(); ++i)
        {
            outQueries[i] = nativeNames[i] != 0u
                                ? registry.Register(RHI::ResourceKind::Query, nativeNames[i], RHI::Backend::OpenGL)
                                : RHI::NullResource;
        }
    }

    void OpenGLRendererAPI::DeleteQueries(std::span<const RHI::ResourceHandle> queries)
    {
        OLO_PROFILE_FUNCTION();

        if (queries.empty())
        {
            return;
        }

        // Both halves. Resolving to call glDeleteQueries is the visible one;
        // unregistering is the one that matters, because a slot whose
        // generation never advances goes on resolving a handle to a destroyed
        // object into a name the driver may reissue.
        std::vector<GLuint> nativeNames;
        nativeNames.reserve(queries.size());
        auto& registry = RHI::ResourceRegistry::Get();
        for (const RHI::ResourceHandle query : queries)
        {
            // A live handle of the wrong family names SOMEONE ELSE'S resource.
            // ResolveNativeAs already refuses to hand back their GL name, but
            // Unregister is not kind-aware — without this guard a mis-wired
            // delete would retire their registry entry while leaving their GL
            // object alive. Same guard as the other Delete* virtuals.
            if (Utils::IsWrongKind(query, RHI::ResourceKind::Query))
                continue;

            if (const GLuint name = Utils::ResolveNativeAs(query, RHI::ResourceKind::Query); name != 0u)
            {
                nativeNames.push_back(name);
            }
            registry.Unregister(query);
        }

        if (!nativeNames.empty())
        {
            glDeleteQueries(static_cast<GLsizei>(nativeNames.size()), nativeNames.data());
        }
    }

    void OpenGLRendererAPI::BeginQuery(RHI::QueryType type, RHI::ResourceHandle query)
    {
        OLO_PROFILE_FUNCTION();

        glBeginQuery(Utils::ToGL(type), Utils::ResolveNativeAs(query, RHI::ResourceKind::Query));
    }

    void OpenGLRendererAPI::EndQuery(RHI::QueryType type)
    {
        OLO_PROFILE_FUNCTION();

        glEndQuery(Utils::ToGL(type));
    }

    void OpenGLRendererAPI::WriteTimestamp(RHI::ResourceHandle query)
    {
        OLO_PROFILE_FUNCTION();

        // glQueryCounter on a retired handle resolves to name 0, which GL
        // rejects with GL_INVALID_OPERATION rather than stamping someone
        // else's query — acceptable for a debug-instrument path, and the
        // resolve guard keeps the failure local.
        if (const GLuint name = Utils::ResolveNativeAs(query, RHI::ResourceKind::Query); name != 0u)
        {
            glQueryCounter(name, GL_TIMESTAMP);
        }
    }

    bool OpenGLRendererAPI::IsQueryResultAvailable(RHI::ResourceHandle query)
    {
        OLO_PROFILE_FUNCTION();

        const GLuint name = Utils::ResolveNativeAs(query, RHI::ResourceKind::Query);
        if (name == 0u)
        {
            // A stale handle has no result and never will. Reporting "available"
            // would make the caller read query 0 and treat the zero it gets back
            // as a real answer — for occlusion that reads as "fully occluded",
            // which silently deletes geometry.
            return false;
        }

        GLint available = 0;
        glGetQueryObjectiv(name, GL_QUERY_RESULT_AVAILABLE, &available);
        return available != 0;
    }

    u32 OpenGLRendererAPI::GetQueryResultU32(RHI::ResourceHandle query)
    {
        OLO_PROFILE_FUNCTION();

        GLuint result = 0;
        const GLuint name = Utils::ResolveNativeAs(query, RHI::ResourceKind::Query);
        if (name != 0u)
        {
            glGetQueryObjectuiv(name, GL_QUERY_RESULT, &result);
        }
        return result;
    }

    u64 OpenGLRendererAPI::GetQueryResultU64(RHI::ResourceHandle query)
    {
        OLO_PROFILE_FUNCTION();

        GLuint64 result = 0;
        const GLuint name = Utils::ResolveNativeAs(query, RHI::ResourceKind::Query);
        if (name != 0u)
        {
            glGetQueryObjectui64v(name, GL_QUERY_RESULT, &result);
        }
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
        // (issue #691; same class of problem as SlugFontProcessor's
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

    // -------------------------------------------------------------------------
    // Handle-taking siblings of the bind family (issue #691).
    //
    // Each resolves the identity to a driver name and delegates to the existing
    // u32 form, so there is exactly one place per operation that talks to GL and
    // the two spellings cannot drift. Resolution happens here, inside
    // Platform/OpenGL/, which is the whole point — see Utils::ResolveNative.
    //
    // A stale handle resolves to 0, and every one of these treats 0 as "unbind",
    // which is the correct degradation: a use-after-free leaves the slot empty
    // rather than bound to whatever object inherited the recycled name.
    // -------------------------------------------------------------------------
    void OpenGLRendererAPI::BindTexture(u32 slot, RHI::ResourceHandle texture)
    {
        BindTexture(slot, Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture));
    }

    void OpenGLRendererAPI::BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered,
                                             u32 layer, RHI::Access access, RHI::Format format)
    {
        BindImageTexture(unit, Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), mipLevel, layered, layer, access, format);
    }

    void OpenGLRendererAPI::BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        BindUniformBuffer(bindingPoint, Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer));
    }

    void OpenGLRendererAPI::BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
    {
        BindStorageBuffer(bindingPoint, Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer));
    }

    void OpenGLRendererAPI::BindShaderProgram(RHI::ResourceHandle program)
    {
        BindShaderProgram(Utils::ResolveNativeAs(program, RHI::ResourceKind::ShaderProgram));
    }

    void OpenGLRendererAPI::BindVertexArrayRaw(RHI::ResourceHandle vertexArray)
    {
        BindVertexArrayRaw(Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray));
    }

    void OpenGLRendererAPI::BindFramebuffer(RHI::ResourceHandle framebuffer)
    {
        BindFramebuffer(Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer));
    }

    // -------------------------------------------------------------------------
    // Handle-returning raw creators (issue #691).
    //
    // Each creates through the existing u32 form and registers the result, so
    // there is one place per operation that talks to GL. The Delete* siblings
    // resolve, delegate, and then RETIRE the identity — the second half is the
    // one that is easy to omit and impossible to notice: without it the slot
    // keeps its generation and a handle to the destroyed object goes on
    // resolving to a name the driver is free to reissue.
    // -------------------------------------------------------------------------
    RHI::ResourceHandle OpenGLRendererAPI::CreateTexture2DHandle(u32 width, u32 height, RHI::Format internalFormat)
    {
        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture,
                                                     CreateTexture2D(width, height, internalFormat),
                                                     RHI::Backend::OpenGL);
    }

    RHI::ResourceHandle OpenGLRendererAPI::CreateTextureCubemapHandle(u32 width, u32 height, RHI::Format internalFormat)
    {
        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture,
                                                     CreateTextureCubemap(width, height, internalFormat),
                                                     RHI::Backend::OpenGL);
    }

    RHI::ResourceHandle OpenGLRendererAPI::CreateFramebufferHandle()
    {
        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Framebuffer, CreateFramebuffer(),
                                                     RHI::Backend::OpenGL);
    }

    RHI::ResourceHandle OpenGLRendererAPI::CreateBufferHandle()
    {
        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Buffer, CreateBuffer(),
                                                     RHI::Backend::OpenGL);
    }

    RHI::ResourceHandle OpenGLRendererAPI::CreateVertexArrayHandle()
    {
        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::VertexArray, CreateVertexArray(),
                                                     RHI::Backend::OpenGL);
    }

    void OpenGLRendererAPI::DeleteTexture(RHI::ResourceHandle texture)
    {
        // A live handle of the wrong family names SOMEONE ELSE'S resource.
        // ResolveNativeAs already refuses to hand back their GL name, but the
        // Unregister below is not kind-aware — without this guard a mis-wired
        // delete would retire their registry entry while leaving their GL
        // object alive, which is worse than the unchecked form it replaced.
        if (Utils::IsWrongKind(texture, RHI::ResourceKind::Texture))
            return;

        // RETIRE THE HEAP DESCRIPTOR FIRST, and do it HERE rather than at the call
        // sites. `OpenGLTexture*` and `OpenGLFramebuffer` own their retire because
        // they manage GL names directly, but systems that mint a texture through
        // `CreateTexture2DHandle` (CloudShadowMap, and anything that follows it)
        // destroy it through this function and have no other hook. Leaving the
        // retire to each of them makes it one-more-thing-to-remember, and the
        // symptom of forgetting is a resident bindless handle on a deleted
        // texture — undefined when sampled, `GL_INVALID_OPERATION ... Not a valid
        // texture` when the heap later withdraws residency.
        //
        // Before Unregister: views are matched by handle, so a retired registry
        // entry finds nothing to retire.
        RHI::DescriptorHeap::Get().RetireResource(texture);

        DeleteTexture(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture));
        RHI::ResourceRegistry::Get().Unregister(texture);
    }

    void OpenGLRendererAPI::DeleteFramebuffer(RHI::ResourceHandle framebuffer)
    {
        // A live handle of the wrong family names SOMEONE ELSE'S resource.
        // ResolveNativeAs already refuses to hand back their GL name, but the
        // Unregister below is not kind-aware — without this guard a mis-wired
        // delete would retire their registry entry while leaving their GL
        // object alive, which is worse than the unchecked form it replaced.
        if (Utils::IsWrongKind(framebuffer, RHI::ResourceKind::Framebuffer))
            return;

        DeleteFramebuffer(Utils::ResolveNativeAs(framebuffer, RHI::ResourceKind::Framebuffer));
        RHI::ResourceRegistry::Get().Unregister(framebuffer);
    }

    void OpenGLRendererAPI::DeleteBuffer(RHI::ResourceHandle buffer)
    {
        // A live handle of the wrong family names SOMEONE ELSE'S resource.
        // ResolveNativeAs already refuses to hand back their GL name, but the
        // Unregister below is not kind-aware — without this guard a mis-wired
        // delete would retire their registry entry while leaving their GL
        // object alive, which is worse than the unchecked form it replaced.
        if (Utils::IsWrongKind(buffer, RHI::ResourceKind::Buffer))
            return;

        DeleteBuffer(Utils::ResolveNativeAs(buffer, RHI::ResourceKind::Buffer));
        RHI::ResourceRegistry::Get().Unregister(buffer);
    }

    void OpenGLRendererAPI::DeleteVertexArray(RHI::ResourceHandle vertexArray)
    {
        // A live handle of the wrong family names SOMEONE ELSE'S resource.
        // ResolveNativeAs already refuses to hand back their GL name, but the
        // Unregister below is not kind-aware — without this guard a mis-wired
        // delete would retire their registry entry while leaving their GL
        // object alive, which is worse than the unchecked form it replaced.
        if (Utils::IsWrongKind(vertexArray, RHI::ResourceKind::VertexArray))
            return;

        DeleteVertexArray(Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray));
        RHI::ResourceRegistry::Get().Unregister(vertexArray);
    }

    void OpenGLRendererAPI::SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter)
    {
        SetTextureFilter(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), minFilter, magFilter);
    }

    void OpenGLRendererAPI::SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap)
    {
        SetTextureWrap(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), wrap);
    }

    void OpenGLRendererAPI::UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height,
                                                    RHI::Format sourceFormat, const void* data)
    {
        UploadTextureSubImage2D(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), width, height, sourceFormat, data);
    }

    // -------------------------------------------------------------------------
    // Handle-taking siblings of the texture copy / clear / upload-at-offset /
    // readback family (issue #691 — attachment consumers).
    //
    // Same shape as the block above: resolve here, delegate to the one u32 form
    // that talks to GL. What made these necessary was migrating the framebuffer
    // attachment getters' consumers — the bakers copy an attachment into a
    // persistent Texture2D/Cubemap and the probe bakers read one back, and
    // neither family appeared in the bind or create/delete survey that produced
    // the earlier additions.
    // -------------------------------------------------------------------------
    void OpenGLRendererAPI::CopyImageSubData(RHI::ResourceHandle src, TextureTargetType srcTarget,
                                             RHI::ResourceHandle dst, TextureTargetType dstTarget,
                                             u32 width, u32 height)
    {
        CopyImageSubData(Utils::ResolveNativeAs(src, RHI::ResourceKind::Texture), srcTarget,
                         Utils::ResolveNativeAs(dst, RHI::ResourceKind::Texture), dstTarget,
                         width, height);
    }

    void OpenGLRendererAPI::CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget,
                                                 i32 srcLevel, i32 srcZ,
                                                 RHI::ResourceHandle dst, TextureTargetType dstTarget,
                                                 i32 dstLevel, i32 dstZ,
                                                 u32 width, u32 height)
    {
        CopyImageSubDataFull(Utils::ResolveNativeAs(src, RHI::ResourceKind::Texture), srcTarget, srcLevel, srcZ,
                             Utils::ResolveNativeAs(dst, RHI::ResourceKind::Texture), dstTarget, dstLevel, dstZ,
                             width, height);
    }

    void OpenGLRendererAPI::CopyImageSubDataRegion(RHI::ResourceHandle src, TextureTargetType srcTarget,
                                                   i32 srcLevel, i32 srcX, i32 srcY, i32 srcZ,
                                                   RHI::ResourceHandle dst, TextureTargetType dstTarget,
                                                   i32 dstLevel, i32 dstX, i32 dstY, i32 dstZ,
                                                   u32 width, u32 height)
    {
        CopyImageSubDataRegion(Utils::ResolveNativeAs(src, RHI::ResourceKind::Texture), srcTarget, srcLevel,
                               srcX, srcY, srcZ,
                               Utils::ResolveNativeAs(dst, RHI::ResourceKind::Texture), dstTarget, dstLevel,
                               dstX, dstY, dstZ,
                               width, height);
    }

    void OpenGLRendererAPI::ClearTextureFloat(RHI::ResourceHandle texture, u32 mipLevel, const glm::vec4& color)
    {
        ClearTextureFloat(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), mipLevel, color);
    }

    bool OpenGLRendererAPI::ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel,
                                             RHI::Format destFormat, sizet destSizeBytes, void* dest)
    {
        // A stale handle resolves to 0 and the u32 form reports failure for
        // texture 0, so the "unbind" degradation the bind family relies on
        // becomes an honest `false` here — the caller must not treat `dest` as
        // populated.
        return ReadTextureImage(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), mipLevel,
                                destFormat, destSizeBytes, dest);
    }

    void OpenGLRendererAPI::DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount,
                                                  u32 patchVertices)
    {
        DrawIndexedPatchesRaw(Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray), indexCount,
                              patchVertices);
    }

    void OpenGLRendererAPI::DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount,
                                                    u32 baseIndex, u32 instanceCount)
    {
        DrawIndexedInstancedRaw(Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray), indexCount,
                                baseIndex, instanceCount);
    }

    void OpenGLRendererAPI::DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount)
    {
        DrawIndexedRaw(Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray), indexCount);
    }

    void OpenGLRendererAPI::DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex)
    {
        DrawIndexedRaw(Utils::ResolveNativeAs(vertexArray, RHI::ResourceKind::VertexArray), indexCount, baseIndex);
    }

    void OpenGLRendererAPI::SetProgramUniformFloat(RHI::ResourceHandle program, std::string_view name, f32 value)
    {
        SetProgramUniformFloat(Utils::ResolveNativeAs(program, RHI::ResourceKind::ShaderProgram), name, value);
    }

    RHI::ResourceHandle OpenGLRendererAPI::CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture,
                                                                                u32 numLayers)
    {
        const GLuint nativeView = CreateDepthArrayCompareOffView(
            Utils::ResolveNativeAs(srcTexture, RHI::ResourceKind::Texture), numLayers);
        if (nativeView == 0u)
            return RHI::NullResource;

        // The view is registered as a Texture in its own right, NOT as an alias
        // of the source: it is a separate GL name with its own sampler state and
        // its own lifetime (ShadowMap deletes it independently of the array).
        return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture, nativeView, RHI::Backend::OpenGL);
    }

    bool OpenGLRendererAPI::ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                i32 x, i32 y, i32 z,
                                                u32 width, u32 height, u32 depth,
                                                RHI::Format destFormat, sizet destSizeBytes, void* dest)
    {
        return ReadTextureSubImage(Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture), mipLevel,
                                   x, y, z, width, height, depth, destFormat, destSizeBytes, dest);
    }

} // namespace OloEngine
