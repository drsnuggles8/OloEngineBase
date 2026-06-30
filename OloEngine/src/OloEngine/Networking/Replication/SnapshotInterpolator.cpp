#include "OloEnginePCH.h"
#include "SnapshotInterpolator.h"
#include "EntitySnapshot.h"
#include "ComponentInterpolationRegistry.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Locate a component's serialized bytes within a parsed entity record by
        // its wire id. Few components per entity, so a linear scan is cheapest.
        [[nodiscard]] const std::vector<u8>* FindComponentBytes(const SnapshotEntity& comps, u32 id)
        {
            for (const auto& sc : comps)
            {
                if (sc.Id == id)
                {
                    return &sc.Bytes;
                }
            }
            return nullptr;
        }
    } // namespace

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

    void SnapshotInterpolator::Interpolate(Scene& scene, [[maybe_unused]] f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Buffer.Size() < 2)
        {
            return;
        }

        // Compute the render tick: latest received tick minus delay in ticks
        f32 const delayTicks = m_RenderDelay * static_cast<f32>(m_ServerTickRate);
        f32 const renderTick = static_cast<f32>(m_LatestReceivedTick) - delayTicks;

        // Defend the integer cast below: a non-finite renderTick (which a NaN slips
        // past `< 0.0f`, since every comparison with NaN is false) would make the
        // static_cast<u32> undefined behaviour. m_RenderDelay is validated at the
        // setter, but guard here too — wire/config data is untrusted.
        if (!std::isfinite(renderTick) || renderTick < 0.0f)
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

        // Parse both snapshots into per-entity, per-component byte-blobs (cached
        // by tick so a static bracket isn't re-parsed every frame).
        if (before->Tick != m_CachedBeforeTick)
        {
            m_CachedBefore = EntitySnapshot::Parse(before->Data);
            m_CachedBeforeTick = before->Tick;
        }
        if (after->Tick != m_CachedAfterTick)
        {
            m_CachedAfter = EntitySnapshot::Parse(after->Data);
            m_CachedAfterTick = after->Tick;
        }

        const auto& beforeEntities = m_CachedBefore;
        const auto& afterEntities = m_CachedAfter;
        const auto& entries = ComponentInterpolationRegistry::GetEntries();

        // Interpolate the registered component set and apply to the scene.
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
            const SnapshotEntity* beforeComps = (itBefore != beforeEntities.end()) ? &itBefore->second : nullptr;
            const SnapshotEntity* afterComps = (itAfter != afterEntities.end()) ? &itAfter->second : nullptr;
            if (beforeComps == nullptr && afterComps == nullptr)
            {
                continue;
            }

            for (const auto& entry : entries)
            {
                if (entry.Has == nullptr || !entry.Has(entity))
                {
                    continue;
                }

                const std::vector<u8>* beforeBytes = (beforeComps != nullptr) ? FindComponentBytes(*beforeComps, entry.Id) : nullptr;
                const std::vector<u8>* afterBytes = (afterComps != nullptr) ? FindComponentBytes(*afterComps, entry.Id) : nullptr;

                if (beforeBytes != nullptr && afterBytes != nullptr && entry.Interpolate != nullptr)
                {
                    // Both brackets carry it — blend per the component's policy.
                    entry.Interpolate(entity, *beforeBytes, *afterBytes, alpha);
                }
                else if (afterBytes != nullptr && entry.Snap != nullptr)
                {
                    // Only the newer snapshot has it — snap to it.
                    entry.Snap(entity, *afterBytes);
                }
                // Only the older snapshot has it (entity leaving the newer one) —
                // keep the last-known value, matching the prior transform behaviour.
            }
        }
    }

    void SnapshotInterpolator::SetRenderDelay(f32 seconds)
    {
        // Render delay feeds the tick math in Interpolate()/GetRenderTick(); a
        // non-finite or negative value would poison renderTick and the u32 cast
        // (NaN slips past `< 0.0f`). Reject it and keep the previous valid delay.
        if (!std::isfinite(seconds) || seconds < 0.0f)
        {
            OLO_CORE_WARN("[SnapshotInterpolator] Ignoring invalid render delay {} (must be finite and >= 0); keeping {}", seconds, m_RenderDelay);
            return;
        }

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
