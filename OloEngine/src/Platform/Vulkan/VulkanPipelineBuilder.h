#pragma once

// VulkanPipelineBuilder — thin PSOs + the root-data binding-mapping ABI.
// Issue #691 Phase 6, ADR 0011 §4 (root data) + §5 (thin PSO).
//
// THE ABI, in one place (the single source of truth both the mapping array
// and the draw-time writer consume — they structurally cannot disagree, the
// amendment (25) property one level up):
//
//   - Push data is EXACTLY 8 BYTES: the VkDeviceAddress of the draw's root
//     struct, pushed at offset 0 via vkCmdPushDataEXT. Never the payload —
//     §4's 128/256-byte push-constant warning, honoured by construction.
//   - The root struct is built per shader from its REFLECTED bindings, in
//     (set, binding) order: first one u64 GPU address per buffer block
//     (UBO/SSBO alike — INDIRECT_ADDRESS mapping reads the block's address
//     from root + Field.Offset), then one u32 heap slot index per sampled
//     texture (HEAP_WITH_INDIRECT_INDEX reads the index from root +
//     Field.Offset and scales by the heap's descriptor stride).
//   - Samplers are EMBEDDED per pipeline for now (pEmbeddedSampler): the
//     pilot passes sample with one known sampler state, and the sampler-heap
//     half (§1.2a's deduplicated second heap) composes in when the engine's
//     RHI::DescriptorHeap starts running on this backend (Phase 7/8).
//
// THIN PSO (§5): no VkPipelineVertexInputStateCreateInfo contents (vertex
// pulling — geometry arrives as a root-struct buffer address), dynamic
// everything the 1.3 core + EDS3 floor allows. What remains BAKED (and is
// therefore the cache key): shader identity, attachment formats + sample
// count, and — only when EDS3's blend states are unavailable — the recorded
// blend state. Dynamic rendering only (VkPipelineRenderingCreateInfo); no
// VkRenderPass objects exist on this backend.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanShader.h"

#include <volk.h>

#include <array>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct VulkanRootDataLayout
    {
        struct Field
        {
            VulkanShaderBinding Binding;
            u32 Offset = 0; ///< Byte offset of this field inside the root struct.
        };

        std::vector<Field> Fields;
        u32 SizeBytes = 0;

        [[nodiscard]] static VulkanRootDataLayout Build(const std::vector<VulkanShaderBinding>& bindings);
        [[nodiscard]] const Field* Find(u32 set, u32 binding) const;
    };

    // VulkanRenderTargetDesc moved to VulkanRendererAPI.h (#691 Phase 7):
    // the draw path's rendering scope holds one, and this header includes
    // that one — declaring it here would cycle.

    class VulkanPipelineBuilder
    {
      public:
        [[nodiscard]] static VulkanPipelineBuilder& Get();

        // Cached lookup or creation of the thin graphics pipeline for
        // (shader, targets[, baked blend]). `embeddedSampler` is baked into
        // every sampled-texture mapping (null → a linear/clamp-to-edge
        // default, the post-process read shape).
        [[nodiscard]] VkPipeline GetOrCreateGraphics(VulkanShader& shader, const VulkanRootDataLayout& layout,
                                                     const VulkanRecordedPipelineState& state,
                                                     const VulkanRenderTargetDesc& targets,
                                                     const VkSamplerCreateInfo* embeddedSampler = nullptr);

        // The compute sibling (#691 Phase 7): same mapping chain, same
        // VK_NULL_HANDLE layout + DESCRIPTOR_HEAP flag, no fixed-function
        // state at all. Keyed on (shaderKey, layout, sampler) — target and
        // blend fields stay zero, and shader keys are process-unique so a
        // compute key can never collide with a graphics one. `shaderKey` and
        // `module` are passed directly so this header needs no
        // VulkanComputeShader dependency; InvalidateShader(shaderKey) covers
        // compute pipelines through the same reverse index.
        [[nodiscard]] VkPipeline GetOrCreateCompute(u64 shaderKey, VkShaderModule module,
                                                    const VulkanRootDataLayout& layout,
                                                    const VkSamplerCreateInfo* embeddedSampler = nullptr);

        // Issue every vkCmdSet* for the states the pipelines above declare
        // dynamic, from the recorded state. Must run after vkCmdBindPipeline,
        // before the draw.
        static void FlushDynamicState(VkCommandBuffer cmd, const VulkanRecordedPipelineState& state,
                                      const VulkanRenderTargetDesc& targets);

        // §3(d)'s shader→pipeline reverse index lives HERE, because the
        // builder's key→pipeline map must be invalidated in the same act —
        // two owners meant a double-destroy AND a dangling map entry on the
        // first device run. Enqueues every dependent pipeline for deferred
        // destruction (an in-flight command buffer may still reference them),
        // erases them from the cache map, returns how many. The next
        // GetOrCreateGraphics recreates lazily against VulkanPipelineCache's
        // disk blob.
        sizet InvalidateShader(u64 shaderKey);

        // Enqueue every cached pipeline for deferred destruction and clear
        // the maps (device teardown).
        void ReleaseAll();

        [[nodiscard]] sizet GetCachedPipelineCount() const
        {
            return m_Pipelines.size();
        }

      private:
        VulkanPipelineBuilder() = default;

        // The §4 mapping array for one root layout — shared verbatim by the
        // graphics and compute paths so the two cannot drift. `sampler` must
        // outlive pipeline creation (pEmbeddedSampler points at it).
        [[nodiscard]] static std::vector<VkDescriptorSetAndBindingMappingEXT>
        BuildBindingMappings(const VulkanRootDataLayout& layout, const VkSamplerCreateInfo& sampler);

        struct Key
        {
            u64 ShaderKey = 0;
            std::array<VkFormat, 8> ColorFormats{};
            VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
            u32 ColorCount = 0;
            u32 Samples = 1;
            u64 BakedBlendHash = 0; ///< 0 when blend is dynamic (EDS3 present).
            u64 SamplerHash = 0;
            u64 LayoutHash = 0; ///< Root-data layout — drives the baked binding mappings.

            bool operator==(const Key&) const = default;
        };
        struct KeyHash
        {
            [[nodiscard]] sizet operator()(const Key& key) const;
        };

        std::unordered_map<Key, VkPipeline, KeyHash> m_Pipelines;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
