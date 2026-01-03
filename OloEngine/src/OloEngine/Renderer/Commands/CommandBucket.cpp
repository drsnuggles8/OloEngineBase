#include "OloEnginePCH.h"
#include "CommandBucket.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include <algorithm>
#include <array>

namespace OloEngine
{
    // LSB Radix Sort for 64-bit keys using 8-bit digits (8 passes)
    // This is a stable sort, preserving relative order of equal keys.
    // Input: array of command packets and their sort keys
    // Uses counting sort for each digit pass.
    namespace
    {
        constexpr sizet RADIX_BITS = 8;
        constexpr sizet RADIX_SIZE = 1 << RADIX_BITS; // 256 buckets
        constexpr sizet NUM_PASSES = sizeof(u64) / sizeof(u8); // 8 passes for 64-bit

        void RadixSort64(std::vector<CommandPacket*>& packets)
        {
            OLO_PROFILE_FUNCTION();

            if (packets.size() <= 1)
                return;

            sizet count = packets.size();

            // Temporary buffer for swapping
            std::vector<CommandPacket*> temp(count);

            // Pre-extract keys for cache efficiency
            std::vector<u64> keys(count);
            for (sizet i = 0; i < count; ++i)
            {
                keys[i] = packets[i]->GetMetadata().m_SortKey.GetKey();
            }
            std::vector<u64> tempKeys(count);

            // Process each byte from LSB to MSB
            for (sizet pass = 0; pass < NUM_PASSES; ++pass)
            {
                sizet shift = pass * RADIX_BITS;

                // Count occurrences of each digit
                std::array<sizet, RADIX_SIZE> histogram{};
                for (sizet i = 0; i < count; ++i)
                {
                    u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                    histogram[digit]++;
                }

                // Convert counts to starting positions (prefix sum)
                std::array<sizet, RADIX_SIZE> offsets{};
                sizet total = 0;
                for (sizet i = 0; i < RADIX_SIZE; ++i)
                {
                    offsets[i] = total;
                    total += histogram[i];
                }

                // Place elements in sorted order
                for (sizet i = 0; i < count; ++i)
                {
                    u8 digit = static_cast<u8>((keys[i] >> shift) & 0xFF);
                    sizet destIdx = offsets[digit]++;
                    temp[destIdx] = packets[i];
                    tempKeys[destIdx] = keys[i];
                }

                // Swap buffers
                std::swap(packets, temp);
                std::swap(keys, tempKeys);
            }
        }
    } // anonymous namespace
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

        // Sort each group internally using RadixSort64 for optimal performance
        for (auto& group : dependencyGroups)
        {
            // Only sort if there's more than one command in a group
            if (group.size() > 1)
            {
                // Use RadixSort64 for O(n) sorting on 64-bit keys
                RadixSort64(group);
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

        // Create a new packet for the instanced command
        // Use AllocatePacketWithCommand because DrawMeshInstancedCommand is not trivially copyable
        // (it contains std::vector<glm::mat4>)
        PacketMetadata metadata = meshPacket->GetMetadata();
        CommandPacket* instancedPacket = allocator.AllocatePacketWithCommand<DrawMeshInstancedCommand>(metadata);
        if (!instancedPacket)
            return nullptr;

        auto* instancedCmd = instancedPacket->GetCommandData<DrawMeshInstancedCommand>();

        // Copy header
        instancedCmd->header.type = CommandType::DrawMeshInstanced;
        instancedCmd->header.dispatchFn = nullptr; // Will be set during initialization

        // Copy mesh data using POD identifiers
        instancedCmd->meshHandle = meshCmd->meshHandle;
        instancedCmd->vertexArrayID = meshCmd->vertexArrayID;
        instancedCmd->indexCount = meshCmd->indexCount;

        // Initial instance count is 1
        instancedCmd->instanceCount = 1;

        // For POD commands, allocate transforms in FrameDataBuffer
        // For now, we'll just set the offset and count; the actual data will be managed by the caller
        // Note: This batching logic may need to be updated for FrameDataBuffer usage
        instancedCmd->transformBufferOffset = 0;
        instancedCmd->transformCount = 1;

        // Copy material properties
        instancedCmd->ambient = meshCmd->ambient;
        instancedCmd->diffuse = meshCmd->diffuse;
        instancedCmd->specular = meshCmd->specular;
        instancedCmd->shininess = meshCmd->shininess;
        instancedCmd->useTextureMaps = meshCmd->useTextureMaps;

        // Copy texture renderer IDs (POD)
        instancedCmd->diffuseMapID = meshCmd->diffuseMapID;
        instancedCmd->specularMapID = meshCmd->specularMapID;

        // Copy shader renderer ID (POD)
        instancedCmd->shaderHandle = meshCmd->shaderHandle;
        instancedCmd->shaderRendererID = meshCmd->shaderRendererID;

        // Copy POD render state
        instancedCmd->renderState = meshCmd->renderState;

        // Set command type and dispatch function
        instancedPacket->SetCommandType(instancedCmd->header.type);
        instancedPacket->SetDispatchFunction(CommandDispatch::GetDispatchFunction(instancedCmd->header.type));

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

            // Note: With POD commands, transforms are stored in FrameDataBuffer
            // Dynamic instance batching would require allocating new FrameDataBuffer space
            // For now, return false to skip dynamic batching (it's handled at command creation time)
            // TODO: Implement proper FrameDataBuffer-based instance batching
            OLO_CORE_WARN("CommandBucket: Dynamic instancing with POD commands not yet fully implemented");
            return false;
        }
        else if (targetType == CommandType::DrawMeshInstanced && sourceType == CommandType::DrawMesh)
        {
            // Note: With POD commands, adding instances requires updating FrameDataBuffer
            // This is not straightforward at batch time since allocations happen during command creation
            // TODO: Implement proper FrameDataBuffer-based instance batching
            return false;
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
} // namespace OloEngine
