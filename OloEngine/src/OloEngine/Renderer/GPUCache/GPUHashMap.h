#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/GPUCache/GPUCacheStorage.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <limits>
#include <utility>
#include <vector>

namespace OloEngine
{
    template<typename K>
    concept GPUHashMapKey = std::integral<K> && GPUSafeStruct<K>;

    template<typename V>
    concept GPUHashMapValue = GPUSafeStruct<V>;

    // @brief Open-addressed hash map with a GPU-readable layout (issue #704,
    // ported from the VoxelEngine reference's gpu_hashmap_allocator.h).
    //
    // The table is one flat array of { key, value } entries, linear-probed,
    // power-of-two capacity, with two reserved key sentinels — which is exactly
    // the layout a compute shader can probe in ~25 lines of GLSL (see
    // assets/shaders/include/GPUCacheResolve.glsl).
    //
    // THREADING CONTRACT. Keys are claimed with a CAS through std::atomic_ref,
    // so concurrent Insert / Erase / Find of **distinct** keys is safe and
    // lock-free — including keys that collide on the same slot, which is the
    // interesting case and the one the tests hammer.
    //
    // Concurrent mutation of the **same** key from multiple threads is NOT
    // supported: the value store is a plain write (V is caller-supplied and can
    // be wider than any lock-free atomic), so two threads writing one key is a
    // data race — undefined behaviour, and TSan reports it as one. The
    // reference's "last-writer-wins" wording claimed otherwise; it was wrong,
    // and it cost a red TSan job to find out. Serialize externally if you need
    // it. Every engine consumer is single-writer, so nothing pays for this.
    //
    // Mutate, CompactTombstones and the HostMirrored device mirror are all
    // single-writer-only for the same reason.
    //
    // Divergence from the reference: Insert is tombstone-aware. The reference
    // claimed the first empty-or-tombstone slot it probed, so updating a key
    // whose probe chain crossed a later-erased slot created a DUPLICATE entry
    // — Erase then removed only the first copy and lookups resurrected the
    // stale second one. Insert here probes the full chain for the key before
    // claiming the earliest tombstone.
    //
    // Backing (GPUCacheBacking):
    //  - HostOnly:     heap table, no device. Headless tests, CPU-only callers.
    //  - HostMirrored: heap table is the authority; every committed mutation is
    //    mirrored into a persistent-mapped device buffer for shader-side
    //    lookup. GPU reads must be frame-synchronized with CPU mutation (mutate,
    //    then dispatch) — the mirror memcpy is not atomic against an in-flight
    //    shader.
    //  - DeviceMapped is NOT supported: probing reads the table, and the
    //    persistent mapping is write-only.
    template<typename K, typename V>
        requires GPUHashMapKey<K> && GPUHashMapValue<V>
    class GPUHashMap
    {
      public:
        static constexpr K kEmptyKey = std::numeric_limits<K>::max();
        static constexpr K kTombstoneKey = std::numeric_limits<K>::max() - 1;

        struct Entry
        {
            K m_Key;
            V m_Value;
        };

        GPUHashMap() = default;

        // `capacity` is rounded up to the next power of two. Load factor is the
        // caller's business (GPUPagedCache creates it at 2x its page count).
        [[nodiscard]] bool Create(u32 capacity, GPUCacheBacking backing = GPUCacheBacking::HostOnly)
        {
            OLO_CORE_ASSERT(backing != GPUCacheBacking::DeviceMapped,
                            "GPUHashMap probing reads the table; the write-only device mapping cannot back it");
            if (capacity == 0)
            {
                return false;
            }
            const u32 actualCapacity = std::bit_ceil(capacity);
            if (!m_Table.Create(actualCapacity, backing))
            {
                return false;
            }
            // The WHOLE rounded-up table must be initialised, not just the first
            // `capacity` entries — the reference initialised [0, capacity) and
            // left the rounded tail as uninitialised memory a probe could read.
            Entry* table = m_Table.Data();
            for (u32 i = 0; i < actualCapacity; ++i)
            {
                table[i].m_Key = kEmptyKey;
            }
            m_Table.MirrorWrite(0, actualCapacity);
            m_TombstoneCount.store(0, std::memory_order_relaxed);
            return true;
        }

        void Destroy()
        {
            m_Table.Destroy();
            m_TombstoneCount.store(0, std::memory_order_relaxed);
        }

        // Insert-or-update. Returns false only when the table has no claimable
        // slot for this key (full of other live keys, or pathological CAS
        // contention). See the class comment for the tombstone-aware contract.
        [[nodiscard]] bool Insert(const K& key, const V& value)
        {
            Entry* table = m_Table.Data();
            OLO_CORE_ASSERT(table != nullptr, "GPUHashMap used before Create / after Destroy");
            OLO_CORE_ASSERT(key != kEmptyKey && key != kTombstoneKey, "GPUHashMap key collides with a sentinel");

            const sizet capacity = m_Table.GetCount();
            const sizet start = HashIndex(key);

            // A lost claim-CAS means another thread just rewrote the slot we
            // decided on; the probe state is stale, so restart the whole probe.
            // Bounded: contention on the same few slots resolves in a handful
            // of rounds, and the single-writer consumers never restart at all.
            constexpr int kMaxRestarts = 16;
            for (int restart = 0; restart < kMaxRestarts; ++restart)
            {
                sizet firstTombstone = capacity; // "none seen"
                sizet claimIndex = capacity;
                K claimExpected = kEmptyKey;

                for (sizet probe = 0; probe < capacity; ++probe)
                {
                    const auto index = static_cast<sizet>((start + probe) & (capacity - 1));
                    Entry& entry = table[index];
                    const std::atomic_ref<K> atomicKey(entry.m_Key);
                    const K current = atomicKey.load(std::memory_order_acquire);

                    if (current == key)
                    {
                        entry.m_Value = value;
                        m_Table.MirrorWrite(static_cast<u32>(index), 1);
                        return true;
                    }
                    if (current == kTombstoneKey)
                    {
                        if (firstTombstone == capacity)
                        {
                            firstTombstone = index;
                        }
                        continue;
                    }
                    if (current == kEmptyKey)
                    {
                        // The key is nowhere in its probe chain (an empty slot
                        // ends every lookup) — claim the earliest reusable slot.
                        claimIndex = (firstTombstone != capacity) ? firstTombstone : index;
                        claimExpected = (firstTombstone != capacity) ? kTombstoneKey : kEmptyKey;
                        break;
                    }
                    // A different live key: probe on.
                }

                if (claimIndex == capacity)
                {
                    if (firstTombstone == capacity)
                    {
                        return false; // full of live keys
                    }
                    claimIndex = firstTombstone;
                    claimExpected = kTombstoneKey;
                }

                Entry& claimed = table[claimIndex];
                const std::atomic_ref<K> atomicKey(claimed.m_Key);
                K expected = claimExpected;
                if (atomicKey.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
                {
                    if (claimExpected == kTombstoneKey)
                    {
                        m_TombstoneCount.fetch_sub(1, std::memory_order_relaxed);
                    }
                    claimed.m_Value = value;
                    m_Table.MirrorWrite(static_cast<u32>(claimIndex), 1);
                    return true;
                }
                if (expected == key)
                {
                    // Another thread claimed the slot with OUR key first. Only
                    // reachable when two threads insert the SAME key, which the
                    // threading contract above excludes — so this write is not
                    // a supported concurrent path, it just keeps the
                    // single-threaded semantics (insert-or-update) intact.
                    claimed.m_Value = value;
                    m_Table.MirrorWrite(static_cast<u32>(claimIndex), 1);
                    return true;
                }
            }
            OLO_CORE_ERROR("GPUHashMap: Insert gave up after {} contended restarts", 16);
            return false;
        }

        // Single-probe in-place update of a PRESENT key: `mutate` receives the
        // stored value by reference (policy-handle mutations included), and the
        // entry is re-mirrored afterwards. Returns false when the key is absent
        // — nothing is called then.
        template<typename F>
        [[nodiscard]] bool Mutate(const K& key, F&& mutate)
        {
            Entry* table = m_Table.Data();
            OLO_CORE_ASSERT(table != nullptr, "GPUHashMap used before Create / after Destroy");
            OLO_CORE_ASSERT(key != kEmptyKey && key != kTombstoneKey, "GPUHashMap key collides with a sentinel");

            const sizet capacity = m_Table.GetCount();
            const sizet start = HashIndex(key);
            for (sizet probe = 0; probe < capacity; ++probe)
            {
                const auto index = static_cast<sizet>((start + probe) & (capacity - 1));
                Entry& entry = table[index];
                const std::atomic_ref<K> atomicKey(entry.m_Key);
                const K current = atomicKey.load(std::memory_order_acquire);
                if (current == kEmptyKey)
                {
                    return false;
                }
                if (current == key)
                {
                    std::forward<F>(mutate)(entry.m_Value);
                    m_Table.MirrorWrite(static_cast<u32>(index), 1);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool Find(const K& key, V& out) const
        {
            const Entry* table = m_Table.Data();
            OLO_CORE_ASSERT(table != nullptr, "GPUHashMap used before Create / after Destroy");
            OLO_CORE_ASSERT(key != kEmptyKey && key != kTombstoneKey, "GPUHashMap key collides with a sentinel");

            const sizet capacity = m_Table.GetCount();
            const sizet start = HashIndex(key);
            for (sizet probe = 0; probe < capacity; ++probe)
            {
                const Entry& entry = table[(start + probe) & (capacity - 1)];
                // atomic_ref over a CV-QUALIFIED type is a later addition
                // (P3323R1) than the C++23 this repo targets, so whether
                // atomic_ref<const K> compiles depends on the standard library
                // — and this builds under both MSVC's STL and libstdc++ in CI.
                // Take the portable non-const form. The const_cast is sound:
                // only the ACCESS path is const here — the table storage itself
                // is mutable heap/mapped memory owned by GPUCacheStorage — and
                // this reference is only ever loaded from, never stored to.
                const std::atomic_ref<K> atomicKey(const_cast<K&>(entry.m_Key));
                const K current = atomicKey.load(std::memory_order_acquire);
                if (current == kEmptyKey)
                {
                    return false;
                }
                if (current == key)
                {
                    out = entry.m_Value;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool Contains(const K& key) const
        {
            V ignored;
            return Find(key, ignored);
        }

        bool Erase(const K& key)
        {
            Entry* table = m_Table.Data();
            OLO_CORE_ASSERT(table != nullptr, "GPUHashMap used before Create / after Destroy");
            OLO_CORE_ASSERT(key != kEmptyKey && key != kTombstoneKey, "GPUHashMap key collides with a sentinel");

            const sizet capacity = m_Table.GetCount();
            const sizet start = HashIndex(key);
            for (sizet probe = 0; probe < capacity; ++probe)
            {
                const auto index = static_cast<u32>((start + probe) & (capacity - 1));
                Entry& entry = table[index];
                const std::atomic_ref<K> atomicKey(entry.m_Key);
                const K current = atomicKey.load(std::memory_order_acquire);
                if (current == kEmptyKey)
                {
                    return false;
                }
                if (current == key)
                {
                    K expected = key;
                    if (atomicKey.compare_exchange_strong(expected, kTombstoneKey, std::memory_order_acq_rel))
                    {
                        m_TombstoneCount.fetch_add(1, std::memory_order_relaxed);
                        m_Table.MirrorWrite(index, 1);
                        return true;
                    }
                    return false;
                }
            }
            return false;
        }

        // Tombstones are only reclaimed when an Insert's own probe chain
        // crosses one, so erase-heavy churn monotonically consumes the empty
        // slots that terminate probes — in the limit every miss degrades to a
        // full-table scan (CPU and GPU alike). Single-writer consumers
        // (GPUPagedCache) rebuild the table in place when the count crosses a
        // quarter of the capacity.
        [[nodiscard]] bool NeedsCompaction() const
        {
            return m_TombstoneCount.load(std::memory_order_relaxed) > m_Table.GetCount() / 4;
        }

        [[nodiscard]] u32 GetTombstoneCount() const
        {
            return m_TombstoneCount.load(std::memory_order_relaxed);
        }

        // Rebuilds the table without tombstones. SINGLE-WRITER ONLY: concurrent
        // readers would observe live keys vanishing mid-rebuild, and GPU
        // consumers must not be probing the mirror while it runs (call between
        // frames, exactly where cache mutation already happens).
        void CompactTombstones()
        {
            Entry* table = m_Table.Data();
            OLO_CORE_ASSERT(table != nullptr, "GPUHashMap used before Create / after Destroy");

            const u32 capacity = m_Table.GetCount();
            std::vector<Entry> live;
            live.reserve(capacity / 2);
            for (u32 i = 0; i < capacity; ++i)
            {
                if (table[i].m_Key != kEmptyKey && table[i].m_Key != kTombstoneKey)
                {
                    live.push_back(table[i]);
                }
                table[i].m_Key = kEmptyKey;
            }
            for (const Entry& entry : live)
            {
                // No tombstones exist mid-rebuild, so plain first-empty
                // placement reproduces exactly what Insert would do.
                const sizet start = HashIndex(entry.m_Key);
                for (sizet probe = 0; probe < capacity; ++probe)
                {
                    Entry& slot = table[(start + probe) & (capacity - 1)];
                    if (slot.m_Key == kEmptyKey)
                    {
                        slot = entry;
                        break;
                    }
                }
            }
            m_TombstoneCount.store(0, std::memory_order_relaxed);
            m_Table.MirrorWrite(0, capacity);
        }

        void Bind(u32 bindingPoint) const
        {
            m_Table.Bind(bindingPoint);
        }

        [[nodiscard]] u32 GetCapacity() const
        {
            return m_Table.GetCount();
        }
        [[nodiscard]] bool IsCreated() const
        {
            return m_Table.IsCreated();
        }
        [[nodiscard]] RHI::ResourceHandle GetDeviceHandle() const
        {
            return m_Table.GetDeviceHandle();
        }

        // The start slot the GLSL side must reproduce: the engine's canonical
        // MurmurHash3 finalizer (Hash::Hash64, Core/Hash.h) masked to the
        // power-of-two capacity. GPUCacheResolve.glsl mirrors those constants;
        // GPUHashMapTest pins the two files against each other.
        [[nodiscard]] static constexpr u64 HashKey(u64 key)
        {
            return Hash::Hash64(key);
        }

      private:
        [[nodiscard]] sizet HashIndex(const K& key) const
        {
            return static_cast<sizet>(HashKey(static_cast<u64>(key)) & (m_Table.GetCount() - 1));
        }

        GPUCacheStorage<Entry> m_Table;
        std::atomic<u32> m_TombstoneCount{ 0 };
    };
} // namespace OloEngine
