#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Tiered network update frequency based on distance from the observer.
    // Closer entities get more frequent updates; distant entities get less.
    enum class ENetworkRelevanceTier : u8
    {
        Full = 0,    // Every tick (close range)
        Reduced = 1, // Every 3rd tick (mid range)
        Minimal = 2  // Every 10th tick (far range)
    };

    // Configuration for tier distance bands (in world units).
    struct RelevanceTierConfig
    {
        f32 CloseRange = 50.0f; // [0, CloseRange] → Full
        f32 MidRange = 150.0f;  // (CloseRange, MidRange] → Reduced
        f32 FarRange = 300.0f;  // (MidRange, FarRange] → Minimal; beyond → not relevant
    };

    // Returns the relevance tier for a given squared distance.
    inline ENetworkRelevanceTier GetRelevanceTier(f32 distanceSq, const RelevanceTierConfig& config)
    {
        if (distanceSq <= config.CloseRange * config.CloseRange)
        {
            return ENetworkRelevanceTier::Full;
        }
        if (distanceSq <= config.MidRange * config.MidRange)
        {
            return ENetworkRelevanceTier::Reduced;
        }
        return ENetworkRelevanceTier::Minimal;
    }

    // Returns true if an entity at the given tier should be updated on the given tick.
    inline bool ShouldUpdateOnTick(ENetworkRelevanceTier tier, u32 tick)
    {
        switch (tier)
        {
            case ENetworkRelevanceTier::Full:
                return true;
            case ENetworkRelevanceTier::Reduced:
                return (tick % 3) == 0;
            case ENetworkRelevanceTier::Minimal:
                return (tick % 10) == 0;
        }
        return true;
    }
} // namespace OloEngine
