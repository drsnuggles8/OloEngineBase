#pragma once

#include "OloEngine/Core/Base.h"
#include <atomic>
#include <vector>
#include <memory>
#include <type_traits>

namespace OloEngine::Audio
{
	//==============================================================================
	/// Lock-free FIFO position tracking inspired by CHOC
	/// Manages read/write positions with atomic operations for thread safety
	struct FIFOPosition
	{
		FIFOPosition() = default;
		~FIFOPosition() = default;

		void reset(u32 capacity)
		{
			m_Capacity = capacity;
			m_WriteMask = capacity - 1u;
			m_ReadPosition.store(0, std::memory_order_relaxed);
			m_WritePosition.store(0, std::memory_order_relaxed);
		}

		u32 getCapacity() const { return m_Capacity; }

		u32 getUsedSlots() const
		{
			const u32 write = m_WritePosition.load(std::memory_order_acquire);
			const u32 read = m_ReadPosition.load(std::memory_order_acquire);
			return write - read;
		}

		u32 getFreeSlots() const
		{
			return m_Capacity - getUsedSlots();
		}

		bool canWrite() const
		{
			return getFreeSlots() > 0;
		}

		bool canRead() const
		{
			return getUsedSlots() > 0;
		}

		u32 getReadIndex() const
		{
			return m_ReadPosition.load(std::memory_order_acquire) & m_WriteMask;
		}

		u32 getWriteIndex() const
		{
			return m_WritePosition.load(std::memory_order_acquire) & m_WriteMask;
		}

		void advanceReadPosition()
		{
			m_ReadPosition.store(m_ReadPosition.load(std::memory_order_relaxed) + 1, 
								 std::memory_order_release);
		}

		void advanceWritePosition()
		{
			m_WritePosition.store(m_WritePosition.load(std::memory_order_relaxed) + 1, 
								  std::memory_order_release);
		}

	private:
		std::atomic<u32> m_ReadPosition{ 0 };
		std::atomic<u32> m_WritePosition{ 0 };
		u32 m_Capacity = 0;
		u32 m_WriteMask = 0;
	};

	//==============================================================================
	/// Single Reader Single Writer Lock-Free FIFO
	/// Optimized for cases where only one thread reads and one thread writes
	/// Provides the highest performance for simple producer-consumer scenarios
	template<typename T>
	class SingleReaderSingleWriterFIFO
	{
	public:
		SingleReaderSingleWriterFIFO() = default;
		~SingleReaderSingleWriterFIFO() = default;

		/// Initialize the FIFO with a given capacity (must be power of 2)
		void reset(u32 capacity)
		{
			OLO_CORE_ASSERT((capacity & (capacity - 1)) == 0, "FIFO capacity must be power of 2");
			m_Items.resize(capacity);
			m_Position.reset(capacity);
		}

		/// Initialize with capacity and default item values
		void reset(u32 capacity, const T& defaultItem)
		{
			OLO_CORE_ASSERT((capacity & (capacity - 1)) == 0, "FIFO capacity must be power of 2");
			m_Items.assign(capacity, defaultItem);
			m_Position.reset(capacity);
		}

		/// Clear the FIFO while keeping capacity
		void clear()
		{
			m_Position.reset(m_Position.getCapacity());
		}

		/// Get number of items currently in FIFO
		u32 getUsedSlots() const { return m_Position.getUsedSlots(); }

		/// Get number of free slots in FIFO  
		u32 getFreeSlots() const { return m_Position.getFreeSlots(); }

		/// Check if FIFO is empty
		bool isEmpty() const { return getUsedSlots() == 0; }

		/// Check if FIFO is full
		bool isFull() const { return getFreeSlots() == 0; }

		/// Try to push an item (copy version)
		bool push(const T& item)
		{
			if (!m_Position.canWrite())
				return false;

			m_Items[m_Position.getWriteIndex()] = item;
			m_Position.advanceWritePosition();
			return true;
		}

		/// Try to push an item (move version)
		bool push(T&& item)
		{
			if (!m_Position.canWrite())
				return false;

			m_Items[m_Position.getWriteIndex()] = std::move(item);
			m_Position.advanceWritePosition();
			return true;
		}

		/// Try to pop an item
		bool pop(T& result)
		{
			if (!m_Position.canRead())
				return false;

			result = std::move(m_Items[m_Position.getReadIndex()]);
			m_Position.advanceReadPosition();
			return true;
		}

		/// Peek at the next item without removing it
		bool peek(T& result) const
		{
			if (!m_Position.canRead())
				return false;

			result = m_Items[m_Position.getReadIndex()];
			return true;
		}

	private:
		std::vector<T> m_Items;
		FIFOPosition m_Position;

		// Non-copyable
		SingleReaderSingleWriterFIFO(const SingleReaderSingleWriterFIFO&) = delete;
		SingleReaderSingleWriterFIFO& operator=(const SingleReaderSingleWriterFIFO&) = delete;
	};

	//==============================================================================
	/// Single Reader Multiple Writer Lock-Free FIFO
	/// Uses a spinlock for write synchronization while keeping reads lock-free
	/// Good for scenarios with one consumer and multiple producers
	template<typename T>
	class SingleReaderMultipleWriterFIFO
	{
	public:
		SingleReaderMultipleWriterFIFO() = default;
		~SingleReaderMultipleWriterFIFO() = default;

		/// Initialize the FIFO with a given capacity (must be power of 2)
		void reset(u32 capacity)
		{
			m_FIFO.reset(capacity);
		}

		/// Initialize with capacity and default item values
		void reset(u32 capacity, const T& defaultItem)
		{
			m_FIFO.reset(capacity, defaultItem);
		}

		/// Clear the FIFO while keeping capacity
		void clear()
		{
			m_FIFO.clear();
		}

		/// Get number of items currently in FIFO
		u32 getUsedSlots() const { return m_FIFO.getUsedSlots(); }

		/// Get number of free slots in FIFO  
		u32 getFreeSlots() const { return m_FIFO.getFreeSlots(); }

		/// Check if FIFO is empty
		bool isEmpty() const { return m_FIFO.isEmpty(); }

		/// Check if FIFO is full
		bool isFull() const { return m_FIFO.isFull(); }

		/// Try to push an item (copy version) - thread-safe for multiple writers
		bool push(const T& item)
		{
			while (m_WriteLock.test_and_set(std::memory_order_acquire))
			{
				// Spin wait
			}
			bool result = m_FIFO.push(item);
			m_WriteLock.clear(std::memory_order_release);
			return result;
		}

		/// Try to push an item (move version) - thread-safe for multiple writers
		bool push(T&& item)
		{
			while (m_WriteLock.test_and_set(std::memory_order_acquire))
			{
				// Spin wait
			}
			bool result = m_FIFO.push(std::move(item));
			m_WriteLock.clear(std::memory_order_release);
			return result;
		}

		/// Try to pop an item - lock-free for single reader
		bool pop(T& result)
		{
			return m_FIFO.pop(result);
		}

		/// Peek at the next item without removing it - lock-free for single reader
		bool peek(T& result) const
		{
			return m_FIFO.peek(result);
		}

	private:
		SingleReaderSingleWriterFIFO<T> m_FIFO;
		mutable std::atomic_flag m_WriteLock = ATOMIC_FLAG_INIT;

		// Non-copyable
		SingleReaderMultipleWriterFIFO(const SingleReaderMultipleWriterFIFO&) = delete;
		SingleReaderMultipleWriterFIFO& operator=(const SingleReaderMultipleWriterFIFO&) = delete;
	};

} // namespace OloEngine::Audio