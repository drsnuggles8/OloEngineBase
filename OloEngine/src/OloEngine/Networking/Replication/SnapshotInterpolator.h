#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Replication/EntitySnapshot.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"

namespace OloEngine
{
    class Scene;

    // Client-side snapshot interpolation.
    // Buffers incoming server snapshots and smoothly interpolates the registered
    // set of interpolatable components (ComponentInterpolationRegistry) between the
    // two surrounding tick states with a configurable render delay — each component
    // blends per its own policy (lerp / slerp / step).
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

        // Identify which entities this client PREDICTS rather than interpolates.
        //
        // Interpolation must not touch an entity whose motion the local client is
        // predicting — its authoritative state arrives through reconciliation, and
        // blending it 100 ms in the past would fight the prediction every frame.
        // The rule is ownership, not authority: every player pawn is
        // client-authoritative, so skipping all of them (which is what an
        // authority-only test does) freezes every OTHER player on screen. Only the
        // pawn this client owns is excluded.
        //
        // 0 (the default) means "predict nothing" — every replicated entity
        // interpolates, which is the right behaviour for a spectator or a client
        // that has not been assigned an id yet.
        void SetLocalClientID(u32 clientID);
        [[nodiscard]] u32 GetLocalClientID() const;

        [[nodiscard]] const SnapshotBuffer& GetBuffer() const;

        // Drop all buffered/cached snapshot state (render delay and tick-rate
        // configuration are left untouched). Call on reconnect so a new session
        // never interpolates against the previous session's snapshots.
        void Reset();

      private:
        SnapshotBuffer m_Buffer;
        f32 m_RenderDelay = 0.1f;  // seconds behind latest tick
        u32 m_ServerTickRate = 20; // ticks per second
        u32 m_LatestReceivedTick = 0;
        u32 m_LocalClientID = 0;

        // Parsed snapshot cache to avoid re-parsing every frame. Each entry is a
        // UUID → per-component byte-blob map (the registry-driven snapshot format).
        u32 m_CachedBeforeTick = UINT32_MAX;
        u32 m_CachedAfterTick = UINT32_MAX;
        ParsedSnapshot m_CachedBefore;
        ParsedSnapshot m_CachedAfter;
    };
} // namespace OloEngine
