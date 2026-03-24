#pragma once
#include "OloEngine/Renderer/GraphicsContext.h"

struct GLFWwindow;

namespace OloEngine
{

    // Which flavour of GL_parallel_shader_compile is available (if any)
    enum class ParallelShaderCompileSupport : unsigned char
    {
        None,
        ARB,
        KHR
    };

    class OpenGLContext : public GraphicsContext
    {
      public:
        explicit OpenGLContext(GLFWwindow* windowHandle);

        void Init() override;
        void SwapBuffers() override;

        // Extension detection — valid after Init()
        static ParallelShaderCompileSupport GetParallelShaderCompileSupport()
        {
            return s_ParallelShaderCompile;
        }
        static bool HasParallelShaderCompile()
        {
            return s_ParallelShaderCompile != ParallelShaderCompileSupport::None;
        }

      private:
        static void DetectParallelShaderCompile();

        GLFWwindow* m_WindowHandle;
        static ParallelShaderCompileSupport s_ParallelShaderCompile;
    };

} // namespace OloEngine
