#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Prediction/NetworkInputBuffer.h"

#include <functional>
#include <vector>

namespace OloEngine
{
    class Scene;

    // Callback type for applying an input command to the local simulation.
    // The game registers this to define how inputs affect entities.
    // Parameters: scene, entityUUID, inputData, inputSize
    using InputApplyCallback = std::function<void(Scene&, u64 entityUUID, const u8* data, u32 size)>;

    // Client-side prediction and server reconciliation.
    //
    // Flow:
    // 1. Client records input → applies locally (prediction) → sends to server
    // 2. Server processes input → responds with snapshot containing LastProcessedInputTick
    // 3. Client receives snapshot → discards confirmed inputs → re-simulates unconfirmed on top of server state
    class ClientPrediction
    {
      public:
        ClientPrediction();

        // Record and apply an input locally, then send it to the server.
        // entityUUID: the entity this input controls (must have Authority::Client).
        void RecordInput(u32 tick, u64 entityUUID, std::vector<u8> inputData);

        // Called when the client receives a server snapshot with the server's authoritative state.
        // Reconciles: resets to server state, re-applies unconfirmed inputs.
        void Reconcile(Scene& scene, u32 lastProcessedInputTick);

        // Set the callback that defines how to apply an input command to the sim.
        void SetInputApplyCallback(InputApplyCallback callback);

        // Set the prediction error smoothing rate (0..1). Lower = smoother but slower correction.
        void SetSmoothingRate(f32 rate);
        [[nodiscard]] f32 GetSmoothingRate() const;

        // Get the input buffer (for inspection/testing).
        [[nodiscard]] NetworkInputBuffer& GetInputBuffer();
        [[nodiscard]] const NetworkInputBuffer& GetInputBuffer() const;

        // Get the last tick that the server confirmed processing.
        [[nodiscard]] u32 GetLastConfirmedTick() const;

        // Get the current prediction tick (client-side).
        [[nodiscard]] u32 GetCurrentTick() const;

      private:
        NetworkInputBuffer m_InputBuffer;
        InputApplyCallback m_ApplyCallback;
        u32 m_CurrentTick = 0;
        u32 m_LastConfirmedTick = 0;
        f32 m_SmoothingRate = 0.1f;
    };
} // namespace OloEngine
