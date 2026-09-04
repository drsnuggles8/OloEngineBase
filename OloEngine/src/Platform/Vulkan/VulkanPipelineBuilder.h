#pragma once

// VulkanPipelineBuilder — thin PSOs + the root-data binding-mapping ABI.
// Issue #691, ADR 0011 §4 (root data) + §5 (thin PSO).
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
//   - Samplers come from the SAMPLER HEAP (#691, §1.2a's
//     deduplicated second heap): each combined-image-sampler field carries a
//     second u32 (Offset + kSamplerIndexOffset) that indexes
//     VulkanSamplerHeap; BindTexture stages it from the image's own recorded
//     state (or an explicit desc). Nothing is embedded per pipeline, so
//     sampler state is not a PSO axis.
//
// THIN PSO (§5): no VkPipelineVertexInputStateCreateInfo contents (vertex
// pulling — geometry arrives as a root-struct buffer address), dynamic
// everything the 1.3 core + EDS3 floor allows. What remains BAKED (and is
// therefore the cache key): shader identity, attachment formats + sample
// count, and — only when EDS3's blend states are unavailable — the recorded
// blend state. Dynamic rendering only (VkPipelineRenderingCreateInfo); no
// VkRenderPass objects exist on this backend.
//
// THREAD-SAFETY (issue #806, ADR 0011 amendment (92) rule 8): GetOrCreate*
// run once per draw / dispatch, from any recording thread. The map lookup
// AND the creation on a miss happen under m_Mutex — a miss is rare after the
// first frame, and two threads building the same pipeline would cost more
// than the wait. InvalidateShader and ReleaseAll take the same lock;
// FlushDynamicState is static and touches no builder state. Everything a
// creation reaches — VulkanPipelineCache::Handle, the sampler heap's
// EnsureCreated (its own lock, always taken INSIDE this one), the resource
// heap's stride getters — is therefore serialised by this mutex, and
// VulkanPipelineCache relies on that instead of locking itself.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanShader.h"

#include <volk.h>

#include <array>
#include <mutex>
#include <shared_mutex>
#include <string>
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

        // A CombinedImageSampler field is TWO u32s (#691): the image
        // heap index at Offset and the sampler heap index at
        // Offset + kSamplerIndexOffset — the mapping's samplerAddressOffset
        // and AssembleAndPushRootData both add this same constant.
        static constexpr u32 kSamplerIndexOffset = 4u;

        std::vector<Field> Fields;
        u32 SizeBytes = 0;

        [[nodiscard]] static VulkanRootDataLayout Build(const std::vector<VulkanShaderBinding>& bindings);
        [[nodiscard]] const Field* Find(u32 set, u32 binding) const;
    };

    // VulkanRenderTargetDesc moved to VulkanRendererAPI.h (#691):
    // the draw path's rendering scope holds one, and this header includes
    // that one — declaring it here would cycle.

    class VulkanPipelineBuilder
    {
      public:
        [[nodiscard]] static VulkanPipelineBuilder& Get();

        // Cached lookup or creation of the thin graphics pipeline for
        // (shader, targets[, baked blend]). Sampler state is NOT a pipeline
        // axis (#691): the mappings source the sampler half from the
        // SAMPLER heap, indexed per draw from root data — the embedded
        // per-pipeline sampler retired with the sampler heap.
        //
        // A shader whose module map carries VK_SHADER_STAGE_MESH_BIT_EXT
        // builds a MESH pipeline (issue #813): stages [task?, mesh, fragment],
        // no vertex-input / input-assembly state, and no topology /
        // primitive-restart dynamic states (declaring either against a mesh
        // pipeline is a validation error). Mesh-ness needs no key axis of its
        // own — ShaderKey identifies the shader, and a reload that changes
        // its stage set runs through InvalidateShader first.
        [[nodiscard]] VkPipeline GetOrCreateGraphics(VulkanShader& shader, const VulkanRootDataLayout& layout,
                                                     const VulkanRecordedPipelineState& state,
                                                     const VulkanRenderTargetDesc& targets);

        // The compute sibling (#691): same mapping chain, same
        // VK_NULL_HANDLE layout + DESCRIPTOR_HEAP flag, no fixed-function
        // state at all. Keyed on (shaderKey, layout) — target and
        // blend fields stay zero, and shader keys are process-unique so a
        // compute key can never collide with a graphics one. `shaderKey` and
        // `module` are passed directly so this header needs no
        // VulkanComputeShader dependency; InvalidateShader(shaderKey) covers
        // compute pipelines through the same reverse index.
        [[nodiscard]] VkPipeline GetOrCreateCompute(u64 shaderKey, VkShaderModule module,
                                                    const VulkanRootDataLayout& layout);

        // Issue every vkCmdSet* for the states the pipelines above declare
        // dynamic, from the recorded state. Must run after vkCmdBindPipeline,
        // before the draw. The pipeline KIND is derived from the currently
        // bound VulkanShader (a mesh-stage shader's pipeline declares neither
        // primitive topology nor primitive-restart dynamic, and setting an
        // undeclared dynamic state is invalid — issue #813); deriving it here
        // rather than taking a flag means no call site can pass the wrong
        // kind for the pipeline that was just bound from that same shader.
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

        // The last creation failure, kept so a caller can report the REASON
        // rather than a bare null handle (issue #1029). GetOrCreate* return
        // VK_NULL_HANDLE and log on a vkCreate*Pipelines failure; the log is
        // not reachable from a test assertion, and "the handle was null" does
        // not say which shader or which VkResult. Recorded under the same
        // lock the creation already holds, so it is the failure of the call
        // that just returned null on this thread as long as no other thread
        // failed in between — a diagnostic, never a control-flow input.
        struct CreationFailure
        {
            bool Valid = false; ///< False until a creation has failed.
            std::string ShaderName;
            VkResult Result = VK_SUCCESS;
        };

        [[nodiscard]] CreationFailure GetLastCreationFailure() const
        {
            std::shared_lock lock(m_Mutex);
            return m_LastCreationFailure;
        }

        void ClearLastCreationFailure()
        {
            std::lock_guard<std::shared_mutex> lock(m_Mutex);
            m_LastCreationFailure = {};
        }

        [[nodiscard]] sizet GetCachedPipelineCount() const
        {
            std::shared_lock lock(m_Mutex);
            return m_Pipelines.size();
        }

      private:
        VulkanPipelineBuilder() = default;

        // The §4 mapping array for one root layout — shared verbatim by the
        // graphics and compute paths so the two cannot drift. Samplers come
        // from the sampler heap (see GetOrCreateGraphics), never embedded.
        // Called under m_Mutex by both.
        [[nodiscard]] static std::vector<VkDescriptorSetAndBindingMappingEXT>
        BuildBindingMappings(const VulkanRootDataLayout& layout);

        struct Key
        {
            u64 ShaderKey = 0;
            std::array<VkFormat, 8> ColorFormats{};
            VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
            u32 ColorCount = 0;
            u32 Samples = 1;
            u64 BakedBlendHash = 0; ///< 0 when blend is dynamic (EDS3 present).
            u64 LayoutHash = 0;     ///< Root-data layout — drives the baked binding mappings.
            /// Baked patch size for a tessellated pipeline; 0 when the shader
            /// has no TCS/TES stage. patchControlPoints is only dynamic under
            /// extendedDynamicState2PatchControlPoints, which is NOT on the
            /// ADR 0010 floor — so it is a PSO axis here (#691, A10).
            u32 PatchControlPoints = 0;

            bool operator==(const Key&) const = default;
        };
        struct KeyHash
        {
            [[nodiscard]] sizet operator()(const Key& key) const;
        };

        // Readers (the per-draw hit path) share; a miss upgrades to exclusive
        // for the creation, re-checking first. The key is built OUTSIDE the
        // lock — hashing a root layout per draw under a mutex was the first
        // contention the #806 measurement found.
        mutable std::shared_mutex m_Mutex;
        std::unordered_map<Key, VkPipeline, KeyHash> m_Pipelines;
        CreationFailure m_LastCreationFailure;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
