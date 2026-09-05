#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"

#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace OloEngine
{
    namespace
    {
        // Kept in sync with the sibling anonymous-namespace copies (trivially
        // small — not worth a shared header).
        void VkCheck(VkResult result, const char* what)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("Vulkan buffers: ") + what + " failed (VkResult " +
                                         std::to_string(static_cast<int>(result)) + ")");
            }
        }

        template<typename T>
        [[nodiscard]] u64 VkHandleToU64(T handle)
        {
            if constexpr (std::is_pointer_v<T>)
            {
                return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
            }
            else
            {
                return static_cast<u64>(handle);
            }
        }

        struct CreatedBuffer
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            void* Mapped = nullptr;
            bool NeedsFlush = false;
        };

        // Create a persistent buffer that PREFERS a host-writable device-local
        // placement (ReBAR/UMA — the common desktop case) and falls back to
        // plain device-local, in which case Mapped stays null and uploads go
        // through a staged one-shot copy.
        [[nodiscard]] CreatedBuffer CreatePersistentBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const char* what)
        {
            auto* device = VulkanDevice::Get();
            OLO_CORE_ASSERT(device != nullptr, "CreatePersistentBuffer requires a live VulkanDevice");
            if (device == nullptr)
            {
                throw std::runtime_error(std::string(what) + ": no live VulkanDevice");
            }

            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = std::max<VkDeviceSize>(size, 1u);
            bufferInfo.usage = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

            CreatedBuffer out;
            VmaAllocationInfo outInfo{};
            VkCheck(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &out.Buffer, &out.Allocation,
                                    &outInfo),
                    what);
            // Owner tag for the teardown leak dump (VulkanDevice::Shutdown):
            // an allocation alive at vmaDestroyAllocator prints this name.
            vmaSetAllocationName(device->GetAllocator(), out.Allocation, what);

            VkMemoryPropertyFlags memProps = 0;
            vmaGetAllocationMemoryProperties(device->GetAllocator(), out.Allocation, &memProps);
            if ((memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
            {
                out.Mapped = outInfo.pMappedData;
                out.NeedsFlush = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
            }
            return out;
        }

    } // namespace

    // =========================================================================
    // VulkanRootObjectRegistry
    // =========================================================================

    VulkanRootObjectRegistry& VulkanRootObjectRegistry::Get()
    {
        static auto* s_Instance = new VulkanRootObjectRegistry(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanRootObjectRegistry::Register(RHI::ResourceHandle handle, VulkanRootObjectKind kind, void* object)
    {
        if (!handle.IsValid() || object == nullptr)
        {
            return;
        }
        m_Entries[Key(handle)] = Entry{ .Kind = kind, .Object = object };
    }

    void VulkanRootObjectRegistry::Unregister(RHI::ResourceHandle handle)
    {
        m_Entries.erase(Key(handle));
    }

    const VulkanRootObjectRegistry::Entry* VulkanRootObjectRegistry::Lookup(RHI::ResourceHandle handle) const
    {
        const auto it = m_Entries.find(Key(handle));
        return it != m_Entries.end() ? &it->second : nullptr;
    }

#if defined(OLO_DEBUG) && defined(OLO_PLATFORM_WINDOWS)
    namespace
    {
        // "Who holds the Ref?" — scan every committed writable page of the
        // process for pointer cells containing `target` and report each hit.
        // A hit inside the exe module's data sections IS a static holder
        // (resolve the printed module+RVA with `cdb ln OloEditor+<rva>`);
        // a heap hit means a container/capture holds it. Teardown-only
        // forensics: a full-VA scan costs seconds and runs once per
        // surviving object, in Debug, at exit.
        void ReportRefHolders(const void* target, const char* name)
        {
            const auto targetValue = reinterpret_cast<std::uintptr_t>(target);
            const auto* moduleBase = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
            MEMORY_BASIC_INFORMATION info{};
            const u8* address = nullptr;
            while (VirtualQuery(address, &info, sizeof(info)) == sizeof(info))
            {
                const u8* regionEnd = static_cast<const u8*>(info.BaseAddress) + info.RegionSize;
                const bool writableData = info.State == MEM_COMMIT &&
                                          (info.Protect == PAGE_READWRITE || info.Protect == PAGE_WRITECOPY) &&
                                          (info.Type == MEM_PRIVATE || info.Type == MEM_IMAGE);
                if (writableData)
                {
                    const auto* begin = static_cast<const std::uintptr_t*>(info.BaseAddress);
                    const auto* end = reinterpret_cast<const std::uintptr_t*>(regionEnd);
                    for (const std::uintptr_t* cell = begin; cell < end; ++cell)
                    {
                        if (*cell != targetValue)
                            continue;
                        const auto* cellBytes = reinterpret_cast<const u8*>(cell);
                        if (info.Type == MEM_IMAGE)
                        {
                            // The RVA is computed against the MAIN module's
                            // base; a hit inside another loaded image would
                            // resolve nonsense there, so always print the
                            // absolute address too (review finding).
                            OLO_CORE_TRACE("[Vulkan]   holder of '{}': STATIC at {:p} (main-module RVA {:#x}; "
                                           "resolve: cdb> ln OloEditor+{:#x}, or ln on the absolute address "
                                           "if the cell is in another module)",
                                           name, static_cast<const void*>(cellBytes),
                                           static_cast<std::uintptr_t>(cellBytes - moduleBase),
                                           static_cast<std::uintptr_t>(cellBytes - moduleBase));
                        }
                        else
                        {
                            OLO_CORE_TRACE("[Vulkan]   holder of '{}': heap cell at {:p}", name,
                                           static_cast<const void*>(cell));
                        }
                    }
                }
                address = regionEnd;
            }
        }
    } // namespace
#endif

    void VulkanRootObjectRegistry::ReleaseSurvivingShaderModules()
    {
        // The forced device-object release (the pattern big engines use: a
        // central registry owns native-handle lifetime; outstanding Refs in
        // stray statics become harmless zombies instead of
        // vkDestroyDevice-time leaks). Trace names the untidy owners so
        // they can still be fixed at the source.
        sizet released = 0;
        for (auto& [key, entry] : m_Entries)
        {
            if (entry.Kind != VulkanRootObjectKind::Shader || entry.Object == nullptr)
            {
                continue;
            }
            auto* shader = static_cast<VulkanShader*>(entry.Object);
            OLO_CORE_TRACE("[Vulkan] releasing modules of surviving shader '{}' (refcount {}) at context teardown",
                           shader->GetName(), shader->GetRefCount());
#if defined(OLO_DEBUG) && defined(OLO_PLATFORM_WINDOWS)
            ReportRefHolders(shader, shader->GetName().c_str());
#endif
            shader->ReleaseDeviceObjects();
            ++released;
        }
        if (released > 0)
        {
            OLO_CORE_TRACE("[Vulkan] {} shader(s) survived teardown — modules force-released (their Refs are now "
                           "inert)",
                           released);
        }
    }

    void VulkanRootObjectRegistry::LogSurvivingVertexArrays() const
    {
#ifdef OLO_DEBUG
        sizet survivors = 0;
        for (const auto& [key, entry] : m_Entries)
        {
            if (entry.Kind != VulkanRootObjectKind::VertexArray || entry.Object == nullptr)
            {
                continue;
            }
            ++survivors;
            const auto* vao = static_cast<const VulkanVertexArray*>(entry.Object);
            std::string trace = std::to_string(vao->GetCreationStack());
            // The full stack is long; the engine-side creator is what names
            // the owner. Keep the first ~12 frames.
            sizet cut = 0;
            for (int newlines = 0; cut < trace.size(); ++cut)
            {
                if (trace[cut] == '\n' && ++newlines == 12)
                    break;
            }
            OLO_CORE_ERROR("[Vulkan] surviving VertexArray ({} stream(s)) created at:\n{}",
                           vao->GetVertexBuffers().size(), trace.substr(0, cut));
        }
        if (survivors > 0)
        {
            OLO_CORE_ERROR("[Vulkan] {} shader/vertex-array object(s) survived full teardown — see the lines above "
                           "for owners",
                           survivors);
        }
#endif
    }

    // =========================================================================
    // VulkanRawBufferRegistry
    // =========================================================================

    VulkanRawBufferRegistry& VulkanRawBufferRegistry::Get()
    {
        static auto* s_Instance = new VulkanRawBufferRegistry(); // deliberately leaked
        return *s_Instance;
    }

    RHI::ResourceHandle VulkanRawBufferRegistry::CreateHandle()
    {
        const RHI::ResourceHandle handle =
            RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Buffer, 0u, RHI::Backend::Vulkan);
        if (handle.IsValid())
        {
            m_Entries.emplace(Key(handle), Entry{});
        }
        return handle;
    }

    void VulkanRawBufferRegistry::Allocate(RHI::ResourceHandle handle, u64 sizeBytes, RHI::MemoryResidency residency,
                                           bool requireHostCoherentMap)
    {
        auto* entry = Lookup(handle);
        if (entry == nullptr)
        {
            OLO_CORE_WARN("[RHI/Vulkan] AllocateBufferStorage on a handle CreateBufferHandle never minted — ignored");
            return;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] AllocateBufferStorage with no live VulkanDevice — ignored");
            return;
        }

        // glNamedBufferData orphaning semantics: a re-allocate retires the old
        // storage (prior frames may still read it) and mints fresh storage
        // under the SAME identity.
        if (entry->Buffer != VK_NULL_HANDLE || entry->Allocation != VK_NULL_HANDLE)
        {
            // Unstage before retiring: a bind point still holding the old
            // address would publish a freed allocation (issue #1052).
            VulkanBindingState::Get().ClearStorageBufferAddress(entry->DeviceAddress);
            VulkanDeferredReclaim::Get().Enqueue(entry->Buffer, entry->Allocation);
            *entry = Entry{};
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = std::max<VkDeviceSize>(sizeBytes, 1u);
        // The GL twin's raw buffers serve as SSBO scratch, copy endpoints and
        // indirect-args storage interchangeably; one conservative usage set
        // keeps the facade's "a buffer is a buffer" contract.
        //
        // SHADER_DEVICE_ADDRESS (issue #1052): a raw buffer bound to an SSBO
        // binding point reaches the shader as a root-data POINTER like every
        // other buffer, and the address cannot be taken without this CREATE-time
        // bit. Its absence is what made SSBO_VIRTUAL_INDICES unbindable and lost
        // the device on every virtual-geometry scene. VulkanVertexBuffer and
        // VulkanIndexBuffer already set it; the raw family was the outlier.
        //
        // INDEX_BUFFER (issue #1052, the same lesson one role later): the raw
        // family is also where a dual-purpose ELEMENT buffer lands —
        // VirtualMeshRegistry's index arena is a GL element buffer AND
        // SSBO_VIRTUAL_INDICES — so SetVertexArrayIndexBuffer's
        // vkCmdBindIndexBuffer needs the bit at CREATE time too. A raw buffer's
        // role is whatever its caller picks, so the usage set has to cover
        // every role the facade offers rather than the one the last caller used.
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        switch (residency)
        {
            case RHI::MemoryResidency::DeviceToHost:
                // The readback-ring case: HOST_VISIBLE + persistently mapped;
                // RANDOM access because the consumer reads it back field-wise.
                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
            case RHI::MemoryResidency::HostToDevice:
                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
            case RHI::MemoryResidency::DeviceLocal:
                break;
        }
        if (requireHostCoherentMap)
        {
            // AllocatePersistentUploadStorage's contract (see the header): the
            // caller writes through the returned pointer and reads it on-device
            // with no flush, so a non-coherent or non-mappable placement is not
            // a slower path, it is a wrong one. Dropping
            // ALLOW_TRANSFER_INSTEAD and stating the memory properties makes
            // vmaCreateBuffer FAIL rather than hand back an unmapped buffer.
            allocInfo.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }

        VmaAllocationInfo outInfo{};
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        if (vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &buffer, &allocation, &outInfo) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] AllocateBufferStorage: vmaCreateBuffer({} bytes) failed", sizeBytes);
            // The orphan path above already retired the previous storage and
            // cleared the entry, so the identity registry is still naming a
            // buffer that no longer exists. Leaving it there makes the next
            // CopyBufferSubData / UploadBufferSubData record a copy into freed
            // storage; zeroing it makes them resolve nothing and refuse
            // loudly, which is the contract everywhere else here (#1052).
            RHI::ResourceRegistry::Get().UpdateNative(handle, 0u);
            return;
        }

        vmaSetAllocationName(device->GetAllocator(), allocation, "raw buffer (CreateBufferHandle)");
        entry->Buffer = buffer;
        entry->Allocation = allocation;
        entry->Size = sizeBytes;
        entry->Residency = residency;
        entry->Mapped = nullptr;
        entry->Coherent = true;
        VkMemoryPropertyFlags memProps = 0;
        vmaGetAllocationMemoryProperties(device->GetAllocator(), allocation, &memProps);
        if ((memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            entry->Mapped = outInfo.pMappedData;
            entry->Coherent = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        }

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = buffer;
        entry->DeviceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        // Keep generic native resolution working (CopyBufferSubData's operands
        // and barrier lowering resolve through the identity registry).
        RHI::ResourceRegistry::Get().UpdateNative(handle, VkHandleToU64(buffer));
    }

    VulkanRawBufferRegistry::Entry* VulkanRawBufferRegistry::Lookup(RHI::ResourceHandle handle)
    {
        const auto it = m_Entries.find(Key(handle));
        return it != m_Entries.end() ? &it->second : nullptr;
    }

    void VulkanRawBufferRegistry::Destroy(RHI::ResourceHandle handle)
    {
        const auto it = m_Entries.find(Key(handle));
        if (it == m_Entries.end())
        {
            return;
        }
        if (it->second.Buffer != VK_NULL_HANDLE || it->second.Allocation != VK_NULL_HANDLE)
        {
            // Unstage before retiring — see the matching call in Allocate's
            // orphan path (issue #1052).
            VulkanBindingState::Get().ClearStorageBufferAddress(it->second.DeviceAddress);
            VulkanDeferredReclaim::Get().Enqueue(it->second.Buffer, it->second.Allocation);
        }
        m_Entries.erase(it);
        RHI::ResourceRegistry::Get().Unregister(handle);
    }

    // =========================================================================
    // VulkanUniformBuffer
    // =========================================================================

    VulkanUniformBuffer::VulkanUniformBuffer(u32 size, u32 binding)
        : m_AllocatedSize(size), m_Binding(binding)
    {
        OLO_PROFILE_FUNCTION();
        EnsureShadow(size);
        m_RHIHandle.Adopt(RHI::ResourceKind::Buffer, 0u, RHI::Backend::Vulkan);
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::UniformBuffer, this);
        // GL twins occupy their binding point from creation (glBindBufferBase
        // in the ctor); mirror that so a pass that never re-Bind()s still
        // resolves.
        VulkanBindingState::Get().SetUniformBuffer(m_Binding, this);
    }

    VulkanUniformBuffer::~VulkanUniformBuffer()
    {
        VulkanBindingState::Get().ClearBuffer(this);
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
        // m_RHIHandle retires via RAII; the base class frees the shadow.
    }

    void VulkanUniformBuffer::Bind() const
    {
        VulkanBindingState::Get().SetUniformBuffer(m_Binding, const_cast<VulkanUniformBuffer*>(this));
    }

    void VulkanUniformBuffer::Unbind() const
    {
        // Only clear the slot if WE are the occupant. A later buffer on the
        // same binding point has already displaced us (a UniformBuffer claims
        // its binding at construction), and unbinding this one must not evict
        // the buffer that legitimately owns the slot now.
        VulkanBindingState& state = VulkanBindingState::Get();
        if (state.GetUniformBuffer(m_Binding) == this)
            state.SetUniformBuffer(m_Binding, nullptr);
    }

    void VulkanUniformBuffer::EnsureShadow(u32 requiredSize)
    {
        if (requiredSize == 0 || requiredSize <= m_Size)
        {
            return;
        }
        auto* newBuf = new u8[requiredSize]{};
        if (m_LocalData)
        {
            std::memcpy(newBuf, m_LocalData, m_Size);
            delete[] static_cast<u8*>(m_LocalData);
        }
        m_LocalData = newBuf;
        m_Size = requiredSize;
    }

    void VulkanUniformBuffer::SetData(const void* data, u32 size, u32 offset)
    {
        // Amendment (92) rule 6, checked at record time and BEFORE the base
        // overload's shadow write: one writer per object per region. A
        // refused item skips the whole write in every build.
        if (!ClaimParallelWriter(m_ParallelWriter, "uniform buffer"))
        {
            return;
        }
        UniformBuffer::SetData(data, size, offset);
    }

    void VulkanUniformBuffer::SetData(const UniformData& data)
    {
        // Direct callers bypass the overload above; same claim, same refusal.
        // A call arriving through it re-claims for the same item, a no-op.
        if (!ClaimParallelWriter(m_ParallelWriter, "uniform buffer"))
        {
            return;
        }
        if (data.data == nullptr || data.size == 0)
        {
            return;
        }
        EnsureShadow(data.offset + data.size);
        // The base convenience SetData(void*,size,offset) already wrote this
        // exact range into m_LocalData before calling us — re-copying is a
        // same-bytes memcpy. Direct SetData(UniformData) callers bypass the
        // base, so this copy is what keeps the shadow authoritative.
        std::memcpy(static_cast<u8*>(m_LocalData) + data.offset, data.data, data.size);
        ++m_DataVersion;
    }

    VkDeviceAddress VulkanUniformBuffer::GetRootDataAddress()
    {
        auto& arena = VulkanFrameArena::Get();
        const u64 generation = arena.GetFrameGeneration();
        if (m_PushedVersion == m_DataVersion && m_PushedFrameGeneration == generation && m_CurrentAddress != 0)
        {
            return m_CurrentAddress;
        }

        if (m_LocalData == nullptr || m_Size == 0)
        {
            return 0;
        }

        // 256 is the spec's maximum minUniformBufferOffsetAlignment — always a
        // legal uniform-block address on every device.
        const auto allocation = arena.Push(m_LocalData, m_Size, 256);
        if (!allocation.IsValid())
        {
            return 0; // arena overflow — caller drops the draw (arena contract)
        }

        m_CurrentAddress = allocation.Gpu;
        m_PushedVersion = m_DataVersion;
        m_PushedFrameGeneration = generation;
        return m_CurrentAddress;
    }

    // =========================================================================
    // VulkanVertexBuffer
    // =========================================================================

    VulkanVertexBuffer::VulkanVertexBuffer(u32 size)
        : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();
        CreateBuffer(nullptr);
        // Diagnostics-only registration (#810) — see VulkanRootObjectKind.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::VertexBuffer, this);
    }

    VulkanVertexBuffer::VulkanVertexBuffer(const void* data, u32 size)
        : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();
        CreateBuffer(data);
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::VertexBuffer, this);
    }

    VulkanVertexBuffer::~VulkanVertexBuffer()
    {
        try
        {
            VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanVertexBuffer: release failed ({}) — leaking the buffer until process exit",
                           e.what());
        }
    }

    void VulkanVertexBuffer::CreateBuffer(const void* initialData)
    {
        // STORAGE_BUFFER: §5 vertex pulling reads the stream through the
        // reserved binding-57 SSBO block. VERTEX_BUFFER kept for a potential
        // future fixed-function fallback; it costs nothing.
        //
        // ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY (issue #978): a BLAS
        // build reads this stream by device address, and the usage bit is a
        // CREATE-time property — a buffer without it cannot be handed to
        // vkCmdBuildAccelerationStructuresKHR at all, however the address is
        // obtained.
        //
        // GATED, and the gate is load-bearing. A usage bit belonging to an
        // optional extension is INVALID on a device that did not enable that
        // extension (VUID-VkBufferCreateInfo-None-09499), so setting it
        // unconditionally makes every vertex-buffer creation an error on
        // hardware without ray tracing — the exact opposite of "unsupported RT
        // keeps the raster renderer usable". Headless tests never see it
        // because they only run where RT exists; it shows up the moment the
        // capability is off, which is what OLO_VULKAN_NO_RAY_TRACING=1 is for.
        //
        // Asking the device here is safe: no buffer can exist before
        // VulkanDevice::Init has decided the capability.
        const auto* rtDevice = VulkanDevice::Get();
        const VkBufferUsageFlags accelerationStructureInput =
            (rtDevice != nullptr && rtDevice->IsRayQueryEnabled())
                ? VkBufferUsageFlags{ VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR }
                : VkBufferUsageFlags{ 0 };
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | accelerationStructureInput;
        const CreatedBuffer created = CreatePersistentBuffer(m_Size, usage, "vmaCreateBuffer (VulkanVertexBuffer)");
        m_Buffer = created.Buffer;
        m_Allocation = created.Allocation;
        m_Mapped = created.Mapped;
        m_NeedsFlush = created.NeedsFlush;

        auto* device = VulkanDevice::Get();
        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
        // Address-range trace for matching a VK_EXT_device_fault report (see
        // VulkanDevice::LogDeviceFaultInfo) to its buffer. Opt-in: dozens of
        // create lines per scene load are noise in a clean session, but when
        // hunting a GPU fault the ranges are the decisive currency (the
        // foliage OOB was named by exactly this pairing).
        if (Levers::VulkanTraceBuffers())
        {
            OLO_CORE_TRACE("[RHI/Vulkan] vertex buffer {:#x}..{:#x} ({} bytes, {})", m_DeviceAddress,
                           m_DeviceAddress + m_Size, m_Size, m_Mapped != nullptr ? "BAR-mapped" : "staged");
        }

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);

        if (initialData != nullptr && m_Size > 0)
        {
            SetData(VertexData{ .data = initialData, .size = m_Size });
        }
    }

    void VulkanVertexBuffer::ReleaseBuffer()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_DeviceAddress = 0;
        }
    }

    void VulkanVertexBuffer::SetData(const VertexData& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.data == nullptr || data.size == 0)
        {
            return;
        }
        if (data.size > m_Size)
        {
            OLO_CORE_ERROR("VulkanVertexBuffer::SetData: {} bytes exceeds the buffer's {} bytes — dropping the upload",
                           data.size, m_Size);
            return;
        }

        // NOTE: mesh data is upload-once at load time. A per-frame rewrite of
        // a buffer the PREVIOUS frame's submission still reads would race it —
        // that streaming shape (Renderer2D batches, video) gets a ring in its
        // own wave; nothing in Waves A/B streams vertex data.
        if (m_Mapped != nullptr)
        {
            std::memcpy(m_Mapped, data.data, data.size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(VulkanDevice::Get()->GetAllocator(), m_Allocation, 0, data.size);
            }
            return;
        }

        VulkanOneShot::UploadToBuffer(m_Buffer, 0, data.data, data.size, "VulkanVertexBuffer::SetData");
    }

    // =========================================================================
    // VulkanIndexBuffer
    // =========================================================================

    VulkanIndexBuffer::VulkanIndexBuffer(const u32* indices, u32 count)
        : m_Count(count)
    {
        OLO_PROFILE_FUNCTION();

        const u64 sizeBytes = static_cast<u64>(count) * sizeof(u32);
        // ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY, gated on the device
        // having actually enabled the extension: see the matching comment on
        // the vertex stream above (issue #978).
        const auto* rtDevice = VulkanDevice::Get();
        const VkBufferUsageFlags accelerationStructureInput =
            (rtDevice != nullptr && rtDevice->IsRayQueryEnabled())
                ? VkBufferUsageFlags{ VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR }
                : VkBufferUsageFlags{ 0 };
        const VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | accelerationStructureInput;
        const CreatedBuffer created =
            CreatePersistentBuffer(sizeBytes, usage, "vmaCreateBuffer (VulkanIndexBuffer)");
        m_Buffer = created.Buffer;
        m_Allocation = created.Allocation;

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(VulkanDevice::Get()->GetDevice(), &addressInfo);

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);
        // Diagnostics-only registration (#810) — see VulkanRootObjectKind.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::IndexBuffer, this);

        if (indices != nullptr && sizeBytes > 0)
        {
            if (created.Mapped != nullptr)
            {
                std::memcpy(created.Mapped, indices, sizeBytes);
                if (created.NeedsFlush)
                {
                    vmaFlushAllocation(VulkanDevice::Get()->GetAllocator(), m_Allocation, 0, sizeBytes);
                }
            }
            else
            {
                VulkanOneShot::UploadToBuffer(m_Buffer, 0, indices, sizeBytes, "VulkanIndexBuffer upload");
            }
        }
    }

    VulkanIndexBuffer::~VulkanIndexBuffer()
    {
        try
        {
            VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanIndexBuffer: release failed ({}) — leaking the buffer until process exit",
                           e.what());
        }
    }

    void VulkanIndexBuffer::ReleaseBuffer()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            m_DeviceAddress = 0;
        }
    }

    // =========================================================================
    // VulkanVertexArray
    // =========================================================================

    VulkanVertexArray::VulkanVertexArray()
    {
#ifdef OLO_DEBUG
        m_CreationStack = std::stacktrace::current(1);
#endif
        m_RHIHandle.Adopt(RHI::ResourceKind::VertexArray, 0u, RHI::Backend::Vulkan);
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::VertexArray, this);
    }

    VulkanVertexArray::~VulkanVertexArray()
    {
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
    }

    void VulkanVertexArray::AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer)
    {
        OLO_CORE_ASSERT(vertexBuffer != nullptr, "VulkanVertexArray::AddVertexBuffer: null buffer");
        m_VertexBuffers.push_back(vertexBuffer);
    }

    void VulkanVertexArray::AddInstanceBuffer(const Ref<VertexBuffer>& vertexBuffer)
    {
        // Stored for layout introspection parity with the GL twin, but the
        // pull path never reads it: per-instance data travels the InstanceData
        // SSBO (glsl-shaders.md §6a). GetPullVertexBuffer() ignores it by
        // taking the FIRST buffer.
        OLO_CORE_ASSERT(vertexBuffer != nullptr, "VulkanVertexArray::AddInstanceBuffer: null buffer");
        m_VertexBuffers.push_back(vertexBuffer);
    }

    void VulkanVertexArray::SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer)
    {
        m_IndexBuffer = indexBuffer;
    }

    const VulkanVertexBuffer* VulkanVertexArray::GetPullVertexBuffer() const
    {
        return GetPullVertexBuffer(0u);
    }

    const VulkanVertexBuffer* VulkanVertexArray::GetPullVertexBuffer(sizet streamIndex) const
    {
        if (streamIndex >= m_VertexBuffers.size())
        {
            return nullptr;
        }
        // Safe: on the Vulkan backend every VertexBuffer the factory hands out
        // is a VulkanVertexBuffer.
        return static_cast<const VulkanVertexBuffer*>(m_VertexBuffers[streamIndex].Raw());
    }

    const VulkanIndexBuffer* VulkanVertexArray::GetVulkanIndexBuffer() const
    {
        return static_cast<const VulkanIndexBuffer*>(m_IndexBuffer.Raw());
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
