#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanComputeShader — the compute half of the SPIR-V route (#691,
// the amendment (56) "compute shaders still have no SPIR-V route" deferral).
//
// GL compute never travelled the shaderc pipeline at all (amendment (31):
// OpenGLComputeShader feeds raw GLSL to glShaderSource), so this class is the
// FIRST SPIR-V consumer for .comp sources: shaderc(vulkan_1_4, OLO_VULKAN=1),
// SPIRV-Cross reflection into the same VulkanShaderBinding vocabulary the
// graphics path uses, the same root-data layout, and a compute PSO through
// VulkanPipelineBuilder::GetOrCreateCompute (same mapping chain — the two
// paths structurally cannot drift).
//
// THE AUTHORING CONSEQUENCE: a .comp that reaches this
// route may not use default-block uniforms — SPIR-V has no queryable default
// block, so SetInt/SetFloat/... are deliberate no-ops here (exactly like
// VulkanShader's). Each ported compute pass migrates its bare uniforms into a
// pass-owned UBO, which the GL raw-GLSL route accepts identically — one
// source, no fork.
//
// Thread-safety: NONE, deliberately — render thread only (async creation goes
// through GPUResourceQueue like the GL twin).
// =============================================================================

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanShader.h" // VulkanShaderBinding + the fwd-declared VulkanRootDataLayout

#include <memory>
#include <vector>

namespace OloEngine
{
    class VulkanComputeShader final : public ComputeShader
    {
      public:
        explicit VulkanComputeShader(const std::string& filepath);
        VulkanComputeShader(std::string name, const std::string& source);
        ~VulkanComputeShader() override;

        VulkanComputeShader(const VulkanComputeShader&) = delete;
        VulkanComputeShader& operator=(const VulkanComputeShader&) = delete;
        VulkanComputeShader(VulkanComputeShader&&) = delete;
        VulkanComputeShader& operator=(VulkanComputeShader&&) = delete;

        void Bind() const override;
        void Unbind() const override;

        // Default-block uniforms do not exist on this backend (header note).
        void SetInt(const std::string& name, int value) const override;
        void SetUint(const std::string& name, u32 value) const override;
        void SetIntArray(const std::string& name, int* values, u32 count) const override;
        void SetFloat(const std::string& name, f32 value) const override;
        void SetFloat2(const std::string& name, const glm::vec2& value) const override;
        void SetFloat3(const std::string& name, const glm::vec3& value) const override;
        void SetFloat4(const std::string& name, const glm::vec4& value) const override;
        void SetMat4(const std::string& name, const glm::mat4& value) const override;

        [[nodiscard]] bool IsValid() const override
        {
            return m_Module != VK_NULL_HANDLE;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // no native GL name exists on this backend
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const std::string& GetName() const override
        {
            return m_Name;
        }
        [[nodiscard]] const std::string& GetFilePath() const override
        {
            return m_FilePath;
        }

        void Reload() override;

        // --- backend-internal (the dispatch path's material) ----------------
        [[nodiscard]] VkShaderModule GetModule() const
        {
            return m_Module;
        }
        [[nodiscard]] const std::vector<VulkanShaderBinding>& GetBindings() const
        {
            return m_Bindings;
        }
        [[nodiscard]] const VulkanRootDataLayout& GetRootDataLayout();
        [[nodiscard]] u64 GetPipelineIndexKey() const
        {
            return m_RHIHandle.Get().Index;
        }

        // The compute shader whose Bind() ran last — the dispatch-time
        // pipeline lookup's source (its own static, distinct from the
        // graphics current: Vulkan's bind points are independent, so the GL
        // one-current-program model would only manufacture false conflicts).
        [[nodiscard]] static VulkanComputeShader* GetCurrentlyBound();

      private:
        [[nodiscard]] bool BuildFromSource(const std::string& preprocessedSource, bool useCache);
        void DestroyModule();

        std::string m_Name;
        std::string m_FilePath;
        std::vector<std::string> m_IncludedFilePaths;
        std::vector<u32> m_SPIRV;
        VkShaderModule m_Module = VK_NULL_HANDLE;
        std::vector<VulkanShaderBinding> m_Bindings;
        std::unique_ptr<VulkanRootDataLayout> m_RootLayout;
        RHI::ScopedResourceHandle m_RHIHandle;

        inline static VulkanComputeShader* s_CurrentlyBound = nullptr;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
