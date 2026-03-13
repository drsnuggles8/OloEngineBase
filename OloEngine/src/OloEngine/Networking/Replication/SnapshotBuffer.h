#pragma once

#include "OloEngine/Core/Base.h"

#include <array>
#include <optional>
#include <vector>

namespace OloEngine
{
    // Ring buffer storing the last N entity snapshots, each tagged with a tick number.
    // Used by server for delta baselines and by client for interpolation.
    class SnapshotBuffer
    {
      public:
        struct Entry
        {
            u32 Tick = 0;
            std::vector<u8> Data;
        };

        static constexpr u32 kDefaultCapacity = 64;

        explicit SnapshotBuffer(u32 capacity = kDefaultCapacity);

        // Push a new snapshot. Overwrites the oldest entry when full.
        void Push(u32 tick, std::vector<u8> data);

        // Get the most recently pushed snapshot.
        [[nodiscard]] const Entry* GetLatest() const;

        // Get a snapshot by exact tick number. Returns nullptr if not found.
        [[nodiscard]] const Entry* GetByTick(u32 tick) const;

        // Get the two entries that bracket the given tick for interpolation.
        // Returns {before, after} where before.Tick <= tick <= after.Tick.
        // Returns nullopt if fewer than 2 entries or tick is out of range.
        struct InterpolationPair
        {
            const Entry* Before = nullptr;
            const Entry* After = nullptr;
        };
        [[nodiscard]] std::optional<InterpolationPair> GetBracketingEntries(u32 tick) const;

        [[nodiscard]] u32 Size() const;
        [[nodiscard]] u32 Capacity() const;
        [[nodiscard]] bool IsEmpty() const;

        void Clear();

      private:
        std::vector<Entry> m_Entries;
        u32 m_Capacity = kDefaultCapacity;
        u32 m_Head = 0; // Next write position
        u32 m_Count = 0;
    };
} // namespace OloEngine
