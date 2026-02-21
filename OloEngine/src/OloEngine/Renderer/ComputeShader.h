#pragma once

#include <string>
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Renderer/RendererResource.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Compute shader abstraction.
    //
    // A compute shader is a single-stage programmable shader used for general-purpose
    // GPU computation (GPGPU). Unlike vertex/fragment shaders, compute shaders do not
    // participate in the rasterization pipeline; they are dispatched explicitly via
    // RendererAPI::DispatchCompute(). The source file should contain a single GLSL
    // compute shader (no #type stage tags required).
    class ComputeShader : public RendererResource
    {
      public:
        virtual ~ComputeShader() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetInt(const std::string& name, int value) const = 0;
        virtual void SetUint(const std::string& name, u32 value) const = 0;
        virtual void SetIntArray(const std::string& name, int* values, u32 count) const = 0;
        virtual void SetFloat(const std::string& name, f32 value) const = 0;
        virtual void SetFloat2(const std::string& name, const glm::vec2& value) const = 0;
        virtual void SetFloat3(const std::string& name, const glm::vec3& value) const = 0;
        virtual void SetFloat4(const std::string& name, const glm::vec4& value) const = 0;
        virtual void SetMat4(const std::string& name, const glm::mat4& value) const = 0;

        [[nodiscard]] virtual bool IsValid() const = 0;
        [[nodiscard]] virtual u32 GetRendererID() const = 0;
        [[nodiscard]] virtual const std::string& GetName() const = 0;
        [[nodiscard]] virtual const std::string& GetFilePath() const = 0;

        virtual void Reload() = 0;

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::ComputeShader;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        static Ref<ComputeShader> Create(const std::string& filepath);
    };
} // namespace OloEngine
