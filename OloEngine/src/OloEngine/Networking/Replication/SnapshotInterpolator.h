#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"
#include "OloEngine/Scene/Components.h"

#include <unordered_map>

namespace OloEngine
{
    class Scene;

    // Client-side snapshot interpolation.
    // Buffers incoming server snapshots and smoothly interpolates entity transforms
    // between the two surrounding tick states with a configurable render delay.
    class SnapshotInterpolator
    {
      public:
        explicit SnapshotInterpolator(u32 bufferCapacity = SnapshotBuffer::kDefaultCapacity);

        // Feed a new server snapshot into the buffer.
        void PushSnapshot(u32 tick, std::vector<u8> data);

        // Advance the interpolation clock and apply interpolated transforms to the scene.
        // Call once per frame with the frame delta time.
        void Interpolate(Scene& scene, f32 dt);

        // Set the render delay in seconds (default 100ms = 0.1f).
        // Higher values make interpolation smoother but add more latency.
        void SetRenderDelay(f32 seconds);
        [[nodiscard]] f32 GetRenderDelay() const;

        // Set the server tick rate so we can convert ticks ↔ time.
        void SetServerTickRate(u32 ticksPerSecond);
        [[nodiscard]] u32 GetServerTickRate() const;

        // Get the current interpolation time in ticks.
        [[nodiscard]] f32 GetRenderTick() const;

        [[nodiscard]] const SnapshotBuffer& GetBuffer() const;

      private:
        SnapshotBuffer m_Buffer;
        f32 m_RenderDelay = 0.1f;  // seconds behind latest tick
        u32 m_ServerTickRate = 20; // ticks per second
        f32 m_CurrentTime = 0.0f;  // accumulated time in seconds
        u32 m_LatestReceivedTick = 0;

        // Parsed snapshot cache to avoid re-parsing every frame
        u32 m_CachedBeforeTick = UINT32_MAX;
        u32 m_CachedAfterTick = UINT32_MAX;
        std::unordered_map<u64, TransformComponent> m_CachedBefore;
        std::unordered_map<u64, TransformComponent> m_CachedAfter;
    };
} // namespace OloEngine
