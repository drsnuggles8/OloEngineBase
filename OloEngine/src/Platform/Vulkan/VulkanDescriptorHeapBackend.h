#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanDescriptorHeapBackend — RHI::IDescriptorHeapBackend over
// VulkanResourceHeap. Issue #691 (the amendment (56) deferral, built).
//
// This puts the ENGINE-side heap — slot lifetime, generations, memoisation,
// poison-on-free, the reserved nulls — in charge of a range of the real
// VK_EXT_descriptor_heap buffer. The composition the interface was audited
// for: AcquireDescriptor → a descriptor WRITE (staged), UploadSlots → writes
// into the slot region, BindHeap → the command-buffer bind (owned by the draw
// path's per-recording CmdBind, so a no-op here).
//
// THE TOKEN MODEL, because the interface's u64 cannot be a Vulkan descriptor:
// under ARB_bindless_texture a descriptor IS a u64 the backend can hand
// around; under VK_EXT_descriptor_heap it is an opaque blob (32 B on NVIDIA)
// the driver writes directly into heap memory at a slot the engine picks
// LATER (UploadSlots' firstSlot). AcquireDescriptor therefore returns a
// TOKEN — a key into a staged map of fully-resolved view descriptions — and
// UploadSlots redeems tokens into vkWriteResourceDescriptorsEXT calls at
// their final slots. Reserved low tokens are the null descriptors, staged
// once against backend-owned null images (below) and redeemed exactly like
// real views.
//
// SAMPLERS: recorded but not consumed. This backend serves the resource heap;
// sampler state is embedded per pipeline (ADR 0011 amendment (50) /
// glsl-shaders.md §5f). The §1.2a sampler HEAP (vkCmdBindSamplerHeapEXT +
// per-draw sampler indices) is the recorded follow-up for when a pass needs
// per-draw sampler variety the embedded model cannot express.
//
// NULLS ARE REAL 1x1 BLACK IMAGES, the GL backend's own discipline, because
// the extension offers no free null write (a null pView is a validation
// error; robustness2's nullDescriptor is not on the device floor). One per
// SAMPLED view dimension (2D / cube / 2D-array — the typed-null
// lesson: constructing samplerCube from a 2D descriptor is undefined on any
// backend) and one STORAGE image per requested format (a layout qualifier
// disagreeing with the view format is undefined too). All reads are
// deterministic zeros/black; the images live for the heap's lifetime and are
// reclaimed with it.
//
// Thread-safety (issue #806, ADR 0011 amendment (92) rule 8): one entry
// point is on the draw path — GetNullSampledHeapSlot, the root-data writer's
// unfed-binding fallback — and it serialises end to end on
// m_NullSampledSlotMutex (memo lookup, null-image creation on a miss, the
// slot-cache acquire, memo insert). LOCK ORDER: that mutex is taken BEFORE
// VulkanDescriptorSlotCache's (AcquireSlot is called while holding it), never
// after. WriteNullAt is the slot cache's call INTO this backend under the
// cache's own lock and deliberately takes no lock here — taking the memo
// mutex there would invert the order. Every other mutator (AcquireDescriptor,
// ReleaseDescriptor, UploadSlots, WriteNullAt, ReleaseDeviceObjects) is a
// resource-creation / engine-heap-flush / teardown path that rule 7 keeps on
// the render thread outside a region, so m_Staged and the null-image table
// are only READ (EnsureNullImage's hit path) while a region is open.
// =============================================================================

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "Platform/Vulkan/VulkanDevice.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace OloEngine
{
    class VulkanDescriptorHeapBackend final : public RHI::IDescriptorHeapBackend
    {
      public:
        // Process-wide instance, deliberately leaked (the engine heap holds a
        // raw pointer for the process lifetime — same rationale as the other
        // backend singletons).
        [[nodiscard]] static VulkanDescriptorHeapBackend& Get();

        // Reserve the engine heap's slot range in VulkanResourceHeap and
        // install this backend on RHI::DescriptorHeap with the SAME capacities
        // the GL backend uses (OpenGLDescriptorHeap.h), so heap-dependent
        // engine behaviour cannot diverge by backend. Idempotent. False when
        // no live device / heap creation failed.
        static bool InstallOntoEngineHeap();

        // --- RHI::IDescriptorHeapBackend -----------------------------------
        [[nodiscard]] auto IsBindlessSupported() const -> bool override;
        [[nodiscard]] auto AcquireDescriptor(RHI::ResourceHandle resource, const RHI::ViewDesc& view,
                                             const RHI::SamplerDesc& sampler) -> u64 override;
        void ReleaseDescriptor(u64 descriptor, RHI::ViewUsage usage) override;
        void UploadSlots(u32 firstSlot, const u64* descriptors, u32 count) override;
        void BindHeap() override;
        [[nodiscard]] auto NullDescriptor(RHI::ViewUsage usage, RHI::NullSamplerKind kind) const -> u64 override;
        [[nodiscard]] auto NullStorageDescriptor(RHI::Format format) const -> u64 override;

        // Write the null descriptor of `type` at an arbitrary heap slot —
        // the poison/prefill primitive the slot cache and the install path
        // share (a freed or never-written slot must read deterministic
        // black). Creates the null images on first use. Takes NO lock: the
        // slot cache calls it while holding its own mutex (lock-order note
        // above).
        bool WriteNullAt(u32 slot, VkDescriptorType type);

        // Heap slot of the 1x1 black null SAMPLED image for `viewType` — the
        // root-data writer's unfed-binding fallback (#691; slot 0
        // leaked the first-registered texture into every unfed sampler).
        // Acquired through VulkanDescriptorSlotCache so the slot dies with
        // the heap; the image is reclaimed by ReleaseDeviceObjects like the
        // token nulls. VulkanResourceHeap::InvalidSlot on failure. Safe from
        // several recording threads at once (m_NullSampledSlotMutex).
        [[nodiscard]] u32 GetNullSampledHeapSlot(VkImageViewType viewType);

        // Enqueue every null image for deferred reclaim and forget the
        // tokens. Called when the resource heap itself is released — the
        // descriptors pointing at these images die with it.
        void ReleaseDeviceObjects();

        // Diagnostic/test affordances.
        [[nodiscard]] sizet GetStagedDescriptorCount() const
        {
            return m_Staged.size();
        }

      private:
        VulkanDescriptorHeapBackend() = default;

        // A staged descriptor: everything vkWriteResourceDescriptorsEXT needs,
        // fully resolved at acquire time so redemption is self-contained (the
        // VkImageViewCreateInfo the driver reads points at THIS storage).
        struct Staged
        {
            VkImage Image = VK_NULL_HANDLE;
            VkImageViewCreateInfo ViewInfo{};
            VkImageLayout Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorType Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        };

        static constexpr u64 kNullSampled2DToken = 1;
        static constexpr u64 kNullSampledCubeToken = 2;
        static constexpr u64 kNullSampledArrayToken = 3;
        static constexpr u64 kFirstNullStorageToken = 4; // per-format, minted upward
        // The unfed-binding fallback's two extra shapes (#691) —
        // parked just below the dynamic range, clear of the storage mints.
        static constexpr u64 kNullSampledCubeArrayToken = 62;
        static constexpr u64 kNullSampled3DToken = 63;
        static constexpr u64 kFirstDynamicToken = 64;

        struct NullImage
        {
            VkImage Image = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
        };

        // Create (lazily) the 1x1 black image behind a null token and stage
        // its descriptor under that token. False on failure.
        bool EnsureNullImage(u64 token, VkImageViewType viewType, VkFormat format, VkDescriptorType type);
        [[nodiscard]] u64 NullStorageTokenFor(VkFormat format);

        std::unordered_map<u64, Staged> m_Staged;
        std::unordered_map<u64, NullImage> m_NullImages;         ///< token -> owned image
        std::unordered_map<u32, u64> m_NullStorageTokenByFormat; ///< VkFormat -> token
        // The draw-path memo and its lock (thread-safety note above). Taken
        // BEFORE the slot cache's mutex, never after.
        std::shared_mutex m_NullSampledSlotMutex; ///< Shared on the memo hit, exclusive to create.

      public:
        // Create every view type's null sampled slot now, on the render
        // thread: a miss inside a RecordParallel item would need a one-shot
        // submit, which is refused there (amendment (92) rule 7). Idempotent.
        void WarmNullSampledSlots();

      private:
        std::unordered_map<u32, u32> m_NullSampledSlots; ///< VkImageViewType -> acquired heap slot
        u64 m_NextNullStorageToken = kFirstNullStorageToken;
        u64 m_NextToken = kFirstDynamicToken;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
