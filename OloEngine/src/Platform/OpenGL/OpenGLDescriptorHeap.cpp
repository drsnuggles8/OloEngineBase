#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLDescriptorHeap.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "Platform/OpenGL/OpenGLRHIConversions.h"

#include <array>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // `LinearMipFilter == false` means NO MIP FILTERING, not "nearest mip".
        // Mapping it to a *_MIPMAP_* enum was a silent divergence between the two
        // paths: SSAO's noise sampler sets the flag false, so the heap sampler
        // would minify through a mip chain while the slot path used the texture's
        // own non-mipmapped filter. Two variants of one shader that sample
        // differently is exactly what this phase must not produce — the whole
        // claim is that only the binding mechanism changes.
        [[nodiscard]] GLenum ToGLMinFilter(RHI::Filter minFilter, bool linearMip)
        {
            if (minFilter == RHI::Filter::Nearest)
            {
                return linearMip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
            }
            return linearMip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
        }
    } // namespace

    void OpenGLDescriptorHeapBackend::Initialize(u32 slotCapacity)
    {
        Shutdown();

        // Both halves are required and they are separate extensions on paper:
        // the handle machinery is ARB_bindless_texture, and putting the handles
        // in a buffer the shader indexes needs the sampler-from-uvec2
        // constructor that same extension provides. Probed as one gate because
        // there is no useful configuration with one and not the other.
        //
        // `GLAD_GL_ARB_bindless_texture` is a LOADER symbol, not a GL call —
        // the same class as the `GLAD_GL_KHR_debug` probe the debug-marker path
        // uses, and deliberately invisible to the boundary ratchet's `gl[A-Z](`
        // pattern (it is a capability test, not a call). It is also more
        // reliable than enumerating GL_EXTENSIONS by hand: glad already
        // resolved every entry point at load time, so a true flag means the
        // function pointers are actually there.
        m_Supported = GLAD_GL_ARB_bindless_texture != 0;
        if (!m_Supported)
        {
            OLO_CORE_INFO("[RHI/GL] GL_ARB_bindless_texture not present — heap-bindless unavailable, "
                          "the slot-based binding path stays in use.");
            return;
        }

        m_SlotCapacity = slotCapacity;

        // The null descriptor: a real, resident, sampleable 1x1 opaque-black
        // texture. NOT handle 0 — sampling an invalid or non-resident
        // ARB_bindless_texture handle is UNDEFINED BEHAVIOUR, so the poison and
        // null-descriptor guarantees would otherwise rest on driver luck.
        // Immutable storage because a handle freezes the texture anyway.
        glCreateTextures(GL_TEXTURE_2D, 1, &m_NullTexture);
        glTextureStorage2D(m_NullTexture, 1, GL_RGBA8, 1, 1);
        constexpr std::array<u8, 4> kBlack = { 0u, 0u, 0u, 255u };
        glTextureSubImage2D(m_NullTexture, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, kBlack.data());
        glObjectLabel(GL_TEXTURE, m_NullTexture, -1, "RHI::DescriptorHeap null");

        glCreateSamplers(1, &m_NullSampler);
        glSamplerParameteri(m_NullSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(m_NullSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        m_NullDescriptor = static_cast<u64>(glGetTextureSamplerHandleARB(m_NullTexture, m_NullSampler));
        if (m_NullDescriptor != 0u && glIsTextureHandleResidentARB(m_NullDescriptor) != GL_TRUE)
        {
            glMakeTextureHandleResidentARB(m_NullDescriptor);
        }

        // std430 uvec2[] — a GLuint64 handle IS a uvec2 in memory, so the CPU
        // mirror uploads verbatim with no packing step. Deliberately NOT a
        // uint64_t[] block: that would additionally require
        // GL_ARB_gpu_shader_int64 in every consuming shader for no gain.
        glCreateBuffers(1, &m_HeapBuffer);
        glNamedBufferStorage(m_HeapBuffer, static_cast<GLsizeiptr>(sizeof(u64) * slotCapacity), nullptr,
                             GL_DYNAMIC_STORAGE_BIT);

        // FILL THE WHOLE TABLE WITH THE NULL DESCRIPTOR. `glNamedBufferStorage`
        // with a null pointer leaves the contents UNDEFINED, and a slot the
        // engine never allocates is never in a dirty range, so it would never be
        // written — including slot 0, the reserved null descriptor that every
        // cleared or failed binding points at. Sampling through undefined bits is
        // not "black", it is whatever the driver left there.
        //
        // Filling with the resident black handle rather than with zeroes is the
        // second half of the same point: a zero handle is not a valid bindless
        // handle either, so sampling it is undefined too.
        //
        // This cost a real debugging round. The null-descriptor test passed in
        // isolation — a fresh context happened to hand back zeroed memory — and
        // failed only in the full suite, after thousands of tests had churned GPU
        // allocations. An "it works alone, fails in the suite" failure on a buffer
        // you never wrote is almost always uninitialised storage rather than test
        // pollution.
        {
            const std::vector<u64> nulls(slotCapacity, m_NullDescriptor);
            glNamedBufferSubData(m_HeapBuffer, 0, static_cast<GLsizeiptr>(sizeof(u64) * slotCapacity), nulls.data());
        }

        glObjectLabel(GL_BUFFER, m_HeapBuffer, -1, "RHI::DescriptorHeap");

        OLO_CORE_INFO("[RHI/GL] GL_ARB_bindless_texture present — descriptor heap buffer created "
                      "({} slots, {} KiB).",
                      slotCapacity, (sizeof(u64) * slotCapacity) / 1024u);
    }

    void OpenGLDescriptorHeapBackend::Shutdown()
    {
        // Residency outlives the buffer if it is not dropped explicitly: a
        // resident handle keeps its texture immutable and its memory mapped, so
        // leaking one leaks the texture's ability to ever be reconfigured.
        for (const auto& [handle, count] : m_Residency)
        {
            if (count > 0u && glIsTextureHandleResidentARB(handle) == GL_TRUE)
            {
                glMakeTextureHandleNonResidentARB(handle);
            }
        }
        m_Residency.clear();

        for (const auto& entry : m_Samplers)
        {
            if (entry.Object != 0u)
            {
                glDeleteSamplers(1, &entry.Object);
            }
        }
        m_Samplers.clear();

        if (m_NullDescriptor != 0u)
        {
            if (glIsTextureHandleResidentARB(m_NullDescriptor) == GL_TRUE)
            {
                glMakeTextureHandleNonResidentARB(m_NullDescriptor);
            }
            m_NullDescriptor = 0u;
        }
        if (m_NullSampler != 0u)
        {
            glDeleteSamplers(1, &m_NullSampler);
            m_NullSampler = 0u;
        }
        if (m_NullTexture != 0u)
        {
            glDeleteTextures(1, &m_NullTexture);
            m_NullTexture = 0u;
        }

        if (m_HeapBuffer != 0u)
        {
            glDeleteBuffers(1, &m_HeapBuffer);
            m_HeapBuffer = 0u;
        }

        m_Supported = false;
        m_SlotCapacity = 0u;
        m_Stats = Stats{};
    }

    auto OpenGLDescriptorHeapBackend::IsBindlessSupported() const -> bool
    {
        return m_Supported;
    }

    auto OpenGLDescriptorHeapBackend::SamplerObjectFor(const RHI::SamplerDesc& sampler, bool depthCompare) -> GLuint
    {
        for (const auto& entry : m_Samplers)
        {
            if (entry.Desc == sampler && entry.DepthCompare == depthCompare)
            {
                return entry.Object;
            }
        }

        GLuint object = 0u;
        glCreateSamplers(1, &object);

        glSamplerParameteri(object, GL_TEXTURE_MIN_FILTER,
                            static_cast<GLint>(ToGLMinFilter(sampler.MinFilter, sampler.LinearMipFilter)));
        glSamplerParameteri(object, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(Utils::ToGL(sampler.MagFilter)));
        glSamplerParameteri(object, GL_TEXTURE_WRAP_S, static_cast<GLint>(Utils::ToGL(sampler.AddressU)));
        glSamplerParameteri(object, GL_TEXTURE_WRAP_T, static_cast<GLint>(Utils::ToGL(sampler.AddressV)));
        glSamplerParameteri(object, GL_TEXTURE_WRAP_R, static_cast<GLint>(Utils::ToGL(sampler.AddressW)));
        glSamplerParameterf(object, GL_TEXTURE_MAX_ANISOTROPY, sampler.MaxAnisotropy);

        // The compare mode is what makes one depth array reachable as two views.
        // `ViewDesc::DepthCompare == false` forces it off regardless of the
        // sampler's own CompareOp, because that is the neutral model's way of
        // spelling "give me the raw depth" — the PCSS blocker search's need,
        // which today costs a whole second GL texture object.
        if (depthCompare && sampler.Compare != RHI::CompareOp::Never)
        {
            glSamplerParameteri(object, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glSamplerParameteri(object, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(Utils::ToGL(sampler.Compare)));
        }
        else
        {
            glSamplerParameteri(object, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }

        m_Samplers.push_back(SamplerEntry{ .Desc = sampler, .DepthCompare = depthCompare, .Object = object });
        m_Stats.SamplerObjects = static_cast<u32>(m_Samplers.size());
        return object;
    }

    auto OpenGLDescriptorHeapBackend::AcquireDescriptor(RHI::ResourceHandle resource, const RHI::ViewDesc& view,
                                                        const RHI::SamplerDesc& sampler) -> u64
    {
        if (!m_Supported)
        {
            return 0u;
        }

        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(resource);
        if (native == 0u)
        {
            // A retired handle resolves to 0 by design. Returning 0 here means
            // the heap declines to mint the view rather than publishing a
            // descriptor for whatever object inherited the name.
            ++m_Stats.AcquireFailures;
            return 0u;
        }

        // SUBRESOURCE RANGES AND FORMAT REINTERPRETATION ARE NOT SUPPORTED HERE,
        // and this is a real limitation rather than an unimplemented stub. An
        // ARB_bindless_texture handle names a whole texture object; a mip/layer
        // subrange or a format reinterpretation needs a glTextureView, which
        // needs the source target and internal format — neither of which the
        // neutral ViewDesc carries, on purpose (it describes intent, not GL
        // state). Wiring it would mean either widening the neutral desc with GL
        // shaped fields or a second registry lookup for the texture's metadata.
        // Recorded as a Phase 4 input: Vulkan needs the same information for
        // VkImageViewCreateInfo, so the neutral desc is under-specified for BOTH
        // backends and that is worth knowing before Vulkan bring-up rather than
        // after.
        const RHI::SubresourceRange defaultRange;
        if (!(view.Range == defaultRange) || view.FormatOverride != RHI::Format::Unknown)
        {
            ++m_Stats.UnsupportedViews;
            return 0u;
        }

        const GLuint texture = static_cast<GLuint>(native);
        const GLuint samplerObject = SamplerObjectFor(sampler, view.DepthCompare);

        // glGetTextureSamplerHandleARB rather than glGetTextureHandleARB: the
        // sampler-object form is the one that models a SPLIT heap, where the
        // same texture serves several sampler configurations. The plain form
        // would bake whatever parameters the texture object happens to carry,
        // which is the GL-ism the neutral SamplerDesc exists to stop leaking.
        const GLuint64 handle = glGetTextureSamplerHandleARB(texture, samplerObject);
        if (handle == 0u)
        {
            ++m_Stats.AcquireFailures;
            return 0u;
        }

        u32& refCount = m_Residency[handle];
        if (refCount == 0u)
        {
            // "Already resident" is an INVALID_OPERATION, and the driver's own
            // view of residency can disagree with ours after a context loss, so
            // the query is not redundant with the refcount.
            if (glIsTextureHandleResidentARB(handle) != GL_TRUE)
            {
                glMakeTextureHandleResidentARB(handle);
            }
            ++m_Stats.ResidentHandles;
        }
        ++refCount;

        return static_cast<u64>(handle);
    }

    void OpenGLDescriptorHeapBackend::ReleaseDescriptor(u64 descriptor)
    {
        if (!m_Supported || descriptor == 0u)
        {
            return;
        }

        const auto it = m_Residency.find(descriptor);
        if (it == m_Residency.end() || it->second == 0u)
        {
            return;
        }

        if (--it->second == 0u)
        {
            if (glIsTextureHandleResidentARB(descriptor) == GL_TRUE)
            {
                glMakeTextureHandleNonResidentARB(descriptor);
            }
            m_Residency.erase(it);
            if (m_Stats.ResidentHandles > 0u)
            {
                --m_Stats.ResidentHandles;
            }
        }
    }

    void OpenGLDescriptorHeapBackend::UploadSlots(u32 firstSlot, const u64* descriptors, u32 count)
    {
        if (!m_Supported || m_HeapBuffer == 0u || descriptors == nullptr || count == 0u)
        {
            return;
        }

        if (firstSlot + count > m_SlotCapacity)
        {
            OLO_CORE_ERROR("[RHI/GL] Descriptor heap upload out of range: [{}, {}) against a {}-slot buffer.",
                           firstSlot, firstSlot + count, m_SlotCapacity);
            return;
        }

        glNamedBufferSubData(m_HeapBuffer, static_cast<GLintptr>(sizeof(u64) * firstSlot),
                             static_cast<GLsizeiptr>(sizeof(u64) * count), descriptors);
    }

    void OpenGLDescriptorHeapBackend::BindHeap()
    {
        if (!m_Supported || m_HeapBuffer == 0u)
        {
            return;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ShaderBindingLayout::SSBO_RESOURCE_HEAP, m_HeapBuffer);
    }

    auto OpenGLDescriptorHeapBackend::NullDescriptor() const -> u64
    {
        return m_NullDescriptor;
    }

    auto OpenGLDescriptorHeapBackend::GetStats() const -> Stats
    {
        return m_Stats;
    }
} // namespace OloEngine
