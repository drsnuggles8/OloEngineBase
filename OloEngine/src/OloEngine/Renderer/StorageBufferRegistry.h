#pragma once

// Process-wide list of every live StorageBuffer, for diagnostics (issue #607:
// `olo_gpu_buffer_read`).
//
// A storage buffer has no name and no owner the engine can enumerate: each pass
// creates its own through StorageBuffer::Create and keeps it in a member. So
// "read the Forward+ light grid" had no route to the buffer at all. The registry
// is filled at the one chokepoint every buffer goes through, the factory, and
// emptied by the base destructor, so a pass gets listed without opting in.
//
// What it can answer is "which live buffers were created for binding N". It
// cannot answer "which buffer is bound at N right now": several systems create
// buffers for the same binding (every GPU particle system owns an
// SSBO_GPU_PARTICLES), and the last bind of a frame is an accident of pass
// order. Callers get every candidate and choose by id. The memory-owner scope
// open at creation is recorded too: a POOLED buffer (TransientPool) is created
// with a nominal binding it never serves, and a caller must be able to tell.
//
// Lifetime follows ShaderRegistry.h: raw pointers, removed in the destructor
// before the memory can be recycled, and handed out only through
// WeakRef::Lock(), so a lookup never resurrects a buffer that is being
// destroyed. Never destructed, for the static-teardown reason given there.
//
// Cost: one hash insert per creation and one hash erase per destruction, under
// a mutex no hot path holds for longer.

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace OloEngine
{
    class StorageBufferRegistry
    {
      public:
        struct Entry
        {
            Ref<StorageBuffer> Buffer;
            StorageBufferUsage Usage = StorageBufferUsage::DynamicDraw;
            // Creation order, 1-based and never reused: a stable way to name one
            // of several buffers that share a binding.
            u64 Id = 0;
            // The RendererMemoryOwnerScope open at creation; empty when none was.
            FString Owner;
            // Created under a Pooled scope: the binding is nominal.
            bool Pooled = false;
        };

        static StorageBufferRegistry& Get()
        {
            static StorageBufferRegistry* const s_Instance = new StorageBufferRegistry();
            return *s_Instance;
        }

        StorageBufferRegistry(const StorageBufferRegistry&) = delete;
        StorageBufferRegistry& operator=(const StorageBufferRegistry&) = delete;
        StorageBufferRegistry(StorageBufferRegistry&&) = delete;
        StorageBufferRegistry& operator=(StorageBufferRegistry&&) = delete;

        void Register(StorageBuffer* buffer, StorageBufferUsage usage, const FString& owner, bool pooled)
        {
            if (buffer == nullptr)
                return;
            const std::scoped_lock lock(m_Mutex);
            m_Entries.insert_or_assign(buffer, Meta{ usage, ++m_NextId, owner, pooled });
        }

        void Unregister(const StorageBuffer* buffer)
        {
            if (buffer == nullptr)
                return;
            const std::scoped_lock lock(m_Mutex);
            m_Entries.erase(const_cast<StorageBuffer*>(buffer));
        }

        // Every live buffer, in creation order. Defined below the relocation
        // trait for Entry, which TArray<Entry> needs.
        [[nodiscard]] TArray<Entry> Snapshot() const;

      private:
        StorageBufferRegistry() = default;

        struct Meta
        {
            StorageBufferUsage Usage = StorageBufferUsage::DynamicDraw;
            u64 Id = 0;
            FString Owner;
            bool Pooled = false;
        };

        mutable std::mutex m_Mutex;
        std::unordered_map<StorageBuffer*, Meta> m_Entries;
        u64 m_NextId = 0;
    };

    // A Ref and an FString, both trivially relocatable.
    template<>
    struct TIsTriviallyRelocatable<StorageBufferRegistry::Entry>
    {
        static constexpr bool Value =
            TIsTriviallyRelocatable<Ref<StorageBuffer>>::Value && TIsTriviallyRelocatable<FString>::Value;
    };

    inline TArray<StorageBufferRegistry::Entry> StorageBufferRegistry::Snapshot() const
    {
        TArray<Entry> live;
        {
            const std::scoped_lock lock(m_Mutex);
            live.Reserve(static_cast<i32>(m_Entries.size()));
            for (const auto& [buffer, meta] : m_Entries)
            {
                if (Ref<StorageBuffer> locked = WeakRef<StorageBuffer>(buffer).Lock())
                    live.Add(Entry{ std::move(locked), meta.Usage, meta.Id, meta.Owner, meta.Pooled });
            }
        }
        std::sort(live.GetData(), live.GetData() + live.Num(), [](const Entry& a, const Entry& b)
                  { return a.Id < b.Id; });
        return live;
    }
} // namespace OloEngine
