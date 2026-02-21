#pragma once

#include "OloEngine/Renderer/ComputeShader.h"
#include <glad/gl.h>

#include <unordered_map>

namespace OloEngine
{
    class OpenGLComputeShader : public ComputeShader
    {
      public:
        explicit OpenGLComputeShader(const std::string& filepath);
        ~OpenGLComputeShader() override;

        void Bind() const override;
        void Unbind() const override;

        void SetInt(const std::string& name, int value) const override;
        void SetUint(const std::string& name, u32 value) const override;
        void SetIntArray(const std::string& name, int* values, u32 count) const override;
        void SetFloat(const std::string& name, f32 value) const override;
        void SetFloat2(const std::string& name, const glm::vec2& value) const override;
        void SetFloat3(const std::string& name, const glm::vec3& value) const override;
        void SetFloat4(const std::string& name, const glm::vec4& value) const override;
        void SetMat4(const std::string& name, const glm::mat4& value) const override;

        [[nodiscard]] bool IsValid() const override
        {
            return m_IsValid;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return m_RendererID;
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

      private:
        void Compile(const std::string& source);

        [[nodiscard]] GLint GetUniformLocation(const std::string& name) const;

      private:
        u32 m_RendererID = 0;
        bool m_IsValid = false;
        std::string m_Name;
        std::string m_FilePath;
        mutable std::unordered_map<std::string, GLint> m_UniformLocationCache;
    };
} // namespace OloEngine
