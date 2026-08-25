#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDescriptorHeapBackend.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/OpenGL/OpenGLDescriptorHeap.h" // the shared kDescriptorHeap* capacities
#include "Platform/Vulkan/VulkanBarrierLowering.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
    } // namespace

    VulkanDescriptorHeapBackend& VulkanDescriptorHeapBackend::Get()
    {
        static auto* s_Instance = new VulkanDescriptorHeapBackend(); // deliberately leaked
        return *s_Instance;
    }

    bool VulkanDescriptorHeapBackend::InstallOntoEngineHeap()
    {
        auto& resourceHeap = VulkanResourceHeap::Get();
        if (!resourceHeap.EnsureCreated())
        {
            return false;
        }
        // The engine heap owns slots [0, kDescriptorHeapSlots); the backend's
        // own bump allocator (draw-path slot cache, pilot) starts past them.
        // A FRESH reservation (0 -> count: first install, or the first after a
        // heap Release/re-create) prefills the whole range with null
        // descriptors — the GL backend's discipline, kept for the same
        // reason: an unwritten heap slot is undefined memory, not zero, and
        // "every reachable slot is defined" must hold from the first frame.
        const bool fresh = resourceHeap.GetReservedSlots() == 0u;
        if (!resourceHeap.ReserveSlotRange(kDescriptorHeapSlots))
        {
            OLO_CORE_ERROR("VulkanDescriptorHeapBackend: cannot reserve {} engine-heap slots", kDescriptorHeapSlots);
            return false;
        }
        if (fresh)
        {
            for (u32 slot = 0; slot < kDescriptorHeapSlots; ++slot)
            {
                (void)Get().WriteNullAt(slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            }
        }

        // Same capacities as the GL install (OpenGLRendererAPI::Init), so
        // heap-dependent engine behaviour cannot diverge by backend.
        RHI::HeapDesc heapDesc;
        heapDesc.ResourceSlotCapacity = kDescriptorHeapPersistentSlots;
        heapDesc.SamplerSlotCapacity = kDescriptorHeapSamplerSlots;
        heapDesc.FrameTransientRingSlots = kDescriptorHeapTransientSlots;
#ifdef OLO_DEBUG
        heapDesc.PoisonOnFree = true;
#else
        heapDesc.PoisonOnFree = false;
#endif

        RHI::DescriptorHeap::Get().Initialize(heapDesc, &Get());
        return true;
    }

    auto VulkanDescriptorHeapBackend::IsBindlessSupported() const -> bool
    {
        // The device gate already refused anything without
        // VK_EXT_descriptor_heap; "supported" here reduces to "the heap
        // buffer exists".
        return VulkanResourceHeap::Get().EnsureCreated();
    }

    auto VulkanDescriptorHeapBackend::AcquireDescriptor(const RHI::ResourceHandle resource, const RHI::ViewDesc& view,
                                                        const RHI::SamplerDesc& sampler) -> u64
    {
        // Samplers are embedded per pipeline on this backend (header note);
        // the value participates in the engine heap's dedup/memo keys and is
        // otherwise unused here.
        (void)sampler;

        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(resource);
        if (native == 0u)
        {
            return 0; // dead resource — the heap points the slot at a null
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            // Not a Vulkan-backend image (mixed-currency callers); refuse
            // rather than write a descriptor for a foreign handle value.
            return 0;
        }

        const bool storage = view.Usage == RHI::ViewUsage::Storage;

        // FormatOverride: Unknown inherits the image's own format — the
        // amendment (30) gap ("cannot resolve an inherited format") closes
        // here, where the backend can see the image's metadata.
        VkFormat format = VulkanBarrierLowering::ToVkFormat(view.FormatOverride);
        if (format == VK_FORMAT_UNDEFINED)
        {
            format = info->Format;
        }

        const u32 mipCount = std::max(info->MipLevels, 1u);
        const u32 layerCount = std::max(info->ArrayLayers, 1u);

        Staged staged;
        staged.Image = image;
        staged.Type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        // A storage descriptor's baked layout must match the barrier plan's
        // GENERAL for storage accesses; a sampled one the SHADER_READ_ONLY
        // the plan transitions inputs to (VulkanBarrierLowering::LayoutFor).
        staged.Layout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        auto& viewInfo = staged.ViewInfo;
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        // The registry's recorded dimensionality is the authority (#691):
        // deriving from layerCount alone handed a CUBE image a
        // 2D_ARRAY view against a samplerCube declaration (and a 3D volume a
        // 2D one). Storage views are the exception — imageCube is not a
        // declared consumer, so cube-compatible images bind as their layer
        // array there (BindImageTexture's rule).
        viewInfo.viewType = layerCount > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (info->ViewType == VK_IMAGE_VIEW_TYPE_3D)
        {
            // A volume's storage consumer is image3D, and a 2D(_ARRAY) view
            // of a 3D image needs a create flag the registry never sets — so
            // 3D stays 3D for BOTH usages (the cube storage exception above
            // does not extend here).
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        }
        else if (!storage && info->ViewType != VK_IMAGE_VIEW_TYPE_2D)
        {
            viewInfo.viewType = info->ViewType;
        }
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask =
            info->HasDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = std::min(view.Range.BaseMip, mipCount - 1u);
        viewInfo.subresourceRange.levelCount =
            (view.Range.MipCount == RHI::SubresourceRange::AllRemaining)
                ? VK_REMAINING_MIP_LEVELS
                : std::max(std::min(view.Range.MipCount, mipCount - viewInfo.subresourceRange.baseMipLevel), 1u);
        viewInfo.subresourceRange.baseArrayLayer = std::min(view.Range.BaseLayer, layerCount - 1u);
        viewInfo.subresourceRange.layerCount =
            (view.Range.LayerCount == RHI::SubresourceRange::AllRemaining)
                ? VK_REMAINING_ARRAY_LAYERS
                : std::max(std::min(view.Range.LayerCount, layerCount - viewInfo.subresourceRange.baseArrayLayer), 1u);

        const u64 token = m_NextToken++;
        m_Staged[token] = staged;
        return token;
    }

    void VulkanDescriptorHeapBackend::ReleaseDescriptor(const u64 descriptor, RHI::ViewUsage /*usage*/)
    {
        // Residency has no Vulkan analogue (the interface's own note); release
        // just drops the staged description. The SLOT's contents are the
        // engine heap's business — its poison-on-free writes a null/poison
        // value into the mirror and the next Flush redeems it below.
        if (descriptor >= kFirstDynamicToken)
        {
            m_Staged.erase(descriptor);
        }
    }

    void VulkanDescriptorHeapBackend::UploadSlots(const u32 firstSlot, const u64* descriptors, const u32 count)
    {
        auto& heap = VulkanResourceHeap::Get();
        // Probed ONCE, not per slot (#803). TryGetRecordingVulkanAPI does a
        // dynamic_cast on RenderCommand::GetRendererAPI(), and neither the
        // installed API object nor its recording state can change inside this
        // loop — a bindless upload of N descriptors was paying N runtime type
        // checks against the same object.
        //
        // Hoisted to the CALL SITE rather than cached in the helper: a cached
        // VulkanRendererAPI* would have to be invalidated on
        // RecreateForSelectedBackend, which is amendment (39)'s
        // construction-order hazard. A loop-local answer has no lifetime at
        // all, so it cannot go stale.
        auto* recordingApi = VulkanUpload::TryGetRecordingVulkanAPI();
        for (u32 i = 0; i < count; ++i)
        {
            const u32 slot = firstSlot + i;
            const u64 token = descriptors[i];

            const auto it = m_Staged.find(token);
            if (it == m_Staged.end())
            {
                // 0, a stale token, or a null token whose image was never
                // created — the slot must still be DEFINED: the 2D black null.
                (void)WriteNullAt(slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
                continue;
            }

            const Staged& staged = it->second;
            // The heap route's half of the bind-time layout seam (#691
            // the slot route's BindTexture/BindImageTexture always
            // had it — the debt the issue text recorded as "the heap
            // path's own mid-pass visibility seam, amendment (63) covers the
            // slot path only"). A descriptor written here BAKES its layout,
            // so an image that is not in that layout when the draw samples it
            // fails VUID-vkCmdDraw-None-09600. No-op outside a recording,
            // where load-time writes get their layout from first use.
            //
            // NOT a fix for the per-resize validation error (#800): that one
            // survives this seam, so its failing sample comes from another
            // path — most likely the ImGui viewport binding. Closing this gap
            // is worth doing on its own terms; do not read it as that fix.
            if (recordingApi != nullptr)
            {
                recordingApi->EnsureImageLayoutForDescriptor(staged.Image, staged.Layout,
                                                             staged.ViewInfo.subresourceRange);
            }
            const bool ok = staged.Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                ? heap.WriteStorageImage(slot, staged.ViewInfo, staged.Layout)
                                : heap.WriteSampledImage(slot, staged.ViewInfo, staged.Layout);
            if (!ok)
            {
                (void)WriteNullAt(slot, staged.Type);
            }
        }
    }

    void VulkanDescriptorHeapBackend::BindHeap()
    {
        // vkCmdBindResourceHeapEXT is COMMAND-BUFFER state; the draw path
        // binds once per recording (VulkanRendererAPI::PrepareDraw). The
        // engine heap's per-frame BindHeap() has nothing to do here — on GL
        // it re-establishes a global SSBO binding, which is exactly the
        // global state Vulkan does not have.
    }

    auto VulkanDescriptorHeapBackend::NullDescriptor(RHI::ViewUsage usage, RHI::NullSamplerKind kind) const -> u64
    {
        // Lazily materialise the backing null image — mutable machinery
        // behind a semantically-const query (the interface is const because
        // GL's nulls are baked at Initialize; ours bake on first ask).
        auto& self = const_cast<VulkanDescriptorHeapBackend&>(*this);
        if (usage == RHI::ViewUsage::Storage)
        {
            return self.NullStorageTokenFor(VK_FORMAT_R32_SFLOAT);
        }
        switch (kind)
        {
            case RHI::NullSamplerKind::Cube:
                return self.EnsureNullImage(kNullSampledCubeToken, VK_IMAGE_VIEW_TYPE_CUBE,
                                            VK_FORMAT_R8G8B8A8_UNORM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                           ? kNullSampledCubeToken
                           : 0u;
            case RHI::NullSamplerKind::Texture2DArray:
            case RHI::NullSamplerKind::Texture2DArrayShadow:
                // One 2-layer array image serves both — "shadow" is sampler
                // state, not an image dimension.
                return self.EnsureNullImage(kNullSampledArrayToken, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                            VK_FORMAT_R8G8B8A8_UNORM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                           ? kNullSampledArrayToken
                           : 0u;
            case RHI::NullSamplerKind::Texture2D:
            default:
                return self.EnsureNullImage(kNullSampled2DToken, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                           ? kNullSampled2DToken
                           : 0u;
        }
    }

    auto VulkanDescriptorHeapBackend::NullStorageDescriptor(RHI::Format format) const -> u64
    {
        // Per-FORMAT null storage images: a shader layout qualifier
        // disagreeing with the view format is undefined, so each declared
        // format gets a matching 1x1 image (the GL backend's per-format rule,
        // same reasoning).
        VkFormat vkFormat = VulkanBarrierLowering::ToVkFormat(format);
        if (vkFormat == VK_FORMAT_UNDEFINED)
        {
            vkFormat = VK_FORMAT_R32_SFLOAT;
        }
        return const_cast<VulkanDescriptorHeapBackend&>(*this).NullStorageTokenFor(vkFormat);
    }

    u64 VulkanDescriptorHeapBackend::NullStorageTokenFor(const VkFormat format)
    {
        if (const auto it = m_NullStorageTokenByFormat.find(static_cast<u32>(format));
            it != m_NullStorageTokenByFormat.end())
        {
            return it->second;
        }
        // Ceiling is the first RESERVED sampled token (62/63 are the
        // cube-array and 3D null-sampled shapes), not the dynamic range —
        // minting past it would alias a storage null over a reserved one.
        if (m_NextNullStorageToken >= kNullSampledCubeArrayToken)
        {
            OLO_CORE_ERROR("VulkanDescriptorHeapBackend: null-storage token space exhausted");
            return 0;
        }
        const u64 token = m_NextNullStorageToken;
        if (!EnsureNullImage(token, VK_IMAGE_VIEW_TYPE_2D, format, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE))
        {
            return 0;
        }
        ++m_NextNullStorageToken;
        m_NullStorageTokenByFormat[static_cast<u32>(format)] = token;
        return token;
    }

    bool VulkanDescriptorHeapBackend::EnsureNullImage(const u64 token, const VkImageViewType viewType,
                                                      const VkFormat format, const VkDescriptorType type)
    {
        if (m_Staged.contains(token))
        {
            return true;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }

        const bool cube = viewType == VK_IMAGE_VIEW_TYPE_CUBE || viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        const bool array = viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        const bool volume = viewType == VK_IMAGE_VIEW_TYPE_3D;
        const u32 layers = cube ? 6u : (array ? 2u : 1u);
        const bool storage = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        imageInfo.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { 1u, 1u, 1u };
        imageInfo.mipLevels = 1u;
        imageInfo.arrayLayers = layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT) |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        NullImage null;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &null.Image, &null.Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanDescriptorHeapBackend: null image creation failed (format {})",
                           static_cast<int>(format));
            return false;
        }

        // Zero-fill and settle into the descriptor's baked layout, once.
        const VkImageLayout finalLayout =
            storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const bool cleared = VulkanOneShot::Submit(
            "VulkanDescriptorHeapBackend::EnsureNullImage",
            [&](VkCommandBuffer cmd)
            {
                VkImageMemoryBarrier2 toDst{};
                toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                toDst.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.image = null.Image;
                toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, layers };
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1u;
                dep.pImageMemoryBarriers = &toDst;
                vkCmdPipelineBarrier2(cmd, &dep);

                const VkClearColorValue zero{};
                const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, layers };
                vkCmdClearColorImage(cmd, null.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1u, &range);

                VkImageMemoryBarrier2 toFinal = toDst;
                toFinal.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                toFinal.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toFinal.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                toFinal.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toFinal.newLayout = finalLayout;
                dep.pImageMemoryBarriers = &toFinal;
                vkCmdPipelineBarrier2(cmd, &dep);
            });
        if (!cleared)
        {
            vmaDestroyImage(device->GetAllocator(), null.Image, null.Allocation);
            return false;
        }

        Staged staged;
        staged.Image = null.Image;
        staged.Type = type;
        staged.Layout = finalLayout;
        auto& viewInfo = staged.ViewInfo;
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = null.Image;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, layers };

        m_Staged[token] = staged;
        m_NullImages[token] = null;
        return true;
    }

    bool VulkanDescriptorHeapBackend::WriteNullAt(const u32 slot, const VkDescriptorType type)
    {
        const u64 token = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                              ? NullStorageTokenFor(VK_FORMAT_R32_SFLOAT)
                              : (EnsureNullImage(kNullSampled2DToken, VK_IMAGE_VIEW_TYPE_2D,
                                                 VK_FORMAT_R8G8B8A8_UNORM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                                     ? kNullSampled2DToken
                                     : 0u);
        const auto it = m_Staged.find(token);
        if (it == m_Staged.end())
        {
            return false;
        }
        auto& heap = VulkanResourceHeap::Get();
        const Staged& staged = it->second;
        return staged.Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                   ? heap.WriteStorageImage(slot, staged.ViewInfo, staged.Layout)
                   : heap.WriteSampledImage(slot, staged.ViewInfo, staged.Layout);
    }

    u32 VulkanDescriptorHeapBackend::GetNullSampledHeapSlot(const VkImageViewType viewType)
    {
        if (const auto it = m_NullSampledSlots.find(static_cast<u32>(viewType)); it != m_NullSampledSlots.end())
        {
            return it->second;
        }

        u64 token = kNullSampled2DToken;
        switch (viewType)
        {
            case VK_IMAGE_VIEW_TYPE_CUBE:
                token = kNullSampledCubeToken;
                break;
            case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
                token = kNullSampledArrayToken;
                break;
            case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
                token = kNullSampledCubeArrayToken;
                break;
            case VK_IMAGE_VIEW_TYPE_3D:
                token = kNullSampled3DToken;
                break;
            default:
                break;
        }
        if (!EnsureNullImage(token, viewType, VK_FORMAT_R8G8B8A8_UNORM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE))
        {
            return VulkanResourceHeap::InvalidSlot;
        }
        const Staged& staged = m_Staged.at(token);
        const u32 slot = VulkanDescriptorSlotCache::Get().AcquireSlot(staged.Image, staged.ViewInfo,
                                                                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, staged.Layout);
        if (slot != VulkanResourceHeap::InvalidSlot)
        {
            m_NullSampledSlots.emplace(static_cast<u32>(viewType), slot);
        }
        return slot;
    }

    void VulkanDescriptorHeapBackend::ReleaseDeviceObjects()
    {
        for (auto& [token, null] : m_NullImages)
        {
            VulkanDeferredReclaim::Get().Enqueue(null.Image, null.Allocation);
            m_Staged.erase(token);
        }
        m_NullImages.clear();
        m_NullStorageTokenByFormat.clear();
        // Slot indices die with the heap (the caller resets the slot cache in
        // the same cascade); the next GetNullSampledHeapSlot re-creates both
        // the image and the slot against the new heap.
        m_NullSampledSlots.clear();
        m_NextNullStorageToken = kFirstNullStorageToken;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
