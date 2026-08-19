#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <cstring>
#include <memory>
#include <type_traits>

namespace OloEngine
{
    // @brief A struct that is safe to place in GPU-visible memory byte-for-byte.
    template<typename T>
    concept GPUSafeStruct = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                            !std::is_polymorphic_v<T> && !std::is_reference_v<T> && !std::is_pointer_v<T>;

    // Where a GPU-cache data structure keeps its bytes (issue #704).
    //
    // The whole substrate runs its allocation/probing logic on a plain CPU
    // pointer, so the SAME code serves all three backings — which is what makes
    // the allocate/evict/split/collision tests runnable headless (HostOnly)
    // while the shipped configuration is GPU-visible.
    enum class GPUCacheBacking : u8
    {
        // Heap memory only. No rendering device required — headless tests, and
        // consumers (like VirtualMeshRegistry) whose shaders don't read the
        // directory.
        HostOnly,
        // Heap memory is the authority for every CPU read/write; each mutation
        // is mirrored into a persistent-mapped device buffer so shaders can
        // read the structure. The RHI's persistent mapping is WRITE-only
        // (GL_MAP_WRITE_BIT without READ — see
        // OpenGLRendererAPI::AllocatePersistentUploadStorage), so reading it
        // back from the CPU would be undefined; the host copy exists precisely
        // so no CPU read ever touches the mapping.
        HostMirrored,
        // Persistent-mapped device buffer only, no host copy. CPU access is
        // WRITE-only (bulk payload staging); CPU-side reads must go through
        // ReadbackRange, which round-trips via the device.
        DeviceMapped
    };

    // @brief Typed storage for the GPU-cache substrate with selectable backing.
    //
    // NOT a general buffer abstraction: it exists so GPUHashMap / GPUPagedBuffer
    // / GPUPagedCache can run identically with or without a rendering device.
    // Single render-thread mutation contract for the device mirror; the host
    // pointer itself supports the lock-free (std::atomic_ref) access the hash
    // map and page bitset perform.
    template<typename Atom>
        requires GPUSafeStruct<Atom>
    class GPUCacheStorage
    {
      public:
        GPUCacheStorage() = default;
        ~GPUCacheStorage()
        {
            Destroy();
        }
        GPUCacheStorage(const GPUCacheStorage&) = delete;
        GPUCacheStorage& operator=(const GPUCacheStorage&) = delete;

        [[nodiscard]] bool Create(u32 count, GPUCacheBacking backing)
        {
            if (m_Count != 0 || count == 0)
            {
                return false;
            }

            m_Backing = backing;

            if (backing != GPUCacheBacking::DeviceMapped)
            {
                m_HostData = std::make_unique<Atom[]>(count);
            }

            if (backing != GPUCacheBacking::HostOnly)
            {
                if (!RenderCommand::IsDeviceAvailable())
                {
                    m_HostData.reset();
                    return false;
                }
                m_DeviceBuffer = RenderCommand::CreateBufferHandle();
                m_MappedPtr = static_cast<Atom*>(RenderCommand::AllocatePersistentUploadStorage(
                    m_DeviceBuffer, static_cast<u64>(count) * sizeof(Atom)));
                if (m_MappedPtr == nullptr)
                {
                    RenderCommand::DeleteBuffer(m_DeviceBuffer);
                    m_DeviceBuffer = RHI::NullResource;
                    m_HostData.reset();
                    return false;
                }
            }

            m_Count = count;
            return true;
        }

        void Destroy()
        {
            if (m_DeviceBuffer.IsValid())
            {
                // Reached at static teardown when an owner's explicit Shutdown
                // was skipped: with the RendererAPI gone, leak the device
                // buffer (as pre-#704 code did) rather than call into it.
                if (RenderCommand::IsDeviceAvailable())
                {
                    if (m_MappedPtr != nullptr)
                    {
                        RenderCommand::UnmapBuffer(m_DeviceBuffer);
                    }
                    RenderCommand::DeleteBuffer(m_DeviceBuffer);
                }
                m_MappedPtr = nullptr;
                m_DeviceBuffer = RHI::NullResource;
            }
            m_HostData.reset();
            m_Count = 0;
        }

        // The authority pointer the substrate's logic operates on. DeviceMapped
        // storage hands back the mapped pointer, which is WRITE-only — callers
        // in that mode must never read through it (see GPUCacheBacking).
        [[nodiscard]] Atom* Data()
        {
            return m_HostData ? m_HostData.get() : m_MappedPtr;
        }
        [[nodiscard]] const Atom* Data() const
        {
            return m_HostData ? m_HostData.get() : m_MappedPtr;
        }

        // HostMirrored: republish [first, first+count) of the host copy into the
        // device mapping. No-op for the other backings, so mutation sites can
        // call it unconditionally.
        void MirrorWrite(u32 first, u32 count)
        {
            if (m_Backing != GPUCacheBacking::HostMirrored || count == 0)
            {
                return;
            }
            OLO_CORE_ASSERT(first + count <= m_Count, "GPUCacheStorage::MirrorWrite out of range");
            std::memcpy(m_MappedPtr + first, m_HostData.get() + first, static_cast<sizet>(count) * sizeof(Atom));
        }

        // DeviceMapped test/inspection path: read a range back through the
        // device (the mapping itself is not CPU-readable). Host-backed storage
        // copies straight from the authority.
        [[nodiscard]] bool ReadbackRange(u32 first, u32 count, Atom* out) const
        {
            if (first + count > m_Count)
            {
                return false;
            }
            if (m_HostData)
            {
                std::memcpy(out, m_HostData.get() + first, static_cast<sizet>(count) * sizeof(Atom));
                return true;
            }
            RenderCommand::ReadBufferSubData(m_DeviceBuffer, static_cast<u64>(first) * sizeof(Atom),
                                             static_cast<u64>(count) * sizeof(Atom), out);
            return true;
        }

        void Bind(u32 bindingPoint) const
        {
            OLO_CORE_ASSERT(m_DeviceBuffer.IsValid(), "GPUCacheStorage::Bind on host-only storage");
            RenderCommand::BindStorageBuffer(bindingPoint, m_DeviceBuffer);
        }

        [[nodiscard]] u32 GetCount() const
        {
            return m_Count;
        }
        [[nodiscard]] GPUCacheBacking GetBacking() const
        {
            return m_Backing;
        }
        [[nodiscard]] bool IsCreated() const
        {
            return m_Count != 0;
        }
        [[nodiscard]] bool IsDeviceBacked() const
        {
            return m_DeviceBuffer.IsValid();
        }
        [[nodiscard]] RHI::ResourceHandle GetDeviceHandle() const
        {
            return m_DeviceBuffer;
        }

      private:
        std::unique_ptr<Atom[]> m_HostData;
        Atom* m_MappedPtr = nullptr;
        RHI::ResourceHandle m_DeviceBuffer{};
        u32 m_Count = 0;
        GPUCacheBacking m_Backing = GPUCacheBacking::HostOnly;
    };
} // namespace OloEngine
