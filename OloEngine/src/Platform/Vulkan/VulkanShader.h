#pragma once

// VulkanShader — the direct-SPIR-V shader path. Issue #691 Phase 6,
// ADR 0011 §3(a)/(b) + §4.
//
// GLSL → shaderc(vulkan_1_4) → SPIR-V → VkShaderModule. No SPIRV-Cross
// round-trip back to GLSL (that hop exists purely so the OpenGL backend can
// run the artefact; this backend deletes it, which was ADR 0010's core
// argument). The SPIR-V is cached per stage under the shaderc TARGET ENV in
// the filename (".cached_vulkan14.<stage>", §3(b)) so this tier and the GL
// path's vulkan_1_2 tier stay independently bumpable.
//
// SHADERS KEEP THEIR CLASSIC BINDING DECLARATIONS. The §4 root-data contract
// is expressed at PIPELINE creation, not in GLSL: reflection (SPIRV-Cross,
// reflection-only use) enumerates every set/binding declaration, and the
// pipeline builder maps each one to a root-struct field via
// VkDescriptorSetAndBindingMappingEXT (INDIRECT_ADDRESS for buffer blocks,
// HEAP_WITH_INDIRECT_INDEX for samplers) with the root pointer arriving
// through one vkCmdPushDataEXT. So one .glsl serves both backends with zero
// per-backend declaration ifdefs — the Phase 3 amendment (25) property, one
// level up. (Vertex-pulling stages are the exception: they declare an
// SSBO-shaped vertex block behind #ifdef OLO_VULKAN, compiled into this
// tier only via the OLO_VULKAN=1 macro.)
//
// UNIFORM SETTERS ARE NO-OPS. SetInt/SetFloat* exist for GL's default-block
// uniforms; Vulkan-side, every scalar reaches the shader through a root-data
// block. The setters intentionally do nothing (not warn — passes call them
// per frame on shared code paths).

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Shader.h"

#include <volk.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Declared in VulkanPipelineBuilder.h (which includes this header, so a
    // forward declaration breaks the cycle; the accessor's definition lives
    // in the .cpp, which includes the builder).
    struct VulkanRootDataLayout;

    // One reflected set/binding declaration — the pipeline builder's input
    // for the §4 mapping array.
    struct VulkanShaderBinding
    {
        enum class Kind : u8
        {
            UniformBuffer,
            StorageBuffer,
            CombinedImageSampler,
            StorageImage,
        };

        // Sampled-image dimensionality from SPIR-V reflection (#691 Phase 8).
        // The unfed-binding fallback must hand the shader a null texture whose
        // VIEW TYPE matches the sampler declaration — a 2D view under a
        // samplerCube is undefined under the descriptor heap, and the old
        // slot-0 fallback leaked whatever texture registered first (the loft
        // HDRI) into every unfed sampler. Meaningful only for
        // CombinedImageSampler bindings; buffers keep the default.
        enum class TexDim : u8
        {
            Tex2D,
            Tex2DArray,
            TexCube,
            TexCubeArray,
            Tex3D,
            // Multisampled declarations (sampler2DMS et al.) are tracked so
            // the fallback can REFUSE them loudly — no null MS image exists,
            // and quietly handing a single-sample null to an MS sampler is
            // exactly the wrong-view-type bug the typed fallback fixed.
            Tex2DMS,
            Tex2DMSArray,
        };

        u32 Set = 0;
        u32 Binding = 0;
        Kind BindingKind = Kind::UniformBuffer;
        TexDim ImageDim = TexDim::Tex2D;
        VkShaderStageFlags Stages = 0;
        std::string Name;
    };

    class VulkanShader final : public Shader
    {
      public:
        explicit VulkanShader(const std::string& filepath);
        VulkanShader(std::string name, const std::string& vertexSrc, const std::string& fragmentSrc);
        ~VulkanShader() override;

        VulkanShader(const VulkanShader&) = delete;
        VulkanShader& operator=(const VulkanShader&) = delete;
        VulkanShader(VulkanShader&&) = delete;
        VulkanShader& operator=(VulkanShader&&) = delete;

        void Bind() const override;
        void Unbind() const override;

        // Default-block uniforms do not exist on this backend — see header.
        void SetInt(const std::string& name, int value) const override;
        void SetIntArray(const std::string& name, int* values, u32 count) const override;
        void SetFloat(const std::string& name, f32 value) const override;
        void SetFloat2(const std::string& name, const glm::vec2& value) const override;
        void SetFloat3(const std::string& name, const glm::vec3& value) const override;
        void SetFloat4(const std::string& name, const glm::vec4& value) const override;
        void SetMat4(const std::string& name, const glm::mat4& value) const override;

        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0; // No native GL name to report (ADR 0011 amendment 49).
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

        [[nodiscard]] ShaderCompilationStatus GetCompilationStatus() const override
        {
            return m_Status;
        }
        [[nodiscard]] bool IsDeferredCapable() const override
        {
            return m_IsDeferredCapable;
        }

        // No GL-style uniform-block registry on this backend.
        ShaderResourceRegistry* GetResourceRegistry() override
        {
            return nullptr;
        }
        const ShaderResourceRegistry* GetResourceRegistry() const override
        {
            return nullptr;
        }

        // --- Pipeline-builder material -------------------------------------
        [[nodiscard]] VkShaderModule GetModule(VkShaderStageFlagBits stage) const;
        // Cached per compile/Reload: the draw front-end asks this on EVERY
        // draw (entry-point/stage-set agreement, issue #813), so it must not
        // be a per-draw map lookup.
        [[nodiscard]] bool HasMeshStage() const
        {
            return m_HasMeshStage;
        }
        [[nodiscard]] const std::vector<VulkanShaderBinding>& GetBindings() const
        {
            return m_Bindings;
        }
        // The shader's root-data layout, built lazily from the reflected
        // bindings and cached; a successful Reload/rebuild resets it (the
        // bindings may have changed). This is THE layout the draw path
        // assembles root structs against — one owner, so the writer and the
        // pipeline's mapping array cannot drift (#691 Phase 7).
        [[nodiscard]] const VulkanRootDataLayout& GetRootDataLayout();
        [[nodiscard]] const std::unordered_map<VkShaderStageFlagBits, std::vector<u32>>& GetSPIRV() const
        {
            return m_SPIRV;
        }
        // Stable key for the shader→pipeline reverse index (survives Reload,
        // amendment (12) — the handle's index does not change when the
        // modules are rebuilt).
        [[nodiscard]] u64 GetPipelineIndexKey() const
        {
            return m_RHIHandle.Get().Index;
        }

        // The shader whose Bind() ran last on the render thread — what the
        // draw-time pipeline lookup consumes. Null when none.
        [[nodiscard]] static VulkanShader* GetCurrentlyBound();

        // Forced device-object release for context teardown (#691 Phase 8):
        // a shader Ref surviving in some static (a library, a cached
        // material) would otherwise carry its VkShaderModules into
        // vkDestroyDevice (VUID-vkDestroyDevice-device-05137). The context
        // walks the root registry and releases every survivor's modules
        // here, turning the straggler into a harmless zombie — the eventual
        // destructor's DestroyModules() is idempotent on the cleared map.
        void ReleaseDeviceObjects()
        {
            DestroyModules();
        }

      private:
        // Shared ctor tail: compile-or-load every stage, reflect, create
        // modules. Returns false on failure (m_Status = Failed).
        [[nodiscard]] bool BuildFromSources(const std::unordered_map<VkShaderStageFlagBits, std::string>& sources,
                                            bool useCache);
        void DestroyModules();
        void ReflectStage(VkShaderStageFlagBits stage, const std::vector<u32>& spirv);

        [[nodiscard]] std::filesystem::path CachePathForStage(VkShaderStageFlagBits stage) const;
        [[nodiscard]] bool IsCacheStale(const std::filesystem::path& cachedPath) const;

        std::string m_Name;
        std::string m_FilePath;
        std::vector<std::string> m_IncludedFilePaths;
        std::unordered_map<VkShaderStageFlagBits, std::vector<u32>> m_SPIRV;
        std::unordered_map<VkShaderStageFlagBits, VkShaderModule> m_Modules;
        bool m_HasMeshStage = false; // mirrors m_Modules at commit time (#813)
        std::vector<VulkanShaderBinding> m_Bindings;
        // Lazily-built root layout (see GetRootDataLayout); reset whenever a
        // rebuild replaces m_Bindings. unique_ptr of a forward-declared type
        // — the out-of-line dtor already exists.
        std::unique_ptr<VulkanRootDataLayout> m_RootLayout;
        RHI::ScopedResourceHandle m_RHIHandle;
        ShaderCompilationStatus m_Status = ShaderCompilationStatus::Pending;
        bool m_IsDeferredCapable = false;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
