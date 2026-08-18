#pragma once

// White-box access to GPUPagedCache internals for validation, debugging and
// unit tests (issue #704 — port of the VoxelEngine reference's
// Tests/Helpers/gpu_cache_inspector.h). Test helper only; the friend
// declaration lives in GPUPagedCache.h.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedCache.h"

#include <algorithm>
#include <vector>

namespace OloEngine
{
    template<typename Cache>
    class GPUPagedCacheInspector
    {
      public:
        using ObjectID = typename Cache::ObjectID;
        using Atom = typename Cache::Atom;
        using ObjectAllocation = typename Cache::ObjectAllocation;

        // The object's atoms in CHAIN order — the order AllocateObject wrote
        // them. Uses the backing-safe readback path, so it works against a
        // DeviceMapped atom arena too.
        static std::vector<Atom> ReadObjectData(const Cache& cache, const ObjectID& id)
        {
            std::vector<Atom> result;

            ObjectAllocation alloc;
            if (!cache.m_ObjectPages.Find(id, alloc) || alloc.m_TotalElementCount == 0 || alloc.IsEmpty())
            {
                return result;
            }

            const sizet pageSize = cache.m_PagedBuffer.GetPageSize();
            std::vector<Atom> pageAtoms(pageSize);
            sizet remaining = alloc.m_TotalElementCount;
            for (u32 current = alloc.m_StartPage; current != Cache::kInvalidPage && remaining > 0;
                 current = cache.m_PageNodes.Data()[current].m_Next)
            {
                if (!cache.m_PagedBuffer.ReadbackPage(current, pageAtoms.data()))
                {
                    return result;
                }
                const sizet count = std::min(pageSize, remaining);
                result.insert(result.end(), pageAtoms.begin(), pageAtoms.begin() + count);
                remaining -= count;
            }
            return result;
        }

        static u32 GetFreePagesCount(const Cache& cache)
        {
            u32 freePages = 0;
            const u32 pageCount = cache.m_PagedBuffer.GetPageCount();
            for (u32 i = 0; i < pageCount; ++i)
            {
                if (!cache.m_PagedBuffer.IsPageReserved(i))
                {
                    ++freePages;
                }
            }
            return freePages;
        }

        static u32 CountAllocatedPages(const Cache& cache, const ObjectAllocation& alloc)
        {
            return cache.CountAllocatedPages(alloc);
        }

        // The object's page indices in CHAIN order.
        static std::vector<u32> ChainPages(const Cache& cache, const ObjectID& id)
        {
            std::vector<u32> pages;
            ObjectAllocation alloc;
            if (!cache.m_ObjectPages.Find(id, alloc))
            {
                return pages;
            }
            for (u32 current = alloc.m_StartPage; current != Cache::kInvalidPage;
                 current = cache.m_PageNodes.Data()[current].m_Next)
            {
                pages.push_back(current);
            }
            return pages;
        }
    };
} // namespace OloEngine
