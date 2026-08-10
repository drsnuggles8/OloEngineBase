#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanResourceHeap.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <algorithm>
#include <bit>
#include <tuple>

namespace OloEngine
{
    namespace
    {
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

        [[nodiscard]] u64 HashSampler(const VkSamplerCreateInfo& info)
        {
            // Every field that vkCreateSampler consumes is key material — a
            // sampler differing only in an unhashed field (maxLod was the
            // caught case: DefaultEmbeddedSampler sets VK_LOD_CLAMP_NONE)
            // must not collide. Floats enter via bit_cast, never operator==.
            u64 hash = 0;
            hash = HashCombine(hash, static_cast<u64>(info.flags));
            hash = HashCombine(hash, static_cast<u64>(info.magFilter));
            hash = HashCombine(hash, static_cast<u64>(info.minFilter));
            hash = HashCombine(hash, static_cast<u64>(info.mipmapMode));
            hash = HashCombine(hash, static_cast<u64>(info.addressModeU));
            hash = HashCombine(hash, static_cast<u64>(info.addressModeV));
            hash = HashCombine(hash, static_cast<u64>(info.addressModeW));
            hash = HashCombine(hash, std::bit_cast<u32>(info.mipLodBias));
            hash = HashCombine(hash, static_cast<u64>(info.anisotropyEnable));
            hash = HashCombine(hash, std::bit_cast<u32>(info.maxAnisotropy));
            hash = HashCombine(hash, static_cast<u64>(info.compareEnable));
            hash = HashCombine(hash, static_cast<u64>(info.compareOp));
            hash = HashCombine(hash, std::bit_cast<u32>(info.minLod));
            hash = HashCombine(hash, std::bit_cast<u32>(info.maxLod));
            hash = HashCombine(hash, static_cast<u64>(info.borderColor));
            hash = HashCombine(hash, static_cast<u64>(info.unnormalizedCoordinates));
            return hash == 0 ? 1 : hash;
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

        // The default embedded sampler: the post-process read shape. Explicit
        // rather than SamplerDesc-derived — amendment (38)'s "no table of
        // defaults is right" lesson says the CALLER states intent; this is
        // only the fallback for callers that don't.
        [[nodiscard]] VkSamplerCreateInfo DefaultEmbeddedSampler()
        {
            VkSamplerCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter = VK_FILTER_LINEAR;
            info.minFilter = VK_FILTER_LINEAR;
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.maxLod = VK_LOD_CLAMP_NONE;
            return info;
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
        hash = HashCombine(hash, key.SamplerHash);
        hash = HashCombine(hash, key.LayoutHash);
        return static_cast<sizet>(hash);
    }

    std::vector<VkDescriptorSetAndBindingMappingEXT>
    VulkanPipelineBuilder::BuildBindingMappings(const VulkanRootDataLayout& layout, const VkSamplerCreateInfo& sampler)
    {
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
                    // Heap slot index lives in the root struct at Field.Offset;
                    // the sampler half is embedded (see header).
                    mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT;
                    mapping.sourceData.indirectIndex = {
                        .heapOffset = static_cast<u32>(VulkanResourceHeap::Get().GetSlotRegionOffset()),
                        .pushOffset = 0,
                        .addressOffset = field.Offset,
                        .heapIndexStride = heapStride,
                        .heapArrayStride = heapStride,
                        .pEmbeddedSampler =
                            field.Binding.BindingKind == VulkanShaderBinding::Kind::CombinedImageSampler ? &sampler
                                                                                                         : nullptr,
                        .useCombinedImageSamplerIndex = VK_FALSE,
                        .samplerHeapOffset = 0,
                        .samplerPushOffset = 0,
                        .samplerAddressOffset = 0,
                        .samplerHeapIndexStride = 0,
                        .samplerHeapArrayStride = 0,
                    };
                    break;
                }
            }
            mappings.push_back(mapping);
        }
        return mappings;
    }

    VkPipeline VulkanPipelineBuilder::GetOrCreateCompute(const u64 shaderKey, const VkShaderModule module,
                                                         const VulkanRootDataLayout& layout,
                                                         const VkSamplerCreateInfo* embeddedSampler)
    {
        auto* device = VulkanDevice::Get();
        if (device == nullptr || module == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }

        const VkSamplerCreateInfo sampler = embeddedSampler != nullptr ? *embeddedSampler : DefaultEmbeddedSampler();

        Key key;
        key.ShaderKey = shaderKey;
        key.SamplerHash = HashSampler(sampler);
        key.LayoutHash = HashLayout(layout);
        // Target/blend fields stay zero — compute has neither, and shader
        // keys are process-unique so no graphics key can collide.

        if (const auto it = m_Pipelines.find(key); it != m_Pipelines.end())
        {
            return it->second;
        }

        const std::vector<VkDescriptorSetAndBindingMappingEXT> mappings = BuildBindingMappings(layout, sampler);
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
                                                          const VulkanRenderTargetDesc& targets,
                                                          const VkSamplerCreateInfo* embeddedSampler)
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
        const VkSamplerCreateInfo sampler = embeddedSampler != nullptr ? *embeddedSampler : DefaultEmbeddedSampler();

        Key key;
        key.ShaderKey = shader.GetPipelineIndexKey();
        key.ColorFormats = safeTargets.ColorFormats;
        key.DepthFormat = safeTargets.DepthFormat;
        key.ColorCount = safeTargets.ColorCount;
        key.Samples = safeTargets.Samples;
        key.SamplerHash = HashSampler(sampler);
        key.LayoutHash = HashLayout(layout);
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
        const std::vector<VkDescriptorSetAndBindingMappingEXT> mappings = BuildBindingMappings(layout, sampler);

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
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // dynamic

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
                attachment.blendEnable = (state.AttachmentBlend[i] || state.Blend) ? VK_TRUE : VK_FALSE;
                attachment.srcColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcRGB);
                attachment.dstColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstRGB);
                attachment.colorBlendOp = ToVk(state.BlendEquation);
                attachment.srcAlphaBlendFactor =
                    ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcAlpha);
                attachment.dstAlphaBlendFactor =
                    ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstAlpha);
                attachment.alphaBlendOp = ToVk(state.BlendEquation);
                const VkColorComponentFlags globalMask = (state.ColorMask[0] ? VK_COLOR_COMPONENT_R_BIT : 0u) |
                                                         (state.ColorMask[1] ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                                                         (state.ColorMask[2] ? VK_COLOR_COMPONENT_B_BIT : 0u) |
                                                         (state.ColorMask[3] ? VK_COLOR_COMPONENT_A_BIT : 0u);
                attachment.colorWriteMask = static_cast<VkColorComponentFlags>(state.AttachmentColorMask[i]) & globalMask;
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
            // [401] The GLOBAL color mask (SetColorMask) must reach the
            // dynamic write masks too — a pass that narrows the global mask
            // and never touches the per-attachment API would otherwise
            // render with the per-attachment default (all channels).
            const VkColorComponentFlags globalMask = (state.ColorMask[0] ? VK_COLOR_COMPONENT_R_BIT : 0u) |
                                                     (state.ColorMask[1] ? VK_COLOR_COMPONENT_G_BIT : 0u) |
                                                     (state.ColorMask[2] ? VK_COLOR_COMPONENT_B_BIT : 0u) |
                                                     (state.ColorMask[3] ? VK_COLOR_COMPONENT_A_BIT : 0u);
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
                const bool enabled = state.AttachmentBlend[i] || state.Blend;
                enables[i] = enabled ? VK_TRUE : VK_FALSE;
                equations[i] = {
                    .srcColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcRGB),
                    .dstColorBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstRGB),
                    .colorBlendOp = ToVk(state.BlendEquation),
                    .srcAlphaBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendSrc[i] : state.BlendSrcAlpha),
                    .dstAlphaBlendFactor = ToVk(useAttachmentFunc ? state.AttachmentBlendDst[i] : state.BlendDstAlpha),
                    .alphaBlendOp = ToVk(state.BlendEquation),
                };
                writeMasks[i] = static_cast<VkColorComponentFlags>(state.AttachmentColorMask[i]) & globalMask;
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
