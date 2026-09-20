#include "OloEnginePCH.h"
#include "ThreadLocalCache.h"
#include <algorithm>
#include <limits>
#include <new>

namespace OloEngine
{
    namespace
    {
        constexpr bool IsPowerOfTwo(sizet value)
        {
            return value != 0 && (value & (value - 1)) == 0;
        }

        // Rejection reporting lives out of line, in cold functions: a formatted
        // log call inlined into Allocate costs registers and code size on the
        // path that never takes it. Measured at 2.25 ns/allocation before this
        // split; see docs/agent-rules/bump-allocator-rollover-padding.md.
        OLO_NOINLINE void ReportBadAlignment(sizet alignment, sizet maxAlignment)
        {
            OLO_CORE_ERROR("ThreadLocalCache::Allocate: unsupported alignment {0} (must be a power of two, at most {1})", alignment, maxAlignment);
        }

        OLO_NOINLINE void ReportNoBlock()
        {
            OLO_CORE_ERROR("ThreadLocalCache::Allocate: no block available (a previous block allocation failed)");
        }

        OLO_NOINLINE void ReportOverflow(sizet size, sizet alignment)
        {
            OLO_CORE_ERROR("ThreadLocalCache::Allocate: size {0} at alignment {1} overflows sizet", size, alignment);
        }

        OLO_NOINLINE void ReportBlockAddFailed(sizet requiredSize, sizet size)
        {
            OLO_CORE_ERROR("ThreadLocalCache::Allocate: could not add a block of {0} bytes for a {1}-byte request", requiredSize, size);
        }

        OLO_NOINLINE void ReportFreshBlockTooSmall(sizet blockSize, sizet size, sizet alignment, sizet padding)
        {
            OLO_CORE_ERROR("ThreadLocalCache::Allocate: fresh block of {0} bytes cannot hold {1} bytes at alignment {2} (padding {3})", blockSize, size,
                           alignment, padding);
        }

        u8* AllocateBlockStorage(sizet bytes)
        {
            return static_cast<u8*>(::operator new(bytes, std::align_val_t(ThreadLocalCache::BLOCK_ALIGNMENT), std::nothrow));
        }

        void FreeBlockStorage(u8* data)
        {
            ::operator delete(data, std::align_val_t(ThreadLocalCache::BLOCK_ALIGNMENT));
        }
    } // namespace

    // ThreadLocalCache implementation
    ThreadLocalCache::ThreadLocalCache(sizet blockSize)
        : m_DefaultBlockSize(blockSize)
    {
        OLO_CORE_ASSERT(blockSize > 0, "Block size must be greater than 0");
        AddBlock(m_DefaultBlockSize);
    }

    ThreadLocalCache::~ThreadLocalCache()
    {
        FreeAll();
    }

    ThreadLocalCache::ThreadLocalCache(ThreadLocalCache&& other) noexcept
        : m_CurrentBlock(other.m_CurrentBlock),
          m_FirstBlock(other.m_FirstBlock),
          m_DefaultBlockSize(other.m_DefaultBlockSize),
          m_TotalAllocated(other.m_TotalAllocated),
          m_WastedMemory(other.m_WastedMemory)
    {
        other.m_CurrentBlock = nullptr;
        other.m_FirstBlock = nullptr;
        other.m_TotalAllocated = 0;
        other.m_WastedMemory = 0;
    }

    ThreadLocalCache& ThreadLocalCache::operator=(ThreadLocalCache&& other) noexcept
    {
        if (this != &other)
        {
            FreeAll();

            m_CurrentBlock = other.m_CurrentBlock;
            m_FirstBlock = other.m_FirstBlock;
            m_DefaultBlockSize = other.m_DefaultBlockSize;
            m_TotalAllocated = other.m_TotalAllocated;
            m_WastedMemory = other.m_WastedMemory;

            other.m_CurrentBlock = nullptr;
            other.m_FirstBlock = nullptr;
            other.m_TotalAllocated = 0;
            other.m_WastedMemory = 0;
        }
        return *this;
    }

    void* ThreadLocalCache::Allocate(sizet size, sizet alignment)
    {
        OLO_PROFILE_FUNCTION();
        if (size == 0)
        {
            return nullptr;
        }

        // Validate alignment to avoid undefined behavior. The aligned-address
        // expression below is a power-of-two mask, so a non-power-of-two
        // alignment does not merely round badly - it produces an address that
        // is neither aligned nor necessarily inside the block.
        if (!IsPowerOfTwo(alignment) || alignment > MAX_ALIGNMENT) [[unlikely]]
        {
            // Reported, not asserted: rejection is the documented contract for
            // a bad alignment, and OLO_CORE_ASSERT is a debugbreak in Debug -
            // which would make the behaviour untestable on exactly the config
            // CI never builds.
            ReportBadAlignment(alignment, MAX_ALIGNMENT);
            return nullptr;
        }

        // A request whose worst case -- size plus the largest padding its
        // alignment can demand -- is not even representable is rejected here,
        // explicitly and by name. The capacity tests below are subtraction-based
        // and would not wrap on their own, but letting such a request fall
        // through would report it as a failed block allocation instead of as
        // the arithmetic overflow it is. This has to precede every AddBlock
        // below, so an unrepresentable size is never handed to the heap.
        if (size > std::numeric_limits<sizet>::max() - (alignment - 1)) [[unlikely]]
        {
            ReportOverflow(size, alignment);
            return nullptr;
        }

        if (!m_CurrentBlock) [[unlikely]]
        {
            // The constructor's AddBlock can fail, and a cache that reported
            // "no block" forever after would log once per allocation from the
            // renderer's hot path and never recover. Retry instead.
            if (!AddBlock(std::max(m_DefaultBlockSize, size)))
            {
                ReportNoBlock();
                return nullptr;
            }
        }

        OLO_CORE_ASSERT(m_CurrentBlock->Offset <= m_CurrentBlock->Size, "ThreadLocalCache: block cursor is past the block end");

        auto currentAddr = reinterpret_cast<sizet>(m_CurrentBlock->Data) + m_CurrentBlock->Offset;
        sizet alignedAddr = (currentAddr + (alignment - 1)) & ~(alignment - 1);
        sizet alignmentPadding = alignedAddr - currentAddr;

        // Room left in the current block. Offset <= Size is an invariant, so
        // this subtraction cannot wrap, and the comparison below is written as
        // two subtractions rather than one sum so it cannot overflow either.
        sizet remaining = m_CurrentBlock->Size - m_CurrentBlock->Offset;

        if (alignmentPadding > remaining || size > remaining - alignmentPadding) [[unlikely]]
        {
            // Rollover. A fresh (or rewound) block's base is BLOCK_ALIGNMENT-
            // aligned and its cursor is 0, so the padding there is *zero* for
            // every supported alignment: reserve exactly `size`, and never size
            // a block using padding computed for the block we are leaving.
            sizet const wasted = remaining;
            sizet const requiredSize = std::max(m_DefaultBlockSize, size);

            if (!AddBlock(requiredSize)) [[unlikely]]
            {
                // Nothing moved: the old block is still current, at its old
                // cursor, and the caller gets a null it can act on.
                ReportBlockAddFailed(requiredSize, size);
                return nullptr;
            }

            m_WastedMemory += wasted;

            currentAddr = reinterpret_cast<sizet>(m_CurrentBlock->Data) + m_CurrentBlock->Offset;
            alignedAddr = (currentAddr + (alignment - 1)) & ~(alignment - 1);
            alignmentPadding = alignedAddr - currentAddr;
            remaining = m_CurrentBlock->Size - m_CurrentBlock->Offset;

            // The final bounds check the old code skipped. It should be
            // unreachable given the block-alignment guarantee above, but the
            // cost of being wrong here is a cursor advanced into memory we do
            // not own, so it is checked rather than assumed.
            if (alignmentPadding > remaining || size > remaining - alignmentPadding) [[unlikely]]
            {
                ReportFreshBlockTooSmall(m_CurrentBlock->Size, size, alignment, alignmentPadding);
                OLO_CORE_ASSERT(false, "ThreadLocalCache::Allocate: fresh block too small after rollover");
                return nullptr;
            }
        }

        // Update the offset in the current block
        m_CurrentBlock->Offset += alignmentPadding + size;
        OLO_CORE_ASSERT(m_CurrentBlock->Offset <= m_CurrentBlock->Size, "ThreadLocalCache: cursor advanced past the block end");

        m_TotalAllocated += size;

        // Return aligned pointer
        return reinterpret_cast<void*>(alignedAddr);
    }

    void ThreadLocalCache::Reset()
    {
        // Reset all blocks to their initial state without freeing memory
        MemoryBlock* block = m_FirstBlock;
        while (block)
        {
            block->Offset = 0;
            block = block->Next;
        }

        // Reset the current block to the first block
        m_CurrentBlock = m_FirstBlock;

        // Reset tracking metrics
        m_TotalAllocated = 0;
        m_WastedMemory = 0;
    }

    void ThreadLocalCache::FreeAll()
    {
        // Free all memory blocks
        MemoryBlock* block = m_FirstBlock;
        while (block)
        {
            MemoryBlock* next = block->Next;
            FreeBlockStorage(block->Data);
            delete block;
            block = next;
        }

        m_FirstBlock = nullptr;
        m_CurrentBlock = nullptr;
        m_TotalAllocated = 0;
        m_WastedMemory = 0;
    }

    bool ThreadLocalCache::AddBlock(sizet minSize)
    {
        // After Reset(), m_CurrentBlock is rewound to m_FirstBlock while the
        // subsequent blocks still exist.  Before allocating a brand-new block,
        // check whether the NEXT block in the existing chain is large enough.
        if (m_CurrentBlock && m_CurrentBlock->Next)
        {
            MemoryBlock* reusable = m_CurrentBlock->Next;
            if (reusable->Size >= minSize)
            {
                reusable->Offset = 0;
                m_CurrentBlock = reusable;
                return true;
            }
        }

        sizet const blockSize = std::max(minSize, m_DefaultBlockSize);

        // nothrow: an over-large request is a caller error to report, not an
        // exception thrown out of the renderer's hot path. The storage is
        // allocated at BLOCK_ALIGNMENT so a fresh block needs no padding for
        // any supported alignment - see Allocate's rollover path.
        u8* data = AllocateBlockStorage(blockSize);
        if (!data)
        {
            OLO_CORE_ERROR("ThreadLocalCache::AddBlock: failed to allocate a block of {0} bytes", blockSize);
            return false;
        }

        // Create a new memory block
        auto* newBlock = new MemoryBlock();
        newBlock->Size = blockSize;
        newBlock->Offset = 0;
        newBlock->Data = data;

        // Preserve the rest of the chain so existing blocks aren't orphaned.
        if (m_CurrentBlock)
        {
            newBlock->Next = m_CurrentBlock->Next;
            m_CurrentBlock->Next = newBlock;
        }
        else
        {
            newBlock->Next = nullptr;
        }

        // Add to the linked list
        if (!m_FirstBlock)
        {
            m_FirstBlock = newBlock;
        }

        m_CurrentBlock = newBlock;

        OLO_CORE_TRACE("ThreadLocalCache: Added new block of size {0} bytes", newBlock->Size);
        return true;
    }
} // namespace OloEngine
