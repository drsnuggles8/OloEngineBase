#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandPacket.h"
#include "ThreadLocalCache.h"

namespace OloEngine
{
    // Linear bump allocator for command packet memory.
    // NOT thread-safe — each thread must use its own instance.
    // Following Molecular Matters' design: zero synchronization on the hot path.
    class CommandAllocator
    {
      public:
        static constexpr sizet DEFAULT_BLOCK_SIZE = 64 * 1024; // 64KB blocks
        // Maximum size of any command. 1024 (the PBR bump) until issue #715
        // slice 3 grew DrawTerrainPatchCommand's inlined TerrainUBO by the
        // 64-sector x 2-vec4 adaptive table (~2.3 KB total command). The
        // allocator packs commands at their EXACT size, so this cap is a
        // sanity bound, not a per-command cost. Mirrored in
        // Commands/RenderCommand.h — keep the two identical.
        static constexpr sizet MAX_COMMAND_SIZE = 4096;
        static constexpr sizet COMMAND_ALIGNMENT = 16; // Ensure commands are aligned properly

        // A packet and its payload are ONE allocation of
        // sizeof(CommandPacket) + sizeof(T), and AllocateCommandMemory rejects
        // anything above MAX_COMMAND_SIZE. So the bound on a payload is the cap
        // minus the header, not the cap itself: binding the compile-time check
        // to MAX_COMMAND_SIZE alone lets a payload pass the static_assert and
        // then be refused at runtime, which is a null packet at draw time
        // instead of a build error.
        static_assert(sizeof(CommandPacket) < MAX_COMMAND_SIZE,
                      "CommandPacket's header must be smaller than MAX_COMMAND_SIZE, or the payload bound below wraps and stops bounding anything");
        static constexpr sizet MAX_COMMAND_PAYLOAD_SIZE = MAX_COMMAND_SIZE - sizeof(CommandPacket);

        static_assert(COMMAND_ALIGNMENT <= ThreadLocalCache::MAX_ALIGNMENT,
                      "COMMAND_ALIGNMENT must be an alignment the underlying cache honours");

        template<typename T>
        static constexpr bool PayloadFitsMaxCommandSize()
        {
            return sizeof(T) <= MAX_COMMAND_PAYLOAD_SIZE;
        }

        // The payload lives at allocationBase + sizeof(CommandPacket), and the
        // base is COMMAND_ALIGNMENT-aligned. BOTH conditions are needed: the
        // header is not a multiple of COMMAND_ALIGNMENT, so an `||` here
        // accepts a 16-aligned payload sitting behind a header that only moves
        // it to an 8-aligned address. Raising alignof(T) past what holds here
        // means padding sizeof(CommandPacket) up to a multiple of it, not
        // relaxing this predicate.
        template<typename T>
        static constexpr bool PayloadPlacementIsAligned()
        {
            return alignof(T) <= COMMAND_ALIGNMENT && (sizeof(CommandPacket) % alignof(T)) == 0;
        }

        explicit CommandAllocator(sizet blockSize = DEFAULT_BLOCK_SIZE);
        ~CommandAllocator() = default;

        // Disallow copying
        CommandAllocator(const CommandAllocator&) = delete;
        CommandAllocator& operator=(const CommandAllocator&) = delete;

        // Move operations
        CommandAllocator(CommandAllocator&& other) noexcept;
        CommandAllocator& operator=(CommandAllocator&& other) noexcept;

        // Allocate memory for a command
        void* AllocateCommandMemory(sizet size);

        // Create a command packet with the given command data
        // WARNING: Uses memcpy internally - only safe for trivially copyable types.
        // For non-trivial types, use AllocatePacketWithCommand() instead.
        template<typename T>
        CommandPacket* CreateCommandPacket(const T& commandData, const PacketMetadata& metadata = {})
        {
            static_assert(PayloadFitsMaxCommandSize<T>(),
                          "Command exceeds maximum size: sizeof(CommandPacket) + sizeof(T) must fit in MAX_COMMAND_SIZE");
            static_assert(std::is_trivially_copyable_v<T>,
                          "CreateCommandPacket() uses memcpy and requires trivially copyable types. "
                          "For non-trivial types, use AllocatePacketWithCommand() instead.");
            static_assert(std::is_trivially_destructible_v<T>,
                          "CommandPacket does not call command destructors; T must be trivially destructible.");
            static_assert(PayloadPlacementIsAligned<T>(),
                          "Command payload placement is not properly aligned for T");

            // Allocate memory for the CommandPacket + command data together
            constexpr sizet packetSize = sizeof(CommandPacket);
            constexpr sizet commandSize = sizeof(T);
            void* block = AllocateCommandMemory(packetSize + commandSize);
            if (!block)
            {
                OLO_CORE_ERROR("CommandAllocator::CreateCommandPacket: allocation of {0} bytes failed", packetSize + commandSize);
                return nullptr;
            }

            // Construct a new CommandPacket in the allocated memory
            auto* packet = new (block) CommandPacket();

            // Set command data size (inline data lives right after the packet header)
            packet->SetCommandSize(commandSize);

            // Initialize the packet with the command data (copies into the allocated region)
            packet->Initialize(commandData, metadata);

            return packet;
        }

        template<typename T>
        CommandPacket* AllocatePacketWithCommand(const PacketMetadata& metadata = {})
        {
            static_assert(std::is_trivially_destructible_v<T>,
                          "AllocatePacketWithCommand requires trivially destructible types "
                          "since CommandPacket does not call command destructors.");
            // Same three rules as CreateCommandPacket, at the same boundary. A
            // rule enforced in one of two entry points is not enforced: this
            // path previously had neither the size bound nor a null check.
            static_assert(PayloadFitsMaxCommandSize<T>(),
                          "Command exceeds maximum size: sizeof(CommandPacket) + sizeof(T) must fit in MAX_COMMAND_SIZE");
            static_assert(PayloadPlacementIsAligned<T>(),
                          "Command payload placement is not properly aligned for T");

            constexpr sizet packetSize = sizeof(CommandPacket);
            constexpr sizet commandSize = sizeof(T);
            constexpr sizet totalSize = packetSize + commandSize;
            void* block = AllocateCommandMemory(totalSize);
            if (!block)
            {
                OLO_CORE_ERROR("CommandAllocator::AllocatePacketWithCommand: allocation of {0} bytes failed", totalSize);
                return nullptr;
            }
            // Placement-new the packet at the start
            auto* packet = new (block) CommandPacket();
            // Placement-new the command immediately after (in the inline data region)
            void* commandMem = static_cast<u8*>(block) + packetSize;
            new (commandMem) T();

            packet->SetCommandSize(commandSize);
            packet->SetMetadata(metadata);
            return packet;
        }

        // Reset the allocator - doesn't free memory, just resets offsets
        void Reset();

        // Statistics
        sizet GetTotalAllocated() const;
        sizet GetAllocationCount() const
        {
            return m_AllocationCount;
        }

      private:
        ThreadLocalCache m_Cache;    // Owned linear allocator — no map, no mutex
        sizet m_AllocationCount = 0; // Plain counter — single-thread access only
    };
} // namespace OloEngine
