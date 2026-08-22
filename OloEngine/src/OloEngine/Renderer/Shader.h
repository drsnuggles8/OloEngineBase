#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
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

        // -------------------------------------------------------------------
        // Whether the CURRENTLY BOUND program reads the descriptor heap
        // (issue #691).
        //
        // `RHI::DescriptorHeap::IsEnabled()` is global; whether a given program
        // actually indexes the heap is PER SHADER, because the bindless compile
        // route is allowed to decline — `CreateProgramFromRawGLSL` falls back to
        // the ordinary slot-based program on any compile or link failure.
        //
        // Without this distinction `BindTextureOrHeapOffset` would record an
        // offset and skip the bind for a program that reads sampler binding
        // points, leaving them unbound and the pass rendering wrong with no
        // diagnostic. The backend's Bind() records the truth here; the pass-side
        // seam consults it.
        //
        // A plain global rather than state threaded through the command context:
        // it is written by exactly one call site per program bind, read by
        // exactly one, and render passes run on the game thread.
        [[nodiscard]] static auto IsBoundProgramBindless() -> bool;
        static void SetBoundProgramBindless(bool bindless);

        // Record whether a NATIVE program id was built through the bindless
        // route, so a bind that never goes through OpenGLShader::Bind() can still
        // publish the flag above. `CommandDispatch` binds by handle through
        // RendererAPI::BindShaderProgram, which is exactly that case — see the
        // registry comment in Shader.cpp for what went wrong without it.
        static void RegisterProgramBindless(u32 programID, bool bindless);

        // "DOES THIS PROGRAM READ ITS MATERIAL TEXTURES OUT OF THE HEAP?" — a
        // DIFFERENT QUESTION from IsBoundProgramBindless(), and conflating the two
        // silently unbinds textures.
        //
        // A program is a bindless VARIANT if it converted ANY input — a pass
        // texture, a storage image, anything. That says nothing about whether its
        // MATERIAL samplers moved: PBR_GBuffer could convert a shadow input and
        // still declare `layout(binding = 0) uniform sampler2D u_AlbedoMap`.
        // CommandDispatch::BindPBRTextures skips nine binds when this is true, so
        // answering the broader question there leaves those samplers unbound and
        // the mesh renders unlit — observed as a black sphere on an unlit ground
        // (issue #691).
        //
        // Set from the source scan in CreateProgramFromRawGLSL: a shader that
        // declares `u_MaterialHeapOffsets` reads its material textures from the
        // material UBO, and nothing else does.
        [[nodiscard("the answer decides whether the material binds are issued at all")]] static auto
        ReadsMaterialHeapOffsets() -> bool;
        static void SetBoundProgramMaterialOffsets(bool reads);
        static void RegisterProgramMaterialOffsets(u32 programID, bool reads);
        [[nodiscard]] static auto ProgramReadsMaterialOffsets(u32 programID) -> bool;

        // Drop `programID` from the bindless registry. MUST be called before
        // glDeleteProgram, at EVERY deletion site.
        //
        // GL reissues freed program names. A registered id that outlives its
        // program is therefore inherited by an unrelated future program:
        // `IsProgramBindless` returns true for it, `BindShaderProgram` publishes
        // that, and the binding seam then records an offset and skips the bind —
        // so a slot-based pass renders with empty sampler bindings and no
        // diagnostic at all. Registration without paired retirement is the whole
        // bug; erasing an id that was never registered is a harmless no-op, so
        // the safe rule is to call this unconditionally rather than reason about
        // which programs reached registration.
        static void UnregisterProgram(u32 programID);
        [[nodiscard]] static auto IsProgramBindless(u32 programID) -> bool;

        // True when any live program was built as the bindless variant. Disabling
        // the heap while this holds strands those programs reading an offset table
        // nobody updates — see DescriptorHeap::SetEnabled.
        [[nodiscard]] static auto AnyBindlessProgramsExist() -> bool;
        virtual void Unbind() const = 0;

        virtual void SetInt(const std::string& name, int value) const = 0;
        virtual void SetIntArray(const std::string& name, int* values, u32 count) const = 0;
        virtual void SetFloat(const std::string& name, f32 value) const = 0;
        virtual void SetFloat2(const std::string& name, const glm::vec2& value) const = 0;
        virtual void SetFloat3(const std::string& name, const glm::vec3& value) const = 0;
        virtual void SetFloat4(const std::string& name, const glm::vec4& value) const = 0;
        virtual void SetMat4(const std::string& name, const glm::mat4& value) const = 0;

        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691). Sibling of GetRendererID during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;

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

        // Deferred-path capability introspection. Returns true when the
        // fragment stage declares at least one G-Buffer MRT output using
        // the engine's opt-in naming convention (marker set:
        // `o_GBuffer*`, `gAlbedo`, `gNormalRoughAO`, `gEmissive`). Detection
        // is performed inline in `OpenGLShader::Reflect` by scanning the
        // fragment stage's `stage_outputs` via spirv_cross.
        // Reflection-populated backends (OpenGL today, Vulkan tomorrow)
        // override this; the base default is false so unimplemented
        // backends keep the conservative "treat as forward-only" behaviour.
        [[nodiscard]] virtual bool IsDeferredCapable() const
        {
            return false;
        }

        // Resource registry access (safe interface)
        virtual ShaderResourceRegistry* GetResourceRegistry() = 0;
        virtual const ShaderResourceRegistry* GetResourceRegistry() const = 0;

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Shader;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        static Ref<Shader> Create(const std::string& filepath);
        static Ref<Shader> Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
    };
} // namespace OloEngine
