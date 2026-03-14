#include "OloEnginePCH.h"
#include "NetworkInputBuffer.h"

namespace OloEngine
{
    NetworkInputBuffer::NetworkInputBuffer(u32 capacity)
        : m_Capacity(capacity > 0 ? capacity : kDefaultCapacity)
    {
        m_Entries.resize(m_Capacity);
    }

    void NetworkInputBuffer::Push(u32 tick, u64 entityUUID, std::vector<u8> data)
    {
        m_Entries[m_Head] = InputCommand{ tick, entityUUID, std::move(data) };
        m_Head = (m_Head + 1) % m_Capacity;
        if (m_Count < m_Capacity)
        {
            ++m_Count;
        }
    }

    const InputCommand* NetworkInputBuffer::GetByTick(u32 tick) const
    {
        for (u32 i = 0; i < m_Count; ++i)
        {
            u32 const idx = (m_Head + m_Capacity - 1 - i) % m_Capacity;
            if (m_Entries[idx].Tick == tick)
            {
                return &m_Entries[idx];
            }
        }
        return nullptr;
    }

    std::vector<const InputCommand*> NetworkInputBuffer::GetUnconfirmedInputs(u32 lastConfirmedTick) const
    {
        std::vector<const InputCommand*> result;

        for (u32 i = 0; i < m_Count; ++i)
        {
            u32 const idx = (m_Head + m_Capacity - 1 - i) % m_Capacity;
            if (m_Entries[idx].Tick > lastConfirmedTick)
            {
                result.push_back(&m_Entries[idx]);
            }
        }

        // Sort oldest-first for replay
        std::sort(result.begin(), result.end(),
                  [](const InputCommand* a, const InputCommand* b)
                  { return a->Tick < b->Tick; });

        return result;
    }

    void NetworkInputBuffer::DiscardUpTo(u32 confirmedTick)
    {
        // Walk from oldest to newest, clearing confirmed entries.
        // Do NOT adjust m_Count — GetByTick / GetUnconfirmedInputs already filter
        // by tick value, and adjusting m_Count after non-contiguous clears can
        // cause valid entries to be skipped during iteration.
        for (u32 i = m_Count; i > 0; --i)
        {
            u32 const idx = (m_Head + m_Capacity - i) % m_Capacity;
            if (m_Entries[idx].Tick <= confirmedTick)
            {
                m_Entries[idx].Tick = 0;
                m_Entries[idx].EntityUUID = 0;
                m_Entries[idx].Data.clear();
            }
        }
    }

    u32 NetworkInputBuffer::Size() const
    {
        return m_Count;
    }

    bool NetworkInputBuffer::IsEmpty() const
    {
        return m_Count == 0;
    }

    void NetworkInputBuffer::Clear()
    {
        m_Head = 0;
        m_Count = 0;
        for (auto& entry : m_Entries)
        {
            entry.Tick = 0;
            entry.EntityUUID = 0;
            entry.Data.clear();
        }
    }
} // namespace OloEngine
