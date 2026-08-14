#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine
{
    // Outcome of a Steam platform call.
    //
    // Deliberately distinguishes "the platform isn't there" from "the platform said no": callers
    // that want to degrade quietly check Unavailable, while callers that want to report a genuine
    // bug (a typo'd achievement id, a full cloud quota) get a distinct value. Collapsing these
    // into a bool is how a mis-typed achievement id becomes indistinguishable from Steam not
    // running, which is exactly the confusion this feature must not create.
    enum class SteamResult : u8
    {
        // The call did what was asked.
        Success = 0,

        // Steam is not usable at all: either OLO_WITH_STEAM=0 (not compiled in) or the client
        // isn't running / SteamAPI_Init() failed. Never an error the game should surface.
        Unavailable,

        // The named thing does not exist — an achievement id Steam doesn't know, a cloud file
        // that isn't there. Usually a content bug worth logging.
        NotFound,

        // The operation was a no-op because the target was already in the requested state (an
        // achievement already unlocked). Not an error; callers may treat it as Success.
        AlreadySet,

        // Steam was available and understood the request but rejected or failed it — quota
        // exhausted, a write that didn't land, stats that wouldn't store.
        Failed,

        // The caller passed something structurally invalid (an empty id, an oversized payload).
        InvalidArgument,
    };

    // True when the call achieved the caller's intent, treating "already in that state" as fine.
    // Most gameplay code wants this rather than an exact-equality check against Success.
    [[nodiscard]] constexpr bool SteamSucceeded(SteamResult result) noexcept
    {
        return result == SteamResult::Success || result == SteamResult::AlreadySet;
    }

    [[nodiscard]] constexpr const char* SteamResultToString(SteamResult result) noexcept
    {
        switch (result)
        {
            case SteamResult::Success:
                return "Success";
            case SteamResult::Unavailable:
                return "Unavailable";
            case SteamResult::NotFound:
                return "NotFound";
            case SteamResult::AlreadySet:
                return "AlreadySet";
            case SteamResult::Failed:
                return "Failed";
            case SteamResult::InvalidArgument:
                return "InvalidArgument";
        }
        return "Unknown";
    }

    // Steam Cloud storage budget, in bytes, as reported by the backend.
    struct SteamCloudQuota
    {
        u64 TotalBytes = 0;
        u64 AvailableBytes = 0;

        [[nodiscard]] constexpr u64 UsedBytes() const noexcept
        {
            return TotalBytes >= AvailableBytes ? TotalBytes - AvailableBytes : 0;
        }
    };
} // namespace OloEngine
