#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanPipelinePrecache.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanSamplerHeap.h"
#include "Platform/Vulkan/VulkanShaderReflection.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#include "Platform/Vulkan/VulkanWarnOnce.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <tuple>

namespace OloEngine
{
    namespace
    {
        // An integer colour format carries no COLOR_ATTACHMENT_BLEND format
        // feature on any implementation (blending is defined on floating-point
        // and normalized formats only), so blendEnable MUST be false against
        // one — VUID-vkCmdDraw-blendEnable-04727. Enumerated rather than
        // queried because the set is closed and this runs per draw: it is the
        // formats VulkanFramebuffer can actually create for an integer
        // attachment (RED_INTEGER / RG*_INTEGER family).
        [[nodiscard]] bool IsIntegerFormat(const VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UINT:
                case VK_FORMAT_R8_SINT:
                case VK_FORMAT_R16_UINT:
                case VK_FORMAT_R16_SINT:
                case VK_FORMAT_R32_UINT:
                case VK_FORMAT_R32_SINT:
                case VK_FORMAT_R8G8_UINT:
                case VK_FORMAT_R8G8_SINT:
                case VK_FORMAT_R16G16_UINT:
                case VK_FORMAT_R16G16_SINT:
                case VK_FORMAT_R32G32_UINT:
                case VK_FORMAT_R32G32_SINT:
                case VK_FORMAT_R8G8B8A8_UINT:
                case VK_FORMAT_R8G8B8A8_SINT:
                case VK_FORMAT_R16G16B16A16_UINT:
                case VK_FORMAT_R16G16B16A16_SINT:
                case VK_FORMAT_R32G32B32A32_UINT:
                case VK_FORMAT_R32G32B32A32_SINT:
                    return true;
                default:
                    return false;
            }
        }

        // Does attachment `i` blend? ONE function, called by both the baked
        // (GetOrCreateGraphics) and the dynamic (FlushDynamicState) route, so
        // the two lowerings cannot drift apart — they were previously two
        // copies of the same expression, kept in step only by a comment.
        //
        // A per-attachment opinion outranks the global `Blend` in BOTH
        // directions; only Inherit falls through to it. The rule, and why each
        // arm is load-bearing, is at the AttachmentBlend declaration in
        // VulkanRendererAPI.h (issues #823 and #896).
        [[nodiscard]] bool ResolveBlendEnable(const VulkanRecordedPipelineState& state, const u32 attachment)
        {
            switch (state.AttachmentBlend[attachment])
            {
                case VulkanRecordedPipelineState::AttachmentBlendOpinion::ForceOn:
                    return true;
                case VulkanRecordedPipelineState::AttachmentBlendOpinion::ForceOff:
                    return false;
                case VulkanRecordedPipelineState::AttachmentBlendOpinion::Inherit:
                    break;
            }
            return state.Blend;
        }

        // --- RHI → Vk state-enum lowering ------------------------------------
        [[nodiscard]] VkCompareOp ToVk(RHI::CompareOp op)
        {
            switch (op)
            {
                case RHI::CompareOp::Never:
                    return VK_COMPARE_OP_NEVER;
                case RHI::CompareOp::Less:
                    return VK_COMPARE_OP_LESS;
                case RHI::CompareOp::Equal:
                    return VK_COMPARE_OP_EQUAL;
                case RHI::CompareOp::LessOrEqual:
                    return VK_COMPARE_OP_LESS_OR_EQUAL;
                case RHI::CompareOp::Greater:
                    return VK_COMPARE_OP_GREATER;
                case RHI::CompareOp::NotEqual:
                    return VK_COMPARE_OP_NOT_EQUAL;
                case RHI::CompareOp::GreaterOrEqual:
                    return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case RHI::CompareOp::Always:
                    return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_ALWAYS;
        }

        [[nodiscard]] VkBlendFactor ToVk(RHI::BlendFactor factor)
        {
            switch (factor)
            {
                case RHI::BlendFactor::Zero:
                    return VK_BLEND_FACTOR_ZERO;
                case RHI::BlendFactor::One:
                    return VK_BLEND_FACTOR_ONE;
                case RHI::BlendFactor::SrcColor:
                    return VK_BLEND_FACTOR_SRC_COLOR;
                case RHI::BlendFactor::OneMinusSrcColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case RHI::BlendFactor::DstColor:
                    return VK_BLEND_FACTOR_DST_COLOR;
                case RHI::BlendFactor::OneMinusDstColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case RHI::BlendFactor::SrcAlpha:
                    return VK_BLEND_FACTOR_SRC_ALPHA;
                case RHI::BlendFactor::OneMinusSrcAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case RHI::BlendFactor::DstAlpha:
                    return VK_BLEND_FACTOR_DST_ALPHA;
                case RHI::BlendFactor::OneMinusDstAlpha:
                    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case RHI::BlendFactor::ConstantColor:
                    return VK_BLEND_FACTOR_CONSTANT_COLOR;
                case RHI::BlendFactor::OneMinusConstantColor:
                    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
                case RHI::BlendFactor::SrcAlphaSaturate:
                    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            }
            return VK_BLEND_FACTOR_ONE;
        }

        [[nodiscard]] VkBlendOp ToVk(RHI::BlendOp op)
        {
            switch (op)
            {
                case RHI::BlendOp::Add:
                    return VK_BLEND_OP_ADD;
                case RHI::BlendOp::Subtract:
                    return VK_BLEND_OP_SUBTRACT;
                case RHI::BlendOp::ReverseSubtract:
                    return VK_BLEND_OP_REVERSE_SUBTRACT;
                case RHI::BlendOp::Min:
                    return VK_BLEND_OP_MIN;
                case RHI::BlendOp::Max:
                    return VK_BLEND_OP_MAX;
            }
            return VK_BLEND_OP_ADD;
        }

        [[nodiscard]] VkStencilOp ToVk(RHI::StencilOp op)
        {
            switch (op)
            {
                case RHI::StencilOp::Keep:
                    return VK_STENCIL_OP_KEEP;
                case RHI::StencilOp::Zero:
                    return VK_STENCIL_OP_ZERO;
                case RHI::StencilOp::Replace:
                    return VK_STENCIL_OP_REPLACE;
                case RHI::StencilOp::IncrementClamp:
                    return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                case RHI::StencilOp::DecrementClamp:
                    return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                case RHI::StencilOp::Invert:
                    return VK_STENCIL_OP_INVERT;
                case RHI::StencilOp::IncrementWrap:
                    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                case RHI::StencilOp::DecrementWrap:
                    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            }
            return VK_STENCIL_OP_KEEP;
        }

        [[nodiscard]] VkCullModeFlags ToVk(RHI::CullMode mode, bool cullingEnabled)
        {
            if (!cullingEnabled)
            {
                return VK_CULL_MODE_NONE;
            }
            switch (mode)
            {
                case RHI::CullMode::None:
                    return VK_CULL_MODE_NONE;
                case RHI::CullMode::Front:
                    return VK_CULL_MODE_FRONT_BIT;
                case RHI::CullMode::Back:
                    return VK_CULL_MODE_BACK_BIT;
                case RHI::CullMode::FrontAndBack:
                    return VK_CULL_MODE_FRONT_AND_BACK;
            }
            return VK_CULL_MODE_NONE;
        }

        [[nodiscard]] u64 CombinePipelineHash(u64 seed, u64 value)
        {
            return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        }

        // [398] The root layout drives every binding mapping baked into the
        // pipeline; it is derived from the shader's reflection today, but the
        // key must stand on its own inputs, not on that invariant.
        // The field's binding with its array length clamped to what Build
        // actually reserved room for. The MAPPING and the root-data WRITER both
        // read ArrayCount off the stored field, so storing the raw reflected
        // value while laying out the clamped one would have them write past the
        // field they were given (#1078).
        [[nodiscard]] VulkanShaderBinding ClampedArrayCount(const VulkanShaderBinding& binding, u32 count)
        {
            VulkanShaderBinding clamped = binding;
            clamped.ArrayCount = count;
            return clamped;
        }

        [[nodiscard]] u64 HashLayout(const VulkanRootDataLayout& layout)
        {
            u64 hash = layout.SizeBytes;
            for (const auto& field : layout.Fields)
            {
                hash = CombinePipelineHash(hash, field.Offset);
                hash = CombinePipelineHash(hash, field.SamplerOffset);
                hash = CombinePipelineHash(hash, field.Binding.Set);
                hash = CombinePipelineHash(hash, field.Binding.Binding);
                hash = CombinePipelineHash(hash, static_cast<u64>(field.Binding.BindingKind));
                // #1078: the array length selects the MAPPING SOURCE, so two
                // layouts that differ only here produce different pipelines
                // and must not share a cache entry.
                hash = CombinePipelineHash(hash, field.Binding.ArrayCount);
            }
            return hash == 0 ? 1 : hash;
        }

        // The first misses of a session are logged with their shader and key
        // (the "which pipeline hitched" answer); after that they are only
        // counted, because a cold start can miss hundreds in its first frames.
        constexpr u32 kMaxLoggedMisses = 64;

        // The non-shader half of a key, as the precache manifest stores it.
        [[nodiscard]] PipelineSignature ToSignature(const VulkanPipelineBuilder::Key& key)
        {
            PipelineSignature signature;
            signature.ColorCount = key.ColorCount;
            for (u32 i = 0; i < key.ColorCount && i < PipelineSignature::kMaxColorAttachments; ++i)
            {
                signature.ColorFormats[i] = static_cast<u32>(key.ColorFormats[i]);
            }
            signature.DepthFormat = static_cast<u32>(key.DepthFormat);
            signature.Samples = key.Samples;
            signature.PatchControlPoints = key.PatchControlPoints;
            return signature;
        }
    } // namespace

    // =========================================================================
    // VulkanRootDataLayout
    // =========================================================================

    VulkanRootDataLayout VulkanRootDataLayout::Build(std::span<const VulkanShaderBinding> bindings)
    {
        VulkanRootDataLayout layout;

        // Deterministic field order: (set, binding), buffer blocks first.
        TArray<VulkanShaderBinding> sorted(bindings.data(), static_cast<i32>(bindings.size()));
        std::ranges::sort(sorted, [](const VulkanShaderBinding& a, const VulkanShaderBinding& b)
                          {
            const bool aBuffer = a.BindingKind == VulkanShaderBinding::Kind::UniformBuffer ||
                                 a.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            const bool bBuffer = b.BindingKind == VulkanShaderBinding::Kind::UniformBuffer ||
                                 b.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            if (aBuffer != bBuffer)
            {
                return aBuffer;
            }
            return std::tie(a.Set, a.Binding) < std::tie(b.Set, b.Binding); });

        u32 offset = 0;
        for (const auto& binding : sorted)
        {
            const bool isBuffer = binding.BindingKind == VulkanShaderBinding::Kind::UniformBuffer ||
                                  binding.BindingKind == VulkanShaderBinding::Kind::StorageBuffer;
            if (isBuffer)
            {
                offset = (offset + 7u) & ~7u; // u64 field
                layout.Fields.push_back({ .Binding = binding, .Offset = offset });
                offset += 8;
            }
            else if (binding.BindingKind == VulkanShaderBinding::Kind::CombinedImageSampler)
            {
                // #691: two u32s — the image heap index at Offset and
                // the SAMPLER heap index at SamplerOffset. The mapping's
                // samplerAddressOffset and the root-data writer both derive
                // from this one layout, so they cannot disagree.
                //
                // #1078: an ARRAY binding needs one index PER ELEMENT, because
                // the array mapping reads element i's index from the root
                // struct instead of deriving it from a heap stride. Images
                // first, then samplers — two contiguous u32 runs.
                // Clamped, not trusted. Reflection already refuses a length
                // past kMaxBindingArrayCount, but this is the arithmetic that
                // would wrap: `offset += 4 * count` on an unchecked u32 leaves
                // SizeBytes small while AssembleRootData still writes 4*count
                // bytes into the scratch buffer sized from it.
                const u32 count = std::clamp(binding.ArrayCount, 1u, VulkanReflection::kMaxBindingArrayCount);
                offset = (offset + 3u) & ~3u;
                const u32 imageOffset = offset;
                offset += 4u * count;
                layout.Fields.push_back(
                    { .Binding = ClampedArrayCount(binding, count), .Offset = imageOffset, .SamplerOffset = offset });
                offset += 4u * count;
            }
            else
            {
                const u32 count = std::clamp(binding.ArrayCount, 1u, VulkanReflection::kMaxBindingArrayCount);
                offset = (offset + 3u) & ~3u; // u32 heap index field(s)
                layout.Fields.push_back({ .Binding = ClampedArrayCount(binding, count), .Offset = offset });
                offset += 4u * count;
            }
        }
        layout.SizeBytes = (offset + 15u) & ~15u; // 16-aligned total, arena-friendly
        return layout;
    }

    const VulkanRootDataLayout::Field* VulkanRootDataLayout::Find(u32 set, u32 binding) const
    {
        for (const auto& field : Fields)
        {
            if (field.Binding.Set == set && field.Binding.Binding == binding)
            {
                return &field;
            }
        }
        return nullptr;
    }

    // =========================================================================
    // VulkanPipelineBuilder
    // =========================================================================

    VulkanPipelineBuilder& VulkanPipelineBuilder::Get()
    {
        static auto* s_Instance = new VulkanPipelineBuilder(); // deliberately leaked
        return *s_Instance;
    }

    sizet VulkanPipelineBuilder::KeyHash::operator()(const Key& key) const
    {
        u64 hash = key.ShaderKey;
        for (u32 i = 0; i < key.ColorCount; ++i)
        {
            hash = CombinePipelineHash(hash, static_cast<u64>(key.ColorFormats[i]));
        }
        hash = CombinePipelineHash(hash, static_cast<u64>(key.DepthFormat));
        hash = CombinePipelineHash(hash, key.ColorCount);
        hash = CombinePipelineHash(hash, key.Samples);
        hash = CombinePipelineHash(hash, key.BakedBlendHash);
        hash = CombinePipelineHash(hash, key.LayoutHash);
        hash = CombinePipelineHash(hash, key.PatchControlPoints);
        return static_cast<sizet>(hash);
    }

    VulkanPipelineBuilder::Key VulkanPipelineBuilder::DeriveGraphicsKey(const u64 shaderKey,
                                                                        const VulkanRootDataLayout& layout,
                                                                        const bool tessellated, const bool dynamicBlend,
                                                                        const VulkanRecordedPipelineState& state,
                                                                        const VulkanRenderTargetDesc& targets)
    {
        const u32 colorCount = std::min(targets.ColorCount, 8u);
        Key key;
        key.ShaderKey = shaderKey;
        // Only the attachments that exist: a stale format past ColorCount in
        // the recorded desc must not split two otherwise identical keys (the
        // hash already ignores it; equality has to agree with the hash).
        for (u32 i = 0; i < colorCount; ++i)
        {
            key.ColorFormats[i] = targets.ColorFormats[i];
        }
        key.DepthFormat = targets.DepthFormat;
        key.ColorCount = colorCount;
        key.Samples = targets.Samples;
        key.LayoutHash = HashLayout(layout);
        // A10 (#691): a shader carrying a tessellation-control stage is
        // a PATCH pipeline — VK_PRIMITIVE_TOPOLOGY_PATCH_LIST is then the ONLY
        // legal topology (VUID-…-pStages-00736) and the patch size must be
        // baked, so it joins the key.
        key.PatchControlPoints = tessellated ? std::max(state.PatchVertexCount, 1u) : 0u;
        if (!dynamicBlend)
        {
            // Blend is a baked axis only where EDS3 is absent (§5's fallback
            // column — expected never to trigger on the desktop floor). The
            // hash must cover EVERY recorded field the baked attachment
            // state consumes — including the color masks and the
            // per-attachment overrides — or two states differing only in an
            // unhashed field silently share one pipeline.
            u64 blendHash = state.Blend ? 1 : 0;
            blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.BlendSrcRGB));
            blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.BlendDstRGB));
            blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.BlendSrcAlpha));
            blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.BlendDstAlpha));
            blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.BlendEquation));
            for (u32 i = 0; i < 4; ++i)
            {
                blendHash = CombinePipelineHash(blendHash, state.ColorMask[i] ? 1u : 0u);
            }
            for (u32 i = 0; i < colorCount; ++i)
            {
                blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.AttachmentBlend[i]));
                blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.AttachmentBlendFunc[i]));
                blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.AttachmentBlendSrc[i]));
                blendHash = CombinePipelineHash(blendHash, static_cast<u64>(state.AttachmentBlendDst[i]));
                blendHash = CombinePipelineHash(blendHash, state.AttachmentColorMask[i]);
            }
            key.BakedBlendHash = blendHash == 0 ? 1 : blendHash;
        }
        return key;
    }

    VulkanPipelineBuilder::Key VulkanPipelineBuilder::DeriveComputeKey(const u64 shaderKey,
                                                                       const VulkanRootDataLayout& layout)
    {
        Key key;
        key.ShaderKey = shaderKey;
        key.LayoutHash = HashLayout(layout);
        // Target/blend fields stay zero — compute has neither, and shader
        // keys are process-unique so no graphics key can collide.
        return key;
    }

    std::vector<VkDescriptorSetAndBindingMappingEXT>
    VulkanPipelineBuilder::BuildBindingMappings(const VulkanRootDataLayout& layout)
    {
        // The sampler-heap strides feed the mappings below, so the heap must
        // exist BEFORE the first pipeline bakes them (#691).
        if (!VulkanSamplerHeap::Get().EnsureCreated())
        {
            OLO_CORE_WARN("VulkanPipelineBuilder: sampler heap unavailable — sampler mappings will use zero strides");
        }
        std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
        mappings.reserve(layout.Fields.size());
        const u32 heapStride = static_cast<u32>(VulkanResourceHeap::Get().GetDescriptorStride());
        for (const auto& field : layout.Fields)
        {
            VkDescriptorSetAndBindingMappingEXT mapping{};
            mapping.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
            mapping.descriptorSet = field.Binding.Set;
            mapping.firstBinding = field.Binding.Binding;
            mapping.bindingCount = 1;
            // The mask is PER RESOURCE KIND, never ALL. GL keeps texture
            // slots, image units, UBO and SSBO binding points in four
            // disjoint namespaces (amendment (29)), so a shader may declare
            // a sampler and a storage image — or a UBO and an SSBO — at the
            // SAME numeric binding. Two ALL-masked mappings at one
            // (set, binding) violate VUID-11244 and fail pipeline creation
            // outright; kind-scoped masks are exactly how the extension
            // expresses coexisting namespaces. First tripped by
            // FroxelFogScatter.comp (sampler2DArrayShadow @0 + image3D @0)
            // — every earlier shader happened to use disjoint numbers.
            switch (field.Binding.BindingKind)
            {
                case VulkanShaderBinding::Kind::UniformBuffer:
                case VulkanShaderBinding::Kind::StorageBuffer:
                {
                    mapping.resourceMask =
                        field.Binding.BindingKind == VulkanShaderBinding::Kind::UniformBuffer
                            ? VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT
                            : (VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
                               VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT);
                    // Block address lives in the root struct at Field.Offset;
                    // the root struct's own address is push data offset 0.
                    mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT;
                    mapping.sourceData.indirectAddress = {
                        .pushOffset = 0,
                        .addressOffset = field.Offset,
                    };
                    break;
                }
                case VulkanShaderBinding::Kind::CombinedImageSampler:
                case VulkanShaderBinding::Kind::StorageImage:
                {
                    mapping.resourceMask =
                        field.Binding.BindingKind == VulkanShaderBinding::Kind::CombinedImageSampler
                            ? VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT
                            : (VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT |
                               VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT);
                    // Heap slot index lives in the root struct at Field.Offset.
                    // The sampler half comes from the SAMPLER heap, indexed by
                    // the u32 at Field.SamplerOffset (#691)
                    // Per-draw sampler state with no PSO axis,
                    // replacing the per-pipeline embedded sampler that baked
                    // linear/clamp into every texture read.
                    const bool combined =
                        field.Binding.BindingKind == VulkanShaderBinding::Kind::CombinedImageSampler;
                    const u32 samplerStride =
                        combined ? static_cast<u32>(VulkanSamplerHeap::Get().GetDescriptorStride()) : 0u;

                    // #1078: an ARRAY binding must NOT use the scalar source.
                    // INDIRECT_INDEX derives element i's descriptor as
                    // `base + i * heapArrayStride`, i.e. it demands that the
                    // array's N textures occupy N CONSECUTIVE heap slots. The
                    // slot cache allocates from a free list and a bump
                    // allocator and guarantees no such thing — it merely
                    // happened to hand out consecutive slots to a freshly
                    // built batch, which is why Renderer2D's 32-sampler array
                    // rendered correctly right up until any one of its
                    // textures got a recycled slot. INDIRECT_INDEX_ARRAY reads
                    // element i's index from the root struct instead (no
                    // heapArrayStride field at all), so the slots may be
                    // anywhere.
                    if (field.Binding.ArrayCount > 1u)
                    {
                        mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT;
                        mapping.sourceData.indirectIndexArray = {
                            .heapOffset = static_cast<u32>(VulkanResourceHeap::Get().GetSlotRegionOffset()),
                            .pushOffset = 0,
                            .addressOffset = field.Offset,
                            .heapIndexStride = heapStride,
                            .pEmbeddedSampler = nullptr,
                            .useCombinedImageSamplerIndex = VK_FALSE,
                            .samplerHeapOffset =
                                combined ? static_cast<u32>(VulkanSamplerHeap::Get().GetSlotRegionOffset()) : 0u,
                            .samplerPushOffset = 0,
                            .samplerAddressOffset = combined ? field.SamplerOffset : 0u,
                            .samplerHeapIndexStride = samplerStride,
                        };
                        break;
                    }

                    mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT;
                    mapping.sourceData.indirectIndex = {
                        .heapOffset = static_cast<u32>(VulkanResourceHeap::Get().GetSlotRegionOffset()),
                        .pushOffset = 0,
                        .addressOffset = field.Offset,
                        .heapIndexStride = heapStride,
                        .heapArrayStride = heapStride,
                        .pEmbeddedSampler = nullptr,
                        .useCombinedImageSamplerIndex = VK_FALSE,
                        .samplerHeapOffset =
                            combined ? static_cast<u32>(VulkanSamplerHeap::Get().GetSlotRegionOffset()) : 0u,
                        .samplerPushOffset = 0,
                        .samplerAddressOffset = combined ? field.SamplerOffset : 0u,
                        .samplerHeapIndexStride = samplerStride,
                        .samplerHeapArrayStride = samplerStride,
                    };
                    break;
                }
            }
            mappings.push_back(mapping);
        }
        return mappings;
    }

    // =========================================================================
    // Snapshots and the driver call (#1345). Nothing below holds m_Mutex.
    // =========================================================================

    VulkanPipelineBuilder::CreateInputs VulkanPipelineBuilder::SnapshotGraphics(VulkanShader& shader,
                                                                                const VulkanRootDataLayout& layout,
                                                                                const VulkanRecordedPipelineState& state,
                                                                                const VulkanRenderTargetDesc& targets)
    {
        CreateInputs inputs;
        inputs.ShaderKey = shader.GetPipelineIndexKey();
        inputs.ShaderName = FString(shader.GetName());
        inputs.Mesh = shader.HasMeshStage();
        // The classic stage set, or [task?, mesh, fragment] for a mesh
        // pipeline (issue #813) — GetModule returning null skips a stage, so
        // the OPTIONAL task stage falls out naturally.
        const std::initializer_list<VkShaderStageFlagBits> classicStages = {
            VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
        };
        const std::initializer_list<VkShaderStageFlagBits> meshStages = {
            VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT
        };
        for (const VkShaderStageFlagBits stageBit : (inputs.Mesh ? meshStages : classicStages))
        {
            if (const VkShaderModule module = shader.GetModule(stageBit); module != VK_NULL_HANDLE)
            {
                inputs.Modules.Add({ stageBit, module });
                inputs.StageMask |= static_cast<u32>(stageBit);
            }
        }
        inputs.Layout = layout;
        inputs.State = state;
        inputs.Targets = targets;
        inputs.Targets.ColorCount = std::min(inputs.Targets.ColorCount, 8u);
        return inputs;
    }

    VulkanPipelineBuilder::CreateInputs VulkanPipelineBuilder::SnapshotCompute(const u64 shaderKey,
                                                                               const VkShaderModule module,
                                                                               const VulkanRootDataLayout& layout,
                                                                               const std::string_view shaderName)
    {
        CreateInputs inputs;
        inputs.Compute = true;
        inputs.ShaderKey = shaderKey;
        inputs.ShaderName = FString(shaderName);
        inputs.StageMask = static_cast<u32>(VK_SHADER_STAGE_COMPUTE_BIT);
        inputs.Modules.Add({ VK_SHADER_STAGE_COMPUTE_BIT, module });
        inputs.Layout = layout;
        return inputs;
    }

    VulkanPipelineBuilder::CreateResult VulkanPipelineBuilder::CreatePipeline(const CreateInputs& inputs, const Key& key,
                                                                              const VkPipelineCache cache)
    {
        CreateResult out;
        auto* device = VulkanDevice::Get();
        if (device == nullptr || inputs.Modules.IsEmpty())
        {
            out.Result = VK_ERROR_INITIALIZATION_FAILED;
            return out;
        }

        const auto start = std::chrono::steady_clock::now();
        const std::vector<VkDescriptorSetAndBindingMappingEXT> mappings = BuildBindingMappings(inputs.Layout);
        VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
        mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
        mappingInfo.mappingCount = static_cast<u32>(mappings.size());
        mappingInfo.pMappings = mappings.data();

        // Creation feedback (core 1.3) is the ONLY honest source for "did the
        // pipeline cache serve this?" — the time alone cannot tell a fast
        // compile from a cache hit (#1345: driver-cache hits are counted
        // separately from creation).
        VkPipelineCreationFeedback feedback{};
        VkPipelineCreationFeedbackCreateInfo feedbackInfo{};
        feedbackInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
        feedbackInfo.pPipelineCreationFeedback = &feedback;

        if (inputs.Compute)
        {
            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pNext = &mappingInfo;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = inputs.Modules[0].Module;
            stage.pName = "main";

            VkPipelineCreateFlags2CreateInfo flags2{};
            flags2.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
            flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
            flags2.pNext = &feedbackInfo;

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.pNext = &flags2;
            pipelineInfo.stage = stage;
            pipelineInfo.layout = VK_NULL_HANDLE; // heap-bindless: no pipeline layout exists

            out.Result = vkCreateComputePipelines(device->GetDevice(), cache, 1, &pipelineInfo, nullptr, &out.Pipeline);
        }
        else
        {
            const VulkanRecordedPipelineState& state = inputs.State;
            const VulkanRenderTargetDesc& safeTargets = inputs.Targets;
            // The key decided this when it was derived: a baked blend hash is
            // non-zero exactly when blend is not dynamic on this device.
            const bool dynamicBlend = key.BakedBlendHash == 0;

            // --- Shader stages, each carrying its binding-mapping chain ------
            // The mapping array is identical across stages (the root struct is
            // one object per draw). The same mappingInfo chains onto EVERY
            // stage present, task and mesh included: each stage create info
            // must carry the §4 binding mappings with the per-kind
            // resourceMask discipline (VUID-11244 keys on (set, binding,
            // mask), not on stage kind).
            std::vector<VkPipelineShaderStageCreateInfo> stages;
            stages.reserve(static_cast<sizet>(inputs.Modules.Num()));
            for (const StageModule& stageModule : inputs.Modules)
            {
                const VkShaderStageFlagBits stageBit = stageModule.Stage;
                const VkShaderModule module = stageModule.Module;
                VkPipelineShaderStageCreateInfo stage{};
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stage.pNext = &mappingInfo;
                stage.stage = stageBit;
                stage.module = module;
                stage.pName = "main";
                stages.push_back(stage);
            }

            // --- Fixed-function: everything dynamic that the floor allows ----
            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            // Zero attributes/bindings — §5: vertex data is pulled through the
            // root struct's buffer address, the axis is REMOVED not parameterised.

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology =
                key.PatchControlPoints != 0 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            // A10: the tessellation state exists ONLY on a patch pipeline; the
            // draw front-end sets PATCH_LIST through the dynamic topology so the
            // baked value above and the recorded one agree.
            //
            // Domain origin (#691, the water-murk bug): Vulkan's default
            // is UPPER_LEFT, GL's convention is LOWER_LEFT. The GLSL tess stages
            // were authored against GL semantics — with the default, the
            // tessellator's v coordinate mirrors and every generated triangle's
            // winding flips, so a back-culled tessellated surface keeps its BACK
            // face: geometry lands in the right place (barycentric weights are a
            // corner permutation, still on the patch) while gl_FrontFacing,
            // two-sided normal flips and NdotV-driven shading all invert. On the
            // water that collapsed fresnel to zero and slammed the depth blend to
            // the deep colour — a murky grey sea with every binding, UBO and the
            // planar-reflection mirror verified correct. LOWER_LEFT restores GL
            // parity by construction for water and terrain alike.
            VkPipelineTessellationStateCreateInfo tessellation{};
            tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
            tessellation.patchControlPoints = key.PatchControlPoints;
            VkPipelineTessellationDomainOriginStateCreateInfo domainOrigin{};
            domainOrigin.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO;
            domainOrigin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
            tessellation.pNext = &domainOrigin;

            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            // Counts come from the dynamic WITH_COUNT states.

            VkPipelineRasterizationStateCreateInfo raster{};
            raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample{};
            multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(safeTargets.Samples);

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            // All dynamic.

            std::array<VkPipelineColorBlendAttachmentState, 8> blendAttachments{};
            VkPipelineColorBlendStateCreateInfo blend{};
            blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blend.attachmentCount = safeTargets.ColorCount;
            blend.pAttachments = blendAttachments.data();
            for (u32 i = 0; i < safeTargets.ColorCount; ++i)
            {
                auto& attachment = blendAttachments[i];
                if (dynamicBlend)
                {
                    // Values ignored — enable/equation/write-mask are dynamic.
                    attachment.colorWriteMask = 0xF;
                }
                else
                {
                    // Same per-attachment selection + global-mask AND as the
                    // dynamic path in FlushDynamicState — the two lowering routes
                    // must produce identical blend behaviour for one recorded
                    // state, and every field read here is in BakedBlendHash.
                    // ENABLE and FUNC divert independently (GL parity, see the
                    // AttachmentBlendFunc comment): glEnablei alone keeps the
                    // GLOBAL blend func; only glBlendFunci diverts the factors.
                    const bool useAttachmentFunc =
                        state.AttachmentBlendFunc[i] == VulkanRecordedPipelineState::AttachmentBlendFuncOpinion::Diverted;
                    // Integer attachments cannot blend — see the identical mask
                    // in FlushDynamicState (the two routes must agree).
                    // A per-attachment OPINION outranks the global flag in both
                    // directions and only Inherit falls through to it — NOT an OR.
                    // See the rule at the AttachmentBlend declaration; both arms
                    // are load-bearing and each was a bug once (#823 / #896).
                    attachment.blendEnable = (!IsIntegerFormat(safeTargets.ColorFormats[i]) &&
                                              ResolveBlendEnable(state, i))
                                                 ? VK_TRUE
                                                 : VK_FALSE;
                    attachment.srcColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcRGB);
                    attachment.dstColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstRGB);
                    attachment.colorBlendOp = ToVk(state.BlendEquation);
                    attachment.srcAlphaBlendFactor =
                        ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcAlpha);
                    attachment.dstAlphaBlendFactor =
                        ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstAlpha);
                    attachment.alphaBlendOp = ToVk(state.BlendEquation);
                    // AttachmentColorMask ALONE — the opposite composition to
                    // blendEnable above, deliberately. SetColorMask fills the array
                    // for every attachment (glColorMask semantics), so ANDing the
                    // global field in on top would make a glColorMaski-shaped WIDEN
                    // after a global narrow fail to reach the attachment, which is
                    // the opposite of GL, where the indexed call wins.
                    attachment.colorWriteMask = static_cast<VkColorComponentFlags>(state.AttachmentColorMask[i]);
                }
            }

            std::vector<VkDynamicState> dynamicStates = {
                // Core 1.0/1.3 (EDS1/EDS2 promotions — no extension at our floor):
                VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
                VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                VK_DYNAMIC_STATE_CULL_MODE,
                VK_DYNAMIC_STATE_FRONT_FACE,
                VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                VK_DYNAMIC_STATE_STENCIL_OP,
                VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                VK_DYNAMIC_STATE_DEPTH_BIAS,
                VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
            };
            if (!inputs.Mesh)
            {
                // Input-assembly dynamic state exists only where input assembly
                // does: DECLARING topology / primitive-restart on a mesh pipeline
                // is itself a validation error (issue #813), not merely setting
                // them — FlushDynamicState mirrors this split on the record side.
                dynamicStates.push_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
                dynamicStates.push_back(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
            }
            if (dynamicBlend)
            {
                dynamicStates.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
                dynamicStates.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
                dynamicStates.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
            }
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = static_cast<u32>(dynamicStates.size());
            dynamic.pDynamicStates = dynamicStates.data();

            // --- Dynamic rendering + heap-mode create flags ------------------
            VkPipelineRenderingCreateInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering.pNext = &feedbackInfo;
            rendering.colorAttachmentCount = safeTargets.ColorCount;
            rendering.pColorAttachmentFormats = safeTargets.ColorFormats.data();
            rendering.depthAttachmentFormat = safeTargets.DepthFormat;
            // A combined depth/stencil target shares the attachment for both
            // aspects (EnsureRenderingScopeForDraw passes pStencilAttachment for
            // it), and VUID-08917 requires the PSO's stencil format to match the
            // rendering info's — the first D24S8/D32S8 scene FB on this backend
            // (#691: OITPrepare / DeferredLighting) tripped it.
            rendering.stencilAttachmentFormat =
                (safeTargets.DepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                 safeTargets.DepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
                    ? safeTargets.DepthFormat
                    : VK_FORMAT_UNDEFINED;

            VkPipelineCreateFlags2CreateInfo createFlags{};
            createFlags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
            createFlags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
            createFlags.pNext = &rendering;

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.pNext = &createFlags;
            pipelineInfo.stageCount = static_cast<u32>(stages.size());
            pipelineInfo.pStages = stages.data();
            // A mesh pipeline has no vertex input and no input assembly — both
            // pointers MUST be null when the mesh stage is present (issue #813);
            // the classic path keeps its empty vertex-input state (§5 vertex
            // pulling removed the contents, mesh shading removes the struct).
            pipelineInfo.pVertexInputState = inputs.Mesh ? nullptr : &vertexInput;
            pipelineInfo.pInputAssemblyState = inputs.Mesh ? nullptr : &inputAssembly;
            pipelineInfo.pTessellationState = key.PatchControlPoints != 0 ? &tessellation : nullptr;
            pipelineInfo.pViewportState = &viewport;
            pipelineInfo.pRasterizationState = &raster;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &blend;
            pipelineInfo.pDynamicState = &dynamic;
            // No VkPipelineLayout: descriptor-heap pipelines take their binding
            // model from the per-stage mapping chains.
            pipelineInfo.layout = VK_NULL_HANDLE;

            out.Result = vkCreateGraphicsPipelines(device->GetDevice(), cache, 1, &pipelineInfo, nullptr, &out.Pipeline);
        }

        out.Milliseconds = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (out.Result != VK_SUCCESS)
        {
            out.Pipeline = VK_NULL_HANDLE;
        }
        if ((feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0)
        {
            out.Cache = (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0
                            ? PipelineDriverCacheResult::Hit
                            : PipelineDriverCacheResult::Miss;
        }
        return out;
    }

    // =========================================================================
    // Lookup, miss resolution, requests (#1345)
    // =========================================================================

    VkPipeline VulkanPipelineBuilder::LookUp(const Key& key)
    {
        bool countFirstUse = false;
        bool record = false;
        FString recordName;
        u32 recordMask = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        {
            // The hit path shares the lock (#806: every draw and dispatch on
            // every item comes through here).
            std::shared_lock readLock(m_Mutex);
            const auto it = m_Pipelines.find(key);
            if (it == m_Pipelines.end())
            {
                return VK_NULL_HANDLE;
            }
            const Entry& entry = it->second;
            pipeline = entry.Pipeline;
            // A prepared pipeline's first use is the complete-PSO hit. The flag
            // makes it once per pipeline; the ledger counter needs the
            // exclusive lock, taken only on that one draw.
            countFirstUse = !entry.FirstUseSeen.load(std::memory_order_relaxed) &&
                            !entry.FirstUseSeen.exchange(true, std::memory_order_acq_rel);
            // Once per recording epoch a hit refreshes its manifest entry.
            const u32 epoch = m_RecordEpoch.load(std::memory_order_relaxed);
            if (entry.RecordedEpoch.load(std::memory_order_relaxed) != epoch &&
                entry.RecordedEpoch.exchange(epoch, std::memory_order_acq_rel) != epoch)
            {
                record = true;
                recordName = entry.ShaderName;
                recordMask = entry.StageMask;
            }
        }
        if (countFirstUse)
        {
            std::lock_guard<std::shared_mutex> lock(m_Mutex);
            m_Ledger.NoteFirstUse(key);
        }
        if (record)
        {
            VulkanPipelinePrecache::Get().RecordDrawCreation(recordName.ToView(), recordMask, ToSignature(key));
        }
        return pipeline;
    }

    VkPipeline VulkanPipelineBuilder::ResolveMiss(const Key& key, const CreateInputs& inputs)
    {
        std::unique_lock<std::shared_mutex> lock(m_Mutex);
        // One hitching draw is one miss: after waiting for another thread it
        // asks the ledger again without counting a second time.
        bool counted = false;
        for (;;)
        {
            if (const auto it = m_Pipelines.find(key); it != m_Pipelines.end())
            {
                // Published while this thread waited: already counted as a
                // late request, so it is not ALSO a prepared first use. A draw
                // that merely raced the publish (nothing counted yet) found a
                // prepared pipeline and is one. The hit path refreshes the
                // manifest entry either way.
                if (counted)
                {
                    it->second.FirstUseSeen.store(true, std::memory_order_relaxed);
                }
                lock.unlock();
                if (const VkPipeline pipeline = LookUp(key); pipeline != VK_NULL_HANDLE)
                {
                    return pipeline;
                }
                lock.lock(); // invalidated in between: resolve again
                continue;
            }

            const auto miss = m_Ledger.ClaimMiss(key, inputs.ShaderKey, !counted);
            if (miss.Action == decltype(m_Ledger)::MissAction::Wait)
            {
                // Another thread (the precache worker, or a second recording
                // item) is creating exactly this pipeline. Waiting costs the
                // rest of its compile; compiling it again would cost all of it.
                const auto waitStart = std::chrono::steady_clock::now();
                {
                    OLO_PERF_SCOPE_AUTO("Vulkan::PipelineWaitForCreation");
                    m_CreationDone.wait(lock, [&]
                                        {
                        const auto state = m_Ledger.GetState(key);
                        return !state.has_value() || *state != decltype(m_Ledger)::State::InFlight; });
                }
                const f64 waitMs =
                    std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
                m_Ledger.NoteBlockingWait(waitMs);
                if (!counted && m_LoggedMisses < kMaxLoggedMisses)
                {
                    ++m_LoggedMisses;
                    OLO_CORE_WARN("[Vulkan/PSO] '{}': {} — the draw waited {:.2f} ms", inputs.ShaderName.ToView(),
                                  ToString(miss.Kind), waitMs);
                }
                counted = true;
                continue;
            }

            if (miss.ReadyClaimViolation)
            {
                // The collector declared this shader render-ready and the draw
                // still had to create a pipeline for it: the claim hid
                // synchronous work. Loud by design (#1345 criterion 2).
                static VulkanWarnOnceSet s_WarnedClaims;
                if (s_WarnedClaims.Insert(inputs.ShaderName.ToView()))
                {
                    OLO_CORE_ERROR("[Vulkan/PSO] '{}' was declared render-ready, but a draw needed a pipeline nobody "
                                   "requested ({} colour attachment(s), {} sample(s), patch {}) — creating it "
                                   "synchronously; counted as a ready-claim violation",
                                   inputs.ShaderName.ToView(), key.ColorCount, key.Samples, key.PatchControlPoints);
                }
            }
            ++m_InFlightByShader[inputs.ShaderKey];
            lock.unlock();
            return CreateAndPublish(key, inputs, miss.Ticket, /*blocking=*/true, miss.Kind);
        }
    }

    VkPipeline VulkanPipelineBuilder::CreateAndPublish(const Key& key, const CreateInputs& inputs, const u64 ticket,
                                                       const bool blocking, const PipelineMissKind missKind)
    {
        // The pipeline cache handle and the test seam are read under the lock
        // (VulkanPipelineCache's lazy create relies on it), the driver call is
        // made without it.
        VkPipelineCache cache = VK_NULL_HANDLE;
        VkResult forced = VK_SUCCESS;
        {
            std::lock_guard<std::shared_mutex> lock(m_Mutex);
            cache = VulkanPipelineCache::Get().Handle();
            if (m_FailNextCreations > 0)
            {
                --m_FailNextCreations;
                forced = m_FailNextResult;
            }
        }

        CreateResult result;
        if (forced != VK_SUCCESS)
        {
            result.Result = forced;
        }
        else if (blocking)
        {
            // Named so a hitch frame in the CPU scope ring (#1604) says what it
            // was waiting on.
            OLO_PERF_SCOPE_AUTO("Vulkan::PipelineBlockingCreate");
            result = CreatePipeline(inputs, key, cache);
        }
        else
        {
            result = CreatePipeline(inputs, key, cache);
        }

        VkPipeline published = VK_NULL_HANDLE;
        bool logMiss = false;
        bool fresh = false;
        {
            std::lock_guard<std::shared_mutex> lock(m_Mutex);
            const bool success = result.Pipeline != VK_NULL_HANDLE;
            fresh = m_Ledger.Complete(key, ticket, success, blocking, result.Milliseconds, result.Cache);
            if (const auto it = m_InFlightByShader.find(inputs.ShaderKey); it != m_InFlightByShader.end())
            {
                if (--it->second == 0)
                {
                    m_InFlightByShader.erase(it);
                }
            }
            ++m_Completions;
            if (success)
            {
                if (fresh)
                {
                    m_FailedPipelines.erase(key);
                    // A draw-created key is recorded in the manifest right below,
                    // so it starts in the current epoch; a prepared one records
                    // on its first draw.
                    const auto [it, inserted] = m_Pipelines.try_emplace(
                        key, result.Pipeline, /*prepared=*/!blocking, inputs.ShaderName, inputs.StageMask,
                        blocking ? m_RecordEpoch.load(std::memory_order_relaxed) : 0u);
                    if (!inserted)
                    {
                        VulkanDeferredReclaim::Get().Enqueue(result.Pipeline);
                    }
                    published = it->second.Pipeline;
                }
                else
                {
                    // The shader was invalidated while this creation ran: the
                    // pipeline was built from modules that are about to go.
                    // A draw that paid for it may still use it this frame (the
                    // reclaim waits out the frames in flight); nobody else may.
                    VulkanDeferredReclaim::Get().Enqueue(result.Pipeline);
                    published = blocking ? result.Pipeline : VK_NULL_HANDLE;
                }
            }
            else
            {
                m_LastCreationFailure = { true, inputs.ShaderName.ToStdString(), result.Result };
                if (fresh)
                {
                    m_FailedPipelines[key] = result.Result;
                }
            }
            if (blocking && m_LoggedMisses < kMaxLoggedMisses)
            {
                ++m_LoggedMisses;
                logMiss = true;
            }
            m_CreationDone.notify_all();
        }

        if (result.Pipeline == VK_NULL_HANDLE)
        {
            OLO_CORE_ERROR("VulkanPipelineBuilder: {} pipeline creation failed for '{}' (VkResult {}){}",
                           inputs.Compute ? "compute" : "graphics", inputs.ShaderName.ToView(),
                           static_cast<int>(result.Result),
                           blocking ? "" : " — requested ahead of the draw; the draw will retry and count it");
            return VK_NULL_HANDLE;
        }
        if (logMiss)
        {
            OLO_CORE_WARN("[Vulkan/PSO] '{}': {} — created synchronously in {:.2f} ms ({} colour, {} sample(s), "
                          "patch {}, pipeline cache {}){}",
                          inputs.ShaderName.ToView(), ToString(missKind), result.Milliseconds, key.ColorCount,
                          key.Samples,
                          key.PatchControlPoints,
                          result.Cache == PipelineDriverCacheResult::Hit    ? "hit"
                          : result.Cache == PipelineDriverCacheResult::Miss ? "miss"
                                                                            : "unknown",
                          m_LoggedMisses >= kMaxLoggedMisses ? "; further misses are counted, not logged" : "");
        }
        if (blocking && fresh)
        {
            // The draw path saw the complete key: record it so the next session
            // (and a packaged runtime shipping the manifest) requests it ahead.
            VulkanPipelinePrecache::Get().RecordDrawCreation(inputs.ShaderName.ToView(), inputs.StageMask,
                                                             ToSignature(key));
        }
        return published;
    }

    VkPipeline VulkanPipelineBuilder::GetOrCreateCompute(const u64 shaderKey, const VkShaderModule module,
                                                         const VulkanRootDataLayout& layout,
                                                         const std::string_view shaderName)
    {
        if (VulkanDevice::Get() == nullptr || module == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }
        const Key key = DeriveComputeKey(shaderKey, layout);
        if (const VkPipeline pipeline = LookUp(key); pipeline != VK_NULL_HANDLE)
        {
            return pipeline;
        }
        return ResolveMiss(key, SnapshotCompute(shaderKey, module, layout, shaderName));
    }

    VkPipeline VulkanPipelineBuilder::GetOrCreateGraphics(VulkanShader& shader, const VulkanRootDataLayout& layout,
                                                          const VulkanRecordedPipelineState& state,
                                                          const VulkanRenderTargetDesc& targets)
    {
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return VK_NULL_HANDLE;
        }
        // [400] ColorCount indexes fixed 8-wide arrays here, in the blend
        // state, and in FlushDynamicState — clamp once, loudly.
        OLO_CORE_ASSERT(targets.ColorCount <= 8, "VulkanRenderTargetDesc: at most 8 color attachments");
        VulkanRenderTargetDesc safeTargets = targets;
        safeTargets.ColorCount = std::min(safeTargets.ColorCount, 8u);

        // Mesh pipeline (issue #813): deliberately NOT a key axis. ShaderKey
        // identifies the shader, a shader is mesh or classic but never both at
        // once, and a reload that changes its stage set runs InvalidateShader
        // first — so the key cannot alias a stale kind.
        const bool tessellated = shader.GetModule(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != VK_NULL_HANDLE;
        const Key key = DeriveGraphicsKey(shader.GetPipelineIndexKey(), layout, tessellated,
                                          device->IsDynamicBlendStateEnabled(), state, safeTargets);
        if (const VkPipeline pipeline = LookUp(key); pipeline != VK_NULL_HANDLE)
        {
            return pipeline;
        }
        CreateInputs inputs = SnapshotGraphics(shader, layout, state, safeTargets);
        if (inputs.Modules.IsEmpty())
        {
            OLO_CORE_ERROR("VulkanPipelineBuilder: shader '{}' has no modules", shader.GetName());
            return VK_NULL_HANDLE;
        }
        return ResolveMiss(key, inputs);
    }

    bool VulkanPipelineBuilder::Request(const Key& key, CreateInputs inputs, const PipelineRequestPriority priority)
    {
        if (inputs.Modules.IsEmpty())
        {
            return false;
        }
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        const u64 shaderKey = inputs.ShaderKey;
        return m_Ledger.Request(key, shaderKey, std::move(inputs), priority);
    }

    std::optional<VulkanPipelineBuilder::Key> VulkanPipelineBuilder::RequestGraphics(VulkanShader& shader,
                                                                                     const VulkanRenderTargetDesc& targets,
                                                                                     const u32 patchControlPoints,
                                                                                     const PipelineRequestPriority priority)
    {
        auto* device = VulkanDevice::Get();
        if (device == nullptr || shader.GetCompilationStatus() != ShaderCompilationStatus::Ready)
        {
            return std::nullopt;
        }
        VulkanRecordedPipelineState state{};
        state.PatchVertexCount = std::max(patchControlPoints, 1u);
        VulkanRenderTargetDesc safeTargets = targets;
        safeTargets.ColorCount = std::min(safeTargets.ColorCount, 8u);
        const VulkanRootDataLayout& layout = shader.GetRootDataLayout();
        const bool tessellated = shader.GetModule(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != VK_NULL_HANDLE;
        const Key key = DeriveGraphicsKey(shader.GetPipelineIndexKey(), layout, tessellated,
                                          device->IsDynamicBlendStateEnabled(), state, safeTargets);
        CreateInputs inputs = SnapshotGraphics(shader, layout, state, safeTargets);
        if (inputs.Modules.IsEmpty())
        {
            return std::nullopt;
        }
        (void)Request(key, std::move(inputs), priority);
        return key;
    }

    std::optional<VulkanPipelineBuilder::Key> VulkanPipelineBuilder::RequestCompute(const u64 shaderKey,
                                                                                    const VkShaderModule module,
                                                                                    const VulkanRootDataLayout& layout,
                                                                                    const std::string_view shaderName,
                                                                                    const PipelineRequestPriority priority)
    {
        if (VulkanDevice::Get() == nullptr || module == VK_NULL_HANDLE)
        {
            return std::nullopt;
        }
        const Key key = DeriveComputeKey(shaderKey, layout);
        (void)Request(key, SnapshotCompute(shaderKey, module, layout, shaderName), priority);
        return key;
    }

    void VulkanPipelineBuilder::QuiesceShader(const u64 shaderKey)
    {
        std::unique_lock<std::shared_mutex> lock(m_Mutex);
        m_Ledger.DropQueued(shaderKey);
        m_CreationDone.wait(lock, [&]
                            { return !m_InFlightByShader.contains(shaderKey); });
    }

    bool VulkanPipelineBuilder::PrepareNextQueued()
    {
        Key key;
        CreateInputs inputs;
        u64 ticket = 0;
        {
            std::lock_guard<std::shared_mutex> lock(m_Mutex);
            auto job = m_Ledger.TakeNextQueued();
            if (!job.has_value())
            {
                return false;
            }
            key = job->RequestKey;
            inputs = std::move(job->Data);
            ticket = job->Ticket;
            ++m_InFlightByShader[inputs.ShaderKey];
        }
        (void)CreateAndPublish(key, inputs, ticket, /*blocking=*/false, PipelineMissKind::NeverRequested);
        return true;
    }

    std::optional<VulkanPipelineBuilder::RequestState> VulkanPipelineBuilder::GetRequestState(const Key& key) const
    {
        std::shared_lock lock(m_Mutex);
        const auto state = m_Ledger.GetState(key);
        if (!state.has_value())
        {
            return std::nullopt;
        }
        using LedgerState = decltype(m_Ledger)::State;
        switch (*state)
        {
            case LedgerState::Queued:
                return RequestState::Queued;
            case LedgerState::InFlight:
                return RequestState::InFlight;
            case LedgerState::Ready:
                return RequestState::Ready;
            case LedgerState::Failed:
                return RequestState::Failed;
        }
        return std::nullopt;
    }

    void VulkanPipelineBuilder::DeclareShaderReady(const u64 shaderKey, const bool ready)
    {
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        if (ready)
        {
            m_Ledger.DeclareShaderReady(shaderKey);
        }
        else
        {
            m_Ledger.DeclareShaderNotReady(shaderKey);
        }
    }

    VulkanPipelineBuilder::RequestStats VulkanPipelineBuilder::GetRequestStats() const
    {
        std::shared_lock lock(m_Mutex);
        RequestStats stats;
        stats.Counters = m_Ledger.GetCounters();
        stats.Queued = m_Ledger.QueuedCount();
        stats.InFlight = m_Ledger.InFlightCount();
        stats.CachedPipelines = m_Pipelines.size();
        stats.Completions = m_Completions;
        return stats;
    }

    u64 VulkanPipelineBuilder::GetCompletions() const
    {
        std::shared_lock lock(m_Mutex);
        return m_Completions;
    }

    void VulkanPipelineBuilder::BeginRecordingEpoch()
    {
        m_RecordEpoch.fetch_add(1u, std::memory_order_relaxed);
    }

    void VulkanPipelineBuilder::ResetRequestCounters()
    {
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        m_Ledger.ResetCounters();
        m_LoggedMisses = 0;
    }

    void VulkanPipelineBuilder::FailNextCreationsForTesting(const u32 count, const VkResult result)
    {
        std::lock_guard<std::shared_mutex> lock(m_Mutex);
        m_FailNextCreations = count;
        m_FailNextResult = result;
    }

    sizet VulkanPipelineBuilder::InvalidateShader(const u64 shaderKey, TArray<Key>* invalidatedKeys)
    {
        std::unique_lock<std::shared_mutex> lock(m_Mutex);
        sizet count = 0;
        std::erase_if(m_Pipelines, [&](const auto& entry)
                      {
            if (entry.first.ShaderKey != shaderKey)
            {
                return false;
            }
            VulkanDeferredReclaim::Get().Enqueue(entry.second.Pipeline);
            ++count;
            return true; });
        std::erase_if(m_FailedPipelines, [shaderKey](const auto& entry)
                      { return entry.first.ShaderKey == shaderKey; });
        TArray<Key> keys = m_Ledger.InvalidateShader(shaderKey);
        if (invalidatedKeys != nullptr)
        {
            *invalidatedKeys = std::move(keys);
        }
        // The caller destroys the old modules next: a creation still running
        // on another thread reads them, so wait it out. Its result is
        // discarded (the ledger entry is gone, Complete() reports stale).
        m_CreationDone.wait(lock, [&]
                            { return !m_InFlightByShader.contains(shaderKey); });
        return count;
    }

    VulkanPipelineBuilder::ShaderPipelines VulkanPipelineBuilder::GetShaderPipelines(u64 shaderKey) const
    {
        std::shared_lock lock(m_Mutex);
        ShaderPipelines result;
        result.Live = static_cast<sizet>(std::ranges::count_if(m_Pipelines, [shaderKey](const auto& entry)
                                                               { return entry.first.ShaderKey == shaderKey; }));
        for (const auto& [key, failure] : m_FailedPipelines)
        {
            if (key.ShaderKey == shaderKey)
            {
                result.Failed = true;
                result.LastFailure = failure;
                break;
            }
        }
        return result;
    }

    void VulkanPipelineBuilder::FlushDynamicState(VkCommandBuffer cmd, const VulkanRecordedPipelineState& state,
                                                  const VulkanRenderTargetDesc& targets)
    {
        // Derived, not passed: the pipeline bound for the current shader is a
        // mesh pipeline exactly when that shader carries a mesh stage, and a
        // flag parameter would let a call site claim otherwise (issue #813).
        const VulkanShader* boundShader = VulkanShader::GetCurrentlyBound();
        const bool meshPipeline = boundShader != nullptr && boundShader->HasMeshStage();
        vkCmdSetCullMode(cmd, ToVk(state.CullFace, state.Culling));
        // A1/A8 (issue #691): the recorded GL winding translates to the
        // SAME VkFrontFace — no swap.
        //
        // Batch 1 shipped the opposite (recorded CCW -> VK_FRONT_FACE_CLOCKWISE)
        // on the reasoning that the projection seam's Y flip mirrors every
        // triangle's apparent winding. Half of that is true and it is the half
        // that cancels: the seam DOES negate clip y, but Vulkan's facing
        // determinant is computed in FRAMEBUFFER coordinates, whose y already
        // points DOWN where GL's window y points UP. The two inversions
        // compose to identity — a triangle GL calls front-facing has the same
        // facing here — so applying a third one made every solid mesh
        // inside-out on Vulkan: back-face culling removed exactly the
        // triangles GL keeps.
        //
        // Nothing caught it until now because the only Vulkan tenants that
        // recorded a winding drew with culling OFF (every fullscreen pass, the
        // decal/particle/foliage bodies), where front-face state is inert. The
        // planar-reflection tenant is the first to cull real geometry, and it
        // pins all four cells of the matrix — {direct, mirrored} x {CCW, CW} —
        // so an inversion here fails loudly in both directions rather than
        // being masked by the reflection's own handedness reversal.
        //
        // THE COMPOSITION IS CONDITIONAL ON THE SEAM BEING IN THE PIPELINE, and
        // that is the half this comment used to leave implicit (issue #1002).
        // Geometry that writes NDC straight out of the vertex stage -- every
        // fullscreen triangle -- never meets the projection seam, so it gets
        // only the framebuffer-y half of the pair and its apparent winding is
        // the OPPOSITE of a projected mesh's: measured live, the shared
        // fullscreen triangle is front-facing on GL under CCW and back-facing
        // here under VK_FRONT_FACE_COUNTER_CLOCKWISE. One winding cannot serve
        // both conventions and neither rule can move (swapping here is the
        // inside-out regression described above), so the rule is a CALL-SITE
        // rule: a fullscreen draw must DISABLE culling, never merely set the
        // cull face. The paragraph above already noted that every fullscreen
        // tenant drew with culling off -- DeferredLightingPass was the one that
        // did not, and its entire draw was silently culled on Vulkan.
        // See docs/agent-rules/rhi-abstraction-boundary.md §19.
        //
        // Composed HERE, in the backend's one state-translation point, never at
        // call sites: a pass-local override (PlanarReflection's Clockwise for
        // the mirrored replay) then means exactly what it means on GL.
        vkCmdSetFrontFace(cmd, state.FrontFaceWinding == RHI::FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE);
        if (!meshPipeline)
        {
            // Input-assembly dynamic state — a mesh pipeline declares
            // neither this nor primitive restart (below), and setting a
            // dynamic state the pipeline did not declare is invalid
            // (issue #813).
            vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        }
        vkCmdSetDepthTestEnable(cmd, state.DepthTest ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd, state.DepthWrite ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(cmd, ToVk(state.DepthFunc));
        vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
        vkCmdSetStencilTestEnable(cmd, state.StencilTest ? VK_TRUE : VK_FALSE);
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, ToVk(state.StencilFail), ToVk(state.StencilPass),
                          ToVk(state.StencilDepthFail), ToVk(state.StencilFunc));
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.StencilReadMask);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.StencilWriteMask);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, static_cast<u32>(state.StencilRef));
        vkCmdSetDepthBiasEnable(cmd, state.PolygonOffsetEnabled ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthBias(cmd, state.PolygonOffsetUnits, 0.0f, state.PolygonOffsetFactor);
        vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
        if (!meshPipeline)
        {
            vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
        }

        auto* device = VulkanDevice::Get();
        const u32 colorCount = std::min(targets.ColorCount, 8u); // same 8-wide bound as the create path
        if (device != nullptr && device->IsDynamicBlendStateEnabled() && colorCount > 0)
        {
            // [401] The GLOBAL color mask (SetColorMask) reaches the dynamic
            // write masks by FILLING AttachmentColorMask, not by being ANDed in
            // here — see the identical expression on the baked route.
            std::array<VkBool32, 8> enables{};
            std::array<VkColorBlendEquationEXT, 8> equations{};
            std::array<VkColorComponentFlags, 8> writeMasks{};
            for (u32 i = 0; i < colorCount; ++i)
            {
                // Per-attachment state where the pass set it, the global
                // recorded state otherwise. ENABLE and FUNC divert
                // independently (GL parity, see the AttachmentBlendFunc
                // comment): glEnablei alone keeps the GLOBAL blend func; only
                // glBlendFunci diverts the factors — AttachmentBlendSrc/Dst
                // are meaningful only where AttachmentBlendFunc[i] is Diverted.
                const bool useAttachmentFunc =
                    state.AttachmentBlendFunc[i] == VulkanRecordedPipelineState::AttachmentBlendFuncOpinion::Diverted;
                // An INTEGER attachment can never blend: R32_SINT (the
                // engine's entity-ID render target) carries no
                // COLOR_ATTACHMENT_BLEND format feature, and enabling blend
                // against it is VUID-vkCmdDraw-blendEnable-04727. GL has the
                // same rule but expresses it silently (blending is ignored
                // for integer attachments), so every pass that flips the
                // GLOBAL blend switch with an ID target attached — the whole
                // forward/G-buffer family — tripped this on the first live
                // frame. Masking it here keeps GL's "just ignore it"
                // behaviour and costs the caller nothing.
                // A per-attachment enable OPINION outranks the global flag —
                // see the identical expression on the baked route in
                // GetOrCreateGraphics, and the rule at the declaration.
                const bool blendableFormat = !IsIntegerFormat(targets.ColorFormats[i]);
                const bool enabled = blendableFormat && ResolveBlendEnable(state, i);
                enables[i] = enabled ? VK_TRUE : VK_FALSE;
                equations[i] = {
                    .srcColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcRGB),
                    .dstColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstRGB),
                    .colorBlendOp = ToVk(state.BlendEquation),
                    .srcAlphaBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcAlpha),
                    .dstAlphaBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstAlpha),
                    .alphaBlendOp = ToVk(state.BlendEquation),
                };
                writeMasks[i] = static_cast<VkColorComponentFlags>(state.AttachmentColorMask[i]);
            }
            vkCmdSetColorBlendEnableEXT(cmd, 0, colorCount, enables.data());
            vkCmdSetColorBlendEquationEXT(cmd, 0, colorCount, equations.data());
            vkCmdSetColorWriteMaskEXT(cmd, 0, colorCount, writeMasks.data());
        }
    }

    void VulkanPipelineBuilder::ReleaseAll()
    {
        std::unique_lock<std::shared_mutex> lock(m_Mutex);
        // Device teardown: nothing may still be creating against it.
        m_CreationDone.wait(lock, [this]
                            { return m_InFlightByShader.empty(); });
        for (const auto& [key, entry] : m_Pipelines)
        {
            VulkanDeferredReclaim::Get().Enqueue(entry.Pipeline);
        }
        m_Pipelines.clear();
        m_FailedPipelines.clear();
        m_Ledger.Clear();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
