#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/StagedBufferReadback.h"

#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>
#include <utility>

namespace OloEngine
{
    StagedBufferReadback::~StagedBufferReadback()
    {
        Release();
    }

    StagedBufferReadback::StagedBufferReadback(StagedBufferReadback&& other) noexcept
        : m_Staging(other.m_Staging), m_Capacity(other.m_Capacity), m_Staged(other.m_Staged)
    {
        other.m_Staging = RHI::NullResource;
        other.m_Capacity = 0;
        other.m_Staged = false;
    }

    StagedBufferReadback& StagedBufferReadback::operator=(StagedBufferReadback&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_Staging = other.m_Staging;
            m_Capacity = other.m_Capacity;
            m_Staged = other.m_Staged;
            other.m_Staging = RHI::NullResource;
            other.m_Capacity = 0;
            other.m_Staged = false;
        }
        return *this;
    }

    void StagedBufferReadback::Release()
    {
        if (m_Staging.IsValid())
        {
            RenderCommand::DeleteBuffer(m_Staging);
            m_Staging = RHI::NullResource;
        }
        m_Capacity = 0;
        m_Staged = false;
    }

    void StagedBufferReadback::Stage(RHI::ResourceHandle source, u64 srcOffsetBytes, u64 sizeBytes,
                                     MemoryBarrierFlags barriers)
    {
        if (!source.IsValid() || sizeBytes == 0)
        {
            return;
        }

        // Grow-only: a readback whose size varies per frame (a probe table, a
        // proxy list) would otherwise reallocate constantly.
        if (!m_Staging.IsValid() || m_Capacity < sizeBytes)
        {
            Release();
            m_Staging = RenderCommand::CreateBufferHandle();
            if (!m_Staging.IsValid())
            {
                return;
            }
            RenderCommand::AllocateBufferStorage(m_Staging, sizeBytes, RHI::MemoryResidency::DeviceToHost);
            m_Capacity = sizeBytes;
        }

        // BufferUpdate is what orders the copy itself; the caller's `barriers`
        // carry whatever ordered the WRITES it is copying (ShaderStorage for the
        // usual atomic case). Dropping either makes the copy read a value that
        // is right most of the time.
        RenderCommand::MemoryBarrier(barriers | MemoryBarrierFlags::BufferUpdate);
        RenderCommand::CopyBufferSubData(source, m_Staging, srcOffsetBytes, 0, sizeBytes);
        m_Staged = true;
    }

    bool StagedBufferReadback::Read(void* dest, u64 sizeBytes)
    {
        if (!m_Staged || !m_Staging.IsValid() || dest == nullptr || sizeBytes == 0)
        {
            return false;
        }
        RenderCommand::ReadBufferSubData(m_Staging, 0, std::min(sizeBytes, m_Capacity), dest);
        m_Staged = false;
        return true;
    }
} // namespace OloEngine
