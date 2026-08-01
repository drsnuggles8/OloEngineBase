#include <atomic>
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include "OloEngine/Renderer/RHI/RHIResources.h"

namespace OloEngine::RHI
{
    auto ToString(ResourceKind kind) -> std::string_view
    {
        switch (kind)
        {
            case ResourceKind::Unknown:
                return "Unknown";
            case ResourceKind::Texture:
                return "Texture";
            case ResourceKind::Framebuffer:
                return "Framebuffer";
            case ResourceKind::Buffer:
                return "Buffer";
            case ResourceKind::VertexArray:
                return "VertexArray";
            case ResourceKind::ShaderProgram:
                return "ShaderProgram";
            case ResourceKind::Query:
                return "Query";
        }

        return "Unknown";
    }

    auto ResourceRegistry::Get() -> ResourceRegistry&
    {
        // Deliberately leaked. A Ref<Texture> can be released during static
        // destruction (file-scope asset caches in a couple of tests do exactly
        // that), and unregistering into a destroyed registry would be a
        // use-after-free of the very thing meant to make lifetimes checkable.
        static ResourceRegistry* s_Instance = new ResourceRegistry();
        return *s_Instance;
    }

    auto ResourceRegistry::SlotAt(u32 index) const -> Slot*
    {
        const u32 chunkIndex = index / kChunkSize;
        if (chunkIndex >= kMaxChunks)
            return nullptr;

        Slot* chunk = m_Chunks[chunkIndex].load(std::memory_order_acquire);
        if (chunk == nullptr)
            return nullptr;

        return chunk + (index % kChunkSize);
    }

    auto ResourceRegistry::EnsureChunk(u32 chunkIndex) -> Slot*
    {
        Slot* chunk = m_Chunks[chunkIndex].load(std::memory_order_relaxed);
        if (chunk != nullptr)
            return chunk;

        chunk = new Slot[kChunkSize];
        // Release-store pairs with the acquire-load in SlotAt(): a reader that
        // sees the pointer also sees the zero-initialised generations.
        m_Chunks[chunkIndex].store(chunk, std::memory_order_release);
        return chunk;
    }

    auto ResourceRegistry::Register(ResourceKind kind, u64 nativeHandle, Backend owner) -> ResourceHandle
    {
        const std::lock_guard lock(m_WriteMutex);

        u32 index = 0;
        if (!m_FreeList.empty())
        {
            index = m_FreeList.back();
            m_FreeList.pop_back();
        }
        else
        {
            index = m_SlotCount.load(std::memory_order_relaxed);
            if (index >= kMaxSlots)
            {
                OLO_CORE_ERROR("RHI::ResourceRegistry exhausted ({} slots). Returning a null handle; "
                               "the caller's resource will be unreachable.",
                               kMaxSlots);
                return {};
            }

            if (EnsureChunk(index / kChunkSize) == nullptr)
            {
                OLO_CORE_ERROR("RHI::ResourceRegistry failed to allocate a slot chunk.");
                return {};
            }

            // Publish the slot only after its chunk exists, so a concurrent
            // reader never indexes past a null chunk pointer.
            m_SlotCount.store(index + 1u, std::memory_order_release);
        }

        Slot* slot = SlotAt(index);
        OLO_CORE_ASSERT(slot, "RHI::ResourceRegistry slot must exist after allocation");

        // Any change invalidates every outstanding handle for this slot; +1 is
        // simply the cheapest change. Generation 0 is reserved for "never handed
        // out" (Handle::IsValid() rejects it), so a wrap skips it. Wrapping
        // needs 2^32 register/unregister cycles on ONE index and is recorded for
        // completeness, not because it is reachable.
        u32 generation = slot->Generation.load(std::memory_order_relaxed) + 1u;
        if (generation == 0u)
            generation = 1u;

        slot->Native.store(nativeHandle, std::memory_order_relaxed);
        slot->Kind.store(static_cast<u8>(kind), std::memory_order_relaxed);
        slot->Owner.store(static_cast<u8>(owner), std::memory_order_relaxed);
        slot->Generation.store(generation, std::memory_order_release);

        ++m_TotalRegistered;

        return ResourceHandle{ index, generation };
    }

    void ResourceRegistry::Unregister(ResourceHandle handle)
    {
        if (!handle.IsValid())
            return;

        const std::lock_guard lock(m_WriteMutex);

        Slot* slot = SlotAt(handle.Index);
        if ((slot == nullptr) || handle.Index >= m_SlotCount.load(std::memory_order_relaxed))
        {
            ++m_StaleUnregisters;
            return;
        }

        if (slot->Generation.load(std::memory_order_relaxed) != handle.Generation)
        {
            // Double-unregister, or a handle that outlived a Clear(). Counted
            // and ignored — pushing the index again would put one slot on the
            // freelist twice and hand two live resources the same index.
            ++m_StaleUnregisters;
            return;
        }

        u32 generation = handle.Generation + 1u;
        if (generation == 0u)
            generation = 1u;

        slot->Native.store(0u, std::memory_order_relaxed);
        slot->Kind.store(static_cast<u8>(ResourceKind::Unknown), std::memory_order_relaxed);
        slot->Owner.store(static_cast<u8>(Backend::None), std::memory_order_relaxed);
        slot->Generation.store(generation, std::memory_order_release);

        m_FreeList.push_back(handle.Index);
    }

    void ResourceRegistry::UpdateNative(ResourceHandle handle, u64 nativeHandle)
    {
        if (!handle.IsValid())
            return;

        const std::lock_guard lock(m_WriteMutex);

        Slot* slot = SlotAt(handle.Index);
        if ((slot == nullptr) || handle.Index >= m_SlotCount.load(std::memory_order_relaxed) ||
            slot->Generation.load(std::memory_order_relaxed) != handle.Generation)
        {
            ++m_StaleUnregisters;
            return;
        }

        // The generation deliberately does NOT move: this is the same logical
        // resource with new storage behind it, so every outstanding handle must
        // keep resolving — now to the new name.
        slot->Native.store(nativeHandle, std::memory_order_release);
    }

    auto ResourceRegistry::ResolveTaggedForBackend(ResourceHandle handle) const -> NativeHandle
    {
        if (!handle.IsValid())
            return {};

        if (handle.Index >= m_SlotCount.load(std::memory_order_acquire))
        {
            m_StaleRejections.fetch_add(1u, std::memory_order_relaxed);
            return {};
        }

        Slot* slot = SlotAt(handle.Index);
        if (slot == nullptr)
        {
            m_StaleRejections.fetch_add(1u, std::memory_order_relaxed);
            return {};
        }

        // Read the generation on both sides of the payload. A writer changes the
        // generation last on Register and first on Unregister, so a mismatch
        // between the two loads means the slot changed hands underneath us and
        // the payload we read may be torn between tenants.
        const u32 before = slot->Generation.load(std::memory_order_acquire);
        const u64 native = slot->Native.load(std::memory_order_relaxed);
        const auto owner = static_cast<Backend>(slot->Owner.load(std::memory_order_relaxed));
        // The acquire on `before` stops later reads floating ABOVE it, but an
        // acquire load does not stop earlier reads from sinking BELOW it — so
        // without this fence the payload reads may be reordered past the second
        // generation load and the tearing check above would validate a
        // generation pair while returning a payload read outside it. The fence
        // orders the preceding loads against everything after it, which is the
        // half the two acquire loads cannot express on their own.
        std::atomic_thread_fence(std::memory_order_acquire);
        const u32 after = slot->Generation.load(std::memory_order_acquire);

        if (before != handle.Generation || after != before)
        {
            m_StaleRejections.fetch_add(1u, std::memory_order_relaxed);
            return {};
        }

        return NativeHandle{ native, owner };
    }

    auto ResourceRegistry::ResolveNativeForBackend(ResourceHandle handle) const -> u64
    {
        return ResolveTaggedForBackend(handle).Value;
    }

    auto ResourceRegistry::IsLive(ResourceHandle handle) const -> bool
    {
        if (!handle.IsValid() || handle.Index >= m_SlotCount.load(std::memory_order_acquire))
            return false;

        const Slot* slot = SlotAt(handle.Index);
        return (slot != nullptr) && slot->Generation.load(std::memory_order_acquire) == handle.Generation;
    }

    auto ResourceRegistry::KindOf(ResourceHandle handle) const -> ResourceKind
    {
        if (!handle.IsValid() || handle.Index >= m_SlotCount.load(std::memory_order_acquire))
            return ResourceKind::Unknown;

        const Slot* slot = SlotAt(handle.Index);
        if (slot == nullptr)
            return ResourceKind::Unknown;

        // Same seqlock discipline as ResolveTaggedForBackend. The previous form
        // was IsLive() followed by a relaxed read, which is a TOCTOU: the slot
        // can be Unregistered and Registered to a new tenant between the
        // liveness check and the load, and the caller would then be told the
        // NEW occupant's kind for their own stale handle. Reading the
        // generation on both sides makes that report Unknown instead.
        const u32 before = slot->Generation.load(std::memory_order_acquire);
        const auto kind = static_cast<ResourceKind>(slot->Kind.load(std::memory_order_relaxed));
        std::atomic_thread_fence(std::memory_order_acquire);
        const u32 after = slot->Generation.load(std::memory_order_acquire);

        if (before != handle.Generation || after != before)
            return ResourceKind::Unknown;

        return kind;
    }

    auto ResourceRegistry::GetStats() const -> Stats
    {
        const std::lock_guard lock(m_WriteMutex);

        Stats stats;
        stats.SlotCount = m_SlotCount.load(std::memory_order_relaxed);
        stats.FreeCount = static_cast<u32>(m_FreeList.size());
        stats.LiveCount = stats.SlotCount - stats.FreeCount;
        stats.TotalRegistered = m_TotalRegistered;
        stats.StaleRejections = m_StaleRejections.load(std::memory_order_relaxed);
        stats.StaleUnregisters = m_StaleUnregisters;
        return stats;
    }

    void ResourceRegistry::ResetCounters()
    {
        const std::lock_guard lock(m_WriteMutex);
        m_StaleRejections.store(0u, std::memory_order_relaxed);
        m_StaleUnregisters = 0u;
    }

    void ResourceRegistry::Clear()
    {
        const std::lock_guard lock(m_WriteMutex);

        const u32 slotCount = m_SlotCount.load(std::memory_order_relaxed);
        m_FreeList.clear();
        m_FreeList.reserve(slotCount);

        for (u32 index = 0u; index < slotCount; ++index)
        {
            Slot* slot = SlotAt(index);
            if (slot == nullptr)
                continue;

            // Advance even already-free slots: a handle retired before the reset
            // must not become resolvable again just because the slot was reused
            // by the next device with the same generation.
            u32 generation = slot->Generation.load(std::memory_order_relaxed) + 1u;
            if (generation == 0u)
                generation = 1u;

            slot->Native.store(0u, std::memory_order_relaxed);
            slot->Kind.store(static_cast<u8>(ResourceKind::Unknown), std::memory_order_relaxed);
            slot->Owner.store(static_cast<u8>(Backend::None), std::memory_order_relaxed);
            slot->Generation.store(generation, std::memory_order_release);

            m_FreeList.push_back(index);
        }
    }

    // -------------------------------------------------------------------------
    // The debug escape hatch declared in RHIResources.h. Defined here because
    // the registry is the only thing that can answer it — Phase 1 declared it
    // and left it undefined, which is why nothing could call it.
    // -------------------------------------------------------------------------
    auto GetNativeHandleForDebug(ResourceHandle handle) -> NativeHandle
    {
        return ResourceRegistry::Get().ResolveTaggedForBackend(handle);
    }
} // namespace OloEngine::RHI
