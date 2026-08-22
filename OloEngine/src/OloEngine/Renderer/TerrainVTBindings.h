#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"

namespace OloEngine
{
    // @brief The three resources a terrain draw needs bound for the virtual-
    // texture path (issue #715).
    //
    // Its own header, small on purpose: it is a parameter of
    // `Renderer3D::DrawTerrainPatch` AND a member of `DrawTerrainPatchCommand`,
    // and Renderer3D.h does not otherwise include the command-struct header.
    //
    // Grouped rather than three more positional parameters on an already
    // twelve-argument submit, and validated as a unit: the shader's VT branch
    // samples the indirection map, samples the cache AND writes feedback, so a
    // partially-supplied set would leave one of the three unbound while the
    // terrain UBO still says the branch is live. That is a silently wrong frame,
    // not a missing one — hence IsEmpty()/IsComplete() rather than a bare
    // null-check per field.
    struct TerrainVTBindings
    {
        RHI::ResourceHandle indirectionTextureID{}; // RGBA8 + mip chain: virtual page -> physical tile
        RHI::ResourceHandle cacheTextureID{};       // RGBA8 2-layer physical cache atlas
        RHI::ResourceHandle feedbackBufferID{};     // uint[] the fragment stage appends page requests to

        [[nodiscard]] bool IsComplete() const
        {
            return indirectionTextureID.IsValid() && cacheTextureID.IsValid() && feedbackBufferID.IsValid();
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return !indirectionTextureID.IsValid() && !cacheTextureID.IsValid() && !feedbackBufferID.IsValid();
        }
    };
} // namespace OloEngine
