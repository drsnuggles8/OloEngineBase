#pragma once

#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"

namespace OloEngine
{
    // @brief FSR2 2.2.1 on the OpenGL 4.6 backend (#684).
    //
    // Owns the FfxFsr2Context and the scratch buffer its backend allocates from.
    // Everything Vulkan-or-OpenGL-agnostic lives in TemporalUpscaler; this class
    // exists to keep the native GLuint lookups (and the FSR2 headers, which drag
    // in their own glad and <Windows.h>) inside Platform/OpenGL/ where the RHI
    // boundary ratchet allows them.
    //
    // The whole class compiles to an "unsupported" stub when OLO_WITH_FSR2 is 0
    // (any non-Windows build — see cmake/fsr2.cmake), so callers get an object
    // that answers GetStatus() honestly instead of a link error.
    //
    // NOT THREAD SAFE and not meant to be: every method must run on the thread
    // holding the GL context, same contract as any other GL resource here.
    [[nodiscard]] Ref<TemporalUpscaler> CreateOpenGLTemporalUpscaler();
} // namespace OloEngine
