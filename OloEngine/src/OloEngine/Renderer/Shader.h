#pragma once

#include <string>
#include <unordered_map>

#include "OloEngine/Core/Ref.h"
#include <glm/glm.hpp>
#include "OloEngine/Asset/AssetTypes.h"
#include "RendererResource.h"
#include "ShaderLibrary.h" // Include the new ShaderLibrary header

namespace OloEngine
{
    // Forward declaration
    class ShaderResourceRegistry;

    // Tracks the lifecycle of a shader through async compilation
    enum class ShaderCompilationStatus : u8
    {
        Pending,   // CPU work not yet started
        Compiling, // GPU link issued, waiting for driver
        Ready,     // Fully linked & finalized — safe to bind
        Failed     // Compilation or link error
    };

    class Shader : public RendererResource
    {
      public:
        virtual ~Shader() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetInt(const std::string& name, int value) const = 0;
        virtual void SetIntArray(const std::string& name, int* values, u32 count) const = 0;
        virtual void SetFloat(const std::string& name, f32 value) const = 0;
        virtual void SetFloat2(const std::string& name, const glm::vec2& value) const = 0;
        virtual void SetFloat3(const std::string& name, const glm::vec3& value) const = 0;
        virtual void SetFloat4(const std::string& name, const glm::vec4& value) const = 0;
        virtual void SetMat4(const std::string& name, const glm::mat4& value) const = 0;

        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        [[nodiscard("Store this!")]] virtual const std::string& GetName() const = 0;
        [[nodiscard("Store this!")]] virtual const std::string& GetFilePath() const = 0;

        virtual void Reload() = 0;

        // --- Async compilation status ---
        [[nodiscard]] virtual ShaderCompilationStatus GetCompilationStatus() const
        {
            return ShaderCompilationStatus::Ready;
        }
        [[nodiscard]] virtual bool IsReady() const
        {
            return GetCompilationStatus() == ShaderCompilationStatus::Ready;
        }

        // Poll driver for link completion (call once per frame for pending shaders).
        // Returns true when the shader transitions to Ready or Failed.
        virtual bool PollCompilationStatus()
        {
            return true;
        }

        // Block until the shader is fully linked (lazy finalization on first Bind).
        virtual void EnsureLinked() {}

        // Resource registry access (safe interface)
        virtual ShaderResourceRegistry* GetResourceRegistry() = 0;
        virtual const ShaderResourceRegistry* GetResourceRegistry() const = 0;

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Shader;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        static Ref<Shader> Create(const std::string& filepath);
        static Ref<Shader> Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
    };
} // namespace OloEngine
