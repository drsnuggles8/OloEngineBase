#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanComputeShader.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"

// Shared preprocessing, same Platform-to-Platform reuse VulkanShader records.
#include "Platform/OpenGL/OpenGLShader.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Relocated behind OLO_SHADER_CACHE_DIR (issue #906) — see
        // ShaderCachePaths::Root() and VulkanShader.cpp's matching helper.
        std::filesystem::path CacheDirectory()
        {
            return ShaderCachePaths::Root() / "vulkan";
        }
        // §3(b): the target env is part of the filename so this tier can never
        // cross-load with the GL path's vulkan_1_2 tier — or with the graphics
        // stages of this one.
        constexpr const char* kComputeCacheExtension = ".cached_vulkan14.comp";
        // Mirrors VulkanShader.cpp's kOptionsDescriptor — same option set, same
        // "keep this in sync with the shaderc calls below" contract (issue #906).
        constexpr std::string_view kOptionsDescriptor =
            "env=vulkan1.4;spirv=1.6;preserve_bindings=1;auto_bind_uniforms=0;"
            "debug_info=1;opt=performance;suppress_warnings=1;define=OLO_VULKAN=1";

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
        // resolve against assets/shaders/ and miss (found by the compute
        // VolumetricFog tenant, issue #691).
        const auto dirEnd = filepath.find_last_of("/\\");
        const std::string directory = (dirEnd != std::string::npos) ? filepath.substr(0, dirEnd) : "";
        const std::string source = OpenGLShader::ProcessIncludes(raw, directory);

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

        // Content-addressed key (issue #906): preprocessed source + the fixed
        // shaderc option set below — existence alone is validity, no mtime
        // staleness check needed.
        const std::string contentHash = std::format(
            "{:016x}", Hash::FNV1a64(preprocessedSource.data(), preprocessedSource.size(),
                                     Hash::FNV1a64(kOptionsDescriptor.data(), kOptionsDescriptor.size())));
        const std::filesystem::path cachePath =
            CacheDirectory() /
            (std::filesystem::path(m_FilePath.empty() ? m_Name : m_FilePath).filename().string() + "." +
             contentHash + kComputeCacheExtension);

        std::vector<u32> spirv;
        bool loaded = false;
        if (useCache && !m_FilePath.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(CacheDirectory(), ec); // best-effort

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

        if (!loaded)
        {
            // Options mirror VulkanShader::BuildFromSources exactly — same
            // tier, same OLO_VULKAN switch, same suppress-warnings rule.
            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
            options.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
            // Pin the SPIR-V dialect explicitly — same reason as
            // VulkanShader.cpp's matching call: an older shaderc can accept
            // the hand-encoded vulkan_1_4 env, fail to recognise it, and
            // silently fall back to an older SPIR-V dialect than production
            // ships (caught in review — this file was missing the pin its
            // graphics-stage sibling has).
            constexpr auto kShadercSpirv16 = static_cast<shaderc_spirv_version>((1u << 16) | (6u << 8));
            options.SetTargetSpirv(kShadercSpirv16);
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
                // Same dimensionality capture as VulkanShader::ReflectStage —
                // the unfed-binding null-texture fallback (#691).
                auto imageDim = VulkanShaderBinding::TexDim::Tex2D;
                if (kind == VulkanShaderBinding::Kind::CombinedImageSampler ||
                    kind == VulkanShaderBinding::Kind::StorageImage)
                {
                    const auto& type = reflector.get_type(resource.type_id);
                    switch (type.image.dim)
                    {
                        case spv::DimCube:
                            imageDim = type.image.arrayed ? VulkanShaderBinding::TexDim::TexCubeArray
                                                          : VulkanShaderBinding::TexDim::TexCube;
                            break;
                        case spv::Dim3D:
                            imageDim = VulkanShaderBinding::TexDim::Tex3D;
                            break;
                        default:
                            imageDim = type.image.arrayed ? VulkanShaderBinding::TexDim::Tex2DArray
                                                          : VulkanShaderBinding::TexDim::Tex2D;
                            break;
                    }
                }
                newBindings.push_back({ .Set = reflector.get_decoration(resource.id, spv::DecorationDescriptorSet),
                                        .Binding = reflector.get_decoration(resource.id, spv::DecorationBinding),
                                        .BindingKind = kind,
                                        .ImageDim = imageDim,
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
        m_RootLayoutBuilt.store(false, std::memory_order_release);

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
        const std::string source = OpenGLShader::ProcessIncludes(raw, directory);

        if (BuildFromSource(source, /*useCache=*/true))
        {
            const sizet invalidated = VulkanPipelineBuilder::Get().InvalidateShader(GetPipelineIndexKey());
            OLO_CORE_INFO("VulkanComputeShader '{}': reloaded ({} dependent pipeline(s) invalidated)", m_Name,
                          invalidated);
        }
    }

    void VulkanComputeShader::Bind() const
    {
        // Per recording context (#806): a RecordParallel item binds into its
        // own slot; the process-wide selection is the render thread's.
        if (VulkanWorkerRecordingContext* worker = CurrentVulkanWorkerContext(); worker != nullptr)
        {
            worker->CurrentComputeShader = const_cast<VulkanComputeShader*>(this);
            return;
        }
        s_CurrentlyBound = const_cast<VulkanComputeShader*>(this);
    }

    void VulkanComputeShader::Unbind() const
    {
        if (VulkanWorkerRecordingContext* worker = CurrentVulkanWorkerContext(); worker != nullptr)
        {
            if (worker->CurrentComputeShader == this)
            {
                worker->CurrentComputeShader = nullptr;
            }
            return;
        }
        if (s_CurrentlyBound == this)
        {
            s_CurrentlyBound = nullptr;
        }
    }

    VulkanComputeShader* VulkanComputeShader::GetCurrentlyBound()
    {
        if (const VulkanWorkerRecordingContext* worker = CurrentVulkanWorkerContext(); worker != nullptr)
        {
            return worker->CurrentComputeShader;
        }
        return s_CurrentlyBound;
    }

    const VulkanRootDataLayout& VulkanComputeShader::GetRootDataLayout()
    {
        // Fast path: built and published. The acquire pairs with the release
        // below so a worker that sees the flag also sees the layout's bytes.
        if (!m_RootLayoutBuilt.load(std::memory_order_acquire))
        {
            const std::scoped_lock lock(m_RootLayoutMutex);
            if (!m_RootLayoutBuilt.load(std::memory_order_relaxed))
            {
                m_RootLayout = std::make_unique<VulkanRootDataLayout>(VulkanRootDataLayout::Build(m_Bindings));
                m_RootLayoutBuilt.store(true, std::memory_order_release);
            }
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
