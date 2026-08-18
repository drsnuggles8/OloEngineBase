#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUCache/GPUCacheStorage.h"

#include <atomic>
#include <bit>
#include <limits>
#include <memory>
#include <vector>

namespace OloEngine
{
    // @brief Page-granular buffer: a flat atom array carved into fixed-size
    // pages, with a lock-free free-page bitset (issue #704, ported from the
    // VoxelEngine reference's GPUPagedBuffer).
    //
    // The bitset always lives on the CPU heap (the GPU never consults it); the
    // atom storage backing is the caller's choice — HostOnly for headless use,
    // DeviceMapped for a real GPU arena the CPU stages payloads into.
    //
    // Directory-only mode: Create with pageSizeAtoms == 0 allocates NO atom
    // storage — the object is purely a page-index allocator. That is how a
    // consumer whose payload lives in its own buffers (VirtualMeshRegistry's
    // vertex/index slot arenas) reuses the allocation machinery without
    // adopting this class's storage.
    template<typename Atom>
        requires GPUSafeStruct<Atom>
    class GPUPagedBuffer
    {
      public:
        static constexpr u32 kInvalidPage = std::numeric_limits<u32>::max();

        GPUPagedBuffer() = default;
        GPUPagedBuffer(const GPUPagedBuffer&) = delete;
        GPUPagedBuffer& operator=(const GPUPagedBuffer&) = delete;

        [[nodiscard]] bool Create(sizet pageSizeAtoms, u32 pageCount, GPUCacheBacking backing)
        {
            if (m_PageCount != 0 || pageCount == 0 || pageCount >= kInvalidPage)
            {
                return false;
            }

            if (pageSizeAtoms > 0)
            {
                const u64 totalAtoms = static_cast<u64>(pageSizeAtoms) * pageCount;
                if (totalAtoms > std::numeric_limits<u32>::max() ||
                    !m_Storage.Create(static_cast<u32>(totalAtoms), backing))
                {
                    return false;
                }
            }

            m_PageSize = pageSizeAtoms;
            m_PageCount = pageCount;

            const sizet wordCount = (pageCount + kWordBits - 1) / kWordBits;
            m_FreePages = std::make_unique<std::atomic<u64>[]>(wordCount);
            for (sizet i = 0; i < wordCount; ++i)
            {
                m_FreePages[i].store(std::numeric_limits<u64>::max(), std::memory_order_relaxed);
            }
            // Ghost pages past pageCount in the last word are marked reserved so
            // the scan can never hand them out.
            if (pageCount % kWordBits != 0)
            {
                const u64 usedBits = pageCount % kWordBits;
                const u64 ghostMask = ~((1ull << usedBits) - 1);
                m_FreePages[wordCount - 1].store(~ghostMask, std::memory_order_relaxed);
            }
            m_FreePageCount.store(pageCount, std::memory_order_relaxed);
            return true;
        }

        void Destroy()
        {
            m_Storage.Destroy();
            m_FreePages.reset();
            m_FreePageCount.store(0, std::memory_order_relaxed);
            m_PageSize = 0;
            m_PageCount = 0;
        }

        // Write-side pointer to a page's atoms. HostOnly: normal memory.
        // DeviceMapped: WRITE-only mapped memory — never read through it.
        [[nodiscard]] Atom* PageData(u32 pageIndex)
        {
            OLO_CORE_ASSERT(m_PageSize > 0, "GPUPagedBuffer is directory-only (pageSizeAtoms == 0)");
            OLO_CORE_ASSERT(pageIndex < m_PageCount, "GPUPagedBuffer page index out of range");
            return m_Storage.Data() + static_cast<sizet>(pageIndex) * m_PageSize;
        }

        // Readback of a page's atoms that is safe in every backing (device
        // round-trip when the storage is DeviceMapped). Test/inspection path.
        [[nodiscard]] bool ReadbackPage(u32 pageIndex, Atom* out) const
        {
            if (m_PageSize == 0 || pageIndex >= m_PageCount)
            {
                return false;
            }
            return m_Storage.ReadbackRange(static_cast<u32>(static_cast<sizet>(pageIndex) * m_PageSize),
                                           static_cast<u32>(m_PageSize), out);
        }

        // Claims as many of `count` free pages as exist, appending them to
        // outPages. Returns the number claimed — the caller decides whether a
        // partial claim is progress (eviction makes up the difference) or must
        // be rolled back.
        //
        // LOWEST INDEX FIRST is a documented guarantee, not an accident of the
        // word scan: consumers rely on it for deterministic layout
        // (VirtualMeshRegistry's slot assignment reproduces its pre-#704
        // free-list order because of it), and a test pins it. An optimisation
        // that reorders the scan is a behaviour change.
        [[nodiscard]] u32 ReserveUpToPages(u32 count, std::vector<u32>& outPages)
        {
            OLO_CORE_ASSERT(m_FreePages != nullptr, "GPUPagedBuffer used before Create");

            // Advisory early-out: a residency cache at budget spends most
            // frames with zero free pages, and scanning the all-zero bitset
            // per allocation is pure waste. Exact under the single-writer
            // usage; concurrent racers merely fall through to the scan.
            if (count == 0 || m_FreePageCount.load(std::memory_order_relaxed) == 0)
            {
                return 0;
            }

            const sizet wordCount = (m_PageCount + kWordBits - 1) / kWordBits;
            u32 remaining = count;
            for (sizet i = 0; i < wordCount && remaining > 0; ++i)
            {
                std::atomic<u64>& word = m_FreePages[i];
                while (remaining > 0)
                {
                    u64 old = word.load(std::memory_order_relaxed);
                    if (old == 0)
                    {
                        break;
                    }
                    const int bit = std::countr_zero(old);
                    const u64 mask = 1ull << bit;
                    const u64 desired = old & ~mask;
                    if (word.compare_exchange_weak(old, desired, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
                    {
                        m_FreePageCount.fetch_sub(1, std::memory_order_relaxed);
                        outPages.push_back(static_cast<u32>(i * kWordBits + static_cast<sizet>(bit)));
                        --remaining;
                    }
                }
            }
            return count - remaining;
        }

        void FreePage(u32 pageIndex)
        {
            OLO_CORE_ASSERT(pageIndex < m_PageCount, "GPUPagedBuffer page index out of range");
            const u64 mask = 1ull << (pageIndex % kWordBits);
            std::atomic<u64>& word = m_FreePages[pageIndex / kWordBits];
            u64 old = word.load(std::memory_order_relaxed);
            while (true)
            {
                OLO_CORE_ASSERT((old & mask) == 0, "GPUPagedBuffer: freeing a page that is already free — "
                                                   "two chains claim the same page");
                if (word.compare_exchange_weak(old, old | mask, std::memory_order_release,
                                               std::memory_order_relaxed))
                {
                    m_FreePageCount.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        }

        [[nodiscard]] bool IsPageReserved(u32 pageIndex) const
        {
            OLO_CORE_ASSERT(pageIndex < m_PageCount, "GPUPagedBuffer page index out of range");
            const u64 word = m_FreePages[pageIndex / kWordBits].load(std::memory_order_relaxed);
            return (word & (1ull << (pageIndex % kWordBits))) == 0;
        }

        void Bind(u32 bindingPoint) const
        {
            m_Storage.Bind(bindingPoint);
        }

        [[nodiscard]] sizet GetPageSize() const
        {
            return m_PageSize;
        }
        [[nodiscard]] u32 GetPageCount() const
        {
            return m_PageCount;
        }
        [[nodiscard]] bool IsCreated() const
        {
            return m_PageCount != 0;
        }
        [[nodiscard]] bool IsDirectoryOnly() const
        {
            return m_PageCount != 0 && m_PageSize == 0;
        }
        [[nodiscard]] RHI::ResourceHandle GetDeviceHandle() const
        {
            return m_Storage.GetDeviceHandle();
        }

      private:
        static constexpr sizet kWordBits = 64;

        GPUCacheStorage<Atom> m_Storage;
        std::unique_ptr<std::atomic<u64>[]> m_FreePages;
        // Advisory mirror of the bitset's population for the empty fast path;
        // exact whenever mutation is single-writer.
        std::atomic<u32> m_FreePageCount{ 0 };
        sizet m_PageSize = 0;
        u32 m_PageCount = 0;
    };
} // namespace OloEngine
