#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderState.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Command types for dispatch and sorting
    enum class CommandType : u8
    {
        None = 0,
        DrawIndexed,
        DrawIndexedInstanced,
        SetBlendState,
        SetDepthState,
        SetStencilState,
        SetCullingState,
        SetLineWidth,
        SetPolygonMode,
        SetScissorState,
        SetColorMask,
        SetPolygonOffset,
        SetMultisampling,
        SetTexture,
        End
    };

    // Forward declarations
    struct CommandPacket;
    class CommandBucket;
    class CommandAllocator;
    using DispatchFn = void(*)(const void* command);

    // Command header for POD command structs
    #pragma pack(push, 1)
    struct CommandHeader
    {
        CommandType Type = CommandType::None;
        u16 Size = 0;         // Size of the command data following the header
        u8 Flags = 0;         // Flags for the command
    };

    // Example command structs (POD types only)
    struct DrawIndexedCommand
    {
        CommandHeader Header;
        u32 IndexCount;
        u32 StartIndex;
        u32 VertexOffset;
        u32 VertexArrayID;  // OpenGL VAO ID
        u32 IndexBufferID;  // OpenGL IBO ID
    };

    struct DrawIndexedInstancedCommand
    {
        CommandHeader Header;
        u32 IndexCount;
        u32 InstanceCount;
        u32 StartIndex;
        u32 VertexOffset;
        u32 VertexArrayID;
        u32 IndexBufferID;
    };

    struct SetBlendStateCommand
    {
        CommandHeader Header;
        bool Enabled;
        GLenum SrcFactor;
        GLenum DstFactor;
        GLenum Equation;
    };

    struct SetDepthStateCommand
    {
        CommandHeader Header;
        bool TestEnabled;
        bool WriteMask;
        GLenum Function;
    };

    struct SetStencilStateCommand
    {
        CommandHeader Header;
        bool Enabled;
        GLenum Function;
        GLint Reference;
        GLuint ReadMask;
        GLuint WriteMask;
        GLenum StencilFail;
        GLenum DepthFail;
        GLenum DepthPass;
    };

    struct SetCullingStateCommand
    {
        CommandHeader Header;
        bool Enabled;
        GLenum Face;
    };

    struct SetLineWidthCommand
    {
        CommandHeader Header;
        f32 Width;
    };

    struct SetPolygonModeCommand
    {
        CommandHeader Header;
        GLenum Face;
        GLenum Mode;
    };

    struct SetScissorStateCommand
    {
        CommandHeader Header;
        bool Enabled;
        GLint X;
        GLint Y;
        GLsizei Width;
        GLsizei Height;
    };

    struct SetColorMaskCommand
    {
        CommandHeader Header;
        bool Red;
        bool Green;
        bool Blue;
        bool Alpha;
    };

    struct SetPolygonOffsetCommand
    {
        CommandHeader Header;
        bool Enabled;
        f32 Factor;
        f32 Units;
    };

    struct SetMultisamplingCommand
    {
        CommandHeader Header;
        bool Enabled;
    };

    struct SetTextureCommand
    {
        CommandHeader Header;
        u32 TextureID;
        u32 Slot;
        u32 Target;
    };
    #pragma pack(pop)

    // Command packet flags
    enum CommandFlags : u8 
    {
        None           = 0,
        StateChange    = 1 << 0,
        DrawCall       = 1 << 1,
        ResourceBind   = 1 << 2,
        ChainStart     = 1 << 3,
        ChainEnd       = 1 << 4,
    };

    // Command packet wrapping a command with chain linkage
    struct CommandPacket 
    {
        CommandPacket* Next = nullptr;
        CommandPacket* Prev = nullptr;
        
        void* AuxiliaryMemory = nullptr;
        DispatchFn Dispatch = nullptr;
        
        CommandHeader Header;

        // Helper methods
        template<typename T>
        T* GetCommand() { return reinterpret_cast<T*>(&Header); }

        template<typename T>
        const T* GetCommand() const { return reinterpret_cast<const T*>(&Header); }
        
        void Execute() const;

        // Chain management
        void LinkAfter(CommandPacket* packet);
        void LinkBefore(CommandPacket* packet);
        void Unlink();
    };

    // Keep old definitions for backward compatibility
    // Command type for sorting
    enum class LegacyCommandType
    {
        Mesh,        // 3D mesh with material
        Quad,        // 2D quad with texture
        LightCube,   // Light visualization cube
        StateChange  // OpenGL state change
    };

    // Base class for all render commands
    class RenderCommandBase
    {
    public:
        virtual ~RenderCommandBase() = default;
        virtual void Execute() = 0;
        [[nodiscard]] virtual LegacyCommandType GetType() const = 0;
        
        // Sorting keys
        [[nodiscard]] virtual u64 GetShaderKey() const = 0;
        [[nodiscard]] virtual u64 GetMaterialKey() const = 0;
        [[nodiscard]] virtual u64 GetTextureKey() const = 0;
        [[nodiscard]] virtual u64 GetStateChangeKey() const { return 0; }

        // Command pool management
        virtual void Reset() = 0;

        // Command batching and merging
        [[nodiscard]] virtual bool CanBatchWith(const RenderCommandBase& other) const = 0;
        virtual bool MergeWith(const RenderCommandBase& other) = 0;
        [[nodiscard]] virtual sizet GetBatchSize() const = 0;
    };

    // Command for changing OpenGL state
    class StateChangeCommand : public RenderCommandBase
    {
    public:
        StateChangeCommand() = default;
        
        template<typename T>
        void Set(const T& state)
        {
            static_assert(std::is_base_of<RenderStateBase, T>::value, "State must derive from RenderStateBase");
            m_StateType = state.Type;
            m_State = CreateRef<T>(state);
        }

        void Execute() override;
        [[nodiscard]] LegacyCommandType GetType() const override { return LegacyCommandType::StateChange; }
        
        // Sorting keys - state changes are sorted by type
        [[nodiscard]] u64 GetShaderKey() const override { return 0; }
        [[nodiscard]] u64 GetMaterialKey() const override { return 0; }
        [[nodiscard]] u64 GetTextureKey() const override { return 0; }
        [[nodiscard]] u64 GetStateChangeKey() const override { return static_cast<u64>(m_StateType); }

        void Reset() override
        {
            m_State.reset();
            m_StateType = StateType::None;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] sizet GetBatchSize() const override { return 1; }
        [[nodiscard]] StateType GetStateType() const { return m_StateType; }
        [[nodiscard]] const Ref<RenderStateBase>& GetState() const { return m_State; }

    private:
        Ref<RenderStateBase> m_State;
        StateType m_StateType = StateType::None;
    };

    // Command for drawing a mesh with material
    class DrawMeshCommand : public RenderCommandBase
    {
    public:
        DrawMeshCommand() = default;
        void Set(const Ref<Mesh>& mesh, const glm::mat4& transform, const Material& material, bool isStatic = false)
        {
            m_Mesh = mesh;
            m_Transforms.clear();
            m_Transforms.push_back(transform);
            m_Material = material;
            m_BatchSize = 1;
            m_IsStatic = isStatic;
        }

        void AddInstance(const glm::mat4& transform)
        {
            m_Transforms.push_back(transform);
            m_BatchSize = m_Transforms.size();
        }

        void Execute() override;
        [[nodiscard]] LegacyCommandType GetType() const override { return LegacyCommandType::Mesh; }
        
        // Sorting keys
        [[nodiscard]] u64 GetShaderKey() const override;
        [[nodiscard]] u64 GetMaterialKey() const override;
        [[nodiscard]] u64 GetTextureKey() const override;

        void Reset() override
        {
            m_Mesh.reset();
            m_Transforms.clear();
            m_Material = Material();
            m_BatchSize = 1;
            m_IsStatic = false;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] sizet GetBatchSize() const override { return m_BatchSize; }
        [[nodiscard]] bool IsStatic() const { return m_IsStatic; }

    private:
        Ref<Mesh> m_Mesh;
        std::vector<glm::mat4> m_Transforms;
        Material m_Material;
        sizet m_BatchSize;
        bool m_IsStatic = false;
    };

    // Command for drawing a textured quad
    class DrawQuadCommand : public RenderCommandBase
    {
    public:
        DrawQuadCommand() = default;
        void Set(const glm::mat4& transform, const Ref<Texture2D>& texture)
        {
            m_Transform = transform;
            m_Texture = texture;
            m_BatchSize = 1;
        }

        void Execute() override;
        [[nodiscard]] LegacyCommandType GetType() const override { return LegacyCommandType::Quad; }
        
        // Sorting keys
        [[nodiscard]] u64 GetShaderKey() const override;
        [[nodiscard]] u64 GetMaterialKey() const override;
        [[nodiscard]] u64 GetTextureKey() const override;

        void Reset() override
        {
            m_Transform = glm::mat4(1.0f);
            m_Texture.reset();
            m_BatchSize = 1;
        }

        // Command batching and merging
        [[nodiscard]] bool CanBatchWith(const RenderCommandBase& other) const override;
        bool MergeWith(const RenderCommandBase& other) override;
        [[nodiscard]] sizet GetBatchSize() const override { return m_BatchSize; }

    private:
        glm::mat4 m_Transform;
        Ref<Texture2D> m_Texture;
        sizet m_BatchSize;
    };
}