#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

// GPU compute BC6H encoder (#624 item 3) — the "fast path" #440 originally proposed.
//
// The CPU encoder in BC6HEncoder.cpp searches fourteen block modes per block, which is
// what makes it good and also what makes a large HDR bake slow. The search is perfectly
// parallel across blocks, so this runs the identical algorithm as a compute shader: one
// invocation per 4x4 block, reading an RGBA32F image and writing one RGBA32UI texel —
// 16 bytes, bit-compatible with a BC6H block — per block.
//
// The two image units are its ENTIRE binding footprint. It takes no UBO and no SSBO
// binding, which matters because ShaderBindingLayout records UBO 65 as the last free
// number engine-wide and the storage-buffer namespace is effectively full. Signedness is
// a second shader variant rather than a parameter block for exactly that reason.
//
// This is a cook-time path: it needs a live graphics context and is never called per
// frame. TextureCompression::EncodeBC6H uses it only when SetGpuBC6HEncoder has been
// handed the hook below, so the CPU-only cook (and every headless test) is unaffected.
//
// WHO MAY REGISTER IT, and why nothing in-tree does yet. Every call here is a GL call,
// so the hook must be registered from the thread that owns the graphics context, and
// EncodeLevel refuses (loudly, once) on any other — the counters in TextureCompression
// then show the levels that fell back. The engine's only bulk texture cook today is
// AssetPackBuilder's, and BOTH of its entry points run the build on a worker thread
// (EditorLayer's Tasks::Launch and AssetPackBuilderPanel's FThread), so registering it
// there would be inert at best. Making the pack build use this needs the texture cook
// moved onto the context-owning thread, which is its own piece of work; until then this
// is a ready fast path with an explicit switch rather than an automatic one.

namespace OloEngine::BC6HGpu
{
    // True when the compute shaders loaded and the context can run them. Loading is
    // attempted once; a failure is logged and latched, so this stays false rather than
    // retrying on every mip level of every texture.
    [[nodiscard]] bool IsAvailable();

    // Encode one mip level of RGB float source (3 floats per texel, row-major) into
    // tightly packed BC6H blocks: ceil(w/4) * ceil(h/4) * 16 bytes.
    //
    // Returns false (having logged why) when the GPU path could not run — the caller is
    // expected to fall back to the CPU encoder, which produces the same format from the
    // same algorithm, only slower.
    [[nodiscard]] bool EncodeLevel(const f32* rgb, u32 width, u32 height, bool isSigned,
                                   std::vector<u8>& outBlocks);

    // Releases the cached shaders and scratch textures. Safe to call without a context
    // and safe to call twice.
    void Shutdown();

    // Registers EncodeLevel as TextureCompression's BC6H GPU hook, and records the
    // calling thread as the one allowed to run it. Separated from EncodeLevel so the
    // caller decides when a cook may reach for the GPU; TextureCompression itself has no
    // GL dependency. Teardown is NOT installed by this — Renderer::Shutdown calls
    // Shutdown() above, while the context is still alive.
    void RegisterWithTextureCompression();
    void UnregisterFromTextureCompression();
} // namespace OloEngine::BC6HGpu
