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

        // Does a residency established as `current` already permit `wanted`?
        // READ_WRITE permits everything; the two narrow modes permit only
        // themselves. Used to decide whether a second acquire of the same handle
        // has to widen — see OpenGLDescriptorHeap.h's ImageResidency.
        [[nodiscard]] bool ImageAccessCovers(GLenum current, GLenum wanted)
        {
            return current == GL_READ_WRITE || current == wanted;
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
        if (m_NullDescriptor == 0u)
        {
            // WITHOUT A NULL DESCRIPTOR THE WHOLE MODEL IS UNSAFE, so refuse to
            // run rather than degrade quietly. A zero here would prefill the
            // table with zeroes and hand `NullDescriptor()` a 0 to the neutral
            // heap, so every poisoned, cleared and unallocated slot would hold an
            // invalid handle — sampling which is undefined behaviour, i.e.
            // exactly the state this descriptor exists to remove. Disabling the
            // backend puts the renderer back on the slot-based path, which is
            // the supported configuration on any device without the extension
            // anyway.
            OLO_CORE_ERROR("[RHI/GL] Could not create the bindless null descriptor "
                           "(glGetTextureSamplerHandleARB returned 0). Disabling heap-bindless; "
                           "the slot-based binding path stays in use.");
            glDeleteSamplers(1, &m_NullSampler);
            m_NullSampler = 0u;
            glDeleteTextures(1, &m_NullTexture);
            m_NullTexture = 0u;
            m_Supported = false;
            m_SlotCapacity = 0u;
            return;
        }

        if (glIsTextureHandleResidentARB(m_NullDescriptor) != GL_TRUE)
        {
            glMakeTextureHandleResidentARB(m_NullDescriptor);
        }

        // The null STORAGE-IMAGE descriptor. Separate texture and separate handle
        // for the same reason the sampler null is not handle 0: an `image2D`
        // constructed from a sampler handle is undefined, so pointing a cleared or
        // failed image binding at the sampler null would trade a stale read for
        // undefined behaviour instead of fixing it.
        //
        // R32F because an image handle bakes in a format and every storage image
        // this engine binds is a float or a uint one; a float read of a zeroed
        // R32F is the deterministic 0.0 the poison instrument promises. A uint
        // image reading it sees the bit pattern 0, which is also 0.
        glCreateTextures(GL_TEXTURE_2D, 1, &m_NullImageTexture);
        glTextureStorage2D(m_NullImageTexture, 1, GL_R32F, 1, 1);
        constexpr std::array<f32, 1> kZero = { 0.0f };
        glTextureSubImage2D(m_NullImageTexture, 0, 0, 0, 1, 1, GL_RED, GL_FLOAT, kZero.data());
        glObjectLabel(GL_TEXTURE, m_NullImageTexture, -1, "RHI::DescriptorHeap null image");

        m_NullImageDescriptor =
            static_cast<u64>(glGetImageHandleARB(m_NullImageTexture, 0, GL_FALSE, 0, GL_R32F));

        bool typedImageFailed = false;
        // The other five declared formats. R32F keeps its own dedicated member
        // above because the fail-closed checks below are written against it; the
        // rest live in the map and are looked up by NullStorageDescriptor.
        {
            struct FormatNull
            {
                RHI::Format Neutral;
                GLenum Internal;
            };
            static constexpr std::array<FormatNull, 5> kFormats{ {
                { RHI::Format::RGBA32Float, GL_RGBA32F },
                { RHI::Format::R8UNorm, GL_R8 },
                { RHI::Format::RGBA16Float, GL_RGBA16F },
                { RHI::Format::RGBA8UNorm, GL_RGBA8 },
                { RHI::Format::R32UInt, GL_R32UI },
            } };
            for (const auto& [neutral, internalFormat] : kFormats)
            {
                NullImage entry;
                glCreateTextures(GL_TEXTURE_2D, 1, &entry.Texture);
                glTextureStorage2D(entry.Texture, 1, internalFormat, 1, 1);
                // Zeroed by glTextureStorage2D? No — storage contents are
                // UNDEFINED until written, the same trap the heap buffer prefill
                // exists for. Clear explicitly.
                glClearTexImage(entry.Texture, 0, internalFormat == GL_R32UI ? GL_RED_INTEGER : GL_RGBA,
                                internalFormat == GL_R32UI ? GL_UNSIGNED_INT : GL_FLOAT, nullptr);
                glObjectLabel(GL_TEXTURE, entry.Texture, -1, "RHI::DescriptorHeap null image (typed)");
                entry.Descriptor =
                    static_cast<u64>(glGetImageHandleARB(entry.Texture, 0, GL_FALSE, 0, internalFormat));
                if (entry.Descriptor != 0u && glIsImageHandleResidentARB(entry.Descriptor) != GL_TRUE)
                {
                    glMakeImageHandleResidentARB(entry.Descriptor, GL_READ_WRITE);
                }
                m_NullImagesByFormat[static_cast<u32>(neutral)] = entry;

                // CHECKED PER ENTRY, not once at the end. A zero handle — or one
                // that refused to become resident — is exactly as unsafe as a
                // missing 2D null: NullStorageDescriptor would hand that zero to a
                // cleared binding, and sampling a zero bindless handle is undefined,
                // not black. Recorded rather than returned from here so every
                // created texture goes through the one cleanup path below.
                if (entry.Descriptor == 0u || glIsImageHandleResidentARB(entry.Descriptor) != GL_TRUE)
                {
                    typedImageFailed = true;
                }
            }
        }

        if (m_NullImageDescriptor == 0u || typedImageFailed)
        {
            // Same fail-closed policy as the sampler null above, and for the same
            // reason: without a real null the storage half of the model rests on
            // undefined behaviour, and the slot-based path is the supported
            // configuration anyway.
            OLO_CORE_ERROR("[RHI/GL] Could not create every bindless null IMAGE descriptor "
                           "(base={}, typed-format failure={}). Disabling heap-bindless; "
                           "the slot-based binding path stays in use.",
                           m_NullImageDescriptor, typedImageFailed);
            ReleaseTypedNullImages();
            glDeleteTextures(1, &m_NullImageTexture);
            m_NullImageTexture = 0u;
            if (glIsTextureHandleResidentARB(m_NullDescriptor) == GL_TRUE)
            {
                glMakeTextureHandleNonResidentARB(m_NullDescriptor);
            }
            m_NullDescriptor = 0u;
            glDeleteSamplers(1, &m_NullSampler);
            m_NullSampler = 0u;
            glDeleteTextures(1, &m_NullTexture);
            m_NullTexture = 0u;
            m_Supported = false;
            m_SlotCapacity = 0u;
            return;
        }

        if (glIsImageHandleResidentARB(m_NullImageDescriptor) != GL_TRUE)
        {
            glMakeImageHandleResidentARB(m_NullImageDescriptor, GL_READ_WRITE);
        }

        // ONE NULL PER SAMPLER TYPE. Same argument as the image null above, one
        // level over: the GLSL constructor's type must match the texture's TARGET
        // or the read is undefined, so a shader that resolves an unset input to a
        // null offset and builds `samplerCube` from it needs a real CUBE there.
        // A 2D null is not a conservative fallback for those — it is the defect
        // (issue #691; it surfaced as an order-dependent visible pop,
        // because undefined behaviour may depend on whatever ran before).
        //
        // FAIL-CLOSED, exactly as the two above are — see the branch below this
        // block. An earlier version of this paragraph argued the opposite, that
        // "degrading to the 2D null keeps the engine running"; that was wrong on
        // its own terms, because NullDescriptor() does not degrade to the 2D
        // descriptor for these kinds. It returns the zero it was given, and a zero
        // handle is not a valid bindless handle either.
        const auto makeResident = [](const GLuint64 handle)
        {
            if (handle != 0u && glIsTextureHandleResidentARB(handle) != GL_TRUE)
            {
                glMakeTextureHandleResidentARB(handle);
            }
        };

        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_NullCubeTexture);
        glTextureStorage2D(m_NullCubeTexture, 1, GL_RGBA8, 1, 1);
        for (GLint face = 0; face < 6; ++face)
        {
            glTextureSubImage3D(m_NullCubeTexture, 0, 0, 0, face, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                kBlack.data());
        }
        glObjectLabel(GL_TEXTURE, m_NullCubeTexture, -1, "RHI::DescriptorHeap null cube");
        m_NullCubeDescriptor = static_cast<u64>(glGetTextureSamplerHandleARB(m_NullCubeTexture, m_NullSampler));
        makeResident(m_NullCubeDescriptor);

        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_NullArrayTexture);
        glTextureStorage3D(m_NullArrayTexture, 1, GL_RGBA8, 1, 1, 1);
        glTextureSubImage3D(m_NullArrayTexture, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, kBlack.data());
        glObjectLabel(GL_TEXTURE, m_NullArrayTexture, -1, "RHI::DescriptorHeap null 2D array");
        m_NullArrayDescriptor = static_cast<u64>(glGetTextureSamplerHandleARB(m_NullArrayTexture, m_NullSampler));
        makeResident(m_NullArrayDescriptor);

        // The shadow null needs a DEPTH format and a COMPARISON sampler, because
        // `sampler2DArrayShadow` requires both — a colour texture or a sampler
        // with GL_TEXTURE_COMPARE_MODE = NONE would reproduce the very mismatch
        // this exists to remove. Depth 1.0 so an unset shadow map reads "lit",
        // matching the opaque-white border the real cascades use.
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_NullArrayShadowTexture);
        glTextureStorage3D(m_NullArrayShadowTexture, 1, GL_DEPTH_COMPONENT32F, 1, 1, 1);
        constexpr std::array<f32, 1> kFar = { 1.0f };
        glTextureSubImage3D(m_NullArrayShadowTexture, 0, 0, 0, 0, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                            kFar.data());
        glObjectLabel(GL_TEXTURE, m_NullArrayShadowTexture, -1, "RHI::DescriptorHeap null shadow array");

        glCreateSamplers(1, &m_NullShadowSampler);
        glSamplerParameteri(m_NullShadowSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(m_NullShadowSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(m_NullShadowSampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(m_NullShadowSampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        m_NullArrayShadowDescriptor =
            static_cast<u64>(glGetTextureSamplerHandleARB(m_NullArrayShadowTexture, m_NullShadowSampler));
        makeResident(m_NullArrayShadowDescriptor);

        if (m_NullCubeDescriptor == 0u || m_NullArrayDescriptor == 0u || m_NullArrayShadowDescriptor == 0u)
        {
            // FAIL CLOSED, exactly as the 2D and storage nulls above do. The
            // earlier version of this branch only warned, and its warning was
            // wrong twice over: `NullDescriptor()` does not fall back to the 2D
            // descriptor for these kinds, it returns the ZERO it was given — and a
            // zero handle is not a valid bindless handle, so sampling it is
            // undefined rather than black. "The pre-heap behaviour" it claimed to
            // restore was never reachable pre-heap either.
            //
            // A typed null is not an optional refinement: a shader whose
            // environment probe is unset resolves to the cube null on the COMMON
            // path, so losing it makes the ordinary case undefined. Refusing the
            // extension puts the renderer back on the slot path, which is the
            // supported configuration on any device without it (issue #691).
            OLO_CORE_ERROR("[RHI/GL] Could not create every typed null descriptor (cube={}, array={}, "
                           "arrayShadow={}). Disabling heap-bindless; the slot-based binding path stays "
                           "in use.",
                           m_NullCubeDescriptor, m_NullArrayDescriptor, m_NullArrayShadowDescriptor);
            const auto retire = [](u64& descriptor)
            {
                if (descriptor != 0u && glIsTextureHandleResidentARB(descriptor) == GL_TRUE)
                {
                    glMakeTextureHandleNonResidentARB(descriptor);
                }
                descriptor = 0u;
            };
            ReleaseTypedNullImages();
            retire(m_NullCubeDescriptor);
            retire(m_NullArrayDescriptor);
            retire(m_NullArrayShadowDescriptor);
            retire(m_NullDescriptor);
            if (m_NullImageDescriptor != 0u && glIsImageHandleResidentARB(m_NullImageDescriptor) == GL_TRUE)
            {
                glMakeImageHandleNonResidentARB(m_NullImageDescriptor);
            }
            m_NullImageDescriptor = 0u;
            glDeleteTextures(1, &m_NullCubeTexture);
            m_NullCubeTexture = 0u;
            glDeleteTextures(1, &m_NullArrayTexture);
            m_NullArrayTexture = 0u;
            glDeleteTextures(1, &m_NullArrayShadowTexture);
            m_NullArrayShadowTexture = 0u;
            glDeleteSamplers(1, &m_NullShadowSampler);
            m_NullShadowSampler = 0u;
            glDeleteTextures(1, &m_NullImageTexture);
            m_NullImageTexture = 0u;
            glDeleteSamplers(1, &m_NullSampler);
            m_NullSampler = 0u;
            glDeleteTextures(1, &m_NullTexture);
            m_NullTexture = 0u;
            m_Supported = false;
            m_SlotCapacity = 0u;
            return;
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

        for (const auto& [handle, entry] : m_ImageResidency)
        {
            if (entry.RefCount > 0u && glIsImageHandleResidentARB(handle) == GL_TRUE)
            {
                glMakeImageHandleNonResidentARB(handle);
            }
        }
        m_ImageResidency.clear();

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
        if (m_NullImageDescriptor != 0u)
        {
            if (glIsImageHandleResidentARB(m_NullImageDescriptor) == GL_TRUE)
            {
                glMakeImageHandleNonResidentARB(m_NullImageDescriptor);
            }
            m_NullImageDescriptor = 0u;
        }
        // The per-format nulls, same lifecycle. Leaving these resident across a
        // re-Initialize would leak a handle per format per device reset.
        ReleaseTypedNullImages();
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
        if (m_NullImageTexture != 0u)
        {
            glDeleteTextures(1, &m_NullImageTexture);
            m_NullImageTexture = 0u;
        }

        // The typed sampler nulls, same order as the two above: drop residency
        // before deleting the object, or the handle outlives what it names.
        for (u64* descriptor : { &m_NullCubeDescriptor, &m_NullArrayDescriptor, &m_NullArrayShadowDescriptor })
        {
            if (*descriptor != 0u)
            {
                if (glIsTextureHandleResidentARB(*descriptor) == GL_TRUE)
                {
                    glMakeTextureHandleNonResidentARB(*descriptor);
                }
                *descriptor = 0u;
            }
        }
        if (m_NullShadowSampler != 0u)
        {
            glDeleteSamplers(1, &m_NullShadowSampler);
            m_NullShadowSampler = 0u;
        }
        for (GLuint* texture : { &m_NullCubeTexture, &m_NullArrayTexture, &m_NullArrayShadowTexture })
        {
            if (*texture != 0u)
            {
                glDeleteTextures(1, texture);
                *texture = 0u;
            }
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

    void OpenGLDescriptorHeapBackend::ReleaseTypedNullImages() noexcept
    {
        for (auto& [formatKey, entry] : m_NullImagesByFormat)
        {
            if (entry.Descriptor != 0u && glIsImageHandleResidentARB(entry.Descriptor) == GL_TRUE)
            {
                glMakeImageHandleNonResidentARB(entry.Descriptor);
            }
            if (entry.Texture != 0u)
            {
                glDeleteTextures(1, &entry.Texture);
            }
        }
        m_NullImagesByFormat.clear();
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

        // A SAMPLER OBJECT DOES NOT INHERIT THE TEXTURE'S BORDER COLOUR. Once a
        // sampler object is in play it supplies the whole sampling state, and its
        // border defaults to transparent black — while the engine's depth arrays
        // set an opaque WHITE border so a lookup outside a cascade reads as "lit".
        // Without this the wrap mode would be reproduced and the border would not,
        // darkening exactly the pixels beyond the shadow map's edge.
        if (sampler.AddressU == RHI::AddressMode::ClampToBorder ||
            sampler.AddressV == RHI::AddressMode::ClampToBorder ||
            sampler.AddressW == RHI::AddressMode::ClampToBorder)
        {
            constexpr std::array<GLfloat, 4> kTransparentBlack{ 0.0f, 0.0f, 0.0f, 0.0f };
            constexpr std::array<GLfloat, 4> kOpaqueBlack{ 0.0f, 0.0f, 0.0f, 1.0f };
            constexpr std::array<GLfloat, 4> kOpaqueWhite{ 1.0f, 1.0f, 1.0f, 1.0f };

            const std::array<GLfloat, 4>& border = (sampler.Border == RHI::BorderColor::OpaqueWhite)   ? kOpaqueWhite
                                                   : (sampler.Border == RHI::BorderColor::OpaqueBlack) ? kOpaqueBlack
                                                                                                       : kTransparentBlack;
            glSamplerParameterfv(object, GL_TEXTURE_BORDER_COLOR, border.data());
        }

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

        const GLuint texture = static_cast<GLuint>(native);

        // The two descriptor kinds are two different API calls with two different
        // residency namespaces — not two configurations of one call. Splitting
        // here rather than threading conditionals through one body keeps each
        // one's preconditions next to its own GL entry point.
        return view.Usage == RHI::ViewUsage::Storage ? AcquireStorageDescriptor(texture, view)
                                                     : AcquireSampledDescriptor(texture, view, sampler);
    }

    auto OpenGLDescriptorHeapBackend::AcquireSampledDescriptor(GLuint texture, const RHI::ViewDesc& view,
                                                               const RHI::SamplerDesc& sampler) -> u64
    {
        // SUBRESOURCE RANGES AND FORMAT REINTERPRETATION ARE NOT SUPPORTED FOR A
        // SAMPLED VIEW, and this is a real limitation rather than an
        // unimplemented stub. An ARB_bindless_texture TEXTURE handle names a whole
        // texture object; a mip/layer subrange or a format reinterpretation needs
        // a glTextureView, which needs the source target and internal format —
        // neither of which the neutral ViewDesc carries, on purpose (it describes
        // intent, not GL state).
        //
        // ADR 0011 amendment (20), PARTIALLY CLOSED BY THE STORAGE PATH BELOW and
        // worth being precise about which part. The gap has two halves:
        //
        //   * the FORMAT and the SUBRESOURCE SELECTION. Both are now expressed
        //     neutrally and are exercised end to end — `glGetImageHandleARB` takes
        //     exactly them, and every converted image site supplies them.
        //   * the VIEW DIMENSION (GL's `glTextureView` target, Vulkan's
        //     `VkImageViewType`) and the resolution of `FormatOverride ==
        //     Unknown` against the resource's own format. Neither is needed by
        //     the image path, so neither has been added — an unexercised neutral
        //     field is the sort of invented vocabulary the call-site sweep paid for.
        //
        // So the remaining half is still a device-bring-up input, and it is now a SMALLER
        // and better-specified one.
        const RHI::SubresourceRange defaultRange;
        if (!(view.Range == defaultRange) || view.FormatOverride != RHI::Format::Unknown)
        {
            ++m_Stats.UnsupportedViews;
            return 0u;
        }

        // A DEFAULT SamplerDesc MEANS "WHATEVER THE SLOT PATH WOULD HAVE USED",
        // and that is not the same thing as "the default sampler state".
        //
        // The slot path samples with the TEXTURE OBJECT's parameters, so a caller
        // that expresses no sampling intent is asking for those. Minting a sampler
        // object from a default-constructed desc instead answers a question nobody
        // asked, and every field it gets wrong is a silent divergence visible only
        // to a converted shader:
        //
        //   * WRAP. OpenGLTexture2D and every framebuffer attachment are REPEAT
        //     (GL's own default); OpenGLTexture2DArray is CLAMP_TO_EDGE for colour
        //     and CLAMP_TO_BORDER for depth; OpenGLTextureCubemap is CLAMP_TO_EDGE;
        //     OpenGLTexture3D takes its from the caller. No single default is right
        //     for all four, which is why chasing this with per-target helpers was
        //     whack-a-mole — fixing 2D broke the terrain arrays.
        //   * FILTER ON AN INTEGER FORMAT. GL makes an integer texture with a
        //     LINEAR filter *incomplete*, and an incomplete texture samples as ZERO
        //     — texelFetch included. Texture.h's IsIntegerFormat records what that
        //     already cost once: every Slug glyph vanished, on AMD only, with the
        //     draw calls and logs looking healthy. The heap reintroduced it for the
        //     RG16UI band texture, the R16UI GTAO Hilbert LUT and the R32I entity
        //     buffer.
        //   * MIP COMPLETENESS. LinearMipFilter defaults true, which resolves to
        //     GL_LINEAR_MIPMAP_LINEAR and makes a single-level texture incomplete —
        //     the hazard HeapBinding::ShadowDepthSampler works around BY HAND.
        //
        // So: no intent stated -> `glGetTextureHandleARB`, which bakes the object's
        // own state and is parity BY CONSTRUCTION rather than by a table of
        // per-target defaults somebody has to keep correct. Intent stated -> the
        // sampler-object form, which is what models a SPLIT heap and is what the
        // sites that genuinely differ from their texture use (ShadowDepthSampler's
        // comparison-on/comparison-off pair over one depth array, SSAO's
        // Nearest+Repeat noise).
        //
        // WHAT THIS COSTS, stated plainly because it is a real trade: the plain
        // form re-admits the GL-ism the neutral SamplerDesc exists to keep out.
        // Vulkan has no "inherit" — a VkSampler must be described — so every site
        // passing a default desc today is a site device bring-up has to give real sampler
        // state. `DefaultSamplerInherits` counts them, so that work is measurable
        // instead of discovered.
        // THE DISCRIMINATOR DECIDES, not a comparison against the defaults.
        // `SamplerDesc::Source` exists so "inherit" and "these exact values" stay
        // distinct when the values coincide, and reading it here is what makes
        // that promise real rather than documentary.
        //
        // The ViewDesc half is COMPARED AGAINST A DEFAULT-CONSTRUCTED ONE, not
        // against `false`. `ViewDesc::DepthCompare` DEFAULTS TO TRUE — true means
        // "whatever the resource's own view would give", and `false` is the
        // deliberate raw-depth override the PCSS blocker search asks for. Writing
        // it as `!view.DepthCompare` inverts the guard and disables inheriting for
        // every ordinary view, which is exactly what
        // HeapGpuFixture.ADefaultSamplerDescInheritsTheTextureObjectRatherThanMintingOne
        // caught on its first run. Spelling it this way survives the default
        // flipping too.
        static constexpr RHI::ViewDesc kUnstatedView;

        // FORGETTING THE DISCRIMINATOR MUST NOT DISCARD THE CALLER'S FIELDS.
        //
        // Making `Source` the sole test introduces a trap the field was added to
        // remove: a caller who sets MinFilter/AddressU/Compare and forgets
        // `Source = Explicit` would have all of it silently replaced by the
        // texture object's state. That is not theoretical — deleting the line from
        // `HeapBinding::ShadowDepthSampler` was tried here and 136 tests passed
        // with the shadow comparison sampler quietly inheriting.
        //
        // So a desc whose FIELDS say something is treated as explicit whatever its
        // Source says, and the disagreement is reported. The fallback is the
        // pre-discriminator behaviour, so the failure mode is a warning rather
        // than a wrong frame — and `Source` keeps its one real job: letting a
        // caller demand these values even when they match the defaults.
        RHI::SamplerDesc unstatedFields;
        unstatedFields.Source = sampler.Source;
        const bool fieldsAreDefault = (sampler == unstatedFields);

        if (sampler.Source == RHI::SamplerSource::InheritTexture && !fieldsAreDefault)
        {
            if (static std::atomic<u64> s_Warned{ 0 };
                s_Warned.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                OLO_CORE_WARN("[RHI/GL] SamplerDesc sets fields but leaves Source = InheritTexture. Honouring the "
                              "fields; set RHI::SamplerSource::Explicit to say so (issue #691).");
            }
        }

        const bool inheritsTextureState = (sampler.Source == RHI::SamplerSource::InheritTexture) &&
                                          fieldsAreDefault &&
                                          (view.DepthCompare == kUnstatedView.DepthCompare);

        GLuint64 handle = 0u;
        if (inheritsTextureState)
        {
            ++m_Stats.DefaultSamplerInherits;
            handle = glGetTextureHandleARB(texture);
        }
        else
        {
            const GLuint samplerObject = SamplerObjectFor(sampler, view.DepthCompare);
            handle = glGetTextureSamplerHandleARB(texture, samplerObject);
        }

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

    auto OpenGLDescriptorHeapBackend::AcquireStorageDescriptor(GLuint texture, const RHI::ViewDesc& view) -> u64
    {
        // A STORAGE IMAGE'S FORMAT IS PART OF ITS BINDING CONTRACT, not an
        // optional reinterpretation: it has to match the shader's format layout
        // qualifier, and `glGetImageHandleARB` has nowhere to put "whatever the
        // texture happens to be". Declining is the only honest answer — guessing
        // the resource's format would produce a handle the shader reads through
        // the wrong interpretation, which is a plausible-looking wrong image
        // rather than a missing one.
        if (view.FormatOverride == RHI::Format::Unknown)
        {
            ++m_Stats.UnsupportedViews;
            return 0u;
        }

        // Invert MakeStorageViewDesc's neutral mapping. One mip, always: an image
        // binding addresses exactly one level, so a MipCount other than 1 is a
        // caller describing something GL cannot bind rather than something this
        // backend has not implemented.
        if (view.Range.MipCount != 1u)
        {
            ++m_Stats.UnsupportedViews;
            return 0u;
        }

        const bool layered = view.Range.LayerCount == RHI::SubresourceRange::AllRemaining;
        if (!layered && view.Range.LayerCount != 1u)
        {
            // A contiguous run of layers that is neither "one" nor "all" has no
            // glBindImageTexture spelling either — the slot-based path could not
            // express it, so the heap path declining it is parity, not a
            // regression.
            ++m_Stats.UnsupportedViews;
            return 0u;
        }

        const GLuint64 handle = glGetImageHandleARB(texture, static_cast<GLint>(view.Range.BaseMip),
                                                    layered ? GL_TRUE : GL_FALSE,
                                                    static_cast<GLint>(layered ? 0u : view.Range.BaseLayer),
                                                    Utils::ToGLInternalFormat(view.FormatOverride));
        if (handle == 0u)
        {
            ++m_Stats.AcquireFailures;
            return 0u;
        }

        // The SAME lowering the slot-based BindImageTexture uses, deliberately.
        // A second spelling of "which GL access is this" is how the two paths
        // would drift into treating one binding differently — the mistake the
        // sampler path already made once with LinearMipFilter.
        const GLenum wanted = Utils::ToGLImageAccess(view.StorageAccess);

        ImageResidency& residency = m_ImageResidency[handle];
        if (residency.RefCount == 0u)
        {
            if (glIsImageHandleResidentARB(handle) != GL_TRUE)
            {
                glMakeImageHandleResidentARB(handle, wanted);
            }
            residency.Access = wanted;
            ++m_Stats.ResidentImageHandles;
        }
        else if (!ImageAccessCovers(residency.Access, wanted))
        {
            // WIDEN. `glGetImageHandleARB` takes no access, so a read-only and a
            // read-write view of the same (texture, level, layer, format) are
            // literally the same handle — and reading a WRITE_ONLY-resident
            // handle, or writing a READ_ONLY one, is undefined. GTAO does exactly
            // this: its edge texture is bound WRITE_ONLY by the main pass and
            // READ_ONLY by the denoise pass in the same frame.
            //
            // Re-residency is not optional here and the transition has to go
            // through non-resident first, because making an already-resident
            // handle resident again is an INVALID_OPERATION.
            if (glIsImageHandleResidentARB(handle) == GL_TRUE)
            {
                glMakeImageHandleNonResidentARB(handle);
            }
            glMakeImageHandleResidentARB(handle, GL_READ_WRITE);
            residency.Access = GL_READ_WRITE;
            ++m_Stats.ImageResidencyWidenings;
        }
        ++residency.RefCount;

        return static_cast<u64>(handle);
    }

    void OpenGLDescriptorHeapBackend::ReleaseDescriptor(u64 descriptor, RHI::ViewUsage usage)
    {
        if (!m_Supported || descriptor == 0u)
        {
            return;
        }

        if (usage == RHI::ViewUsage::Storage)
        {
            const auto it = m_ImageResidency.find(descriptor);
            if (it == m_ImageResidency.end() || it->second.RefCount == 0u)
            {
                return;
            }

            if (--it->second.RefCount == 0u)
            {
                if (glIsImageHandleResidentARB(descriptor) == GL_TRUE)
                {
                    glMakeImageHandleNonResidentARB(descriptor);
                }
                m_ImageResidency.erase(it);
                if (m_Stats.ResidentImageHandles > 0u)
                {
                    --m_Stats.ResidentImageHandles;
                }
            }
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

    auto OpenGLDescriptorHeapBackend::NullStorageDescriptor(const RHI::Format format) const -> u64
    {
        // The R32F null is the documented fallback for a format this table does
        // not carry — defined-but-wrong beats undefined, the same trade every
        // other null here makes.
        // NORMALISED FIRST, so this agrees with NullOffsetForStorageFormat — which
        // folds RGBA8SRGB onto the RGBA8 slot. Without the fold, an SRGB view
        // resolved to the RGBA8 reserved OFFSET but poisoned with the R32F
        // DESCRIPTOR: the offset and the descriptor disagreeing about format is
        // exactly the mismatch these nulls exist to prevent.
        const RHI::Format lookup = format == RHI::Format::RGBA8SRGB ? RHI::Format::RGBA8UNorm : format;
        const auto it = m_NullImagesByFormat.find(static_cast<u32>(lookup));
        return it != m_NullImagesByFormat.end() ? it->second.Descriptor : m_NullImageDescriptor;
    }

    auto OpenGLDescriptorHeapBackend::NullDescriptor(const RHI::ViewUsage usage,
                                                     const RHI::NullSamplerKind kind) const -> u64
    {
        if (usage == RHI::ViewUsage::Storage)
        {
            // An image binding has no sampler type to mismatch, so `kind` is
            // meaningless here rather than merely unused.
            return m_NullImageDescriptor;
        }

        switch (kind)
        {
            case RHI::NullSamplerKind::Cube:
                return m_NullCubeDescriptor;
            case RHI::NullSamplerKind::Texture2DArray:
                return m_NullArrayDescriptor;
            case RHI::NullSamplerKind::Texture2DArrayShadow:
                return m_NullArrayShadowDescriptor;
            case RHI::NullSamplerKind::Texture2D:
            default:
                return m_NullDescriptor;
        }
    }

    auto OpenGLDescriptorHeapBackend::GetStats() const -> Stats
    {
        return m_Stats;
    }
} // namespace OloEngine
