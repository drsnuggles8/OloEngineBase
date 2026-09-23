#pragma once

#include "RenderCommand.h"
#include "DrawKey.h"
#include "CommandLifecycle.h"
#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <atomic>

/*
 * CommandPacket — a command plus its metadata, prepared and then frozen.
 *
 * A packet is written while it is being PREPARED: filled in by its producer
 * (Renderer3D::DrawMesh and friends), bone-remapped and batched by its
 * bucket. Its first replay FREEZES it, and from then on the bytes are fixed:
 * parallel replay reads them from several workers with no lock. The mutating
 * accessors below report a CommandLifecycle violation on a frozen packet; a
 * pass that needs a different version of one clones it and edits the clone.
 * See CommandLifecycle.h for the whole contract.
 */

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    class CommandAllocator;
    class RenderContext;

    // Command packet metadata
    struct PacketMetadata
    {
        // Primary sorting key - packed bitfield for maximum performance
        DrawKey m_SortKey;

        // Execution properties
        bool m_DependsOnPrevious = false; // This command must execute after the previous one
        u32 m_GroupID = 0;                // Commands with the same groupId should be kept together
        u32 m_ExecutionOrder = 0;         // Sequence number for preserving order when needed

        // Statistics/debugging
        bool m_IsStatic = false;           // Command doesn't change between frames
        const char* m_DebugName = nullptr; // Optional name for debugging
    };

    // Command packet that wraps a command with metadata and links to other packets
    class CommandPacket
    {
      public:
        CommandPacket() = default;
        ~CommandPacket();

        // Non-copyable, non-movable — packets live in bump-allocated memory
        // with inline command data appended right after sizeof(CommandPacket).
        CommandPacket(const CommandPacket&) = delete;
        CommandPacket& operator=(const CommandPacket&) = delete;
        CommandPacket(CommandPacket&&) = delete;
        CommandPacket& operator=(CommandPacket&&) = delete;

        // Initialize a command packet with a specific command
        // WARNING: This uses memcpy which is only safe for trivially copyable types.
        // For non-trivial types (containing std::vector, smart pointers, etc.),
        // use CommandAllocator::AllocatePacketWithCommand() instead.
        template<typename T>
        void Initialize(const T& commandData, const PacketMetadata& metadata = {})
        {
            static_assert(sizeof(T) <= MAX_COMMAND_SIZE, "Command exceeds maximum size");
            static_assert(std::is_trivially_copyable_v<T>,
                          "Initialize() uses memcpy and requires trivially copyable types. "
                          "For non-trivial types like DrawMeshInstancedCommand, use "
                          "CommandAllocator::AllocatePacketWithCommand() instead.");
            NotePreparationWrite("CommandPacket::Initialize");

            // Copy the command data into inline storage right after the packet header
            std::memcpy(GetInlineData(), &commandData, sizeof(T));
            m_CommandSize = sizeof(T);
            m_CommandType = commandData.header.type;

            // NOTE: Dispatch function is resolved lazily in Execute() rather than
            // here, to avoid a compile-time dependency on CommandDispatch.cpp
            // (which transitively includes Application, Renderer3D, AssetManager,
            //  glad, etc. — heavy statics that crash test executables).

            // Metadata (including sort key) is caller-provided. Callers
            // (Renderer3D::DrawMesh, etc.) are responsible for computing
            // ShaderID, MaterialID and Depth in the sort key before submission.
            m_Metadata = metadata;
        }

        // ——— Lifecycle (issue #1335) ———

        [[nodiscard]] bool IsFrozen() const
        {
            return m_Lifecycle.load(std::memory_order_acquire) != Lifecycle::Preparing;
        }

        // Freeze the packet for replay. Idempotent. Const because a replay
        // view is const and freezing is the one transition it makes: the
        // packet's content is not changed, only declared final. With
        // `recordDigest` (CommandLifecycle::IsValidationEnabled(), read once
        // by the caller for the whole span) also records the digest
        // MatchesFrozenContent checks.
        void Freeze(bool recordDigest) const;

        // False only when validation recorded a digest at freeze time and
        // the packet's bytes no longer match it. Always true for a packet
        // that is not frozen, or was frozen with validation off.
        [[nodiscard]] bool MatchesFrozenContent() const;

        // Digest of everything replay reads: type, size, dispatch function,
        // metadata and the inline command bytes.
        [[nodiscard]] u64 ComputeContentDigest() const;

        // Execute the command with a RendererAPI
        void Execute(RendererAPI& rendererAPI) const;

        // Packet comparison for sorting
        bool operator<(const CommandPacket& other) const;

        // Getters for command properties
        CommandType GetCommandType() const
        {
            return m_CommandType;
        }
        const PacketMetadata& GetMetadata() const
        {
            return m_Metadata;
        }

        // Returns true if this packet can be batched with another packet
        bool CanBatchWith(const CommandPacket& other) const;

        // Set command data size (inline data lives at this + sizeof(CommandPacket))
        void SetCommandSize(sizet size)
        {
            NotePreparationWrite("CommandPacket::SetCommandSize");
            m_CommandSize = size;
        }

        // The mutable accessor is for PREPARATION. Reading a frozen packet
        // goes through the const overload (a bucket's GetPackets() is a const
        // view for exactly this reason); calling this one on a frozen packet
        // is reported, because the pointer it returns is how a write would
        // reach bytes another thread is replaying.
        template<typename T>
        T* GetCommandData()
        {
            NotePreparationWrite("CommandPacket::GetCommandData");
            return reinterpret_cast<T*>(GetInlineData());
        }

        template<typename T>
        const T* GetCommandData() const
        {
            return reinterpret_cast<const T*>(GetInlineData());
        }

        // Get raw command data as void pointer (for operations that don't need to know the type)
        const void* GetRawCommandData() const
        {
            return GetInlineData();
        }
        void* GetRawCommandData()
        {
            NotePreparationWrite("CommandPacket::GetRawCommandData");
            return GetInlineData();
        }

        // Return command size for memory management
        sizet GetCommandSize() const
        {
            return m_CommandSize;
        }

        // Clone this packet (for cases where we need to duplicate commands)
        CommandPacket* Clone(class CommandAllocator& allocator) const;

        // For debugging - get the command type as a string
        const char* GetCommandTypeString() const
        {
            return CommandTypeToString(m_CommandType);
        }

        // Setters for command properties when working with raw data
        void SetCommandType(CommandType type)
        {
            NotePreparationWrite("CommandPacket::SetCommandType");
            m_CommandType = type;
        }
        void SetDispatchFunction(CommandDispatchFn fn)
        {
            NotePreparationWrite("CommandPacket::SetDispatchFunction");
            m_DispatchFn = fn;
        }
        void SetMetadata(const PacketMetadata& metadata)
        {
            NotePreparationWrite("CommandPacket::SetMetadata");
            m_Metadata = metadata;
        }

        // Runtime dispatch resolver — set once during engine init
        // (e.g. CommandDispatch::Initialize sets this to CommandDispatch::GetDispatchFunction).
        // Keeps CommandPacket.obj free of any link-time dependency on CommandDispatch.obj.
        using DispatchResolverFn = CommandDispatchFn (*)(CommandType);
        static void SetDispatchResolver(DispatchResolverFn resolver);
        [[nodiscard]] static DispatchResolverFn GetDispatchResolver()
        {
            return s_DispatchResolver;
        }

      private:
        // Freezing is the transient state while the one thread that won the
        // freeze records the digest. Two buckets can share a packet and be
        // replayed from concurrent recording items, so the first freeze is a
        // compare-exchange, not a store: exactly one thread writes the digest
        // and every other thread reads it only after the release of Frozen.
        enum class Lifecycle : u8
        {
            Preparing,
            Freezing,
            Frozen
        };

        // One predictable branch on the preparation path; the report itself
        // is out of line so the accessors stay small enough to inline.
        void NotePreparationWrite(const char* where) const
        {
            if (m_Lifecycle.load(std::memory_order_relaxed) != Lifecycle::Preparing) [[unlikely]]
                CommandLifecycle::ReportViolation(CommandLifecycle::Violation::PacketMutatedAfterFreeze, where);
        }

        // Inline command data lives in the allocation immediately after the packet header.
        // Eliminates pointer indirection — data is always at (u8*)this + sizeof(CommandPacket).
        void* GetInlineData()
        {
            return reinterpret_cast<u8*>(this) + sizeof(CommandPacket);
        }
        const void* GetInlineData() const
        {
            return reinterpret_cast<const u8*>(this) + sizeof(CommandPacket);
        }

        static DispatchResolverFn s_DispatchResolver;
        sizet m_CommandSize = 0;
        CommandType m_CommandType = CommandType::Invalid;
        // Sits in the padding after the u8 CommandType, so it costs no size.
        // Mutable: see Freeze().
        mutable std::atomic<Lifecycle> m_Lifecycle{ Lifecycle::Preparing };
        static_assert(sizeof(std::atomic<Lifecycle>) == 1 && std::atomic<Lifecycle>::is_always_lock_free,
                      "the packet state must stay a lock-free byte in the header's padding");
        CommandDispatchFn m_DispatchFn = nullptr;
        PacketMetadata m_Metadata;
        // Recorded by Freeze() when validation is on; 0 otherwise. Also keeps
        // the header a multiple of COMMAND_ALIGNMENT (64 bytes, was 56).
        mutable u64 m_FrozenDigest = 0;
    };

    static_assert(sizeof(CommandPacket) % 16 == 0,
                  "CommandPacket's header stays a multiple of CommandAllocator::COMMAND_ALIGNMENT, so every payload placed after it is 16-aligned");
} // namespace OloEngine
