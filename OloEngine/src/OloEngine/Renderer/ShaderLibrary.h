#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    // Forward declaration of Shader class
    class Shader;

    class ShaderLibrary
    {
      public:
        void Add(const std::string& name, const Ref<Shader>& shader);
        void Add(const Ref<Shader>& shader);
        Ref<Shader> Load(const std::string& filepath);
        Ref<Shader> Load(const std::string& name, const std::string& filepath);

        Ref<Shader> Get(const std::string& name);

        void ReloadShaders();

        [[nodiscard("Store this!")]] bool Exists(const std::string& name) const;

        // Enumerate all loaded shader names (for editor/scripting)
        [[nodiscard]] std::vector<std::string> GetAllShaderNames() const;

        // --- Async shader compilation support ---

        // Poll all shaders that are still compiling.
        // Returns the number of shaders that completed this frame.
        u32 PollPendingShaders();

        // Force all pending shaders to complete synchronously (for shutdown or sync points).
        void FlushPendingShaders();

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
        std::unordered_map<std::string, Ref<Shader>> m_Shaders;

        static Ref<Shader> s_FallbackShader;
    };
} // namespace OloEngine
