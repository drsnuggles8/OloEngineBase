#pragma once

#include "OloEngine/Renderer/ComputeShader.h"
#include <glad/gl.h>

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
        void SetFloat(const std::string& name, f32 value) const override;
        void SetFloat2(const std::string& name, const glm::vec2& value) const override;
        void SetFloat3(const std::string& name, const glm::vec3& value) const override;
        void SetFloat4(const std::string& name, const glm::vec4& value) const override;
        void SetMat4(const std::string& name, const glm::mat4& value) const override;

        [[nodiscard]] u32 GetRendererID() const override { return m_RendererID; }
        [[nodiscard]] const std::string& GetName() const override { return m_Name; }
        [[nodiscard]] const std::string& GetFilePath() const override { return m_FilePath; }

      private:
        static std::string ReadFile(const std::string& filepath);
        void Compile(const std::string& source);

        [[nodiscard]] GLint GetUniformLocation(const std::string& name) const;

      private:
        u32 m_RendererID = 0;
        std::string m_Name;
        std::string m_FilePath;
    };
} // namespace OloEngine
