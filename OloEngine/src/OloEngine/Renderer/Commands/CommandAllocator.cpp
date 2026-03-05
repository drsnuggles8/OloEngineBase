#include "OloEnginePCH.h"
#include "CommandAllocator.h"

namespace OloEngine
{
    CommandAllocator::CommandAllocator(sizet blockSize)
        : m_Cache(blockSize)
    {
    }

    CommandAllocator::CommandAllocator(CommandAllocator&& other) noexcept
        : m_Cache(std::move(other.m_Cache)),
          m_AllocationCount(other.m_AllocationCount)
    {
        other.m_AllocationCount = 0;
    }

    CommandAllocator& CommandAllocator::operator=(CommandAllocator&& other) noexcept
    {
        if (this != &other)
        {
            m_Cache = std::move(other.m_Cache);
            m_AllocationCount = other.m_AllocationCount;
            other.m_AllocationCount = 0;
        }
        return *this;
    }

    void* CommandAllocator::AllocateCommandMemory(sizet size)
    {
        OLO_PROFILE_FUNCTION();

        if (size > MAX_COMMAND_SIZE)
        {
            OLO_CORE_ERROR("CommandAllocator: Requested size {0} exceeds maximum command size {1}", size, MAX_COMMAND_SIZE);
            return nullptr;
        }

        void* result = m_Cache.Allocate(size, COMMAND_ALIGNMENT);
        if (result)
        {
            ++m_AllocationCount;
        }
        return result;
    }

    void CommandAllocator::Reset()
    {
        OLO_PROFILE_FUNCTION();

        m_Cache.Reset();
        m_AllocationCount = 0;
    }

    sizet CommandAllocator::GetTotalAllocated() const
    {
        return m_Cache.GetTotalAllocated();
    }

} // namespace OloEngine
