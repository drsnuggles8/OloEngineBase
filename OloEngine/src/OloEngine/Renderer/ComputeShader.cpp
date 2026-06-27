#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Async/Async.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
    Ref<ComputeShader> ComputeShader::Create(const std::string& filepath)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<ComputeShader>(new OpenGLComputeShader(filepath));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<ComputeShader> ComputeShader::CreateFromSource(const std::string& name, const std::string& source)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<ComputeShader>(new OpenGLComputeShader(name, source));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    ComputeShader::SourceLoadResult ComputeShader::LoadSourceFromFile(const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        SourceLoadResult result;

        // Derive the shader name from the filename (strip directory + extension),
        // matching the naming used by OpenGLComputeShader(filepath). The same
        // last-separator position is reused below for the #include search directory.
        const auto lastSlash = filepath.find_last_of("/\\");
        const auto lastDot = filepath.rfind('.');
        const auto nameStart = (lastSlash == std::string::npos) ? 0 : (lastSlash + 1);
        const auto count = (lastDot == std::string::npos) ? (filepath.size() - nameStart) : (lastDot - nameStart);
        result.Name = filepath.substr(nameStart, count);

        const std::string rawSource = FileSystem::ReadFileText(filepath);
        if (rawSource.empty())
        {
            OLO_CORE_ERROR("ComputeShader::LoadSourceFromFile: failed to read source file '{0}'", filepath);
            return result; // Source stays empty -> IsValid() == false
        }

        // Resolve #include directives relative to the shader's own directory.
        // This is plain text/file work (no GPU calls); the GLSL include processor
        // is shared with the regular shader path.
        const std::string directory = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
        result.Source = OpenGLShader::ProcessIncludes(rawSource, directory);

        if (result.Source.empty())
        {
            OLO_CORE_ERROR("ComputeShader::LoadSourceFromFile: include processing produced empty source for '{0}'", filepath);
        }

        return result;
    }

    void ComputeShader::CreateFromFileAsync(std::string filepath, std::function<void(Ref<ComputeShader>)> onReady)
    {
        // Off-thread: read + preprocess the GLSL (disk + string work, no GPU),
        // then hand GPU program creation to the main-thread resource queue. The
        // callback fires (on the main thread) from CreateComputeShaderCommand::Execute()
        // when GPUResourceQueue::ProcessAll() next runs — with the shader, or
        // nullptr if the load or the compile failed.
        AsyncTask(LowLevelTasks::ETaskPriority::BackgroundNormal,
                  [filepath = std::move(filepath), onReady = std::move(onReady)]() mutable
                  {
                      SourceLoadResult loaded = LoadSourceFromFile(filepath);

                      RawShaderData data;
                      data.Name = std::move(loaded.Name);
                      // Empty on failure -> CreateComputeShaderCommand reports nullptr.
                      data.ComputeSource = std::move(loaded.Source);

                      GPUResourceQueue::Enqueue<CreateComputeShaderCommand>(std::move(data), std::move(onReady));
                  });
    }
} // namespace OloEngine
