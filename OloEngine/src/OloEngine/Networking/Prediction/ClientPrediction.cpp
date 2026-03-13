#include "OloEnginePCH.h"
#include "ClientPrediction.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Debug/Profiler.h"

#include <glm/glm.hpp>
#include <unordered_map>

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

            // Capture server-authoritative positions before resimulation for smoothing
            std::unordered_map<u64, glm::vec3> preReconcilePositions;
            for (const auto* input : unconfirmed)
            {
                auto entityOpt = scene.TryGetEntityWithUUID(UUID(input->EntityUUID));
                if (entityOpt.has_value() && entityOpt->HasComponent<TransformComponent>())
                {
                    preReconcilePositions[input->EntityUUID] =
                        entityOpt->GetComponent<TransformComponent>().Translation;
                }
            }

            for (const auto* input : unconfirmed)
            {
                m_ApplyCallback(scene, input->EntityUUID, input->Data.data(),
                                static_cast<u32>(input->Data.size()));
            }

            // Blend between pre-reconciliation and post-resimulation positions
            if (m_SmoothingRate < 1.0f)
            {
                for (const auto& [entityUUID, prePos] : preReconcilePositions)
                {
                    auto entityOpt = scene.TryGetEntityWithUUID(UUID(entityUUID));
                    if (entityOpt.has_value() && entityOpt->HasComponent<TransformComponent>())
                    {
                        auto& transform = entityOpt->GetComponent<TransformComponent>();
                        transform.Translation = glm::mix(prePos, transform.Translation, m_SmoothingRate);
                    }
                }
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
