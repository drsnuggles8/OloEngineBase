#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanComputeShader.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"

// Shared preprocessing, same Platform-to-Platform reuse VulkanShader records.
#include "Platform/OpenGL/OpenGLShader.h"

#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>

#include <filesystem>
#include <fstream>
#include <utility>

namespace OloEngine
{
    namespace
    {
        constexpr const char* kCacheDirectory = "assets/cache/shader/vulkan";
        // §3(b): the target env is part of the filename so this tier can never
        // cross-load with the GL path's vulkan_1_2 tier — or with the graphics
        // stages of this one.
        constexpr const char* kComputeCacheExtension = ".cached_vulkan14.comp";

        [[nodiscard]] std::string ReadWholeFile(const std::string& filepath)
        {
            std::ifstream in(filepath, std::ios::in | std::ios::binary);
            if (!in)
            {
                return {};
            }
            std::string contents;
            in.seekg(0, std::ios::end);
            contents.resize(static_cast<sizet>(in.tellg()));
            in.seekg(0, std::ios::beg);
            in.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            return contents;
        }
    } // namespace

    VulkanComputeShader::VulkanComputeShader(const std::string& filepath)
        : m_FilePath(filepath)
    {
        OLO_PROFILE_FUNCTION();

        m_Name = std::filesystem::path(filepath).stem().string();

        const std::string raw = ReadWholeFile(filepath);
        if (raw.empty())
        {
            OLO_CORE_ERROR("VulkanComputeShader '{}': cannot read '{}'", m_Name, filepath);
            return;
        }

        // Resolve #include against the .comp file's own directory (the GL
        // twin's rule, OpenGLComputeShader.cpp) — "" made `../include/*.glsl`
        // resolve against assets/shaders/ and miss (found by the Wave B
        // VolumetricFog tenant, issue #691 Phase 7).
        const auto dirEnd = filepath.find_last_of("/\\");
        const std::string directory = (dirEnd != std::string::npos) ? filepath.substr(0, dirEnd) : "";
        std::vector<std::string> includes;
        const std::string source = OpenGLShader::ProcessIncludes(raw, directory, includes);
        m_IncludedFilePaths = std::move(includes);
        std::sort(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end());
        m_IncludedFilePaths.erase(std::unique(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end()),
                                  m_IncludedFilePaths.end());

        (void)BuildFromSource(source, /*useCache=*/true);
    }

    VulkanComputeShader::VulkanComputeShader(std::string name, const std::string& source)
        : m_Name(std::move(name))
    {
        OLO_PROFILE_FUNCTION();
        (void)BuildFromSource(OpenGLShader::ProcessIncludes(source, ""), /*useCache=*/false);
    }

    VulkanComputeShader::~VulkanComputeShader()
    {
        if (s_CurrentlyBound == this)
        {
            s_CurrentlyBound = nullptr;
        }
        VulkanPipelineBuilder::Get().InvalidateShader(GetPipelineIndexKey());
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
        DestroyModule();
        m_RHIHandle.Reset();
    }

    bool VulkanComputeShader::BuildFromSource(const std::string& preprocessedSource, const bool useCache)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            OLO_CORE_ERROR("VulkanComputeShader '{}': no live VulkanDevice", m_Name);
            return false;
        }
        if (preprocessedSource.empty())
        {
            OLO_CORE_ERROR("VulkanComputeShader '{}': empty source", m_Name);
            return false;
        }

        const std::filesystem::path cachePath =
            std::filesystem::path(kCacheDirectory) /
            (std::filesystem::path(m_FilePath.empty() ? m_Name : m_FilePath).filename().string() +
             kComputeCacheExtension);

        std::vector<u32> spirv;
        bool loaded = false;
        if (useCache && !m_FilePath.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(kCacheDirectory, ec); // best-effort

            // mtime + transitive includes staleness — the VulkanShader rule.
            bool stale = true;
            const auto cacheTime = std::filesystem::last_write_time(cachePath, ec);
            if (!ec)
            {
                const auto sourceTime = std::filesystem::last_write_time(m_FilePath, ec);
                stale = ec || sourceTime > cacheTime;
                for (const auto& include : m_IncludedFilePaths)
                {
                    if (stale)
                    {
                        break;
                    }
                    const auto includeTime = std::filesystem::last_write_time(include, ec);
                    if (!ec && includeTime > cacheTime)
                    {
                        stale = true;
                    }
                }
            }
            if (!stale)
            {
                std::ifstream in(cachePath, std::ios::in | std::ios::binary | std::ios::ate);
                if (in)
                {
                    const auto size = static_cast<sizet>(in.tellg());
                    if (size > 0 && size % sizeof(u32) == 0)
                    {
                        spirv.resize(size / sizeof(u32));
                        in.seekg(0);
                        loaded = static_cast<bool>(
                            in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(size)));
                        if (!loaded)
                        {
                            spirv.clear();
                        }
                    }
                }
            }
        }

        if (!loaded)
        {
            // Options mirror VulkanShader::BuildFromSources exactly — same
            // tier, same OLO_VULKAN switch, same suppress-warnings rule.
            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
            options.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
            options.SetPreserveBindings(true);
            options.SetAutoBindUniforms(false);
            options.SetGenerateDebugInfo();
            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            options.SetSuppressWarnings();
            options.AddMacroDefinition("OLO_VULKAN", "1");

            const auto result = compiler.CompileGlslToSpv(preprocessedSource, shaderc_glsl_compute_shader,
                                                          m_FilePath.empty() ? m_Name.c_str() : m_FilePath.c_str(),
                                                          options);
            if (result.GetCompilationStatus() != shaderc_compilation_status_success)
            {
                OLO_CORE_ERROR("VulkanComputeShader '{}': {}", m_Name, result.GetErrorMessage());
                return false;
            }
            spirv.assign(result.cbegin(), result.cend());

            if (useCache && !m_FilePath.empty())
            {
                std::ofstream out(cachePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(reinterpret_cast<const char*>(spirv.data()),
                              static_cast<std::streamsize>(spirv.size() * sizeof(u32)));
                }
            }
        }

        // Build into locals, commit on success — a failed Reload must leave
        // the executing module and its reflection untouched (the VulkanShader
        // rule; the root layout derives from m_Bindings).
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spirv.size() * sizeof(u32);
        moduleInfo.pCode = spirv.data();
        VkShaderModule newModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device->GetDevice(), &moduleInfo, nullptr, &newModule) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanComputeShader '{}': vkCreateShaderModule failed", m_Name);
            return false;
        }

        std::vector<VulkanShaderBinding> newBindings;
        {
            const spirv_cross::Compiler reflector(spirv);
            const spirv_cross::ShaderResources resources = reflector.get_shader_resources();
            const auto append = [&](const spirv_cross::Resource& resource, VulkanShaderBinding::Kind kind)
            {
                newBindings.push_back({ .Set = reflector.get_decoration(resource.id, spv::DecorationDescriptorSet),
                                        .Binding = reflector.get_decoration(resource.id, spv::DecorationBinding),
                                        .BindingKind = kind,
                                        .Stages = VK_SHADER_STAGE_COMPUTE_BIT,
                                        .Name = resource.name });
            };
            for (const auto& resource : resources.uniform_buffers)
            {
                append(resource, VulkanShaderBinding::Kind::UniformBuffer);
            }
            for (const auto& resource : resources.storage_buffers)
            {
                append(resource, VulkanShaderBinding::Kind::StorageBuffer);
            }
            for (const auto& resource : resources.sampled_images)
            {
                append(resource, VulkanShaderBinding::Kind::CombinedImageSampler);
            }
            for (const auto& resource : resources.storage_images)
            {
                append(resource, VulkanShaderBinding::Kind::StorageImage);
            }
        }

        // Commit.
        DestroyModule();
        m_Module = newModule;
        m_SPIRV = std::move(spirv);
        m_Bindings = std::move(newBindings);
        m_RootLayout.reset();

        m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram,
                         static_cast<u64>(reinterpret_cast<std::uintptr_t>(m_Module)), RHI::Backend::Vulkan);
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::Shader, this);
        return true;
    }

    void VulkanComputeShader::DestroyModule()
    {
        auto* device = VulkanDevice::Get();
        if (m_Module != VK_NULL_HANDLE && device != nullptr)
        {
            vkDestroyShaderModule(device->GetDevice(), m_Module, nullptr);
        }
        m_Module = VK_NULL_HANDLE;
    }

    void VulkanComputeShader::Reload()
    {
        if (m_FilePath.empty())
        {
            return; // source-born shaders have nothing to re-read
        }
        const std::string raw = ReadWholeFile(m_FilePath);
        if (raw.empty())
        {
            OLO_CORE_ERROR("VulkanComputeShader '{}': reload cannot read '{}'", m_Name, m_FilePath);
            return;
        }
        const auto dirEnd = m_FilePath.find_last_of("/\\");
        const std::string directory = (dirEnd != std::string::npos) ? m_FilePath.substr(0, dirEnd) : "";
        std::vector<std::string> includes;
        const std::string source = OpenGLShader::ProcessIncludes(raw, directory, includes);
        m_IncludedFilePaths = std::move(includes);

        if (BuildFromSource(source, /*useCache=*/true))
        {
            const sizet invalidated = VulkanPipelineBuilder::Get().InvalidateShader(GetPipelineIndexKey());
            OLO_CORE_INFO("VulkanComputeShader '{}': reloaded ({} dependent pipeline(s) invalidated)", m_Name,
                          invalidated);
        }
    }

    void VulkanComputeShader::Bind() const
    {
        s_CurrentlyBound = const_cast<VulkanComputeShader*>(this);
    }

    void VulkanComputeShader::Unbind() const
    {
        if (s_CurrentlyBound == this)
        {
            s_CurrentlyBound = nullptr;
        }
    }

    VulkanComputeShader* VulkanComputeShader::GetCurrentlyBound()
    {
        return s_CurrentlyBound;
    }

    const VulkanRootDataLayout& VulkanComputeShader::GetRootDataLayout()
    {
        if (!m_RootLayout)
        {
            m_RootLayout = std::make_unique<VulkanRootDataLayout>(VulkanRootDataLayout::Build(m_Bindings));
        }
        return *m_RootLayout;
    }

    // Default-block uniforms do not exist on this backend (see header).
    void VulkanComputeShader::SetInt(const std::string&, int) const {}
    void VulkanComputeShader::SetUint(const std::string&, u32) const {}
    void VulkanComputeShader::SetIntArray(const std::string&, int*, u32) const {}
    void VulkanComputeShader::SetFloat(const std::string&, f32) const {}
    void VulkanComputeShader::SetFloat2(const std::string&, const glm::vec2&) const {}
    void VulkanComputeShader::SetFloat3(const std::string&, const glm::vec3&) const {}
    void VulkanComputeShader::SetFloat4(const std::string&, const glm::vec4&) const {}
    void VulkanComputeShader::SetMat4(const std::string&, const glm::mat4&) const {}
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
