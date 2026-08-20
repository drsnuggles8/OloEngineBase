#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanSamplerHeap.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <algorithm>
#include <bit>
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

        [[nodiscard]] u64 HashCombine(u64 seed, u64 value)
        {
            return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        }

        // [398] The root layout drives every binding mapping baked into the
        // pipeline; it is derived from the shader's reflection today, but the
        // key must stand on its own inputs, not on that invariant.
        [[nodiscard]] u64 HashLayout(const VulkanRootDataLayout& layout)
        {
            u64 hash = layout.SizeBytes;
            for (const auto& field : layout.Fields)
            {
                hash = HashCombine(hash, field.Offset);
                hash = HashCombine(hash, field.Binding.Set);
                hash = HashCombine(hash, field.Binding.Binding);
                hash = HashCombine(hash, static_cast<u64>(field.Binding.BindingKind));
            }
            return hash == 0 ? 1 : hash;
        }
    } // namespace

    // =========================================================================
    // VulkanRootDataLayout
    // =========================================================================

    VulkanRootDataLayout VulkanRootDataLayout::Build(const std::vector<VulkanShaderBinding>& bindings)
    {
        VulkanRootDataLayout layout;

        // Deterministic field order: (set, binding), buffer blocks first.
        std::vector<VulkanShaderBinding> sorted = bindings;
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
                // #691 Phase 8: two u32s — the image heap index at Offset and
                // the SAMPLER heap index at Offset + kSamplerIndexOffset. The
                // mapping's samplerAddressOffset and the root-data writer both
                // derive from this one layout, so they cannot disagree.
                offset = (offset + 3u) & ~3u;
                layout.Fields.push_back({ .Binding = binding, .Offset = offset });
                offset += 8;
            }
            else
            {
                offset = (offset + 3u) & ~3u; // u32 heap index field
                layout.Fields.push_back({ .Binding = binding, .Offset = offset });
                offset += 4;
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
            hash = HashCombine(hash, static_cast<u64>(key.ColorFormats[i]));
        }
        hash = HashCombine(hash, static_cast<u64>(key.DepthFormat));
        hash = HashCombine(hash, key.ColorCount);
        hash = HashCombine(hash, key.Samples);
        hash = HashCombine(hash, key.BakedBlendHash);
        hash = HashCombine(hash, key.LayoutHash);
        hash = HashCombine(hash, key.PatchControlPoints);
        return static_cast<sizet>(hash);
    }

    std::vector<VkDescriptorSetAndBindingMappingEXT>
    VulkanPipelineBuilder::BuildBindingMappings(const VulkanRootDataLayout& layout)
    {
        // The sampler-heap strides feed the mappings below, so the heap must
        // exist BEFORE the first pipeline bakes them (#691 Phase 8).
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
                    // the u32 at Field.Offset + kSamplerIndexOffset (#691
                    // Phase 8) — per-draw sampler state with no PSO axis,
                    // replacing the per-pipeline embedded sampler that baked
                    // linear/clamp into every texture read.
                    const bool combined =
                        field.Binding.BindingKind == VulkanShaderBinding::Kind::CombinedImageSampler;
                    const u32 samplerStride =
                        combined ? static_cast<u32>(VulkanSamplerHeap::Get().GetDescriptorStride()) : 0u;
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
                        .samplerAddressOffset =
                            combined ? field.Offset + VulkanRootDataLayout::kSamplerIndexOffset : 0u,
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

    VkPipeline VulkanPipelineBuilder::GetOrCreateCompute(const u64 shaderKey, const VkShaderModule module,
                                                         const VulkanRootDataLayout& layout)
    {
        auto* device = VulkanDevice::Get();
        if (device == nullptr || module == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }

        Key key;
        key.ShaderKey = shaderKey;
        key.LayoutHash = HashLayout(layout);
        // Target/blend fields stay zero — compute has neither, and shader
        // keys are process-unique so no graphics key can collide.

        if (const auto it = m_Pipelines.find(key); it != m_Pipelines.end())
        {
            return it->second;
        }

        const std::vector<VkDescriptorSetAndBindingMappingEXT> mappings = BuildBindingMappings(layout);
        VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
        mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
        mappingInfo.mappingCount = static_cast<u32>(mappings.size());
        mappingInfo.pMappings = mappings.data();

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pNext = &mappingInfo;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";

        VkPipelineCreateFlags2CreateInfo flags2{};
        flags2.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &flags2;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = VK_NULL_HANDLE; // heap-bindless: no pipeline layout exists

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(device->GetDevice(), VulkanPipelineCache::Get().Handle(), 1,
                                                         &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanPipelineBuilder: compute pipeline creation failed (VkResult {})",
                           static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        m_Pipelines[key] = pipeline;
        return pipeline;
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
        VulkanRenderTargetDesc clamped = targets;
        clamped.ColorCount = std::min(clamped.ColorCount, 8u);
        const VulkanRenderTargetDesc& safeTargets = clamped;

        const bool dynamicBlend = device->IsDynamicBlendStateEnabled();

        Key key;
        key.ShaderKey = shader.GetPipelineIndexKey();
        key.ColorFormats = safeTargets.ColorFormats;
        key.DepthFormat = safeTargets.DepthFormat;
        key.ColorCount = safeTargets.ColorCount;
        key.Samples = safeTargets.Samples;
        key.LayoutHash = HashLayout(layout);
        // A10 (#691 Wave C): a shader carrying a tessellation-control stage is
        // a PATCH pipeline — VK_PRIMITIVE_TOPOLOGY_PATCH_LIST is then the ONLY
        // legal topology (VUID-…-pStages-00736) and the patch size must be
        // baked, so it joins the key.
        const bool tessellated = shader.GetModule(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != VK_NULL_HANDLE;
        key.PatchControlPoints = tessellated ? std::max(state.PatchVertexCount, 1u) : 0u;
        if (!dynamicBlend)
        {
            // Blend is a baked axis only where EDS3 is absent (§5's fallback
            // column — expected never to trigger on the desktop floor). The
            // hash must cover EVERY recorded field the baked attachment
            // state below consumes — including the color masks and the
            // per-attachment overrides — or two states differing only in an
            // unhashed field silently share one pipeline.
            u64 blendHash = state.Blend ? 1 : 0;
            blendHash = HashCombine(blendHash, static_cast<u64>(state.BlendSrcRGB));
            blendHash = HashCombine(blendHash, static_cast<u64>(state.BlendDstRGB));
            blendHash = HashCombine(blendHash, static_cast<u64>(state.BlendSrcAlpha));
            blendHash = HashCombine(blendHash, static_cast<u64>(state.BlendDstAlpha));
            blendHash = HashCombine(blendHash, static_cast<u64>(state.BlendEquation));
            for (u32 i = 0; i < 4; ++i)
            {
                blendHash = HashCombine(blendHash, state.ColorMask[i] ? 1u : 0u);
            }
            for (u32 i = 0; i < safeTargets.ColorCount; ++i)
            {
                blendHash = HashCombine(blendHash, state.AttachmentBlend[i] ? 1u : 0u);
                blendHash = HashCombine(blendHash, state.AttachmentBlendFuncSet[i] ? 1u : 0u);
                blendHash = HashCombine(blendHash, static_cast<u64>(state.AttachmentBlendSrc[i]));
                blendHash = HashCombine(blendHash, static_cast<u64>(state.AttachmentBlendDst[i]));
                blendHash = HashCombine(blendHash, state.AttachmentColorMask[i]);
            }
            key.BakedBlendHash = blendHash == 0 ? 1 : blendHash;
        }

        if (const auto it = m_Pipelines.find(key); it != m_Pipelines.end())
        {
            return it->second;
        }

        // --- Shader stages, each carrying its binding-mapping chain ----------
        // The mapping array is identical across stages (the root struct is one
        // object per draw); resourceMask ALL lets one mapping per binding
        // cover whatever SPIR-V resource type the stage declares there.
        const std::vector<VkDescriptorSetAndBindingMappingEXT> mappings = BuildBindingMappings(layout);

        VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
        mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
        mappingInfo.mappingCount = static_cast<u32>(mappings.size());
        mappingInfo.pMappings = mappings.data();

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (const VkShaderStageFlagBits stageBit :
             { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
               VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT })
        {
            const VkShaderModule module = shader.GetModule(stageBit);
            if (module == VK_NULL_HANDLE)
            {
                continue;
            }
            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pNext = &mappingInfo;
            stage.stage = stageBit;
            stage.module = module;
            stage.pName = "main";
            stages.push_back(stage);
        }
        if (stages.empty())
        {
            OLO_CORE_ERROR("VulkanPipelineBuilder: shader '{}' has no modules", shader.GetName());
            return VK_NULL_HANDLE;
        }

        // --- Fixed-function: everything dynamic that the floor allows --------
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
        // Domain origin (#691 Phase 8, the water-murk bug): Vulkan's default
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
                // AttachmentBlendFuncSet comment): glEnablei alone keeps the
                // GLOBAL blend func; only glBlendFunci diverts the factors.
                const bool useAttachmentFunc = state.AttachmentBlendFuncSet[i];
                // Integer attachments cannot blend — see the identical mask
                // in FlushDynamicState (the two routes must agree).
                // The OR is deliberate and is NOT the colour-mask rule below:
                // a per-attachment enable is an opt-in LAYERED ON TOP of the
                // global flag, which is what lets DecalRenderPass blend RT2
                // additively for an Emissive decal whose PODRenderState carries
                // blendEnabled=false. Pinned by the Emissive arm of
                // VulkanPassSuite.DecalGBufferModeMatrixMasksItsTargetRenderTargets.
                attachment.blendEnable = (!IsIntegerFormat(safeTargets.ColorFormats[i]) &&
                                          (state.AttachmentBlend[i] || state.Blend))
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
            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
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
            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        };
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

        // --- Dynamic rendering + heap-mode create flags ----------------------
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = safeTargets.ColorCount;
        rendering.pColorAttachmentFormats = safeTargets.ColorFormats.data();
        rendering.depthAttachmentFormat = safeTargets.DepthFormat;
        // A combined depth/stencil target shares the attachment for both
        // aspects (EnsureRenderingScopeForDraw passes pStencilAttachment for
        // it), and VUID-08917 requires the PSO's stencil format to match the
        // rendering info's — the first D24S8/D32S8 scene FB on this backend
        // (#691 Wave C: OITPrepare / DeferredLighting) tripped it.
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
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
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

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(device->GetDevice(), VulkanPipelineCache::Get().Handle(), 1,
                                                          &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanPipelineBuilder: vkCreateGraphicsPipelines failed for '{}' (VkResult {})",
                           shader.GetName(), static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        m_Pipelines[key] = pipeline;
        return pipeline;
    }

    sizet VulkanPipelineBuilder::InvalidateShader(u64 shaderKey)
    {
        sizet count = 0;
        std::erase_if(m_Pipelines, [&](const auto& entry)
                      {
            if (entry.first.ShaderKey != shaderKey)
            {
                return false;
            }
            VulkanDeferredReclaim::Get().Enqueue(entry.second);
            ++count;
            return true; });
        return count;
    }

    void VulkanPipelineBuilder::FlushDynamicState(VkCommandBuffer cmd, const VulkanRecordedPipelineState& state,
                                                  const VulkanRenderTargetDesc& targets)
    {
        vkCmdSetCullMode(cmd, ToVk(state.CullFace, state.Culling));
        // A1/A8 (issue #691 Phase 7): the recorded GL winding translates to the
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
        // Composed HERE, in the backend's one state-translation point, never at
        // call sites: a pass-local override (PlanarReflection's Clockwise for
        // the mirrored replay) then means exactly what it means on GL.
        vkCmdSetFrontFace(cmd, state.FrontFaceWinding == RHI::FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
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
        vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);

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
                // independently (GL parity, see the AttachmentBlendFuncSet
                // comment): glEnablei alone keeps the GLOBAL blend func; only
                // glBlendFunci diverts the factors — AttachmentBlendSrc/Dst
                // are meaningful only when AttachmentBlendFuncSet[i] is true.
                const bool useAttachmentFunc = state.AttachmentBlendFuncSet[i];
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
                // Per-attachment enable ORs on top of the global flag — see the
                // identical expression on the baked route in GetOrCreateGraphics.
                const bool blendableFormat = !IsIntegerFormat(targets.ColorFormats[i]);
                const bool enabled = blendableFormat && (state.AttachmentBlend[i] || state.Blend);
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
        for (const auto& [key, pipeline] : m_Pipelines)
        {
            VulkanDeferredReclaim::Get().Enqueue(pipeline);
        }
        m_Pipelines.clear();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
