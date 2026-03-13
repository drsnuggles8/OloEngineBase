#include "OloEnginePCH.h"
#include "ClientPrediction.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Debug/Profiler.h"

namespace OloEngine
{
    ClientPrediction::ClientPrediction() = default;

    void ClientPrediction::RecordInput(u32 tick, u64 entityUUID, std::vector<u8> inputData)
    {
        OLO_PROFILE_FUNCTION();

        m_CurrentTick = tick;
        m_InputBuffer.Push(tick, entityUUID, std::move(inputData));
    }

    void ClientPrediction::Reconcile(Scene& scene, u32 lastProcessedInputTick)
    {
        OLO_PROFILE_FUNCTION();

        if (lastProcessedInputTick <= m_LastConfirmedTick)
        {
            return; // Nothing new confirmed
        }

        m_LastConfirmedTick = lastProcessedInputTick;

        // Discard all inputs the server has already processed
        m_InputBuffer.DiscardUpTo(lastProcessedInputTick);

        // The server snapshot has already been applied to the scene (via SnapshotInterpolator or Apply).
        // Now re-simulate all unconfirmed inputs on top of the server state.
        if (m_ApplyCallback)
        {
            auto unconfirmed = m_InputBuffer.GetUnconfirmedInputs(lastProcessedInputTick);
            for (const auto* input : unconfirmed)
            {
                m_ApplyCallback(scene, input->EntityUUID, input->Data.data(),
                                static_cast<u32>(input->Data.size()));
            }
        }
    }

    void ClientPrediction::SetInputApplyCallback(InputApplyCallback callback)
    {
        m_ApplyCallback = std::move(callback);
    }

    void ClientPrediction::SetSmoothingRate(f32 rate)
    {
        m_SmoothingRate = rate;
    }

    f32 ClientPrediction::GetSmoothingRate() const
    {
        return m_SmoothingRate;
    }

    NetworkInputBuffer& ClientPrediction::GetInputBuffer()
    {
        return m_InputBuffer;
    }

    const NetworkInputBuffer& ClientPrediction::GetInputBuffer() const
    {
        return m_InputBuffer;
    }

    u32 ClientPrediction::GetLastConfirmedTick() const
    {
        return m_LastConfirmedTick;
    }

    u32 ClientPrediction::GetCurrentTick() const
    {
        return m_CurrentTick;
    }
} // namespace OloEngine
