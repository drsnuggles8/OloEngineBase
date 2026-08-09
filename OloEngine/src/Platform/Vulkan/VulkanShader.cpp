#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"

// Shared preprocessing: the #include resolution (and its include-path capture
// for staleness) is shader-generic and deliberately reused from the GL class
// rather than reimplemented — OpenGLComputeShader set the precedent. This is a
// Platform-to-Platform include inside the backend layer, not a neutral-layer
// leak (the ratchet polices OloEngine/* → Platform/*, not backends helping
// each other).
#include "Platform/OpenGL/OpenGLShader.h"

#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_cross.hpp>

#include <fstream>
#include <utility>

namespace OloEngine
{
    namespace
    {
        VulkanShader* s_CurrentlyBound = nullptr; // render thread only

        constexpr const char* kCacheDirectory = "assets/cache/shader/vulkan";

        [[nodiscard]] const char* StageCacheExtension(VkShaderStageFlagBits stage)
        {
            // The shaderc target env is part of the name (ADR 0011 §3(b)):
            // this tier targets vulkan_1_4 and can never be confused with the
            // GL path's vulkan_1_2 tier.
            switch (stage)
            {
                case VK_SHADER_STAGE_VERTEX_BIT:
                    return ".cached_vulkan14.vert";
                case VK_SHADER_STAGE_FRAGMENT_BIT:
                    return ".cached_vulkan14.frag";
                case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
                    return ".cached_vulkan14.tesc";
                case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
                    return ".cached_vulkan14.tese";
                default:
                    OLO_CORE_ASSERT(false, "Unsupported shader stage");
                    return ".cached_vulkan14.unknown";
            }
        }

        [[nodiscard]] shaderc_shader_kind StageToShaderC(VkShaderStageFlagBits stage)
        {
            switch (stage)
            {
                case VK_SHADER_STAGE_VERTEX_BIT:
                    return shaderc_glsl_vertex_shader;
                case VK_SHADER_STAGE_FRAGMENT_BIT:
                    return shaderc_glsl_fragment_shader;
                case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
                    return shaderc_glsl_tess_control_shader;
                case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
                    return shaderc_glsl_tess_evaluation_shader;
                default:
                    OLO_CORE_ASSERT(false, "Unsupported shader stage");
                    return shaderc_glsl_vertex_shader;
            }
        }

        [[nodiscard]] const char* StageName(VkShaderStageFlagBits stage)
        {
            switch (stage)
            {
                case VK_SHADER_STAGE_VERTEX_BIT:
                    return "vertex";
                case VK_SHADER_STAGE_FRAGMENT_BIT:
                    return "fragment";
                case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
                    return "tess_control";
                case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
                    return "tess_evaluation";
                default:
                    return "unknown";
            }
        }

        // #type splitting — mirrors OpenGLShader::PreProcess's marker set
        // (vertex, fragment|pixel, tess_control|tesscontrol,
        // tess_evaluation|tesseval; no geometry, compute lives elsewhere).
        [[nodiscard]] std::unordered_map<VkShaderStageFlagBits, std::string> SplitStages(const std::string& source)
        {
            std::unordered_map<VkShaderStageFlagBits, std::string> stages;
            static constexpr const char* kToken = "#type";
            const sizet tokenLength = std::strlen(kToken);

            sizet pos = source.find(kToken, 0);
            while (pos != std::string::npos)
            {
                const sizet eol = source.find_first_of("\r\n", pos);
                if (eol == std::string::npos)
                {
                    OLO_CORE_ERROR("VulkanShader: #type line has no newline");
                    break;
                }
                // Tolerate any run of spaces/tabs after #type (the GL
                // preprocessor accepts "#type  vertex"; acceptance must not
                // be backend-dependent).
                sizet begin = pos + tokenLength;
                while (begin < eol && (source[begin] == ' ' || source[begin] == '\t'))
                {
                    ++begin;
                }
                std::string type = source.substr(begin, eol - begin);
                // Trim trailing whitespace/CR.
                while (!type.empty() && (type.back() == ' ' || type.back() == '\r' || type.back() == '\t'))
                {
                    type.pop_back();
                }

                VkShaderStageFlagBits stage{};
                if (type == "vertex")
                    stage = VK_SHADER_STAGE_VERTEX_BIT;
                else if (type == "fragment" || type == "pixel")
                    stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                else if (type == "tess_control" || type == "tesscontrol")
                    stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
                else if (type == "tess_evaluation" || type == "tesseval")
                    stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
                else
                {
                    OLO_CORE_ERROR("VulkanShader: unknown #type '{}'", type);
                    return {};
                }

                const sizet nextLinePos = source.find_first_not_of("\r\n", eol);
                if (nextLinePos == std::string::npos)
                {
                    // A trailing #type with no body: record an empty stage —
                    // shaderc rejects it with a proper diagnostic instead of
                    // substr(npos) throwing out of the constructor.
                    stages[stage] = {};
                    break;
                }
                pos = source.find(kToken, nextLinePos);
                stages[stage] = (pos == std::string::npos)
                                    ? source.substr(nextLinePos)
                                    : source.substr(nextLinePos, pos - nextLinePos);
            }
            return stages;
        }

        [[nodiscard]] std::string ReadWholeFile(const std::string& filepath)
        {
            std::ifstream in(filepath, std::ios::in | std::ios::binary);
            if (!in)
            {
                OLO_CORE_ERROR("VulkanShader: could not open '{}'", filepath);
                return {};
            }
            in.seekg(0, std::ios::end);
            const auto size = in.tellg();
            if (size < 0)
            {
                OLO_CORE_ERROR("VulkanShader: could not size '{}'", filepath);
                return {};
            }
            std::string result(static_cast<sizet>(size), '\0');
            in.seekg(0, std::ios::beg);
            in.read(result.data(), static_cast<std::streamsize>(result.size()));
            return result;
        }

        [[nodiscard]] u64 VkHandleToKey(VkShaderModule module)
        {
            return reinterpret_cast<u64>(module); // NOLINT(performance-no-int-to-ptr) non-dispatchable handle is 64-bit
        }
    } // namespace

    VulkanShader::VulkanShader(const std::string& filepath) : m_FilePath(filepath)
    {
        OLO_PROFILE_FUNCTION();

        const std::filesystem::path path(filepath);
        m_Name = path.stem().string();

        const std::string raw = ReadWholeFile(filepath);
        if (raw.empty())
        {
            m_Status = ShaderCompilationStatus::Failed;
            return;
        }

        // Split FIRST, then resolve includes per stage with a fresh include
        // set — two stages including the same header is normal, not circular
        // (the OpenGLShader::PreProcess ordering rule).
        auto stages = SplitStages(raw);
        m_IncludedFilePaths.clear();
        for (auto& [stage, source] : stages)
        {
            std::vector<std::string> stageIncludes;
            source = OpenGLShader::ProcessIncludes(source, "", stageIncludes);
            m_IncludedFilePaths.insert(m_IncludedFilePaths.end(), stageIncludes.begin(), stageIncludes.end());
        }
        std::sort(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end());
        m_IncludedFilePaths.erase(std::unique(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end()),
                                  m_IncludedFilePaths.end());

        if (BuildFromSources(stages, /*useCache=*/true))
        {
            m_Status = ShaderCompilationStatus::Ready;
        }
    }

    VulkanShader::VulkanShader(std::string name, const std::string& vertexSrc, const std::string& fragmentSrc)
        : m_Name(std::move(name))
    {
        OLO_PROFILE_FUNCTION();
        const std::unordered_map<VkShaderStageFlagBits, std::string> stages = {
            { VK_SHADER_STAGE_VERTEX_BIT, OpenGLShader::ProcessIncludes(vertexSrc, "") },
            { VK_SHADER_STAGE_FRAGMENT_BIT, OpenGLShader::ProcessIncludes(fragmentSrc, "") },
        };
        if (BuildFromSources(stages, /*useCache=*/false))
        {
            m_Status = ShaderCompilationStatus::Ready;
        }
    }

    VulkanShader::~VulkanShader()
    {
        if (s_CurrentlyBound == this)
        {
            s_CurrentlyBound = nullptr;
        }
        // Dependent pipelines are deferred-destroyed; modules may then go
        // immediately (the spec allows destroying a module once its pipelines
        // are created, and lazily-recreated pipelines belong to the NEW
        // modules after a reload).
        VulkanPipelineBuilder::Get().InvalidateShader(GetPipelineIndexKey());
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
        DestroyModules();
        m_RHIHandle.Reset();
    }

    std::filesystem::path VulkanShader::CachePathForStage(VkShaderStageFlagBits stage) const
    {
        return std::filesystem::path(kCacheDirectory) /
               (std::filesystem::path(m_FilePath).filename().string() + StageCacheExtension(stage));
    }

    bool VulkanShader::IsCacheStale(const std::filesystem::path& cachedPath) const
    {
        std::error_code ec;
        const auto cacheTime = std::filesystem::last_write_time(cachedPath, ec);
        if (ec)
        {
            return true;
        }
        const auto sourceTime = std::filesystem::last_write_time(m_FilePath, ec);
        if (ec || sourceTime > cacheTime)
        {
            return true;
        }
        for (const auto& include : m_IncludedFilePaths)
        {
            const auto includeTime = std::filesystem::last_write_time(include, ec);
            if (ec)
            {
                continue; // A vanished include does not invalidate (GL-tier rule).
            }
            if (includeTime > cacheTime)
            {
                return true;
            }
        }
        return false;
    }

    bool VulkanShader::BuildFromSources(const std::unordered_map<VkShaderStageFlagBits, std::string>& sources,
                                        bool useCache)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            OLO_CORE_ERROR("VulkanShader '{}': no live VulkanDevice", m_Name);
            m_Status = ShaderCompilationStatus::Failed;
            return false;
        }
        if (sources.empty())
        {
            OLO_CORE_ERROR("VulkanShader '{}': no stages found", m_Name);
            m_Status = ShaderCompilationStatus::Failed;
            return false;
        }

        std::error_code ec;
        if (useCache)
        {
            std::filesystem::create_directories(kCacheDirectory, ec); // best-effort
        }

        std::unordered_map<VkShaderStageFlagBits, std::vector<u32>> spirv;
        for (const auto& [stage, source] : sources)
        {
            const auto cachePath = CachePathForStage(stage);
            bool loaded = false;
            if (useCache && !m_FilePath.empty() && !IsCacheStale(cachePath))
            {
                std::ifstream in(cachePath, std::ios::in | std::ios::binary | std::ios::ate);
                if (in)
                {
                    const auto size = static_cast<sizet>(in.tellg());
                    if (size > 0 && size % sizeof(u32) == 0)
                    {
                        auto& data = spirv[stage];
                        data.resize(size / sizeof(u32));
                        in.seekg(0);
                        loaded = static_cast<bool>(in.read(reinterpret_cast<char*>(data.data()),
                                                           static_cast<std::streamsize>(size)));
                        if (!loaded)
                        {
                            spirv.erase(stage);
                        }
                    }
                }
            }
            if (loaded)
            {
                continue;
            }

            // Options mirror the GL tier (OpenGLShader::CompileOrGetVulkanBinaries)
            // with two deliberate differences: the vulkan_1_4 target env
            // (§3(b) — this backend's own tier), and the OLO_VULKAN macro (the
            // one authoring-side switch, used by vertex-pulling stages).
            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            // The vulkan_1_4 enumerator exists only in SDK-current shaderc
            // (the Windows toolchain floor); an older system shaderc (Linux
            // CI) lacks the NAME but the encoding is fixed — env versions are
            // VK_MAKE_API_VERSION(0, major, minor, 0), i.e. (1<<22)|(4<<12)
            // for 1.4 (see shaderc/env.h). Runtime behaviour on an old
            // shaderc is moot: no ADR 0010 contract device exists on a box
            // whose toolchain predates the 1.4 SDK, so those builds only need
            // to COMPILE (the device-gated tests SKIP).
            constexpr auto kShadercEnvVulkan14 = static_cast<shaderc_env_version>((1u << 22) | (4u << 12));
            static_assert(kShadercEnvVulkan14 == ((1u << 22) | (4u << 12)));
            options.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
            options.SetPreserveBindings(true);
            options.SetAutoBindUniforms(false);
            options.SetGenerateDebugInfo();
            options.SetOptimizationLevel(shaderc_optimization_level_performance);
            // Load-bearing, not cosmetic: shaderc's message parser asserts on
            // malformed glslang warning strings (same rule as the GL tier).
            options.SetSuppressWarnings();
            options.AddMacroDefinition("OLO_VULKAN", "1");

            const auto result = compiler.CompileGlslToSpv(source, StageToShaderC(stage),
                                                          m_FilePath.empty() ? m_Name.c_str() : m_FilePath.c_str(),
                                                          options);
            if (result.GetCompilationStatus() != shaderc_compilation_status_success)
            {
                OLO_CORE_ERROR("VulkanShader '{}' ({} stage): {}", m_Name, StageName(stage), result.GetErrorMessage());
                m_Status = ShaderCompilationStatus::Failed;
                return false;
            }
            auto& data = spirv[stage];
            data.assign(result.cbegin(), result.cend());

            if (useCache && !m_FilePath.empty())
            {
                std::ofstream out(cachePath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(data.size() * sizeof(u32)));
                }
            }
        }

        // Modules and reflection are built into LOCALS and committed only
        // when everything succeeded: a partial failure on a Reload must leave
        // m_Bindings describing the modules still in use — the root-data
        // layout is derived from GetBindings(), and a mismatch writes offsets
        // the executing SPIR-V does not read (silently, via memcpy).
        std::unordered_map<VkShaderStageFlagBits, VkShaderModule> newModules;
        const auto destroyNewModules = [&]
        {
            for (auto& [stage, module] : newModules)
            {
                if (module != VK_NULL_HANDLE)
                {
                    vkDestroyShaderModule(device->GetDevice(), module, nullptr);
                }
            }
            newModules.clear();
        };
        for (const auto& [stage, data] : spirv)
        {
            VkShaderModuleCreateInfo moduleInfo{};
            moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            moduleInfo.codeSize = data.size() * sizeof(u32);
            moduleInfo.pCode = data.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device->GetDevice(), &moduleInfo, nullptr, &module) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanShader '{}': vkCreateShaderModule failed ({} stage)", m_Name, StageName(stage));
                destroyNewModules();
                m_Status = ShaderCompilationStatus::Failed;
                return false;
            }
            newModules[stage] = module;
        }

        // Reflection (spirv_cross can throw on a corrupt cached blob — that
        // too must not tear the committed state).
        auto oldBindings = std::move(m_Bindings);
        const bool oldDeferredCapable = m_IsDeferredCapable;
        m_Bindings.clear();
        m_IsDeferredCapable = false;
        try
        {
            for (const auto& [stage, data] : spirv)
            {
                ReflectStage(stage, data);
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanShader '{}': reflection failed ({})", m_Name, e.what());
            destroyNewModules();
            m_Bindings = std::move(oldBindings);
            m_IsDeferredCapable = oldDeferredCapable;
            m_Status = ShaderCompilationStatus::Failed;
            return false;
        }

        // Commit — nothing below can fail.
        m_SPIRV = std::move(spirv);
        m_Modules = std::move(newModules);
        // The bindings just changed hands: any cached root layout describes
        // the OLD reflection and must rebuild on next use (#691 Phase 7).
        m_RootLayout.reset();

        // Identity: minted once, survives Reload (amendment (12) — the
        // reverse-index key and every cached reference stay valid while the
        // modules behind them are replaced).
        const auto vertexIt = m_Modules.find(VK_SHADER_STAGE_VERTEX_BIT);
        const u64 native = vertexIt != m_Modules.end() ? VkHandleToKey(vertexIt->second)
                                                       : VkHandleToKey(m_Modules.begin()->second);
        m_RHIHandle.Sync(RHI::ResourceKind::ShaderProgram, native, RHI::Backend::Vulkan);
        // Root-object registration so BindShaderProgram packets can resolve
        // the handle back to this shader (#691 Phase 7). Identity survives
        // Reload, so re-registering refreshes the same entry.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::Shader, this);
        return true;
    }

    const VulkanRootDataLayout& VulkanShader::GetRootDataLayout()
    {
        if (!m_RootLayout)
        {
            m_RootLayout = std::make_unique<VulkanRootDataLayout>(VulkanRootDataLayout::Build(m_Bindings));
        }
        return *m_RootLayout;
    }

    void VulkanShader::ReflectStage(VkShaderStageFlagBits stage, const std::vector<u32>& spirv)
    {
        const spirv_cross::Compiler compiler(spirv);
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        const auto append = [&](const spirv_cross::Resource& resource, VulkanShaderBinding::Kind kind)
        {
            const u32 set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            for (auto& existing : m_Bindings)
            {
                if (existing.Set == set && existing.Binding == binding && existing.BindingKind == kind)
                {
                    existing.Stages |= static_cast<VkShaderStageFlags>(stage);
                    return;
                }
            }
            m_Bindings.push_back({ .Set = set,
                                   .Binding = binding,
                                   .BindingKind = kind,
                                   .Stages = static_cast<VkShaderStageFlags>(stage),
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

        // Deferred-capability marker set — mirrors OpenGLShader::Reflect's
        // criteria (pinned against the raw-GLSL scan by
        // ShaderDeferredCapabilityTest on the GL side).
        if (stage == VK_SHADER_STAGE_FRAGMENT_BIT)
        {
            for (const auto& output : resources.stage_outputs)
            {
                const std::string& name = output.name;
                if (name.rfind("o_GBuffer", 0) == 0 || name == "gAlbedo" || name == "gNormalRoughAO" ||
                    name == "gEmissive")
                {
                    m_IsDeferredCapable = true;
                    break;
                }
            }
        }
    }

    void VulkanShader::DestroyModules()
    {
        auto* device = VulkanDevice::Get();
        for (auto& [stage, module] : m_Modules)
        {
            if (module != VK_NULL_HANDLE && device != nullptr)
            {
                vkDestroyShaderModule(device->GetDevice(), module, nullptr);
            }
        }
        m_Modules.clear();
    }

    VkShaderModule VulkanShader::GetModule(VkShaderStageFlagBits stage) const
    {
        const auto it = m_Modules.find(stage);
        return it != m_Modules.end() ? it->second : VK_NULL_HANDLE;
    }

    void VulkanShader::Bind() const
    {
        // No program object to bind — the pipeline binds at draw time. Record
        // the selection for the draw-time pipeline lookup.
        s_CurrentlyBound = const_cast<VulkanShader*>(this);
    }

    void VulkanShader::Unbind() const
    {
        if (s_CurrentlyBound == this)
        {
            s_CurrentlyBound = nullptr;
        }
    }

    VulkanShader* VulkanShader::GetCurrentlyBound()
    {
        return s_CurrentlyBound;
    }

    void VulkanShader::Reload()
    {
        OLO_PROFILE_FUNCTION();
        if (m_FilePath.empty())
        {
            return; // Source-string shaders have nothing to re-read.
        }

        const std::string raw = ReadWholeFile(m_FilePath);
        if (raw.empty())
        {
            return;
        }
        auto stages = SplitStages(raw);
        m_IncludedFilePaths.clear();
        for (auto& [stage, source] : stages)
        {
            std::vector<std::string> stageIncludes;
            source = OpenGLShader::ProcessIncludes(source, "", stageIncludes);
            m_IncludedFilePaths.insert(m_IncludedFilePaths.end(), stageIncludes.begin(), stageIncludes.end());
        }
        std::sort(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end());
        m_IncludedFilePaths.erase(std::unique(m_IncludedFilePaths.begin(), m_IncludedFilePaths.end()),
                                  m_IncludedFilePaths.end());

        // Dependent pipelines are invalidated only AFTER a successful rebuild
        // (deferred destruction, §3(d)); the old modules keep the
        // failed-reload path safe — only on success do they get replaced.
        const ShaderCompilationStatus previousStatus = m_Status;
        auto oldModules = std::move(m_Modules);
        m_Modules.clear();
        if (!BuildFromSources(stages, /*useCache=*/true))
        {
            // Failed reload: keep the old modules and pipelines working (the
            // GL path's restore rule) — and the old STATUS: forcing Ready on
            // a shader that never built would let the pilot accept a
            // zero-module shader.
            m_Modules = std::move(oldModules);
            m_Status = previousStatus;
            OLO_CORE_ERROR("VulkanShader '{}': reload failed — keeping the previous modules", m_Name);
            return;
        }

        const sizet invalidated = VulkanPipelineBuilder::Get().InvalidateShader(GetPipelineIndexKey());
        auto* device = VulkanDevice::Get();
        for (auto& [stage, module] : oldModules)
        {
            if (module != VK_NULL_HANDLE && device != nullptr)
            {
                vkDestroyShaderModule(device->GetDevice(), module, nullptr);
            }
        }
        m_Status = ShaderCompilationStatus::Ready;
        OLO_CORE_INFO("VulkanShader '{}': reloaded ({} dependent pipeline(s) invalidated, lazy recreation)",
                      m_Name, invalidated);
    }

    // Default-block uniforms do not exist on this backend (see header).
    void VulkanShader::SetInt(const std::string&, int) const {}
    void VulkanShader::SetIntArray(const std::string&, int*, u32) const {}
    void VulkanShader::SetFloat(const std::string&, f32) const {}
    void VulkanShader::SetFloat2(const std::string&, const glm::vec2&) const {}
    void VulkanShader::SetFloat3(const std::string&, const glm::vec3&) const {}
    void VulkanShader::SetFloat4(const std::string&, const glm::vec4&) const {}
    void VulkanShader::SetMat4(const std::string&, const glm::mat4&) const {}
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
