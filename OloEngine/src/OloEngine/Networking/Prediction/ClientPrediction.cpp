#include "OloEnginePCH.h"
#include "ClientPrediction.h"
#include "OloEngine/Networking/Replication/ComponentInterpolationRegistry.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Debug/Profiler.h"

#include <algorithm>
#include <vector>

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
        if (!m_ApplyCallback)
        {
            return;
        }

        auto unconfirmed = m_InputBuffer.GetUnconfirmedInputs(lastProcessedInputTick);

        // Reconciliation smoothing generalizes beyond TransformComponent: capture
        // each predicted entity's server-authoritative (pre-resim) state for every
        // registered interpolatable component that opts into smoothing, then ease
        // the resimulated state back toward it per the component's Smooth fn (issue
        // #462). Step components leave Smooth null and are skipped.
        const bool smooth = m_SmoothingRate < 1.0f;
        const auto& entries = ComponentInterpolationRegistry::GetEntries();

        // Unique predicted entities (an entity can drive several unconfirmed
        // inputs; capture/smooth it once to avoid double-applying the blend).
        std::vector<u64> predictedEntities;
        for (const auto* input : unconfirmed)
        {
            if (std::find(predictedEntities.begin(), predictedEntities.end(), input->EntityUUID) == predictedEntities.end())
            {
                predictedEntities.push_back(input->EntityUUID);
            }
        }

        struct PreReconcileState
        {
            u64 EntityUUID;
            const InterpolationEntry* Entry;
            std::vector<u8> Bytes;
        };
        std::vector<PreReconcileState> preStates;
        if (smooth)
        {
            for (u64 const entityUUID : predictedEntities)
            {
                auto entityOpt = scene.TryGetEntityWithUUID(UUID(entityUUID));
                if (!entityOpt.has_value())
                {
                    continue;
                }
                Entity entity = *entityOpt;
                for (const auto& entry : entries)
                {
                    if (entry.Smooth == nullptr || entry.Capture == nullptr || entry.Has == nullptr || !entry.Has(entity))
                    {
                        continue;
                    }
                    std::vector<u8> bytes;
                    FMemoryWriter writer(bytes);
                    writer.ArIsNetArchive = true;
                    entry.Capture(writer, entity);
                    preStates.push_back({ entityUUID, &entry, std::move(bytes) });
                }
            }
        }

        for (const auto* input : unconfirmed)
        {
            m_ApplyCallback(scene, input->EntityUUID, input->Data.data(),
                            static_cast<u32>(input->Data.size()));
        }

        // Blend between the captured pre-reconciliation state and the
        // post-resimulation state, per component.
        if (smooth)
        {
            for (const auto& ps : preStates)
            {
                auto entityOpt = scene.TryGetEntityWithUUID(UUID(ps.EntityUUID));
                if (!entityOpt.has_value())
                {
                    continue;
                }
                Entity entity = *entityOpt;
                if (!ps.Entry->Has(entity))
                {
                    continue;
                }
                ps.Entry->Smooth(entity, ps.Bytes, m_SmoothingRate, m_HardSnapThreshold);
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

    void ClientPrediction::SetHardSnapThreshold(f32 distance)
    {
        m_HardSnapThreshold = distance;
    }

    f32 ClientPrediction::GetHardSnapThreshold() const
    {
        return m_HardSnapThreshold;
    }

    void ClientPrediction::ResetSession()
    {
        m_InputBuffer.Clear();
        m_CurrentTick = 0;
        m_LastConfirmedTick = 0;
    }
} // namespace OloEngine
