#include "OloEnginePCH.h"
#include "CommandBucket.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include <algorithm>

namespace OloEngine
{
	CommandBucket::CommandBucket(const CommandBucketConfig& config)
		: m_Config(config)
	{
		// Initialize statistics
		m_Stats = Statistics();
	}

	CommandBucket::~CommandBucket()
	{
		// The actual command memory is managed by CommandAllocator
		// We just need to clear our references
		Clear();
	}

	CommandBucket::CommandBucket(CommandBucket&& other) noexcept
		: m_Head(other.m_Head),
		m_Tail(other.m_Tail),
		m_CommandCount(other.m_CommandCount),
		m_SortedCommands(std::move(other.m_SortedCommands)),
		m_Config(other.m_Config),
		m_IsSorted(other.m_IsSorted),
		m_IsBatched(other.m_IsBatched),
		m_Stats(other.m_Stats)
	{
		// Reset the source object
		other.m_Head = nullptr;
		other.m_Tail = nullptr;
		other.m_CommandCount = 0;
		other.m_IsSorted = false;
		other.m_IsBatched = false;
		other.m_Stats = Statistics();
	}

	CommandBucket& CommandBucket::operator=(CommandBucket&& other) noexcept
	{
		if (this != &other)
		{
			m_Head = other.m_Head;
			m_Tail = other.m_Tail;
			m_CommandCount = other.m_CommandCount;
			m_SortedCommands = std::move(other.m_SortedCommands);
			m_Config = other.m_Config;
			m_IsSorted = other.m_IsSorted;
			m_IsBatched = other.m_IsBatched;
			m_Stats = other.m_Stats;

			// Reset the source object
			other.m_Head = nullptr;
			other.m_Tail = nullptr;
			other.m_CommandCount = 0;
			other.m_IsSorted = false;
			other.m_IsBatched = false;
			other.m_Stats = Statistics();
		}
		return *this;
	}

	void CommandBucket::AddCommand(CommandPacket* packet)
	{
		OLO_PROFILE_FUNCTION();

		if (!packet)
			return;

		packet->SetNext(nullptr); 

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

	void CommandBucket::SortCommands()
	{
		OLO_PROFILE_FUNCTION();

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (!m_Config.EnableSorting || m_IsSorted || m_CommandCount <= 1)
			return;

		// Build a vector of command pointers for sorting
		m_SortedCommands.clear();
		m_SortedCommands.reserve(m_CommandCount);

		// First, group commands by dependency chains
		std::vector<std::vector<CommandPacket*>> dependencyGroups;
		std::vector<CommandPacket*> currentGroup;
		CommandPacket* current = m_Head;
		while (current)
		{
			// Start a new group if this is the first command or if it depends on previous
			if (currentGroup.empty() || current->GetMetadata().m_DependsOnPrevious)
			{
				// If we have an existing group, finalize it
				if (!currentGroup.empty())
				{
					dependencyGroups.push_back(std::move(currentGroup));
					currentGroup = std::vector<CommandPacket*>();
				}
			}

			// Add the current command to the current group
			currentGroup.push_back(current);
			current = current->GetNext();
		}

		// Add the last group if it's not empty
		if (!currentGroup.empty())
			dependencyGroups.push_back(std::move(currentGroup));

		// Now sort each group internally by key
		for (auto& group : dependencyGroups)
		{
			// Only sort if there's more than one command in a group
			if (group.size() > 1)
			{
				std::stable_sort(group.begin(), group.end(),
					[](const CommandPacket* a, const CommandPacket* b)
				{
					return *a < *b;
				});
			}
		}

		// Rebuild the linked list from the sorted groups
		if (!dependencyGroups.empty())
		{
			// Link the first command
			m_Head = dependencyGroups[0][0];
			current = m_Head;
			m_SortedCommands.push_back(current);

			// Link each group
			for (sizet groupIdx = 0; groupIdx < dependencyGroups.size(); groupIdx++)
			{
				const auto& group = dependencyGroups[groupIdx];

				// Link commands within the group
				for (sizet cmdIdx = (groupIdx == 0 ? 1 : 0); cmdIdx < group.size(); cmdIdx++)
				{
					current->SetNext(group[cmdIdx]);
					current = group[cmdIdx];
					m_SortedCommands.push_back(current);
				}

				// Link to the next group
				if (groupIdx < dependencyGroups.size() - 1)
				{
					current->SetNext(dependencyGroups[groupIdx + 1][0]);
					current = dependencyGroups[groupIdx + 1][0];
					m_SortedCommands.push_back(current);
				}
			}

			// Set the tail and terminate the list
			m_Tail = current;
			m_Tail->SetNext(nullptr);
		}

		m_IsSorted = true;
	}

	CommandPacket* CommandBucket::ConvertToInstanced(CommandPacket* meshPacket, CommandAllocator& allocator)
	{
		OLO_PROFILE_FUNCTION();

		if (!meshPacket || meshPacket->GetCommandType() != CommandType::DrawMesh)
			return nullptr;

		auto const* meshCmd = meshPacket->GetCommandData<DrawMeshCommand>();
		if (!meshCmd)
			return nullptr;

		// Create a new instanced command
		DrawMeshInstancedCommand instancedCmd;

		// Copy header
		instancedCmd.header.type = CommandType::DrawMeshInstanced;
		instancedCmd.header.dispatchFn = nullptr; // Will be set during initialization

		// Copy mesh data using full objects
		instancedCmd.mesh = meshCmd->mesh;
		instancedCmd.vertexArray = meshCmd->vertexArray;
		instancedCmd.indexCount = meshCmd->indexCount;

		// Initial instance count is 1
		instancedCmd.instanceCount = 1;

		// Allocate transforms array
		instancedCmd.transforms.resize(m_Config.MaxMeshInstances);
		instancedCmd.transforms[0] = meshCmd->transform;

		// Copy material properties
		instancedCmd.ambient = meshCmd->ambient;
		instancedCmd.diffuse = meshCmd->diffuse;
		instancedCmd.specular = meshCmd->specular;
		instancedCmd.shininess = meshCmd->shininess;
		instancedCmd.useTextureMaps = meshCmd->useTextureMaps;

		// Copy textures using actual references
		instancedCmd.diffuseMap = meshCmd->diffuseMap;
		instancedCmd.specularMap = meshCmd->specularMap;

		// Copy shader reference
		instancedCmd.shader = meshCmd->shader;

		// Create a new packet for the instanced command
		PacketMetadata metadata = meshPacket->GetMetadata();
		CommandPacket* instancedPacket = allocator.CreateCommandPacket(instancedCmd, metadata);

		return instancedPacket;
	}


	bool CommandBucket::TryMergeCommands(CommandPacket* target, CommandPacket* source, CommandAllocator& allocator)
	{
		OLO_PROFILE_FUNCTION();

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (!target || !source || !target->CanBatchWith(*source))
			return false;

		// Handle batching based on command type
		CommandType targetType = target->GetCommandType();

		// We can have a few different scenarios:
		// 1. Both are DrawMeshCommand - Convert to DrawMeshInstancedCommand
		// 2. target is DrawMeshInstancedCommand, source is DrawMeshCommand - Add to instance
		// 3. Both are DrawQuadCommand - Can't combine directly with the current design

		if (CommandType sourceType = source->GetCommandType();
			targetType == CommandType::DrawMesh && sourceType == CommandType::DrawMesh)
		{
			// Convert to instanced command if it's not already
			CommandPacket* instancedPacket = ConvertToInstanced(target, allocator);
			if (!instancedPacket)
				return false;

			// Replace target with instancedPacket in the linked list
			instancedPacket->SetNext(target->GetNext());

			// Find the packet that points to target and update it
			if (m_Head == target)
			{
				m_Head = instancedPacket;
			}
			else
			{
				CommandPacket* prev = m_Head;
				while (prev && prev->GetNext() != target)
					prev = prev->GetNext();

				if (prev)
					prev->SetNext(instancedPacket);
			}

			if (m_Tail == target)
				m_Tail = instancedPacket;

			// Now add source transform to the new instanced command
			auto const* sourceCmd = source->GetCommandData<DrawMeshCommand>();

			if (auto* instancedCmd = instancedPacket->GetCommandData<DrawMeshInstancedCommand>();
				instancedCmd && sourceCmd && instancedCmd->instanceCount < instancedCmd->transforms.size())
			{
				instancedCmd->transforms[instancedCmd->instanceCount++] = sourceCmd->transform;
				return true;
			}

			return false;
		}
		else if (targetType == CommandType::DrawMeshInstanced && sourceType == CommandType::DrawMesh)
		{
			// Add source transform to existing instanced command
			auto* instancedCmd = target->GetCommandData<DrawMeshInstancedCommand>();
			auto const* sourceCmd = source->GetCommandData<DrawMeshCommand>();

			if (instancedCmd && sourceCmd && instancedCmd->instanceCount < instancedCmd->transforms.size())
			{
				instancedCmd->transforms[instancedCmd->instanceCount++] = sourceCmd->transform;
				return true;
			}
		}

		// Other command types can't be merged directly
		return false;
	}

	void CommandBucket::BatchCommands(CommandAllocator& allocator)
	{
		OLO_PROFILE_FUNCTION();

		std::lock_guard<std::mutex> lock(m_Mutex);

		if (!m_Config.EnableBatching || m_IsBatched || m_CommandCount <= 1)
			return;

		// Make sure commands are sorted first for optimal batching
		if (!m_IsSorted)
			SortCommands();

		// Start with the first command
		CommandPacket* current = m_Head;

		while (current)
		{
			CommandPacket* next = current->GetNext();
			// Try to merge with subsequent compatible commands
			while (next && TryMergeCommands(current, next, allocator))
			{
				// Merging succeeded, remove the next command from the list
				next = next->GetNext();
				current->SetNext(next);

				// Decrement command count since we've merged one
				m_CommandCount--;
				m_Stats.BatchedCommands++;
			}

			// Move to the next command
			current = next;
		}

		// Update the tail pointer to the last node
		if (m_Head)
		{
			current = m_Head;
			while (current->GetNext())
				current = current->GetNext();
			m_Tail = current;
		}

		// Rebuild sorted commands array if it exists
		if (!m_SortedCommands.empty())
		{
			m_SortedCommands.clear();
			m_SortedCommands.reserve(m_CommandCount);

			current = m_Head;
			while (current)
			{
				m_SortedCommands.push_back(current);
				current = current->GetNext();
			}
		}

		m_IsBatched = true;
	}

	void CommandBucket::Execute(RendererAPI& rendererAPI)
	{
		OLO_PROFILE_FUNCTION();

		// Take a snapshot of the head pointer under lock
		CommandPacket const* current;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Reset execution statistics
			m_Stats.DrawCalls = 0;
			m_Stats.StateChanges = 0;
			current = m_Head;
		}

		// Execute all commands in order
		while (current)
		{
			// Track statistics
			if (CommandType type = current->GetCommandType();
				type == CommandType::DrawMesh ||
				type == CommandType::DrawMeshInstanced ||
				type == CommandType::DrawQuad)
			{
				m_Stats.DrawCalls++;
			}
			else if (type != CommandType::Invalid)
			{
				m_Stats.StateChanges++;
			}

			current->Execute(rendererAPI);
			current = current->GetNext();
		}
	}

	void CommandBucket::Clear()
	{
		m_Head = nullptr;
		m_Tail = nullptr;
		m_CommandCount = 0;
		m_SortedCommands.clear();

		// Important: clear transform buffers to prevent memory leaks
		m_TransformBuffers.clear();
		m_PacketToBufferIndex.clear();

		m_IsSorted = false;
		m_IsBatched = false;
	}

	void CommandBucket::Reset(CommandAllocator& allocator)
	{
		OLO_PROFILE_FUNCTION();

		Clear();

		// Clear transform buffers
		m_TransformBuffers.clear();
		m_PacketToBufferIndex.clear();

		// Reset the allocator to free memory
		allocator.Reset();

		// Reset statistics
		m_Stats = Statistics();
	}
}
