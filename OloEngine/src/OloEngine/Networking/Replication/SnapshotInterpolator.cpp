#include "OloEnginePCH.h"
#include "SnapshotInterpolator.h"
#include "EntitySnapshot.h"
#include "ComponentReplicator.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <algorithm>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    // Parse a snapshot buffer into a map of UUID → TransformComponent.
    static std::unordered_map<u64, TransformComponent> ParseSnapshot(const std::vector<u8>& data)
    {
        std::unordered_map<u64, TransformComponent> result;
        if (data.empty())
        {
            return result;
        }

        FMemoryReader reader(data);
        reader.ArIsNetArchive = true;

        while (reader.Tell() < reader.TotalSize() && !reader.IsError())
        {
            u64 uuid = 0;
            reader << uuid;

            TransformComponent transform;
            ComponentReplicator::Serialize(reader, transform);
            result[uuid] = transform;
        }

        return result;
    }

    SnapshotInterpolator::SnapshotInterpolator(u32 bufferCapacity)
        : m_Buffer(bufferCapacity)
    {
    }

    void SnapshotInterpolator::PushSnapshot(u32 tick, std::vector<u8> data)
    {
        OLO_PROFILE_FUNCTION();

        m_Buffer.Push(tick, std::move(data));
        if (tick > m_LatestReceivedTick)
        {
            m_LatestReceivedTick = tick;
        }
    }

    void SnapshotInterpolator::Interpolate(Scene& scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        m_CurrentTime += dt;

        if (m_Buffer.Size() < 2)
        {
            return;
        }

        // Compute the render tick: latest received tick minus delay in ticks
        f32 const delayTicks = m_RenderDelay * static_cast<f32>(m_ServerTickRate);
        f32 const renderTick = static_cast<f32>(m_LatestReceivedTick) - delayTicks;

        if (renderTick < 0.0f)
        {
            return;
        }

        u32 const renderTickFloor = static_cast<u32>(renderTick);

        auto bracket = m_Buffer.GetBracketingEntries(renderTickFloor);
        if (!bracket.has_value())
        {
            return;
        }

        const auto* before = bracket->Before;
        const auto* after = bracket->After;

        if (before->Tick == after->Tick)
        {
            // Same tick — just apply directly
            EntitySnapshot::Apply(scene, before->Data);
            return;
        }

        // Compute interpolation factor (0..1) between the two bracketing snapshots.
        f32 const tickRange = static_cast<f32>(after->Tick - before->Tick);
        f32 const alpha = std::clamp((renderTick - static_cast<f32>(before->Tick)) / tickRange, 0.0f, 1.0f);

        // Parse both snapshots (with caching)
        if (before->Tick != m_CachedBeforeTick)
        {
            m_CachedBefore = ParseSnapshot(before->Data);
            m_CachedBeforeTick = before->Tick;
        }
        if (after->Tick != m_CachedAfterTick)
        {
            m_CachedAfter = ParseSnapshot(after->Data);
            m_CachedAfterTick = after->Tick;
        }

        const auto& beforeEntities = m_CachedBefore;
        const auto& afterEntities = m_CachedAfter;

        // Interpolate and apply to the scene
        auto view = scene.GetAllEntitiesWith<NetworkIdentityComponent, TransformComponent>();
        for (auto entityHandle : view)
        {
            Entity entity{ entityHandle, &scene };
            auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
            if (!nic.IsReplicated)
            {
                continue;
            }

            // Only interpolate server-authoritative entities
            if (nic.Authority != ENetworkAuthority::Server)
            {
                continue;
            }

            u64 const uuid = static_cast<u64>(entity.GetUUID());

            auto itBefore = beforeEntities.find(uuid);
            auto itAfter = afterEntities.find(uuid);

            if (itBefore != beforeEntities.end() && itAfter != afterEntities.end())
            {
                auto& transform = entity.GetComponent<TransformComponent>();
                const auto& tb = itBefore->second;
                const auto& ta = itAfter->second;

                // Lerp translation
                transform.Translation = glm::mix(tb.Translation, ta.Translation, alpha);
                // Slerp rotation via quaternion for correct spherical interpolation
                glm::quat const qBefore = glm::quat(tb.Rotation);
                glm::quat const qAfter = glm::quat(ta.Rotation);
                glm::quat const qInterp = glm::slerp(qBefore, qAfter, alpha);
                transform.Rotation = glm::eulerAngles(qInterp);
                // Lerp scale
                transform.Scale = glm::mix(tb.Scale, ta.Scale, alpha);
            }
            else if (itAfter != afterEntities.end())
            {
                // Entity only exists in the newer snapshot — snap to it
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = itAfter->second.Translation;
                transform.Rotation = itAfter->second.Rotation;
                transform.Scale = itAfter->second.Scale;
            }
        }
    }

    void SnapshotInterpolator::SetRenderDelay(f32 seconds)
    {
        m_RenderDelay = seconds;
    }

    f32 SnapshotInterpolator::GetRenderDelay() const
    {
        return m_RenderDelay;
    }

    void SnapshotInterpolator::SetServerTickRate(u32 ticksPerSecond)
    {
        m_ServerTickRate = ticksPerSecond;
    }

    u32 SnapshotInterpolator::GetServerTickRate() const
    {
        return m_ServerTickRate;
    }

    f32 SnapshotInterpolator::GetRenderTick() const
    {
        f32 const delayTicks = m_RenderDelay * static_cast<f32>(m_ServerTickRate);
        return static_cast<f32>(m_LatestReceivedTick) - delayTicks;
    }

    const SnapshotBuffer& SnapshotInterpolator::GetBuffer() const
    {
        return m_Buffer;
    }
} // namespace OloEngine
