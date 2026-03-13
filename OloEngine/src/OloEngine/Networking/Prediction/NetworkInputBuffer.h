#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    // A single timestamped input command sent from client to server.
    struct InputCommand
    {
        u32 Tick = 0;
        u64 EntityUUID = 0;
        std::vector<u8> Data;
    };

    // Ring buffer of timestamped InputCommands.
    // Client records inputs here for prediction; server receives and processes them.
    class NetworkInputBuffer
    {
      public:
        static constexpr u32 kDefaultCapacity = 128;

        explicit NetworkInputBuffer(u32 capacity = kDefaultCapacity);

        // Record a new input command at the given tick.
        void Push(u32 tick, u64 entityUUID, std::vector<u8> data);

        // Get a specific input by tick. Returns nullptr if not found.
        [[nodiscard]] const InputCommand* GetByTick(u32 tick) const;

        // Get all unconfirmed inputs (ticks > lastConfirmedTick).
        [[nodiscard]] std::vector<const InputCommand*> GetUnconfirmedInputs(u32 lastConfirmedTick) const;

        // Discard all inputs with tick <= confirmedTick.
        void DiscardUpTo(u32 confirmedTick);

        [[nodiscard]] u32 Size() const;
        [[nodiscard]] bool IsEmpty() const;

        void Clear();

      private:
        std::vector<InputCommand> m_Entries;
        u32 m_Capacity = kDefaultCapacity;
        u32 m_Head = 0;
        u32 m_Count = 0;
    };
} // namespace OloEngine
