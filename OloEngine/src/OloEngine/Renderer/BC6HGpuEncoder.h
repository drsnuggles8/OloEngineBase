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
// THREADING. Every call in here is a GL call, so all of it happens on the thread that
// owns the graphics context — the one that called SetContextThreadToCurrent(), which
// Renderer::Init does. The engine's only bulk texture cook is AssetPackBuilder's, and
// BOTH of its entry points run the build on a WORKER thread (EditorLayer's
// Tasks::Launch, AssetPackBuilderPanel's FThread), so an encode call from a cook does
// not arrive on the context thread.
//
// Rather than refuse it, an off-thread call is marshalled: the job goes on a queue and
// the caller blocks until the context thread runs it. The context thread picks it up
// through the game thread's existing task queue — Application::Run already drains that
// at the top of every frame — so no new pump and no new call site in the frame loop.
// PumpPendingJobs() is that drain, and is also what a test calls to play the part of
// the game thread.
//
// AssetPackBuilder's TextureCookScope registers the hook for the length of a build, so
// an asset-pack bake with compression on encodes its HDR textures this way.
//
// If the queue is never drained (a host with no frame loop, a context thread that
// never came up), the wait is bounded: the job gives up, the level finishes on the CPU,
// and the whole GPU path latches off so the next level does not pay the timeout again.

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

    // Records the CALLING thread as the one holding the graphics context. Called by
    // Renderer::Init; a test that brings up its own context calls it directly. Until it
    // is called, EncodeLevel has nowhere to marshal to and refuses off-thread work.
    void SetContextThreadToCurrent();

    // Runs any encode jobs queued by other threads. MUST be called on the context
    // thread; it is a no-op anywhere else and a no-op when the queue is empty.
    // Returns how many jobs it ran.
    //
    // In the editor this is reached through a game-thread task the encoder enqueues
    // itself, so the existing per-frame drain in Application::Run services it. A test
    // calls it directly.
    u32 PumpPendingJobs();

    // Releases the cached shaders and scratch textures, and fails any queued jobs so
    // their waiters are not left blocked. Safe to call without a context and twice.
    void Shutdown();

    // Registers EncodeLevel as TextureCompression's BC6H GPU hook, and records the
    // calling thread as the one allowed to run it. Separated from EncodeLevel so the
    // caller decides when a cook may reach for the GPU; TextureCompression itself has no
    // GL dependency. Teardown is NOT installed by this — Renderer::Shutdown calls
    // Shutdown() above, while the context is still alive.
    void RegisterWithTextureCompression();
    void UnregisterFromTextureCompression();
} // namespace OloEngine::BC6HGpu
