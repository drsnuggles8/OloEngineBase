#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomEvaluationScratch.h"

#include <atomic>

namespace OloEngine
{
    namespace
    {
        // Relaxed: each figure is one owner's, published from the thread that
        // used it, and the report reads a total, not an ordering.
        std::atomic<u64> s_RetainedBytes{ 0 };
        std::atomic<u32> s_InstancesRetaining{ 0 };

        template<typename T>
        [[nodiscard]] u64 VectorBytes(const std::vector<T>& v) noexcept
        {
            return static_cast<u64>(v.capacity()) * sizeof(T);
        }

        template<typename T>
        void Free(std::vector<T>& v) noexcept
        {
            std::vector<T>().swap(v);
        }
    } // namespace

    u64 GroomEvaluationScratchRetainedBytes() noexcept
    {
        return s_RetainedBytes.load(std::memory_order_relaxed);
    }

    u32 GroomEvaluationScratchInstancesRetaining() noexcept
    {
        return s_InstancesRetaining.load(std::memory_order_relaxed);
    }

    GroomEvaluationScratchLedgerEntry::~GroomEvaluationScratchLedgerEntry()
    {
        Publish(0);
    }

    void GroomEvaluationScratchLedgerEntry::Publish(u64 bytes) noexcept
    {
        if (bytes == m_Published)
        {
            return;
        }
        if (bytes > m_Published)
        {
            s_RetainedBytes.fetch_add(bytes - m_Published, std::memory_order_relaxed);
            ++m_Growths;
        }
        else
        {
            s_RetainedBytes.fetch_sub(m_Published - bytes, std::memory_order_relaxed);
        }
        if (m_Published == 0)
        {
            s_InstancesRetaining.fetch_add(1u, std::memory_order_relaxed);
        }
        else if (bytes == 0)
        {
            s_InstancesRetaining.fetch_sub(1u, std::memory_order_relaxed);
        }
        m_Published = bytes;
    }

    GroomSurfaceSkinScratch::~GroomSurfaceSkinScratch()
    {
        Release();
    }

    void GroomSurfaceSkinScratch::Account() noexcept
    {
        Publish(VectorBytes(Current) + VectorBytes(Previous) + VectorBytes(Weighted));
    }

    void GroomSurfaceSkinScratch::Release() noexcept
    {
        Free(Current);
        Free(Previous);
        Free(Weighted);
        Publish(0);
    }
} // namespace OloEngine
