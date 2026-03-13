#include "OloEnginePCH.h"
#include "SnapshotBuffer.h"

namespace OloEngine
{
	SnapshotBuffer::SnapshotBuffer(u32 capacity)
		: m_Capacity(capacity)
	{
		m_Entries.resize(capacity);
	}

	void SnapshotBuffer::Push(u32 tick, std::vector<u8> data)
	{
		m_Entries[m_Head] = Entry{ tick, std::move(data) };
		m_Head = (m_Head + 1) % m_Capacity;
		if (m_Count < m_Capacity)
		{
			++m_Count;
		}
	}

	const SnapshotBuffer::Entry* SnapshotBuffer::GetLatest() const
	{
		if (m_Count == 0)
		{
			return nullptr;
		}

		u32 const idx = (m_Head + m_Capacity - 1) % m_Capacity;
		return &m_Entries[idx];
	}

	const SnapshotBuffer::Entry* SnapshotBuffer::GetByTick(u32 tick) const
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

	std::optional<SnapshotBuffer::InterpolationPair> SnapshotBuffer::GetBracketingEntries(u32 tick) const
	{
		if (m_Count < 2)
		{
			return std::nullopt;
		}

		// Walk from newest to oldest, find the first entry with Tick <= tick
		// and the first entry with Tick > tick, forming a bracket.
		const Entry* before = nullptr;
		const Entry* after = nullptr;

		for (u32 i = 0; i < m_Count; ++i)
		{
			u32 const idx = (m_Head + m_Capacity - 1 - i) % m_Capacity;
			const Entry& entry = m_Entries[idx];

			if (entry.Tick <= tick && !before)
			{
				before = &entry;
			}
			else if (entry.Tick > tick)
			{
				after = &entry;
			}

			if (before && after)
			{
				break;
			}
		}

		// If tick is beyond the newest entry, use the two newest
		if (!after && before)
		{
			// tick >= all entries — find the two newest
			u32 const newestIdx = (m_Head + m_Capacity - 1) % m_Capacity;
			u32 const secondIdx = (m_Head + m_Capacity - 2) % m_Capacity;
			before = &m_Entries[secondIdx];
			after = &m_Entries[newestIdx];
		}

		// If tick is before the oldest entry, use the two oldest
		if (!before && after)
		{
			u32 const oldestIdx = (m_Head + m_Capacity - m_Count) % m_Capacity;
			u32 const secondOldestIdx = (oldestIdx + 1) % m_Capacity;
			before = &m_Entries[oldestIdx];
			after = &m_Entries[secondOldestIdx];
		}

		if (before && after)
		{
			return InterpolationPair{ before, after };
		}

		return std::nullopt;
	}

	u32 SnapshotBuffer::Size() const
	{
		return m_Count;
	}

	u32 SnapshotBuffer::Capacity() const
	{
		return m_Capacity;
	}

	bool SnapshotBuffer::IsEmpty() const
	{
		return m_Count == 0;
	}

	void SnapshotBuffer::Clear()
	{
		m_Head = 0;
		m_Count = 0;
		for (auto& entry : m_Entries)
		{
			entry.Tick = 0;
			entry.Data.clear();
		}
	}
} // namespace OloEngine
