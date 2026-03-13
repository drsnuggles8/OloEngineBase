#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Lockstep/StateHash.h"
#include "OloEngine/Networking/Replication/SnapshotBuffer.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class Scene;

    // Manages deterministic lockstep simulation for RTS-style games.
    // All peers submit inputs for tick N; sim only advances when all inputs are received.
    class LockstepManager
    {
      public:
        // Callback for applying a peer's input to the simulation.
        using InputApplyCallback = std::function<void(Scene&, u32 peerID, const u8* data, u32 size)>;

        LockstepManager();

        // Set the tick rate for the lockstep sim (default 10Hz).
        void SetTickRate(u32 hz);
        [[nodiscard]] u32 GetTickRate() const;

        // Set the input delay in ticks (default 2). Local input is scheduled
        // for CurrentTick + InputDelay.
        void SetInputDelay(u32 delayTicks);
        [[nodiscard]] u32 GetInputDelay() const;

        // Register the set of peer IDs participating in the lockstep session.
        void SetPeers(std::unordered_set<u32> peerIDs);

        // Submit local input for a future tick (CurrentTick + InputDelay).
        void SubmitInput(u32 localPeerID, std::vector<u8> data);

        // Receive input from a remote peer for a specific tick.
        void ReceiveInput(u32 peerID, u32 tick, std::vector<u8> data);

        // Check if all peers have submitted inputs for the given tick.
        [[nodiscard]] bool HasAllInputsForTick(u32 tick) const;

        // Advance the simulation by one tick if all inputs are available.
        // Returns true if the tick was advanced, false if still waiting.
        bool AdvanceTick(Scene& scene);

        // Get the current lockstep tick.
        [[nodiscard]] u32 GetCurrentTick() const;

        // --- Desync Detection ---

        // Set the interval (in ticks) between state hash comparisons (default 60).
        void SetHashCheckInterval(u32 interval);

        // Compute and store the state hash for the current tick.
        void RecordStateHash(const std::vector<u8>& snapshotData);

        // Receive a remote peer's hash for comparison. Returns true if hashes match.
        bool CompareRemoteHash(u32 peerID, u32 tick, u32 remoteHash);

        // Check if a desync has been detected.
        [[nodiscard]] bool IsDesynced() const;

        // Clear desync state (e.g. after a resync).
        void ClearDesync();

        // Set the callback for applying inputs.
        void SetInputApplyCallback(InputApplyCallback callback);

      private:
        u32 m_TickRate = 10;
        u32 m_InputDelay = 2;
        u32 m_CurrentTick = 0;
        u32 m_HashCheckInterval = 60;
        bool m_Desynced = false;

        std::unordered_set<u32> m_Peers;

        // tick → (peerID → input data)
        std::unordered_map<u32, std::unordered_map<u32, std::vector<u8>>> m_InputsByTick;

        // tick → local state hash (for comparison)
        std::unordered_map<u32, u32> m_LocalHashes;

        InputApplyCallback m_ApplyCallback;
    };
} // namespace OloEngine
