#pragma once

#include <functional>
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
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        static Ref<ComputeShader> Create(const std::string& filepath);
        static Ref<ComputeShader> CreateFromSource(const std::string& name, const std::string& source);

        // Result of the CPU-only load step of a compute shader: the source file
        // read from disk with its #include directives resolved. Holds no GPU
        // resources, so it can be produced on a worker thread.
        struct SourceLoadResult
        {
            std::string Name;   // Derived from the filename (no directory, no extension).
            std::string Source; // Fully preprocessed GLSL; empty when the load failed.

            [[nodiscard]] bool IsValid() const
            {
                return !Source.empty();
            }
        };

        // Read a compute shader file and resolve its #include directives. This is
        // pure disk + string work (no GPU calls), so it is safe to call from any
        // thread — it is the off-main-thread half of CreateFromFileAsync().
        // Returns an invalid result (empty Source) if the file cannot be read.
        static SourceLoadResult LoadSourceFromFile(const std::string& filepath);

        // Asynchronously create a compute shader from a file. The file read and
        // #include preprocessing run on a background worker thread; the GPU
        // program is then created on the main thread the next time
        // GPUResourceQueue::ProcessAll() runs, at which point `onReady` is invoked
        // (always on the main thread) with the created shader — or nullptr if the
        // file could not be read or compilation failed. This is the asynchronous
        // counterpart to the blocking Create(filepath).
        static void CreateFromFileAsync(std::string filepath, std::function<void(Ref<ComputeShader>)> onReady);
    };
} // namespace OloEngine
