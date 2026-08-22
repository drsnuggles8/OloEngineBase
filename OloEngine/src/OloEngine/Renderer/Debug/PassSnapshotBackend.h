// =============================================================================
// PassSnapshotBackend.h
//
// Internal seam between the backend-neutral RenderGraphPassSnapshot
// orchestration (Renderer/Debug/RenderGraphPassSnapshot.cpp) and its GL clone
// engine (Platform/OpenGL/OpenGLPassSnapshot.cpp) — issue #691,
// ADR 0011 §1.6 (what cannot convert in place RELOCATES). Same shape as
// StateGuardBackend.h: free functions declared here, defined in the OpenGL TU,
// resolved at link time, so the Renderer/Debug TU stays free of <glad/gl.h>
// and Platform/ includes.
//
// The vocabulary here is NATIVE u32 texture names and native enum values
// carried as opaque u32s, deliberately: this instrument's contract is pinned
// native at both ends — RenderGraphPassSnapshot::Resolver hands in a raw
// texture name (the MCP tools construct u32-returning resolvers) and
// Result::TextureID hands one back out for their readbacks — and `native ->
// handle` is not recoverable, so no RHI::ResourceHandle can exist on this
// path. That is the sanctioned currency for an explicitly GL-side debug
// instrument (native-currency debug info, ADR 0011 amendment (77)); the
// RendererAPI facade's copy/create family takes handle pairs by design and
// cannot serve it.
// =============================================================================

#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine::Detail
{
    // Everything the clone path needs to know about a native texture object.
    // Target / InternalFormat are native enum values as opaque u32s; Samples
    // is reported so the caller can reject multisample sources (whose storage
    // description is left defaulted).
    struct NativeTextureCloneInfo
    {
        bool IsTexture = false;
        i32 Samples = 0;
        u32 Target = 0;
        u32 InternalFormat = 0;
        u32 Width = 0;
        u32 Height = 0;
        u32 DepthOrLayers = 1; // cube maps report 6 (glCopyImageSubData addresses a cube as 6 array layers)
        u32 MipLevels = 1;
    };
    [[nodiscard]] NativeTextureCloneInfo QueryNativeTextureCloneInfo(u32 textureId);

    // Allocate a scratch texture with the exact native storage description
    // (identical internal format — a bitwise clone target). Returns 0 when
    // the target is unsupported or allocation fails.
    [[nodiscard]] u32 CreateNativeScratchTexture(u32 target, u32 internalFormat,
                                                 u32 width, u32 height, u32 depthOrLayers,
                                                 u32 mipLevels);

    void DeleteNativeTexture(u32 textureId);

    // Bitwise-copy every mip level from `sourceId` into `scratchId` (same
    // target, same storage description). Returns 0 on success, or the native
    // error code the copy raised (opaque u32, logged as hex by the caller).
    [[nodiscard]] u32 CopyNativeTextureAllMips(u32 sourceId, u32 scratchId, u32 target,
                                               u32 width, u32 height, u32 depthOrLayers,
                                               u32 mipLevels);

    // Drain any queued native errors so subsequent checks attribute failures
    // to THIS clone, not to whatever the executing pass left behind.
    void DrainNativeErrors();
} // namespace OloEngine::Detail
