#pragma once

#include "OloEngine/Core/Base.h"
#include <glad/gl.h>

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
        SetShaderProgram,
        End
    };

    // Forward declarations
    struct CommandPacket;
    class CommandBucket;
    class CommandAllocator;
    using DispatchFn = void(*)(const void* command);

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

    struct SetShaderProgramCommand 
    {
        CommandHeader Header;
        u32 ProgramID;
    };
    #pragma pack(pop)
}