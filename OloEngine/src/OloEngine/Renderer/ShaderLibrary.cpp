#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderPack.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
    Ref<Shader> ShaderLibrary::s_FallbackShader = nullptr;

    ShaderLibrary::ShaderLibrary() = default;
    ShaderLibrary::~ShaderLibrary() = default;
    ShaderLibrary::ShaderLibrary(ShaderLibrary&&) noexcept = default;
    auto ShaderLibrary::operator=(ShaderLibrary&&) noexcept -> ShaderLibrary& = default;

    void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
    {
        OLO_CORE_ASSERT(!Exists(name), "Shader '{}' already exists!", name);
        m_Shaders[name] = shader;

        // Registration with ShaderDebugger is handled by FinalizeProgram
        // (OLO_SHADER_REGISTER_MANUAL).  Do NOT register here — it would
        // duplicate the entry for sync shaders and deadlock for async ones
        // (calling GetRendererID while the ShaderDebugger mutex is held).
    }

    void ShaderLibrary::Add(const Ref<Shader>& shader)
    {
        auto& name = shader->GetName();
        Add(name, shader);
    }

    Ref<Shader> ShaderLibrary::Load(const std::string& filepath)
    {
        // Try shader pack first (pre-compiled SPIR-V)
        if (auto shader = TryLoadFromPack(filepath))
        {
            Add(shader);
            return shader;
        }

        auto shader = Shader::Create(filepath);
        Add(shader);
        return shader;
    }

    Ref<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
    {
        if (auto shader = TryLoadFromPack(filepath))
        {
            Add(name, shader);
            return shader;
        }

        auto shader = Shader::Create(filepath);
        Add(name, shader);
        return shader;
    }

    ShaderLibrary::PreparedShaderBatch ShaderLibrary::PrepareParallel(const std::vector<std::string>& filepaths, std::atomic<u32>* progressCounter) const
    {
        OLO_PROFILE_FUNCTION();

        const sizet count = filepaths.size();
        PreparedShaderBatch batch;
        batch.m_FilePaths = filepaths;
        batch.m_Prepared.resize(count);
        batch.m_IsPackLoaded.assign(count, false);
        batch.m_PackEntries.resize(count);

        // Shader packs are pre-compiled SPIR-V — a lookup + decode, not a
        // compile — so resolve them sequentially up front; only what's left
        // needs the parallel CPU-compile path. TryReadPackEntry() (unlike the
        // old TryLoadFromPack() this replaced here) makes NO GL call, so this
        // whole loop stays safe on whatever thread PrepareParallel() runs on
        // — the actual GL program is materialized later, in
        // FinalizeParallel(), which is contractually the render thread.
        std::vector<std::string> toCompile;
        std::vector<sizet> toCompileIndices;
        toCompile.reserve(count);
        toCompileIndices.reserve(count);

        for (sizet i = 0; i < count; ++i)
        {
            if (auto entry = TryReadPackEntry(filepaths[i]))
            {
                batch.m_PackEntries[i] = std::move(entry);
                batch.m_IsPackLoaded[i] = true;
                if (progressCounter != nullptr)
                {
                    progressCounter->fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                toCompile.push_back(filepaths[i]);
                toCompileIndices.push_back(i);
            }
        }

        if (!toCompile.empty())
        {
            std::vector<Ref<Shader>> prepared = Shader::PrepareBatch(toCompile, progressCounter);
            const sizet preparedCount = prepared.size();
            for (sizet j = 0; j < preparedCount; ++j)
            {
                batch.m_Prepared[toCompileIndices[j]] = prepared[j];
            }
        }

        return batch;
    }

    std::vector<Ref<Shader>> ShaderLibrary::FinalizeParallel(PreparedShaderBatch batch)
    {
        OLO_PROFILE_FUNCTION();

        // GL calls below — the caller contract (ShaderLibrary::FinalizeParallel)
        // requires this to run on the render thread. Materialize every pack-
        // loaded entry's GL program NOW (deferred from PrepareParallel() —
        // see PackEntryCPUData) before handing the batch to
        // Shader::FinalizeBatch, which skips indices already marked
        // m_IsPackLoaded and passes them through untouched.
        const sizet count = batch.m_Prepared.size();
        for (sizet i = 0; i < count; ++i)
        {
            if (batch.m_IsPackLoaded[i])
            {
                batch.m_Prepared[i] = CreateShaderFromPackEntry(std::move(*batch.m_PackEntries[i]));
            }
        }

        std::vector<Ref<Shader>> finalized = Shader::FinalizeBatch(batch.m_FilePaths, std::move(batch.m_Prepared), batch.m_IsPackLoaded);

        // A null entry means PrepareBatch() caught an exception for that
        // shader (OpenGLShader::PrepareBatch) — dropping it silently is not
        // enough: Renderer3D::Init resolves every shader by name through an
        // unchecked Get(), which asserts on a missing entry (or, with asserts
        // compiled out, hands the caller a null Ref that crashes at draw
        // time). Register the fallback shader under the expected name so one
        // broken shader stays a visible magenta mesh instead of a startup
        // assert or a null dereference (issue #568's contract, extended to
        // this batch path).
        const sizet finalizedCount = finalized.size();
        for (sizet i = 0; i < finalizedCount; ++i)
        {
            if (finalized[i])
            {
                Add(finalized[i]);
                continue;
            }

            const std::string name = std::filesystem::path(batch.m_FilePaths[i]).stem().string();
            OLO_CORE_ERROR("[ShaderLibrary] '{}' failed CPU preparation — registering the fallback shader", name);
            if (auto fallback = GetFallbackShader(); fallback && !Exists(name))
            {
                Add(name, fallback);
                finalized[i] = fallback;
            }
        }
        return finalized;
    }

    std::vector<Ref<Shader>> ShaderLibrary::LoadParallel(const std::vector<std::string>& filepaths)
    {
        return FinalizeParallel(PrepareParallel(filepaths));
    }

    Ref<Shader> ShaderLibrary::Get(const std::string& name)
    {
        OLO_CORE_ASSERT(Exists(name), "Shader '{}' not found!", name);
        return m_Shaders[name];
    }

    void ShaderLibrary::ReloadShaders()
    {
        for (auto& [name, shader] : m_Shaders)
        {
            shader->Reload();
        }
    }

    bool ShaderLibrary::Exists(const std::string& name) const
    {
        return m_Shaders.contains(name);
    }

    std::vector<std::string> ShaderLibrary::GetAllShaderNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Shaders.size());
        for (const auto& [name, shader] : m_Shaders)
        {
            names.push_back(name);
        }
        return names;
    }

    // ====================================================================
    // Async shader compilation support
    // ====================================================================

    u32 ShaderLibrary::PollPendingShaders()
    {
        OLO_PROFILE_FUNCTION();

        u32 completed = 0;
        for (auto& [name, shader] : m_Shaders)
        {
            if (shader->GetCompilationStatus() == ShaderCompilationStatus::Compiling)
            {
                if (shader->PollCompilationStatus())
                {
                    ++completed;
                    // Now that the shader is finalized, initialize its resource registry
                    if (shader->IsReady())
                    {
                        auto* glShader = static_cast<OpenGLShader*>(shader.get());
                        glShader->InitializeResourceRegistry(shader);
                    }
                }
            }
            // Catch shaders that became Ready outside the library (e.g. via EnsureLinked/Bind)
            // but whose registry was never initialized.
            else if (shader->IsReady())
            {
                if (const auto* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                {
                    auto* glShader = static_cast<OpenGLShader*>(shader.get());
                    glShader->InitializeResourceRegistry(shader);
                    ++completed;
                }
            }
            else
            {
                // No additional handling required.
            }
        }
        return completed;
    }

    void ShaderLibrary::FlushPendingShaders()
    {
        OLO_PROFILE_FUNCTION();

        for (auto& [name, shader] : m_Shaders)
        {
            if (shader->GetCompilationStatus() == ShaderCompilationStatus::Compiling)
            {
                shader->EnsureLinked();
                if (shader->IsReady())
                {
                    auto* glShader = static_cast<OpenGLShader*>(shader.get());
                    glShader->InitializeResourceRegistry(shader);
                }
            }
            // Same guard as PollPendingShaders — catch ready-but-uninitialized.
            else if (shader->IsReady())
            {
                if (const auto* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                {
                    auto* glShader = static_cast<OpenGLShader*>(shader.get());
                    glShader->InitializeResourceRegistry(shader);
                }
            }
            else
            {
                // No additional handling required.
            }
        }
    }

    u32 ShaderLibrary::GetPendingCount() const
    {
        u32 count = 0;
        for (const auto& [name, shader] : m_Shaders)
        {
            if (shader->GetCompilationStatus() == ShaderCompilationStatus::Compiling || shader->GetCompilationStatus() == ShaderCompilationStatus::Pending)
            {
                ++count;
            }
            else if (shader->IsReady())
            {
                if (auto const* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                {
                    ++count;
                }
            }
            else
            {
                // No additional handling required.
            }
        }
        return count;
    }

    bool ShaderLibrary::HasPendingShaders() const
    {
        for (const auto& [name, shader] : m_Shaders)
        {
            if (auto status = shader->GetCompilationStatus(); status == ShaderCompilationStatus::Compiling || status == ShaderCompilationStatus::Pending)
                return true;
            if (shader->IsReady())
            {
                if (auto const* reg = shader->GetResourceRegistry(); reg && !reg->IsInitialized())
                    return true;
            }
        }
        return false;
    }

    // ====================================================================
    // Fallback shader — solid magenta, compiled synchronously
    // ====================================================================

    static constexpr const char* s_FallbackVertexSrc = R"glsl(
#version 450 core
layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjectionMatrix;
    mat4 u_ViewMatrix;
    mat4 u_ProjectionMatrix;
    vec4 u_CameraPosition;
};

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

void main()
{
    gl_Position = u_ViewProjectionMatrix * u_Model * vec4(a_Position, 1.0);
}
)glsl";

    static constexpr const char* s_FallbackFragmentSrc = R"glsl(
#version 450 core
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Octahedral encoding: maps a unit normal to [-1,1]^2
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return n.xy;
}

void main()
{
    // Magenta — instantly recognizable as "shader not ready"
    o_Color = vec4(1.0, 0.0, 1.0, 1.0);
    o_EntityID = u_EntityID;
    o_ViewNormal = octEncode(vec3(0.0, 0.0, 1.0));
}
)glsl";

    void ShaderLibrary::InitFallbackShader()
    {
        if (s_FallbackShader)
            return;

        s_FallbackShader = Shader::Create("__Fallback", s_FallbackVertexSrc, s_FallbackFragmentSrc);
        OLO_CORE_INFO("Fallback shader initialized (magenta)");
    }

    void ShaderLibrary::ShutdownFallbackShader()
    {
        s_FallbackShader.Reset();
    }

    Ref<Shader> ShaderLibrary::GetFallbackShader()
    {
        return s_FallbackShader;
    }

    // ====================================================================
    // Shader Pack support
    // ====================================================================

    void ShaderLibrary::LoadShaderPack(const std::filesystem::path& path)
    {
        // A CI-baked pack (issue #908) is optional — the common case (no
        // pack baked, or a fresh worktree that never fetched one) shouldn't
        // log a spurious "failed to load" from the ShaderPack constructor
        // trying to open a file that was never expected to exist. Checked
        // here, once, rather than duplicated at every LoadShaderPack call
        // site — Renderer2D::Init() and Renderer3D::Init() used to each
        // carry their own copy of this exact guard.
        if (!std::filesystem::exists(path))
        {
            return;
        }

        m_ShaderPack = std::make_unique<ShaderPack>(path);
        if (!m_ShaderPack->IsLoaded())
        {
            OLO_CORE_WARN("[ShaderLibrary] Shader pack failed to load: {}", path.string());
            m_ShaderPack.reset();
        }
    }

    bool ShaderLibrary::HasShaderPack() const
    {
        return m_ShaderPack && m_ShaderPack->IsLoaded();
    }

    Ref<Shader> ShaderLibrary::TryLoadFromPack(const std::string& filepath)
    {
        auto entry = TryReadPackEntry(filepath);
        if (!entry)
        {
            return nullptr;
        }
        return CreateShaderFromPackEntry(std::move(*entry));
    }

    // CPU-only: pack lookup + SPIR-V decode. See the class-level comment on
    // PackEntryCPUData — deliberately makes NO GL call (issue #907), unlike
    // the old TryLoadFromPack() this was split out of, which called straight
    // through to OpenGLShader::CreateFromPackData (glCreateProgram et al.).
    std::optional<ShaderLibrary::PackEntryCPUData> ShaderLibrary::TryReadPackEntry(const std::string& filepath) const
    {
        if (!m_ShaderPack || !m_ShaderPack->IsLoaded())
        {
            return std::nullopt;
        }

        if (!m_ShaderPack->Contains(filepath))
        {
            return std::nullopt;
        }

        // Content-hash validation (issue #908): a pack entry is only served
        // when its baked hash matches what the CURRENT on-disk source hashes
        // to right now — recomputed fresh, not trusted from the pack build. A
        // name match alone (the pre-#908 contract) would serve stale SPIR-V
        // for a shader that has since changed, silently — the exact "old
        // defect" a name-keyed cache used to have (#906's motivation for the
        // per-stage compile cache). Both sides call the SAME hash function
        // (OpenGLShader::ComputeContentHash), so two inputs that hash equal
        // ARE the same bytes shaderc would produce — no separate staleness
        // check needed, and a mismatch is unambiguously a miss.
        const std::string currentHash = OpenGLShader::ComputeContentHash(filepath);
        const auto packHash = m_ShaderPack->GetContentHash(filepath);
        if (currentHash.empty() || !packHash || *packHash != currentHash)
        {
            OLO_CORE_WARN("[ShaderLibrary] Pack entry '{}' content hash mismatch — "
                          "falling back to compile",
                          filepath);
            return std::nullopt;
        }

        auto entry = m_ShaderPack->LoadEntry(filepath);
        if (!entry || entry->Stages.empty())
        {
            OLO_CORE_WARN("[ShaderLibrary] Pack entry '{}' loaded but empty", filepath);
            return std::nullopt;
        }

        // Convert pack stage data (u8-encoded stages) back to GLenum-keyed maps
        PackEntryCPUData data;
        data.m_Name = entry->Name;
        data.m_FilePath = filepath;

        for (auto& stageData : entry->Stages)
        {
            u32 glStage = 0;
            switch (stageData.Stage)
            {
                case 1:
                    glStage = 0x8B31;
                    break; // GL_VERTEX_SHADER
                case 2:
                    glStage = 0x8B30;
                    break; // GL_FRAGMENT_SHADER
                case 3:
                    glStage = 0x8E88;
                    break; // GL_TESS_CONTROL_SHADER
                case 4:
                    glStage = 0x8E87;
                    break; // GL_TESS_EVALUATION_SHADER
                case 5:
                    glStage = 0x91B9;
                    break; // GL_COMPUTE_SHADER
                default:
                    OLO_CORE_ERROR("[ShaderLibrary] Unknown stage {} in pack entry '{}'", stageData.Stage, filepath);
                    return std::nullopt;
            }

            data.m_VulkanSPIRV[glStage] = std::move(stageData.VulkanSPIRV);
            data.m_OpenGLSPIRV[glStage] = std::move(stageData.OpenGLSPIRV);
        }

        OLO_CORE_TRACE("[ShaderLibrary] Read '{}' from shader pack", filepath);
        return data;
    }

    // GL-touching: MUST run on the render thread.
    Ref<Shader> ShaderLibrary::CreateShaderFromPackEntry(PackEntryCPUData entry)
    {
        OLO_CORE_TRACE("[ShaderLibrary] Loading '{}' from shader pack", entry.m_FilePath);
        return OpenGLShader::CreateFromPackData(entry.m_Name, entry.m_FilePath,
                                                std::move(entry.m_VulkanSPIRV),
                                                std::move(entry.m_OpenGLSPIRV));
    }
} // namespace OloEngine
