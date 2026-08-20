#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/GPUCache/GPUCachePolicy.h"
#include "OloEngine/Renderer/GPUCache/GPUCacheStorage.h"
#include "OloEngine/Renderer/GPUCache/GPUHashMap.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedBuffer.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace OloEngine
{
    template<typename Cache>
    class GPUPagedCacheInspector; // test-only white-box access (tests/Rendering/GPUCacheInspector.h)

    // @brief General GPU-visible paged cache: page-chain allocation over one
    // large buffer, an open-addressed object directory a compute shader can
    // probe, and pluggable eviction (issue #704, ported from the VoxelEngine
    // reference's gpu_cache_allocator.h).
    //
    // Objects are variable-sized atom runs stored as CHAINS of fixed-size
    // pages; the page-node table (singly-linked next indices) and the
    // object->allocation hash map are both GPU-readable when created with the
    // HostMirrored backing, so a shader resolves "is object X resident, and
    // where?" inline — see assets/shaders/include/GPUCacheResolve.glsl.
    //
    // Divergences from the reference, all deliberate:
    //  - Allocation is FALLIBLE. The reference looped `while (short) evict()`
    //    with no exit, which hangs the render thread the moment the policy has
    //    no evictable victim (everything pinned/in-use). AllocatePages returns
    //    false instead and rolls the object back to its pre-call allocation
    //    (evictions already performed on its behalf stand — they cannot be
    //    undone).
    //  - Eviction notifies an optional listener with the victim id, so a
    //    consumer whose payload lives outside this cache (VirtualMeshRegistry)
    //    can reclaim its side of the state.
    //  - MoveObject re-registers the surviving object with the policy and
    //    Swap exchanges allocation contents while each object KEEPS its own
    //    policy handle — the reference leaked/aliased policy nodes in both.
    //  - No ThreadMode parameter: mutation is render-thread-only (matching
    //    every engine consumer); the underlying bitset/hash-map primitives are
    //    lock-free regardless.
    //
    // Backing:
    //  - GPUCacheBacking::HostOnly — no rendering device touched at all;
    //    headless tests and CPU-only directories.
    //  - GPUCacheBacking::HostMirrored — the directory (hash map + page nodes)
    //    is mirrored into persistent-mapped device buffers for shader lookup,
    //    and the atom store (when present) is a DeviceMapped arena.
    template<typename TObjectID, typename TAtom, template<typename> typename Policy>
        requires EvictionPolicy<Policy<TObjectID>, TObjectID> && GPUHashMapKey<TObjectID> && GPUSafeStruct<TAtom>
    class GPUPagedCache
    {
      public:
        using ObjectID = TObjectID;
        using Atom = TAtom;

        static constexpr u32 kInvalidPage = GPUPagedBuffer<TAtom>::kInvalidPage;

        // GPU-visible page-chain link. kInvalidPage terminates a chain.
        struct PageNode
        {
            u32 m_Next = kInvalidPage;
        };

        // The hash-map value: where an object's atoms live. The policy handle
        // rides along so the map is the single source of truth per object.
        // GPU-visible layout — a shader mirroring this struct must match the
        // concrete instantiation (Policy handle size included).
        struct ObjectAllocation
        {
            u32 m_TotalElementCount = 0;
            u32 m_StartPage = kInvalidPage;
            u32 m_EndPage = kInvalidPage;
            Policy<TObjectID>::Handle m_PolicyHandle;

            [[nodiscard]] bool IsEmpty() const
            {
                return m_StartPage == kInvalidPage;
            }
        };

        // Contiguous atom span inside the paged buffer (element units).
        struct BufferRange
        {
            sizet m_StartAtom = 0;
            sizet m_AtomCount = 0;

            bool operator==(const BufferRange&) const = default;
        };

        using EvictionListener = std::function<void(const ObjectID&)>;

        GPUPagedCache() = default;
        ~GPUPagedCache()
        {
            Destroy();
        }
        GPUPagedCache(const GPUPagedCache&) = delete;
        GPUPagedCache& operator=(const GPUPagedCache&) = delete;

        // pageSizeAtoms == 0 creates a DIRECTORY-ONLY cache: page chains, hash
        // map and eviction, but no atom storage — the consumer owns the payload
        // and maps page indices to its own buffers (VirtualMeshRegistry mode).
        // AllocateObject / PushBackToObject / ReadObject are unavailable then.
        [[nodiscard]] bool Create(sizet pageSizeAtoms, u32 pageCount,
                                  GPUCacheBacking backing = GPUCacheBacking::HostOnly)
        {
            OLO_CORE_ASSERT(backing != GPUCacheBacking::DeviceMapped,
                            "GPUPagedCache backing selects HostOnly or HostMirrored (the atom arena becomes "
                            "DeviceMapped internally)");
            // The upper bound keeps every derived capacity (2x map, 4x policy)
            // inside u32 — and is still absurdly beyond any real cache.
            if (IsCreated() || pageCount == 0 || pageCount >= kInvalidPage / 4)
            {
                return false;
            }

            const GPUCacheBacking atomBacking =
                (backing == GPUCacheBacking::HostOnly) ? GPUCacheBacking::HostOnly : GPUCacheBacking::DeviceMapped;
            if (!m_PagedBuffer.Create(pageSizeAtoms, pageCount, atomBacking))
            {
                return false;
            }

            // MAX_LOAD 0.5, as in the reference: the map is created at twice the
            // page count (an object occupies at least one page, so live objects
            // can never exceed half the capacity).
            if (!m_ObjectPages.Create(pageCount * 2, backing) || !m_PageNodes.Create(pageCount, backing))
            {
                Destroy();
                return false;
            }

            PageNode* nodes = m_PageNodes.Data();
            for (u32 i = 0; i < pageCount; ++i)
            {
                nodes[i].m_Next = kInvalidPage;
            }
            m_PageNodes.MirrorWrite(0, pageCount);

            // Capacity is a reservation hint only — every shipped policy grows
            // its node storage on demand, and live objects never exceed the
            // page count (each holds at least one page).
            m_Policy.emplace(pageCount);
            return true;
        }

        void Destroy()
        {
            m_ObjectPages.Destroy();
            m_PageNodes.Destroy();
            m_PagedBuffer.Destroy();
            m_Policy.reset();
        }

        // Reclaim pages under pressure through the policy until `pageCount`
        // pages back `obj` (appending to any existing chain). On success the
        // returned allocation is also committed to the directory. On failure
        // the object keeps exactly its pre-call allocation and false is
        // returned — never a hang (see class comment).
        [[nodiscard]] bool AllocatePages(const ObjectID& obj, u32 pageCount, ObjectAllocation& outAlloc)
        {
            OLO_PROFILE_FUNCTION();
            OLO_CORE_ASSERT(pageCount > 0, "GPUPagedCache::AllocatePages of zero pages");
            OLO_CORE_ASSERT(IsCreated(), "GPUPagedCache used before Create");

            ObjectAllocation alloc;
            const bool isCached = m_ObjectPages.Find(obj, alloc);
            const ObjectAllocation before = alloc;

            u32 allocated = TryReservePages(alloc, pageCount);
            // A Retry outcome consumed a policy entry the directory no longer
            // knows. The shipped list-based policies never produce one, but a
            // consumer-supplied policy could keep re-deriving the same unknown
            // victim — cap consecutive retries so that bug degrades to a loud
            // failed allocation instead of a render-thread hang (the exact
            // hang class this method's fallibility exists to eliminate).
            u32 consecutiveRetries = 0;
            const u32 maxConsecutiveRetries = m_PagedBuffer.GetPageCount() + 16;
            while (allocated < pageCount)
            {
                u32 taken = 0;
                const EvictOutcome outcome = EvictAndTakePages(obj, alloc, pageCount - allocated, taken);
                if (outcome == EvictOutcome::Exhausted ||
                    (outcome == EvictOutcome::Retry && ++consecutiveRetries > maxConsecutiveRetries))
                {
                    if (outcome == EvictOutcome::Retry)
                    {
                        OLO_CORE_ERROR("GPUPagedCache: eviction policy kept naming victims the directory "
                                       "does not hold — failing the allocation instead of spinning");
                    }
                    RollBackAppendedPages(alloc, isCached ? &before : nullptr);
                    return false;
                }
                if (outcome == EvictOutcome::Progress)
                {
                    consecutiveRetries = 0;
                }
                allocated += taken;
            }

            if (isCached)
            {
                m_Policy->OnAccess(alloc.m_PolicyHandle);
            }
            else
            {
                alloc.m_PolicyHandle = m_Policy->OnInsert(obj);
            }

            if (!m_ObjectPages.Insert(obj, alloc))
            {
                OLO_CORE_ERROR("GPUPagedCache: object directory full — rolling back allocation");
                if (!isCached)
                {
                    m_Policy->OnRemove(alloc.m_PolicyHandle);
                }
                RollBackAppendedPages(alloc, isCached ? &before : nullptr);
                return false;
            }

            outAlloc = alloc;
            return true;
        }

        // Replaces the object's contents with `count` atoms (freeing any prior
        // allocation first, as the reference does). count == 0 still allocates
        // one page so the object exists. Returns false when the pages could not
        // be reclaimed — the object is then absent from the cache.
        [[nodiscard]] bool AllocateObject(const ObjectID& obj, const Atom* data, sizet count)
        {
            OLO_CORE_ASSERT(!m_PagedBuffer.IsDirectoryOnly(),
                            "GPUPagedCache is directory-only — the consumer owns the payload");
            if (m_PagedBuffer.IsDirectoryOnly())
            {
                return false; // Release: refuse rather than write through a null atom store
            }
            DeallocateObject(obj);

            ObjectAllocation alloc;
            if (count == 0)
            {
                return AllocatePages(obj, 1, alloc);
            }

            const sizet pageSize = m_PagedBuffer.GetPageSize();
            const auto pagesNeeded = static_cast<u32>((count + pageSize - 1) / pageSize);
            if (!AllocatePages(obj, pagesNeeded, alloc))
            {
                return false;
            }

            u32 current = alloc.m_StartPage;
            sizet remaining = count;
            sizet srcIndex = 0;
            while (current != kInvalidPage && remaining > 0)
            {
                const sizet copyCount = std::min(remaining, pageSize);
                std::memcpy(m_PagedBuffer.PageData(current), data + srcIndex, copyCount * sizeof(Atom));
                srcIndex += copyCount;
                remaining -= copyCount;
                current = m_PageNodes.Data()[current].m_Next;
            }

            [[maybe_unused]] const bool updated =
                m_ObjectPages.Mutate(obj, [count](ObjectAllocation& stored)
                                     { stored.m_TotalElementCount = static_cast<u32>(count); });
            OLO_CORE_ASSERT(updated, "GPUPagedCache: updating a just-inserted key cannot fail");
            return true;
        }

        // Appends one atom, allocating a page when the current chain is full.
        // Returns false when a needed page could not be reclaimed.
        [[nodiscard]] bool PushBackToObject(const ObjectID& obj, const Atom& data)
        {
            OLO_PROFILE_FUNCTION();
            OLO_CORE_ASSERT(!m_PagedBuffer.IsDirectoryOnly(),
                            "GPUPagedCache is directory-only — the consumer owns the payload");
            if (m_PagedBuffer.IsDirectoryOnly())
            {
                return false; // Release: refuse rather than write through a null atom store
            }

            ObjectAllocation alloc;
            if (!m_ObjectPages.Find(obj, alloc) && !AllocatePages(obj, 1, alloc))
            {
                return false;
            }

            const sizet pageSize = m_PagedBuffer.GetPageSize();
            const auto pageIndex = static_cast<u32>(alloc.m_TotalElementCount / pageSize);
            const sizet pageElemOffset = alloc.m_TotalElementCount % pageSize;

            if (pageIndex >= CountAllocatedPages(alloc) && !AllocatePages(obj, 1, alloc))
            {
                return false;
            }

            // The write lands in the chain page at ordinal `pageIndex` — NOT
            // necessarily the end page: after ClearObject the element count
            // rewinds while the chain keeps its pages, so the reference's
            // "write to endPage" put post-clear appends in the wrong page.
            const u32 targetPage = ChainPageAt(alloc, pageIndex);
            OLO_CORE_ASSERT(targetPage != kInvalidPage, "GPUPagedCache: chain shorter than its element count");
            m_PagedBuffer.PageData(targetPage)[pageElemOffset] = data;

            const bool updated = m_ObjectPages.Mutate(obj,
                                                      [this](ObjectAllocation& stored)
                                                      {
                                                          ++stored.m_TotalElementCount;
                                                          m_Policy->OnAccess(stored.m_PolicyHandle);
                                                      });
            OLO_CORE_ASSERT(updated, "GPUPagedCache: updating a present key cannot fail");
            return updated;
        }

        // Renames src to dst. An existing dst is overwritten (its chain freed,
        // its policy node removed). The surviving object is re-registered with
        // the policy under its new identity — FIFO's handle carries the id, so
        // keeping the old node (as the reference did) would leave the object
        // unevictable.
        void MoveObject(const ObjectID& src, const ObjectID& dst)
        {
            OLO_PROFILE_FUNCTION();

            if (src == dst)
            {
                // Without this, the dst-exists branch below would free the
                // object's own live chain and OnRemove its policy node twice.
                return;
            }

            ObjectAllocation srcAlloc;
            if (!m_ObjectPages.Find(src, srcAlloc))
            {
                OLO_CORE_WARN("GPUPagedCache: MoveObject source not found");
                return;
            }
            if (ObjectAllocation dstAlloc; m_ObjectPages.Find(dst, dstAlloc))
            {
                FreeChain(dstAlloc.m_StartPage);
                m_Policy->OnRemove(dstAlloc.m_PolicyHandle);
            }

            m_Policy->OnRemove(srcAlloc.m_PolicyHandle);
            srcAlloc.m_PolicyHandle = m_Policy->OnInsert(dst);

            [[maybe_unused]] const bool inserted = m_ObjectPages.Insert(dst, srcAlloc);
            OLO_CORE_ASSERT(inserted, "GPUPagedCache: object directory full during MoveObject");
            m_ObjectPages.Erase(src);
            MaintainDirectory();
        }

        // Exchanges the two objects' allocation CONTENTS. Each object keeps its
        // own policy handle — handles are bound to object identity, and the
        // identities don't move here, only the data does.
        void Swap(const ObjectID& obj1, const ObjectID& obj2)
        {
            OLO_PROFILE_FUNCTION();

            if (obj1 == obj2)
            {
                return;
            }
            ObjectAllocation alloc1;
            ObjectAllocation alloc2;
            if (!m_ObjectPages.Find(obj1, alloc1) || !m_ObjectPages.Find(obj2, alloc2))
            {
                OLO_CORE_WARN("GPUPagedCache: Swap with a missing object");
                return;
            }

            // Exchange contents in place; each object keeps its own policy
            // handle (handles are bound to identity, and identities stay put).
            auto writeContents = [this](const ObjectAllocation& from)
            {
                return [this, &from](ObjectAllocation& stored)
                {
                    stored.m_TotalElementCount = from.m_TotalElementCount;
                    stored.m_StartPage = from.m_StartPage;
                    stored.m_EndPage = from.m_EndPage;
                    m_Policy->OnAccess(stored.m_PolicyHandle);
                };
            };
            [[maybe_unused]] bool updated = m_ObjectPages.Mutate(obj1, writeContents(alloc2));
            updated = m_ObjectPages.Mutate(obj2, writeContents(alloc1)) && updated;
            OLO_CORE_ASSERT(updated, "GPUPagedCache: updating present keys cannot fail");
        }

        void DeallocateObject(const ObjectID& obj)
        {
            OLO_PROFILE_FUNCTION();

            ObjectAllocation alloc;
            if (!m_ObjectPages.Find(obj, alloc))
            {
                return;
            }
            FreeChain(alloc.m_StartPage);
            m_Policy->OnRemove(alloc.m_PolicyHandle);
            m_ObjectPages.Erase(obj);
            MaintainDirectory();
        }

        // Rewinds the element count to zero; the chain keeps its pages.
        void ClearObject(const ObjectID& obj)
        {
            OLO_PROFILE_FUNCTION();

            [[maybe_unused]] const bool updated = m_ObjectPages.Mutate(obj,
                                                                       [this](ObjectAllocation& stored)
                                                                       {
                                                                           stored.m_TotalElementCount = 0;
                                                                           m_Policy->OnAccess(stored.m_PolicyHandle);
                                                                       });
            // Clearing a non-existing object is a no-op, as in the reference.
        }

        // The object's atoms as contiguous spans of the paged buffer, in
        // ascending address order (element units). Chain pages are full except
        // the one holding the data tail, so a range never spans the garbage
        // beyond the tail — a partially-used page always ends its range.
        [[nodiscard]] std::vector<BufferRange> GetObjectBufferRanges(const ObjectID& obj)
        {
            OLO_PROFILE_FUNCTION();

            std::vector<BufferRange> result;
            ObjectAllocation alloc;
            const bool found = m_ObjectPages.Mutate(obj,
                                                    [this, &alloc](ObjectAllocation& stored)
                                                    {
                                                        m_Policy->OnAccess(stored.m_PolicyHandle);
                                                        alloc = stored;
                                                    });
            if (!found)
            {
                return result;
            }

            const sizet pageSize = m_PagedBuffer.GetPageSize();
            if (pageSize == 0 || alloc.m_TotalElementCount == 0 || alloc.IsEmpty())
            {
                return result;
            }

            // (page index, atoms used) per chain page, in CHAIN order — every
            // page full except the last one data reaches.
            std::vector<std::pair<u32, sizet>> usedPages;
            sizet remaining = alloc.m_TotalElementCount;
            for (u32 current = alloc.m_StartPage; current != kInvalidPage && remaining > 0;
                 current = m_PageNodes.Data()[current].m_Next)
            {
                const sizet used = std::min(remaining, pageSize);
                usedPages.emplace_back(current, used);
                remaining -= used;
            }

            std::ranges::sort(usedPages);
            for (sizet i = 0; i < usedPages.size();)
            {
                const sizet rangeStartAtom = static_cast<sizet>(usedPages[i].first) * pageSize;
                sizet rangeAtoms = 0;
                // Extend while pages are address-adjacent AND the previous page
                // was completely full (a partial page ends the readable run).
                sizet j = i;
                while (true)
                {
                    rangeAtoms += usedPages[j].second;
                    const bool partial = usedPages[j].second < pageSize;
                    ++j;
                    if (partial || j >= usedPages.size() || usedPages[j].first != usedPages[j - 1].first + 1)
                    {
                        break;
                    }
                }
                result.push_back({ rangeStartAtom, rangeAtoms });
                i = j;
            }
            return result;
        }

        // Re-register `obj` as most recently used without touching its
        // allocation. Returns false when the object is not in the directory.
        //
        // THE EVICTION-ORDERING HALF OF A CACHE HIT. A consumer that finds an
        // object already resident still has to tell the policy it was used, or
        // the object ages towards the LRU tail while in active use and is
        // evicted from under a live frame. `AllocatePages` cannot serve this: it
        // APPENDS the requested pages to whatever the object already holds, so
        // calling it to record a hit grows the allocation every frame.
        //
        // Note the ordering consequence for a caller touching a whole priority
        // list: `LRUPolicy::OnAccess` moves to the FRONT and the victim is the
        // TAIL, so touching in priority order leaves the highest-priority entry
        // nearest the tail. Touch in REVERSE priority order.
        bool Touch(const ObjectID& obj)
        {
            return m_ObjectPages.Mutate(obj, [this](ObjectAllocation& stored)
                                        { m_Policy->OnAccess(stored.m_PolicyHandle); });
        }

        [[nodiscard]] bool Has(const ObjectID& obj) const
        {
            return m_ObjectPages.Contains(obj);
        }

        [[nodiscard]] bool Find(const ObjectID& obj, ObjectAllocation& out) const
        {
            return m_ObjectPages.Find(obj, out);
        }

        // Fires whenever the POLICY reclaims a victim under memory pressure
        // (never for explicit DeallocateObject/MoveObject calls) — the hook an
        // external-payload consumer uses to drop its side of the object. The
        // listener runs mid-allocation and must NOT call back into this cache.
        void SetEvictionListener(EvictionListener listener)
        {
            m_EvictionListener = std::move(listener);
        }

        // Access for consumers whose policy needs configuration beyond its
        // capacity constructor (e.g. a context pointer).
        [[nodiscard]] Policy<TObjectID>& GetPolicy()
        {
            OLO_CORE_ASSERT(m_Policy.has_value(), "GPUPagedCache used before Create");
            return *m_Policy;
        }

        void BindAtomData(u32 bindingPoint) const
        {
            m_PagedBuffer.Bind(bindingPoint);
        }
        void BindLookup(u32 hashMapBinding, u32 pageNodesBinding) const
        {
            m_ObjectPages.Bind(hashMapBinding);
            m_PageNodes.Bind(pageNodesBinding);
        }

        [[nodiscard]] bool IsCreated() const
        {
            return m_PagedBuffer.IsCreated();
        }
        [[nodiscard]] sizet GetPageSize() const
        {
            return m_PagedBuffer.GetPageSize();
        }
        [[nodiscard]] u32 GetPageCount() const
        {
            return m_PagedBuffer.GetPageCount();
        }

      private:
        template<typename Cache>
        friend class GPUPagedCacheInspector; // validation, debugging and unit tests

        enum class EvictOutcome : u8
        {
            Progress,  // pages were taken from a victim
            Retry,     // a stale policy entry was consumed; ask again
            Exhausted, // the policy has nothing evictable — allocation fails
        };

        void SetNodeNext(u32 page, u32 next)
        {
            m_PageNodes.Data()[page].m_Next = next;
            m_PageNodes.MirrorWrite(page, 1);
        }

        // Claims whatever free pages exist (no eviction) and appends them to
        // the chain; eviction makes up any shortfall in the caller's loop.
        // Greedy on purpose: an all-or-nothing reserve would leave free pages
        // idle while a victim is evicted for the whole request.
        [[nodiscard]] u32 TryReservePages(ObjectAllocation& alloc, u32 pageCount)
        {
            OLO_PROFILE_FUNCTION();

            // Member scratch: the pre-#704 free list allocated nothing per
            // page load, so this path shouldn't either. Safe because cache
            // mutation is single-writer and the eviction listener is forbidden
            // from re-entering the cache.
            m_ScratchPages.clear();
            const u32 reserved = m_PagedBuffer.ReserveUpToPages(pageCount, m_ScratchPages);
            AppendChain(alloc, m_ScratchPages);
            return reserved;
        }

        void AppendChain(ObjectAllocation& alloc, const std::vector<u32>& pages)
        {
            if (pages.empty())
            {
                return;
            }
            if (alloc.IsEmpty())
            {
                alloc.m_StartPage = pages.front();
            }
            else
            {
                SetNodeNext(alloc.m_EndPage, pages.front());
            }
            const sizet pageCount = pages.size();
            for (sizet i = 0; i + 1 < pageCount; ++i)
            {
                SetNodeNext(pages[i], pages[i + 1]);
            }
            SetNodeNext(pages.back(), kInvalidPage);
            alloc.m_EndPage = pages.back();
        }

        [[nodiscard]] EvictOutcome EvictAndTakePages(const ObjectID& obj, ObjectAllocation& targetAlloc,
                                                     u32 pagesNeeded, u32& outTaken)
        {
            OLO_PROFILE_FUNCTION();

            outTaken = 0;
            ObjectID victimID{};
            if (!m_Policy->TrySelectVictim(obj, victimID))
            {
                return EvictOutcome::Exhausted;
            }

            ObjectAllocation victimAlloc;
            if (!m_ObjectPages.Find(victimID, victimAlloc))
            {
                // The policy named an object the directory no longer holds.
                // None of the shipped list-based policies can produce this
                // (OnRemove unlinks eagerly), but a consumer-supplied policy
                // might — the caller bounds consecutive retries.
                return EvictOutcome::Retry;
            }

            const u32 victimPages = CountAllocatedPages(victimAlloc);
            const u32 pagesToTake = std::min(victimPages, pagesNeeded);
            if (pagesToTake == 0)
            {
                // A cached object with no pages cannot arise today, but a
                // zero-take must not report Progress or the caller spins.
                m_Policy->OnRemove(victimAlloc.m_PolicyHandle);
                m_ObjectPages.Erase(victimID);
                NotifyEviction(victimID);
                return EvictOutcome::Retry;
            }

            // Split the victim's chain: the first pagesToTake pages transfer to
            // the requester, the remainder is freed outright.
            u32 current = victimAlloc.m_StartPage;
            u32 prev = kInvalidPage;
            for (u32 i = 0; i < pagesToTake; ++i)
            {
                prev = current;
                current = m_PageNodes.Data()[current].m_Next;
            }
            const u32 takeStart = victimAlloc.m_StartPage;
            const u32 takeEnd = prev;
            const u32 remainingStart = current;
            SetNodeNext(takeEnd, kInvalidPage);

            AttachPages(targetAlloc, takeStart, takeEnd);
            FreeChain(remainingStart);

            m_Policy->OnRemove(victimAlloc.m_PolicyHandle);
            m_ObjectPages.Erase(victimID);
            MaintainDirectory();
            NotifyEviction(victimID);

            outTaken = pagesToTake;
            return EvictOutcome::Progress;
        }

        // Every Erase mints a tombstone, and tombstones are what turn misses
        // into full-table scans over time (see GPUHashMap::NeedsCompaction).
        // The cache is the single-writer context the map's compaction demands,
        // so it amortises the rebuild across its own erase sites.
        void MaintainDirectory()
        {
            if (m_ObjectPages.NeedsCompaction())
            {
                m_ObjectPages.CompactTombstones();
            }
        }

        // Attach an already-linked chain segment [start, end] to the target.
        void AttachPages(ObjectAllocation& target, u32 start, u32 end)
        {
            OLO_CORE_ASSERT(end != kInvalidPage, "GPUPagedCache: attaching an empty segment");
            if (target.IsEmpty())
            {
                target.m_StartPage = start;
            }
            else
            {
                SetNodeNext(target.m_EndPage, start);
            }
            target.m_EndPage = end;
            SetNodeNext(end, kInvalidPage);
        }

        // Frees every page from `startPage` to the chain end, resetting links.
        void FreeChain(u32 startPage)
        {
            u32 current = startPage;
            while (current != kInvalidPage)
            {
                const u32 next = m_PageNodes.Data()[current].m_Next;
                SetNodeNext(current, kInvalidPage);
                m_PagedBuffer.FreePage(current);
                current = next;
            }
        }

        // Undo the pages AllocatePages appended past the pre-call allocation.
        // `before` is null when the object had no allocation at all.
        void RollBackAppendedPages(ObjectAllocation& alloc, const ObjectAllocation* before)
        {
            if (before == nullptr || before->IsEmpty())
            {
                FreeChain(alloc.m_StartPage);
                alloc = (before != nullptr) ? *before : ObjectAllocation{};
                return;
            }
            const u32 appendedStart = m_PageNodes.Data()[before->m_EndPage].m_Next;
            FreeChain(appendedStart);
            SetNodeNext(before->m_EndPage, kInvalidPage);
            alloc = *before;
        }

        [[nodiscard]] u32 CountAllocatedPages(const ObjectAllocation& alloc) const
        {
            u32 count = 0;
            for (u32 current = alloc.m_StartPage; current != kInvalidPage;
                 current = m_PageNodes.Data()[current].m_Next)
            {
                ++count;
            }
            return count;
        }

        // The chain page at ordinal `index` (0 = start page).
        [[nodiscard]] u32 ChainPageAt(const ObjectAllocation& alloc, u32 index) const
        {
            u32 current = alloc.m_StartPage;
            for (u32 i = 0; i < index && current != kInvalidPage; ++i)
            {
                current = m_PageNodes.Data()[current].m_Next;
            }
            return current;
        }

        void NotifyEviction(const ObjectID& victim)
        {
            if (m_EvictionListener)
            {
                m_EvictionListener(victim);
            }
        }

        std::optional<Policy<TObjectID>> m_Policy;
        GPUHashMap<TObjectID, ObjectAllocation> m_ObjectPages;
        GPUCacheStorage<PageNode> m_PageNodes;
        GPUPagedBuffer<TAtom> m_PagedBuffer;
        EvictionListener m_EvictionListener;
        std::vector<u32> m_ScratchPages; // TryReservePages scratch (single-writer)
    };
} // namespace OloEngine
