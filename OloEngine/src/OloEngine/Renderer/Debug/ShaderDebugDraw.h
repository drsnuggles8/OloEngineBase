#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
// COMPLETE types, not forward declarations. `Channel` below holds a
// Ref<StorageBuffer> BY VALUE, so instantiating it instantiates
// ~Ref<StorageBuffer>, which converts the pointee to RefCounted* for
// RefUtils::Release. With only a forward declaration Clang cannot prove
// that derived-to-base conversion and rejects the header outright — MSVC
// happens to accept it, which is why this surfaced on the Linux sanitizer
// build and not locally. Public headers must be self-contained anyway.
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace OloEngine
{

    // @brief The GPU-pushable shader debug-draw channels (issue #725).
    //
    // Read `ShaderDebugDrawTypes.h` first — it carries the buffer contract this
    // class implements. What lives here is the lifecycle around it:
    //
    //   Init()          allocate + bind the seven channels at their header-only
    //                   size and create the params UBO. Called once from
    //                   Renderer3D::Init.
    //   Draw*()         CPU-side appends. Frame scoped, staged CPU side, and
    //                   flushed into the SAME buffers the GPU appends to — that
    //                   shared buffer is the point of the feature, since it is
    //                   what lets a CPU-computed bound and its GPU-computed
    //                   counterpart be drawn together and compared.
    //   BeginFrame()    drain last frame's stats, then reset every channel
    //                   header and upload this frame's CPU entries. Runs after
    //                   scene traversal and before the render graph executes, so
    //                   CPU entries occupy slots [0, n) and GPU appends start at
    //                   n. Called from RenderPipeline::UploadExecutionState.
    //   ShaderDebugDrawPass expands and draws each channel, then stages this
    //                   frame's headers for the next BeginFrame() to read.
    //
    // ORDERING CONSEQUENCE worth knowing: a CPU push made after EndScene (from an
    // editor panel, say) lands in the NEXT frame's buffer, not this one. The
    // alternative — flushing CPU pushes inside the render pass — would put them
    // *after* the GPU appends and make slot indices depend on pass order, which
    // is worse.
    //
    // THREAD SAFETY: the CPU appenders take a mutex, because gameplay systems
    // marked `.Parallelizable()` run on worker threads and a debug draw is
    // exactly the kind of call that gets added to one of them. Everything else
    // (BeginFrame, the pass, readback) is render-thread only.
    class ShaderDebugDraw
    {
      public:
        // Entries each channel can hold when enabled. Chosen so the whole set is
        // ~1.5 MB: big enough that a per-cluster or per-probe visualization of a
        // real scene fits, small enough to leave resident permanently.
        static constexpr u32 kDefaultChannelCapacity = 4096;

        static void Init();
        static void Shutdown();
        [[nodiscard]] static bool IsInitialised();

        // Master switch. Off: no clear, no upload, no readback, no draw, and
        // every channel keeps Capacity == 0 so the GLSL push helpers early-out on
        // a single scalar load. Flipping it takes effect at the next BeginFrame().
        static void SetEnabled(bool enabled);
        [[nodiscard]] static bool IsEnabled();

        // Line width in pixels for the screen-space quad expansion.
        static void SetLineWidth(f32 pixels);
        [[nodiscard]] static f32 GetLineWidth();

        // Per-channel capacity in entries. Applied at the next BeginFrame().
        static void SetChannelCapacity(u32 entries);
        [[nodiscard]] static u32 GetChannelCapacity();

        // ---- CPU appenders -------------------------------------------------
        static void DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color,
                             ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);
        static void DrawCircle(const glm::vec3& center, const glm::vec3& normal, f32 radius, const glm::vec3& color,
                               ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);
        static void DrawRectangle(const glm::vec3& center, const glm::vec3& axisU, const glm::vec3& axisV,
                                  const glm::vec3& color,
                                  ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);
        static void DrawAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec3& color,
                             ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);
        static void DrawBox(const std::array<glm::vec3, 8>& corners, const glm::vec3& color,
                            ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);
        static void DrawCone(const glm::vec3& apex, const glm::vec3& axis, f32 radius, const glm::vec3& color,
                             ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);
        static void DrawSphere(const glm::vec3& center, f32 radius, const glm::vec3& color,
                               ShaderDebugDrawSpace space = ShaderDebugDrawSpace::World);

        // ---- Frame lifecycle ------------------------------------------------
        // Drain last frame's staged headers into GetStats(), then reset the
        // headers and upload this frame's CPU entries.
        static void BeginFrame();

        // Stage this frame's headers for the next BeginFrame() to read (a GPU->GPU
        // copy into a DeviceToHost buffer; see the readback note in the .cpp).
        // Called by ShaderDebugDrawPass at the end of its Execute.
        static void StageStatsForReadback();

        // Upload the render-side params for one channel's indirect draw.
        static void UploadDrawParams(const glm::mat4& viewProjection, const glm::mat4& observerInvViewProjection,
                                     const glm::vec2& viewportSize, ShaderDebugDrawPrimitive primitive);

        [[nodiscard]] static Ref<StorageBuffer> GetChannelBuffer(ShaderDebugDrawPrimitive primitive);
        [[nodiscard]] static Ref<UniformBuffer> GetParamsUBO();

        // True when at least one channel has entries worth drawing this frame.
        // Only meaningful between BeginFrame() and the pass; GPU appends are not
        // visible to the CPU, so this is deliberately optimistic — it reports
        // true whenever the channels are live at all, because the whole point is
        // that the CPU does not know what the GPU pushed.
        [[nodiscard]] static bool HasWorkThisFrame();

        // Stats from the PREVIOUS frame (see the readback note). `StatsValid` is
        // false until the first drain lands.
        [[nodiscard]] static const ShaderDebugDrawStats& GetStats();

      private:
        struct Channel
        {
            Ref<StorageBuffer> Buffer;
            u32 Capacity = 0;                    // entries the current allocation holds
            u32 CpuCount = 0;                    // CPU entries uploaded this frame
            u32 CpuRequested = 0;                // CPU pushes attempted this frame (unclamped)
            RHI::ResourceHandle StagingBuffer{}; // DeviceToHost header mirror
        };

        struct Data
        {
            std::array<Channel, kShaderDebugDrawPrimitiveCount> Channels{};
            Ref<UniformBuffer> ParamsUBO;

            std::vector<ShaderDebugDrawLine> CpuLines;
            std::vector<ShaderDebugDrawCircle> CpuCircles;
            std::vector<ShaderDebugDrawRectangle> CpuRectangles;
            std::vector<ShaderDebugDrawAABB> CpuAABBs;
            std::vector<ShaderDebugDrawBox> CpuBoxes;
            std::vector<ShaderDebugDrawCone> CpuCones;
            std::vector<ShaderDebugDrawSphere> CpuSpheres;
            std::mutex CpuMutex;

            ShaderDebugDrawStats Stats;
            // ATOMIC because the Draw*() appenders are documented as callable
            // from worker threads (see the class comment) while SetEnabled /
            // Init / Shutdown run on the render thread. A plain bool read and
            // written from two threads is a data race under the C++ memory
            // model — undefined behaviour, however benign the generated code
            // looks. The appenders additionally RE-CHECK this gate after taking
            // CpuMutex, because passing the pre-lock test says nothing about
            // still being enabled once the lock is held.
            std::atomic<bool> Enabled{ false };
            std::atomic<bool> Initialised{ false };
            bool StatsStaged = false;
            f32 LineWidth = 2.0f;
            u32 RequestedCapacity = kDefaultChannelCapacity;
        };

        static Data& Get();
        static void EnsureChannelCapacity(Channel& channel, ShaderDebugDrawPrimitive primitive, u32 capacity);
        static void UploadChannel(ShaderDebugDrawPrimitive primitive, const void* entries, u32 entryCount,
                                  u32 requestedCount);
        static void ClearCpuEntries();
        static void ReadbackStats();
    };
} // namespace OloEngine
