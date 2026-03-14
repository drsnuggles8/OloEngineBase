#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Scene;

    // Callback for applying an input to the simulation during rollback re-simulation.
    using RollbackInputCallback = std::function<void(Scene&, u32 peerID, const u8* data, u32 size)>;

    // Manages rollback-based netcode for P2P games (fighting, racing, co-op).
    //
    // Flow:
    // 1. Each frame: predict remote inputs (repeat last known), simulate, save state
    // 2. When late remote input arrives: rollback to that tick, re-simulate forward with correct inputs
    // 3. Display the re-simulated current frame
    class RollbackManager
    {
      public:
        static constexpr u32 s_DefaultMaxRollbackFrames = 7;
        static constexpr u32 s_DefaultStateBufferSize = 16;

        RollbackManager();

        // Configure max rollback depth (default 7 frames).
        void SetMaxRollbackFrames(u32 maxFrames);
        [[nodiscard]] u32 GetMaxRollbackFrames() const;

        // Set the input apply callback.
        void SetInputApplyCallback(RollbackInputCallback callback);

        // Save the current simulation state for potential rollback.
        void SaveState(u32 tick, Scene& scene);

        // Submit local input for the current tick.
        void SubmitLocalInput(u32 localPeerID, u32 tick, std::vector<u8> data);

        // Receive a remote peer's input. If this input is for a past tick,
        // triggers a rollback + re-simulation.
        // Returns the number of frames rolled back (0 if no rollback needed).
        u32 ReceiveRemoteInput(u32 peerID, u32 tick, std::vector<u8> data, Scene& scene);

        // Get the predicted input for a peer at a given tick.
        // If no real input is available, returns the last known input (input prediction).
        [[nodiscard]] std::optional<std::vector<u8>> GetInputForTick(u32 peerID, u32 tick) const;

        // Get the current rollback frame (tick).
        [[nodiscard]] u32 GetCurrentTick() const;
        void SetCurrentTick(u32 tick);

        // Get total number of rollbacks that have occurred (for debugging).
        [[nodiscard]] u32 GetRollbackCount() const;

      private:
        void Rollback(u32 toTick, Scene& scene);

        u32 m_MaxRollbackFrames = s_DefaultMaxRollbackFrames;
        u32 m_CurrentTick = 0;
        u32 m_RollbackCount = 0;

        // State history: tick → snapshot data
        SnapshotBuffer m_StateBuffer{ s_DefaultStateBufferSize };

        // Input history: peerID → (tick → data)
        std::unordered_map<u32, std::unordered_map<u32, std::vector<u8>>> m_Inputs;

        RollbackInputCallback m_ApplyCallback;
    };
} // namespace OloEngine
