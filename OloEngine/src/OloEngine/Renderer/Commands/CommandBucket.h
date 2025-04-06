#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandPacket.h"
#include "CommandAllocator.h"
#include <vector>

namespace OloEngine
{
	// Forward declarations
	class RendererAPI;

	// Configuration for command bucket processing
	struct CommandBucketConfig
	{
		bool EnableSorting = true;      // Sort commands to minimize state changes
		bool EnableBatching = true;     // Attempt to batch similar commands
		u32 MaxMeshInstances = 100;     // Maximum instances for instanced mesh rendering
	};

	// A bucket of command packets that can be sorted and executed together
	class CommandBucket
	{
	public:
		CommandBucket(const CommandBucketConfig& config = CommandBucketConfig());
		~CommandBucket();

		// Disallow copying
		CommandBucket(const CommandBucket&) = delete;
		CommandBucket& operator=(const CommandBucket&) = delete;

		// Allow moving
		CommandBucket(CommandBucket&& other) noexcept;
		CommandBucket& operator=(CommandBucket&& other) noexcept;

		// Add a command packet to the bucket
		void AddCommand(CommandPacket* packet);

		template<typename T>
		CommandPacket* Submit(const T& commandData, const PacketMetadata& metadata = {}, CommandAllocator* allocator = nullptr)
		{
			static_assert(std::is_pod<T>::value, "Command data must be POD");

			if (!allocator) // Now this check is valid
			{
				OLO_CORE_ERROR("CommandBucket::Submit: No allocator provided");
				return nullptr;
			}

			std::lock_guard<std::mutex> lock(m_Mutex);

			CommandPacket* packet = allocator->CreateCommandPacket(commandData, metadata); // Use -> instead of .
			if (packet)
			{
				// The rest of your code remains the same
				if (!m_Head)
				{
					m_Head = packet;
					m_Tail = packet;
				}
				else
				{
					m_Tail->SetNext(packet);
					m_Tail = packet;
				}

				m_CommandCount++;
				m_Stats.TotalCommands++;

				// Adding a new command invalidates sorting and batching
				m_IsSorted = false;
				m_IsBatched = false;
			}

			return packet;
		}


		// Sort commands for optimal rendering (minimizes state changes)
		void SortCommands();

		// Batch compatible commands into instanced commands where possible
		void BatchCommands(CommandAllocator& allocator);

		// Execute all commands in the bucket
		void Execute(RendererAPI& api);

		// Clear the bucket (doesn't free memory, just resets)
		void Clear();

		// Reset the bucket and free all memory
		void Reset(CommandAllocator& allocator);

		// Statistics
		struct Statistics
		{
			u32 TotalCommands = 0;       // Total commands in the bucket
			u32 BatchedCommands = 0;     // Commands that were successfully batched
			u32 DrawCalls = 0;           // Actual draw calls executed
			u32 StateChanges = 0;        // State changes performed
		};

		// Get execution statistics
		Statistics GetStatistics() const { return m_Stats; }

		// Get command count
		sizet GetCommandCount() const { return m_CommandCount; }

	private:
		// Transform buffer for instanced rendering
		class InstancedTransformBuffer
		{
		public:
			InstancedTransformBuffer(u32 maxInstances)
				: m_Capacity(maxInstances)
			{
				m_Buffer = new glm::mat4[m_Capacity];
			}

			~InstancedTransformBuffer()
			{
				delete[] m_Buffer;
			}

			// Disallow copying
			InstancedTransformBuffer(const InstancedTransformBuffer&) = delete;
			InstancedTransformBuffer& operator=(const InstancedTransformBuffer&) = delete;

			// Allow moving
			InstancedTransformBuffer(InstancedTransformBuffer&& other) noexcept
				: m_Buffer(other.m_Buffer), m_Capacity(other.m_Capacity)
			{
				other.m_Buffer = nullptr;
				other.m_Capacity = 0;
			}

			InstancedTransformBuffer& operator=(InstancedTransformBuffer&& other) noexcept
			{
				if (this != &other)
				{
					delete[] m_Buffer;
					m_Buffer = other.m_Buffer;
					m_Capacity = other.m_Capacity;
					other.m_Buffer = nullptr;
					other.m_Capacity = 0;
				}
				return *this;
			}

			glm::mat4* Allocate(u32 count)
			{
				if (m_Capacity < count)
					return nullptr;
				return m_Buffer;
			}

			glm::mat4* GetBuffer() { return m_Buffer; }
			u32 GetCapacity() const { return m_Capacity; }

		private:
			glm::mat4* m_Buffer = nullptr;
			u32 m_Capacity = 0;
		};

		// Instance transform buffer management
		std::vector<InstancedTransformBuffer> m_TransformBuffers;
		// Maps for tracking instanced command transform buffers
		std::unordered_map<CommandPacket*, u32> m_PacketToBufferIndex;

		// Try to merge compatible commands for batching
		bool TryMergeCommands(CommandPacket* target, CommandPacket* source, CommandAllocator& allocator);

		// Convert a DrawMeshCommand to DrawMeshInstancedCommand for batching
		CommandPacket* ConvertToInstanced(CommandPacket* meshPacket, CommandAllocator& allocator);

		// Head of the linked list of commands
		CommandPacket* m_Head = nullptr;

		// Tail of the linked list for O(1) append
		CommandPacket* m_Tail = nullptr;

		// Count of commands in the bucket
		sizet m_CommandCount = 0;

		// Cached sorted array of commands (built during SortCommands)
		std::vector<CommandPacket*> m_SortedCommands;

		// Configuration
		CommandBucketConfig m_Config;

		// Flags for bucket state
		bool m_IsSorted = false;
		bool m_IsBatched = false;

		// Statistics
		Statistics m_Stats;

		mutable std::mutex m_Mutex;
	};
}
