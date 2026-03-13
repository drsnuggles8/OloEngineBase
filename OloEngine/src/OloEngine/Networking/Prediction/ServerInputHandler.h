#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Prediction/ClientPrediction.h"

#include <unordered_map>

namespace OloEngine
{
    class Scene;

    // Server-side handler for InputCommand messages.
    // Validates authority, applies inputs to the authoritative sim,
    // and tracks the last processed input tick per client for reconciliation.
    class ServerInputHandler
    {
      public:
        ServerInputHandler();

        // Process an incoming input command from a client.
        // senderClientID: the client who sent the input.
        // data/size: the serialized InputCommand payload (tick + entityUUID + input data).
        void ProcessInput(Scene& scene, u32 senderClientID, const u8* data, u32 size);

        // Set the callback that defines how to apply an input command to the sim.
        void SetInputApplyCallback(InputApplyCallback callback);

        // Get the last processed input tick for a specific client.
        [[nodiscard]] u32 GetLastProcessedTick(u32 clientID) const;

        // Get all per-client last-processed ticks (for embedding in snapshots).
        [[nodiscard]] const std::unordered_map<u32, u32>& GetAllLastProcessedTicks() const;

      private:
        InputApplyCallback m_ApplyCallback;
        std::unordered_map<u32, u32> m_LastProcessedTicks; // clientID → last processed tick
    };
} // namespace OloEngine
