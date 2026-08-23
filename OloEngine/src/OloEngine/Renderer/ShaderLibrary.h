#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    // Forward declarations
    class Shader;
    class ShaderPack;

    class ShaderLibrary
    {
      public:
        ShaderLibrary();
        ~ShaderLibrary();

        ShaderLibrary(ShaderLibrary&&) noexcept;
        auto operator=(ShaderLibrary&&) noexcept -> ShaderLibrary&;

        ShaderLibrary(const ShaderLibrary&) = delete;
        auto operator=(const ShaderLibrary&) -> ShaderLibrary& = delete;

        void Add(const std::string& name, const Ref<Shader>& shader);
        void Add(const Ref<Shader>& shader);
        Ref<Shader> Load(const std::string& filepath);
        Ref<Shader> Load(const std::string& name, const std::string& filepath);

        // --- Batch loading with cross-shader CPU parallelism (issue #907) ---
        //
        // Result returned in the same order as `filepaths`, one entry per
        // path (regardless of whether it came from a shader pack or a fresh
        // compile). Every entry is added to this library under the name
        // derived from its filepath, same as Load(filepath).

        // CPU-decoded shader-pack entry (issue #907): the pack-lookup half of
        // the old TryLoadFromPack, with the GL-touching half
        // (CreateShaderFromPackEntry, below) split out — see PreparedShaderBatch.
        struct PackEntryCPUData
        {
            std::string m_Name;
            std::string m_FilePath;
            std::unordered_map<u32, std::vector<u32>> m_VulkanSPIRV;
            std::unordered_map<u32, std::vector<u32>> m_OpenGLSPIRV;
        };

        // Two-phase form: PrepareParallel() may run on ANY thread — it makes
        // NO GL call, including for a shader-pack hit: decoding a pack
        // entry's SPIR-V (TryReadPackEntry, below) is a pure CPU read, and
        // materializing the actual GL program from it is deferred to
        // FinalizeParallel(). FinalizeParallel() MUST run on the render
        // thread (it issues every GL program creation/link, pack-loaded or
        // freshly compiled alike). Split them yourself when you want to pump
        // a render loop (progress bar, window events) while PrepareParallel()
        // runs on a background task — see ShaderWarmup::LoadShadersParallel
        // for the pattern.
        struct PreparedShaderBatch
        {
            std::vector<Ref<Shader>> m_Prepared;                        // non-pack entries: CPU-prepared, GL not yet created. Pack entries: null until FinalizeParallel() materializes them from m_PackEntries.
            std::vector<bool> m_IsPackLoaded;                           // same size as m_Prepared — true where the entry came from a shader pack
            std::vector<std::optional<PackEntryCPUData>> m_PackEntries; // same size — decoded pack data for m_IsPackLoaded[i]==true entries, nullopt otherwise
        };
        PreparedShaderBatch PrepareParallel(const std::vector<std::string>& filepaths, std::atomic<u32>* progressCounter = nullptr);
        std::vector<Ref<Shader>> FinalizeParallel(PreparedShaderBatch batch);

        // Convenience one-call form: PrepareParallel() + FinalizeParallel()
        // with no progress polling, called synchronously on this thread
        // (which must then be the render thread). Use the two-phase form
        // above instead when a UI needs to stay responsive during the load.
        std::vector<Ref<Shader>> LoadParallel(const std::vector<std::string>& filepaths);

        Ref<Shader> Get(const std::string& name);

        void ReloadShaders();

        [[nodiscard("Store this!")]] bool Exists(const std::string& name) const;

        // Enumerate all loaded shader names (for editor/scripting)
        [[nodiscard]] std::vector<std::string> GetAllShaderNames() const;

        // --- Shader Pack support ---

        // Load a shader pack file. Future Load() calls will try the pack first.
        void LoadShaderPack(const std::filesystem::path& path);

        // Check whether a shader pack is loaded
        [[nodiscard]] bool HasShaderPack() const;

        // --- Async shader compilation support ---

        // Poll all shaders that are still compiling.
        // Returns the number of shaders that completed this frame.
        u32 PollPendingShaders();

        // Force all pending shaders to complete synchronously (for shutdown or sync points).
        void FlushPendingShaders();

        // Drop every owned shader Ref. Renderer shutdown must call this
        // before the graphics context dies: the library is a static member
        // (Renderer3D::m_ShaderLibrary), and shaders surviving to static
        // destruction leak their VkShaderModules into vkDestroyDevice
        // (VUID-vkDestroyDevice-device-05137, #691).
        void Clear()
        {
            m_Shaders.clear();
        }

        // Progress reporting
        [[nodiscard]] u32 GetTotalCount() const
        {
            return static_cast<u32>(m_Shaders.size());
        }
        [[nodiscard]] u32 GetPendingCount() const;
        [[nodiscard]] bool HasPendingShaders() const;

        // Fallback shader — compiled synchronously at startup, used when a real shader isn't ready
        static void InitFallbackShader();
        static void ShutdownFallbackShader();
        [[nodiscard]] static Ref<Shader> GetFallbackShader();

      private:
        // Try to create a shader from the loaded shader pack.
        // Returns nullptr if no pack or shader not in pack.
        Ref<Shader> TryLoadFromPack(const std::string& filepath);

        // CPU-only half of TryLoadFromPack (issue #907): pack lookup + SPIR-V
        // decode, no GL call — safe from any thread. Returns nullopt on the
        // same conditions TryLoadFromPack would have returned nullptr for.
        [[nodiscard]] std::optional<PackEntryCPUData> TryReadPackEntry(const std::string& filepath) const;

        // GL-touching half of TryLoadFromPack: materializes the actual GL
        // program from already-decoded pack data. MUST run on the render
        // thread.
        [[nodiscard]] static Ref<Shader> CreateShaderFromPackEntry(PackEntryCPUData entry);

        std::unordered_map<std::string, Ref<Shader>> m_Shaders;
        std::unique_ptr<ShaderPack> m_ShaderPack;

        static Ref<Shader> s_FallbackShader;
    };
} // namespace OloEngine
