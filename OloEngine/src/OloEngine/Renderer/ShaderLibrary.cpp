#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderPack.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
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
        const auto name = shader->GetName();
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
        batch.m_FilePaths.Reserve(static_cast<i32>(count));
        for (const auto& path : filepaths)
        {
            batch.m_FilePaths.Emplace(path);
        }
        batch.m_Prepared.SetNum(static_cast<i32>(count));
        batch.m_IsPackLoaded.Init(false, static_cast<i32>(count));
        batch.m_PackEntries.SetNum(static_cast<i32>(count));

        // Shader packs are pre-compiled SPIR-V — a lookup + decode, not a
        // compile — so resolve them sequentially up front; only what's left
        // needs the parallel CPU-compile path. TryReadPackEntry() (unlike the
        // old TryLoadFromPack() this replaced here) makes NO GL call, so this
        // whole loop stays safe on whatever thread PrepareParallel() runs on
        // — the actual GL program is materialized later, in
        // FinalizeParallel(), which is contractually the render thread.
        TArray<FString> toCompile;
        TArray<sizet> toCompileIndices;
        toCompile.Reserve(static_cast<i32>(count));
        toCompileIndices.Reserve(static_cast<i32>(count));

        for (sizet i = 0; i < count; ++i)
        {
            if (auto entry = TryReadPackEntry(filepaths[i]))
            {
                batch.m_PackEntries[i] = std::move(*entry);
                batch.m_IsPackLoaded[i] = true;
                if (progressCounter != nullptr)
                {
                    progressCounter->fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                toCompile.Emplace(filepaths[i]);
                toCompileIndices.Emplace(i);
            }
        }

        if (!toCompile.IsEmpty())
        {
            TArray<Ref<Shader>> prepared = Shader::PrepareBatch(std::span{ toCompile.GetData(), static_cast<sizet>(toCompile.Num()) }, progressCounter);
            const sizet preparedCount = static_cast<sizet>(prepared.Num());
            for (sizet j = 0; j < preparedCount; ++j)
            {
                batch.m_Prepared[toCompileIndices[j]] = prepared[j];
            }
        }

        return batch;
    }

    TArray<Ref<Shader>> ShaderLibrary::FinalizeParallel(PreparedShaderBatch batch)
    {
        OLO_PROFILE_FUNCTION();

        // GL calls below — the caller contract (ShaderLibrary::FinalizeParallel)
        // requires this to run on the render thread. Materialize every pack-
        // loaded entry's GL program NOW (deferred from PrepareParallel() —
        // see PackEntryCPUData) before handing the batch to
        // Shader::FinalizeBatch, which skips indices already marked
        // m_IsPackLoaded and passes them through untouched.
        const sizet count = static_cast<sizet>(batch.m_Prepared.Num());
        for (sizet i = 0; i < count; ++i)
        {
            if (batch.m_IsPackLoaded[i])
            {
                batch.m_Prepared[i] = CreateShaderFromPackEntry(std::move(batch.m_PackEntries[i]));
            }
        }

        TArray<Ref<Shader>> finalized = Shader::FinalizeBatch(std::span{ batch.m_FilePaths.GetData(), static_cast<sizet>(batch.m_FilePaths.Num()) }, std::move(batch.m_Prepared), std::span{ batch.m_IsPackLoaded.GetData(), static_cast<sizet>(batch.m_IsPackLoaded.Num()) });

        // A null entry means PrepareBatch() caught an exception for that
        // shader (OpenGLShader::PrepareBatch) — dropping it silently is not
        // enough: Renderer3D::Init resolves every shader by name through an
        // unchecked Get(), which asserts on a missing entry (or, with asserts
        // compiled out, hands the caller a null Ref that crashes at draw
        // time). Register the fallback shader under the expected name so one
        // broken shader stays a visible magenta mesh instead of a startup
        // assert or a null dereference (issue #568's contract, extended to
        // this batch path).
        const sizet finalizedCount = static_cast<sizet>(finalized.Num());
        for (sizet i = 0; i < finalizedCount; ++i)
        {
            if (finalized[i])
            {
                Add(finalized[i]);
                continue;
            }

            const std::string name = std::filesystem::path(batch.m_FilePaths[i].ToStdString()).stem().string();
            OLO_CORE_ERROR("[ShaderLibrary] '{}' failed CPU preparation — registering the fallback shader", name);
            if (auto fallback = GetFallbackShader(); fallback && !Exists(name))
            {
                Add(name, fallback);
                finalized[i] = fallback;
            }
        }
        return finalized;
    }

    TArray<Ref<Shader>> ShaderLibrary::LoadParallel(const std::vector<std::string>& filepaths)
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
        const auto reloadStart = std::chrono::steady_clock::now();
        u32 failed = 0;
        for (auto& [name, shader] : m_Shaders)
        {
            // Reload() answers whether the NEW program is live: a failed reload
            // keeps the previous one and (on Vulkan) its previous status, so the
            // status alone cannot tell a kept shader from a rebuilt one.
            if (!shader->Reload())
                ++failed;
        }

        // The automation event bus (#1131): one `compile_finished` per library
        // reload, whichever path asked for it (the Shaders menu, the Shader
        // Debugger's Refresh All, the file watcher). Each shader that kept its
        // previous program counts as one error; there is no warning count.
        const auto seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - reloadStart).count();
        DiagnosticsEventLog::Get().RecordCompileFinished("shader", std::to_string(m_Shaders.size()) + " shaders",
                                                         failed == 0, failed, 0, seconds);
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
        //
        // The std::error_code overload: this runs on every engine startup,
        // so an exception here (a permissions error, a broken symlink, ...)
        // would be an uncaught throw straight out of Init() rather than a
        // graceful "no pack" — treat any query failure as "not present"
        // rather than crashing startup over an optional feature.
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec)
        {
            OLO_CORE_WARN("[ShaderLibrary] Couldn't check for a shader pack at '{}': {}", path.string(), ec.message());
            return;
        }
        if (!exists)
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
        if (!entry || entry->Stages.IsEmpty())
        {
            OLO_CORE_WARN("[ShaderLibrary] Pack entry '{}' loaded but empty", filepath);
            return std::nullopt;
        }

        PackEntryCPUData data;
        data.m_Name = entry->Name;
        data.m_FilePath = filepath;
        for (const auto& stage : entry->Stages)
        {
            if (stage.Stage < 1 || stage.Stage > 5)
            {
                OLO_CORE_ERROR("[ShaderLibrary] Unknown stage {} in pack entry '{}'", stage.Stage, filepath);
                return std::nullopt;
            }
        }
        data.m_Stages = std::move(entry->Stages);

        OLO_CORE_TRACE("[ShaderLibrary] Read '{}' from shader pack", filepath);
        return data;
    }

    // GL-touching: MUST run on the render thread.
    Ref<Shader> ShaderLibrary::CreateShaderFromPackEntry(PackEntryCPUData entry)
    {
        OLO_CORE_TRACE("[ShaderLibrary] Loading '{}' from shader pack", entry.m_FilePath.ToView());
        // Map the packed stage codes to backend stage keys, transferring the
        // owned word arrays without copying their compiled payloads.
        std::unordered_map<u32, TArray<u32>> vulkan;
        std::unordered_map<u32, TArray<u32>> openGL;
        constexpr u32 stages[]{ 0, 0x8B31, 0x8B30, 0x8E88, 0x8E87, 0x91B9 };
        for (auto& stage : entry.m_Stages)
        {
            vulkan[stages[stage.Stage]] = std::move(stage.VulkanSPIRV);
            openGL[stages[stage.Stage]] = std::move(stage.OpenGLSPIRV);
        }
        return OpenGLShader::CreateFromPackData(entry.m_Name.ToStdString(), entry.m_FilePath.ToStdString(),
                                                std::move(vulkan), std::move(openGL));
    }
} // namespace OloEngine
