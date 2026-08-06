#pragma once

// =============================================================================
// OpenGLDescriptorHeap.h — the ARB_bindless_texture half of the Phase 3 heap.
//
// Issue #691 Phase 3, ADR 0011 §1.2 / §1.2a.
//
// `RHI::DescriptorHeap` (Renderer/RHI/) owns slot allocation, the two lifetime
// classes, generation validation and poisoning — all backend-independent. This
// owns the two things that are not: what descriptor a view produces, and how
// the table is published to the shader.
//
// WHAT DOES NOT TRANSFER TO VULKAN, and it is most of this file:
//
//   * RESIDENCY. `ARB_bindless_texture` requires an explicit
//     resident/non-resident transition per handle, and making an already
//     resident handle resident again is an error — so this class refcounts.
//     `VK_EXT_descriptor_heap` has no such concept: you write the descriptor
//     and you are done.
//   * IMMUTABILITY. Once a texture has yielded a handle it may not have its
//     parameters changed, and a handle names the underlying object rather than
//     a view of it. Recreated storage therefore silently invalidates every
//     descriptor for that texture while leaving the engine's ResourceHandle
//     valid on purpose — which is why RHI::DescriptorHeap::InvalidateResource
//     exists.
//   * SAMPLER FOLDING. GL bakes sampler state into the texture handle, so the
//     sampler heap here is bookkeeping only. Under a split heap it is real, and
//     an exhausted sampler heap becomes a correctness failure rather than the
//     warning it is here.
//
// What DOES transfer is everything above this file: views owning slots, the
// two lifetimes, offsets as data, and fetch-don't-store. That asymmetry is the
// point of running the rehearsal here first.
// =============================================================================

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"

#include <glad/gl.h>

#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Heap sizing. Declared here rather than at the call site so the buffer the
    // backend allocates and the slot budget the neutral heap hands out cannot
    // drift apart — an upload past the end of the buffer is silent on some
    // drivers and a device-lost on others.
    inline constexpr u32 kDescriptorHeapPersistentSlots = 4096u;
    inline constexpr u32 kDescriptorHeapTransientSlots = 1024u;
    inline constexpr u32 kDescriptorHeapSlots = kDescriptorHeapPersistentSlots + kDescriptorHeapTransientSlots;
    // Distinct SamplerDesc values. Deliberately small: ADR 0011 §1.2a's whole
    // argument for a separate sampler heap is that a handful of configurations
    // serve hundreds of textures, so a number in the thousands here would mean
    // the deduplication is not working and should be seen rather than absorbed.
    inline constexpr u32 kDescriptorHeapSamplerSlots = 64u;

    class OpenGLDescriptorHeapBackend final : public RHI::IDescriptorHeapBackend
    {
      public:
        // Probes the extension and builds the shader-visible buffer. Safe to
        // call without the extension present — the object then reports
        // unsupported and every entry point is inert, which is the state a
        // machine below the feature floor runs in.
        void Initialize(u32 slotCapacity);
        void Shutdown();

        [[nodiscard]] auto IsBindlessSupported() const -> bool override;

        [[nodiscard]] auto AcquireDescriptor(RHI::ResourceHandle resource, const RHI::ViewDesc& view,
                                             const RHI::SamplerDesc& sampler) -> u64 override;

        void ReleaseDescriptor(u64 descriptor, RHI::ViewUsage usage) override;

        void UploadSlots(u32 firstSlot, const u64* descriptors, u32 count) override;

        void BindHeap() override;

        [[nodiscard]] auto NullDescriptor(RHI::ViewUsage usage, RHI::NullSamplerKind kind) const
            -> u64 override;

        struct Stats
        {
            u32 ResidentHandles = 0;         ///< distinct SAMPLER handles currently resident
            u32 ResidentImageHandles = 0;    ///< distinct STORAGE-IMAGE handles currently resident
            u32 SamplerObjects = 0;          ///< deduplicated GL sampler objects
            u64 AcquireFailures = 0;         ///< dead resource, or a view GL cannot express
            u64 UnsupportedViews = 0;        ///< subresource/format reinterpretation, see below
            u64 ImageResidencyWidenings = 0; ///< a handle re-made resident READ_WRITE, see below
        };
        [[nodiscard]] auto GetStats() const -> Stats;

      private:
        [[nodiscard]] auto SamplerObjectFor(const RHI::SamplerDesc& sampler, bool depthCompare) -> GLuint;
        [[nodiscard]] auto AcquireSampledDescriptor(GLuint texture, const RHI::ViewDesc& view,
                                                    const RHI::SamplerDesc& sampler) -> u64;
        [[nodiscard]] auto AcquireStorageDescriptor(GLuint texture, const RHI::ViewDesc& view) -> u64;

        bool m_Supported = false;
        GLuint m_HeapBuffer = 0u;
        // A resident 1x1 opaque-black texture and its handle. Every unallocated,
        // freed or cleared slot holds this rather than 0, because sampling an
        // invalid bindless handle is undefined behaviour and this model leans on
        // poison reads being DETERMINISTIC.
        GLuint m_NullTexture = 0u;
        GLuint m_NullSampler = 0u;
        u64 m_NullDescriptor = 0u;
        // The same idea for the second descriptor kind: a 1x1 R32F texture whose
        // IMAGE handle is resident READ_WRITE. It must be a separate texture
        // because an image handle carries a format and R32F is the only one an
        // `imageLoad` of any float image can safely read as zero; and it must be a
        // separate HANDLE because `image2D(samplerHandle)` is undefined.
        GLuint m_NullImageTexture = 0u;
        u64 m_NullImageDescriptor = 0u;

        // And one null per SAMPLER TYPE, for the same reason one level over: the
        // GLSL type used to construct a sampler from a handle must match the
        // texture's target or the read is undefined. A shader whose environment
        // probe is unset resolves to a null offset and builds `samplerCube` from
        // it — so a 2D null there is not a safe fallback, it is the bug
        // (issue #691 Phase 3). The array-shadow null carries a depth format and a
        // comparison sampler because `sampler2DArrayShadow` demands both.
        GLuint m_NullCubeTexture = 0u;
        u64 m_NullCubeDescriptor = 0u;
        GLuint m_NullArrayTexture = 0u;
        u64 m_NullArrayDescriptor = 0u;
        GLuint m_NullArrayShadowTexture = 0u;
        GLuint m_NullShadowSampler = 0u;
        u64 m_NullArrayShadowDescriptor = 0u;

        u32 m_SlotCapacity = 0u;

        // Residency refcount. Two views that differ only in a field GL folds
        // away (a subresource range this backend cannot express, say) produce
        // the SAME handle, and glMakeTextureHandleResidentARB rejects a second
        // residency transition — so the count is required for correctness, not
        // for tidiness.
        std::unordered_map<u64, u32> m_Residency;

        // Image residency, kept in a SEPARATE map rather than a flag on the one
        // above. Two reasons, and the second is the load-bearing one:
        //
        //  * The transitions are different calls
        //    (`glMakeImageHandleResidentARB` / `...NonResidentARB`), and the spec
        //    does not promise a texture handle and an image handle are
        //    numerically distinguishable — so one map keyed on the value alone
        //    could dispatch the wrong call for a colliding pair.
        //  * An image residency carries an ACCESS, and it has to be WIDENED.
        //    `glGetImageHandleARB` takes no access, so a read-only and a
        //    read-write view of the same (texture, level, layer, format) are the
        //    SAME handle — but reading a WRITE_ONLY-resident handle, or writing a
        //    READ_ONLY one, is undefined. When a second acquire needs an access
        //    the current residency does not cover, the handle is re-made resident
        //    as READ_WRITE (counted in Stats::ImageResidencyWidenings).
        struct ImageResidency
        {
            u32 RefCount = 0u;
            GLenum Access = 0u; ///< GL_READ_ONLY / GL_WRITE_ONLY / GL_READ_WRITE
        };
        std::unordered_map<u64, ImageResidency> m_ImageResidency;

        // Deduplicated sampler objects. This is what makes one depth array
        // reachable as both a comparison sampler and a raw-depth one WITHOUT
        // the second GL texture object CreateDepthArrayCompareOffViewHandle
        // creates today — two sampler objects over one texture, two handles,
        // two heap slots.
        struct SamplerEntry
        {
            RHI::SamplerDesc Desc;
            bool DepthCompare = true;
            GLuint Object = 0u;
        };
        std::vector<SamplerEntry> m_Samplers;

        Stats m_Stats;
    };
} // namespace OloEngine
