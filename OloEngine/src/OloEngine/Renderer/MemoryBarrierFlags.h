#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @brief API-agnostic memory barrier flags for GPU synchronization.
    //
    // These flags abstract away OpenGL-specific barrier bits so that callers
    // of RenderCommand::MemoryBarrier() do not need to include platform headers.
    enum class MemoryBarrierFlags : u32
    {
        None                = 0,
        VertexAttribArray   = OloBit32(0),
        ElementArray        = OloBit32(1),
        Uniform             = OloBit32(2),
        TextureFetch        = OloBit32(3),
        ShaderImageAccess   = OloBit32(4),
        Command             = OloBit32(5),
        PixelBuffer         = OloBit32(6),
        TextureUpdate       = OloBit32(7),
        BufferUpdate        = OloBit32(8),
        Framebuffer         = OloBit32(9),
        TransformFeedback   = OloBit32(10),
        AtomicCounter       = OloBit32(11),
        ShaderStorage       = OloBit32(12),
        ClientMappedBuffer  = OloBit32(13),
        QueryBuffer         = OloBit32(14),
        All                 = 0xFFFFFFFF
    };

    inline MemoryBarrierFlags operator|(MemoryBarrierFlags lhs, MemoryBarrierFlags rhs)
    {
        return static_cast<MemoryBarrierFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    inline MemoryBarrierFlags operator&(MemoryBarrierFlags lhs, MemoryBarrierFlags rhs)
    {
        return static_cast<MemoryBarrierFlags>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
    }

    inline MemoryBarrierFlags& operator|=(MemoryBarrierFlags& lhs, MemoryBarrierFlags rhs)
    {
        lhs = lhs | rhs;
        return lhs;
    }

} // namespace OloEngine
