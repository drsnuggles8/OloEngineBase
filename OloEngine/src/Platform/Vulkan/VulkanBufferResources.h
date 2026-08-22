#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanBufferResources.h — #691: the buffer-shaped resource factories
// (UniformBuffer / VertexBuffer / IndexBuffer / VertexArray) on Vulkan.
//
// The design center is ADR 0011 §4: per-draw data reaches shaders as ONE GPU
// pointer into the frame arena, so a Vulkan "uniform buffer" is not a bound
// object at all — it is a CPU shadow plus a lazily (re)pushed arena range
// whose device address the draw-time root-data writer embeds. Binding is
// what dies; the address is what survives.
//
//  - VulkanUniformBuffer: CPU shadow (the base class's m_LocalData) + a
//    versioned per-frame arena push. SetData mid-frame allocates a NEW range,
//    so draws recorded before the write keep the old contents — the GL
//    ordering semantics, reproduced without any hazard tracking.
//  - VulkanVertexBuffer / VulkanIndexBuffer: persistent VMA buffers (mesh
//    data is upload-once); vertex data is PULLED via buffer device address
//    (§5 — there is no vertex-input state), index data feeds
//    vkCmdBindIndexBuffer.
//  - VulkanVertexArray: a pure CPU aggregate (buffer refs + layout). Vulkan
//    has no VAO object; the draw path resolves the aggregate through
//    VulkanRootObjectRegistry from the packet's vertexArrayID handle.
//
// This header exposes Vulkan types directly — it is included only by
// Platform/Vulkan siblings and by OLO_WITH_VULKAN-guarded engine factory TUs
// (the sanctioned factory-include pattern, rhi-abstraction-boundary.md).
//
// Thread-safety: NONE, deliberately — render thread only, like the rest of
// the backend (GPU object creation is marshalled there by the asset system).
// =============================================================================

#include "Platform/Vulkan/VulkanDevice.h"

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <unordered_map>
#include <vector>
#ifdef OLO_DEBUG
#include <stacktrace>
#endif

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // VulkanRootObjectRegistry — handle → backend object side table.
    //
    // The dispatch path receives RHI::ResourceHandle in its packets
    // (BindUniformBuffer / BindStorageBuffer / BindVertexArrayRaw), and the
    // Vulkan draw path needs the OBJECT back (to ask for a device address or
    // an index buffer), not a native name — RHI::ResourceRegistry only
    // resolves handle → native u64, and for arena-backed UBOs and CPU-side
    // VAOs there is no native object at all. Same side-table pattern as
    // VulkanImageInfoRegistry: registered in constructors, unregistered in
    // destructors, render-thread only, process-wide and deliberately leaked.
    //
    // Lookup validates the GENERATION (the key packs index + generation), so
    // a stale handle to a destroyed object misses instead of dereferencing a
    // dangling pointer.
    // -------------------------------------------------------------------------
    enum class VulkanRootObjectKind : u8
    {
        UniformBuffer,
        StorageBuffer,
        VertexArray,
        Shader,       ///< VulkanShader — resolved by BindShaderProgram packets.
        VertexBuffer, ///< VulkanVertexBuffer — registered for DIAGNOSTICS only (#810):
                      ///< the draw path reaches a vertex stream by device address, so
                      ///< nothing binds through this entry. It exists because the
                      ///< resource inspector's Vulkan arm needs a handle -> object hop
                      ///< to answer "how big is this buffer?", which the RHI registry
                      ///< (handle -> native only) cannot.
        IndexBuffer,  ///< VulkanIndexBuffer — same, diagnostics only.
        Framebuffer,  ///< VulkanFramebuffer — resolved by the raw-handle framebuffer ops
                      ///< (clears / blits / draw-attachment selection, #691).
                      ///< Needed because the FB registers native = 0 (no VkFramebuffer
                      ///< exists under dynamic rendering), so ResourceRegistry cannot
                      ///< resolve it — the same "no native object" reason VAOs live here.
    };

    class VulkanRootObjectRegistry
    {
      public:
        [[nodiscard]] static VulkanRootObjectRegistry& Get();

        void Register(RHI::ResourceHandle handle, VulkanRootObjectKind kind, void* object);
        void Unregister(RHI::ResourceHandle handle);

        struct Entry
        {
            VulkanRootObjectKind Kind{};
            void* Object = nullptr;
        };
        // nullptr when the handle was never registered or is stale. The
        // pointer is invalidated by the next Register/Unregister — use, don't
        // hold.
        [[nodiscard]] const Entry* Lookup(RHI::ResourceHandle handle) const;

        // Teardown forensics (#691 — the close-button VMA abort):
        // after the full renderer/layer teardown, every VertexArray still
        // registered here is a leak suspect keeping its vertex/index buffers'
        // VMA allocations alive into vmaDestroyAllocator. Logs each survivor
        // with its Debug-captured creation call stack; no-op in non-Debug.
        void LogSurvivingVertexArrays() const;

        // Forced release of every surviving shader's VkShaderModules at
        // context teardown — the central-registry-owns-native-lifetime
        // pattern: a shader Ref lingering in a stray static becomes an inert
        // zombie instead of a vkDestroyDevice leak
        // (VUID-vkDestroyDevice-device-05137). Runs in ALL configs.
        void ReleaseSurvivingShaderModules();

      private:
        VulkanRootObjectRegistry() = default;

        [[nodiscard]] static u64 Key(RHI::ResourceHandle handle)
        {
            return (static_cast<u64>(handle.Generation) << 32) | handle.Index;
        }

        std::unordered_map<u64, Entry> m_Entries;
    };

    // -------------------------------------------------------------------------
    // VulkanRawBufferRegistry — the object-less buffer family behind the
    // CreateBufferHandle / AllocateBufferStorage / ReadBufferSubData /
    // DeleteBuffer facade entries (#691).
    //
    // GL's shape is a bare glCreateBuffers name whose storage is allocated
    // later by glNamedBufferData; production consumers are readback staging
    // rings (ShaderDebugDraw's per-channel DeviceToHost header copies) and
    // GPU-written scratch. There is no engine-side C++ object to hang the
    // VkBuffer off, so this side table owns {VkBuffer, VmaAllocation, mapping}
    // keyed by the minted handle. The handle's registry native is kept in sync
    // (0 until AllocateBufferStorage creates storage, the VkBuffer after), so
    // generic native resolution (CopyBufferSubData, barrier lowering) works on
    // raw buffers exactly as on object-backed ones.
    //
    // Render-thread only, same as everything else here.
    // -------------------------------------------------------------------------
    class VulkanRawBufferRegistry
    {
      public:
        struct Entry
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VmaAllocation Allocation = VK_NULL_HANDLE;
            void* Mapped = nullptr; ///< Non-null for host-visible residencies.
            bool Coherent = true;
            u64 Size = 0;
            RHI::MemoryResidency Residency = RHI::MemoryResidency::DeviceLocal;
        };

        [[nodiscard]] static VulkanRawBufferRegistry& Get();

        // Mints the identity (registry native = 0 until storage exists).
        [[nodiscard]] RHI::ResourceHandle CreateHandle();
        // (Re)creates the VMA buffer at `sizeBytes` for `residency`
        // (DeviceToHost => HOST_VISIBLE mapped, the readback-ring case). An
        // existing buffer goes through VulkanDeferredReclaim — GL's
        // glNamedBufferData orphaning semantics.
        void Allocate(RHI::ResourceHandle handle, u64 sizeBytes, RHI::MemoryResidency residency);
        // Null when the handle was never minted here (or is stale).
        [[nodiscard]] Entry* Lookup(RHI::ResourceHandle handle);
        // Deferred-reclaims the buffer, unregisters the identity, drops the
        // entry. Safe on foreign/stale handles (no-op).
        void Destroy(RHI::ResourceHandle handle);

      private:
        VulkanRawBufferRegistry() = default;

        [[nodiscard]] static u64 Key(RHI::ResourceHandle handle)
        {
            return (static_cast<u64>(handle.Generation) << 32) | handle.Index;
        }

        std::unordered_map<u64, Entry> m_Entries;
    };

    // -------------------------------------------------------------------------
    // VulkanUniformBuffer — arena-versioned uniform data, no VkBuffer of its
    // own.
    //
    // GetRootDataAddress() is the draw-record-time seam: it (re)pushes the
    // shadow into the current frame-arena slot when the data changed or a new
    // frame began, and returns the address the root-data writer embeds. A
    // mid-frame SetData therefore gives later draws a NEW range while earlier
    // draws keep referencing the old one — exactly GL's command-ordered
    // glNamedBufferSubData semantics, with no sync tracking, because arena
    // ranges live for the full kFramesInFlight window by contract.
    // -------------------------------------------------------------------------
    class VulkanUniformBuffer : public UniformBuffer
    {
      public:
        VulkanUniformBuffer(u32 size, u32 binding);
        ~VulkanUniformBuffer() override;

        void SetData(const UniformData& data) override;

        // No driver bind — the address travels in root data. What Bind()
        // MEANS here is GL's glBindBufferBase semantics: publish this buffer
        // as its binding point's occupant in VulkanBindingState so the
        // draw-time root writer can find it.
        void Bind() const override;

        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }

        [[nodiscard]] u32 GetBinding() const
        {
            return m_Binding;
        }
        [[nodiscard]] u32 GetAllocatedSize() const
        {
            return m_AllocatedSize;
        }

        // Draw-record-time address of this buffer's current contents in the
        // frame arena. 0 on arena overflow (the draw's root writer treats
        // that as a dropped draw, mirroring the arena's own contract).
        [[nodiscard]] VkDeviceAddress GetRootDataAddress();

      private:
        // Grows the base-class shadow to at least `requiredSize`, zero-filling
        // new bytes (the base convenience SetData grows it too; direct
        // SetData(UniformData) callers bypass that, so we own growth here).
        void EnsureShadow(u32 requiredSize);

        u32 m_AllocatedSize = 0;
        u32 m_Binding = 0;

        u64 m_DataVersion = 1;
        u64 m_PushedVersion = 0;
        u64 m_PushedFrameGeneration = ~0ull;
        VkDeviceAddress m_CurrentAddress = 0;

        // Identity with native = 0: there is no persistent VkBuffer to name —
        // the arena slot buffer is shared and its ranges are transient.
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    // -------------------------------------------------------------------------
    // VulkanVertexBuffer — persistent VMA buffer, pulled by device address.
    //
    // Usage includes STORAGE_BUFFER because §5's vertex pulling reads the
    // stream through `layout(std430, binding = 57) readonly buffer` — the
    // pipeline maps that block to this buffer's address via the root struct.
    // -------------------------------------------------------------------------
    class VulkanVertexBuffer : public VertexBuffer
    {
      public:
        explicit VulkanVertexBuffer(u32 size);
        VulkanVertexBuffer(const void* data, u32 size);
        ~VulkanVertexBuffer() override;

        // No-ops: geometry reaches the shader via device address, and the
        // VAO-shaped bind state lives in the draw path's binding tracker.
        void Bind() const override
        {
        }
        void Unbind() const override
        {
        }

        void SetData(const VertexData& data) override;

        [[nodiscard]] const BufferLayout& GetLayout() const override
        {
            return m_Layout;
        }
        void SetLayout(const BufferLayout& layout) override
        {
            m_Layout = layout;
        }

        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard]] u32 GetBufferHandle() const override
        {
            return 0;
        }

        [[nodiscard]] VkBuffer GetVkBuffer() const
        {
            return m_Buffer;
        }
        [[nodiscard]] VkDeviceAddress GetDeviceAddress() const
        {
            return m_DeviceAddress;
        }
        [[nodiscard]] u32 GetSize() const
        {
            return m_Size;
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const
        {
            return m_RHIHandle.Get();
        }

      private:
        void CreateBuffer(const void* initialData);
        void ReleaseBuffer();

        BufferLayout m_Layout;
        u32 m_Size = 0;

        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        void* m_Mapped = nullptr; ///< Non-null when VMA gave a host-visible placement.
        bool m_NeedsFlush = false;
        VkDeviceAddress m_DeviceAddress = 0;
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    // -------------------------------------------------------------------------
    // VulkanIndexBuffer — persistent VMA buffer for vkCmdBindIndexBuffer.
    // The engine's index format is fixed 32-bit (IndexBuffer.h contract).
    // -------------------------------------------------------------------------
    class VulkanIndexBuffer : public IndexBuffer
    {
      public:
        VulkanIndexBuffer(const u32* indices, u32 count);
        ~VulkanIndexBuffer() override;

        void Bind() const override
        {
        }
        void Unbind() const override
        {
        }

        [[nodiscard]] u32 GetCount() const override
        {
            return m_Count;
        }
        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard]] u32 GetBufferHandle() const override
        {
            return 0;
        }

        [[nodiscard]] VkBuffer GetVkBuffer() const
        {
            return m_Buffer;
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const
        {
            return m_RHIHandle.Get();
        }

      private:
        void ReleaseBuffer();

        u32 m_Count = 0;
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = VK_NULL_HANDLE;
        RHI::ScopedResourceHandle m_RHIHandle;
    };

    // -------------------------------------------------------------------------
    // VulkanVertexArray — CPU-side aggregate; no GPU object exists.
    //
    // The draw path resolves the packet's vertexArrayID through
    // VulkanRootObjectRegistry to this object, then takes the FIRST vertex
    // buffer's device address as the §5 pull stream and the index buffer for
    // vkCmdBindIndexBuffer. Multi-stream pulling (a second vertex buffer
    // holding per-instance attributes) is NOT modelled — instancing data
    // travels the InstanceData SSBO (glsl-shaders.md §6a), and a pass that
    // genuinely needs a second attribute stream surfaces as a loud draw-time
    // error rather than reading garbage.
    // -------------------------------------------------------------------------
    class VulkanVertexArray : public VertexArray
    {
      public:
        VulkanVertexArray();
        ~VulkanVertexArray() override;

        void Bind() const override
        {
        }
        void Unbind() const override
        {
        }

        void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
        void AddInstanceBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
        // Deliberate no-op, and deliberately NOT recorded in m_VertexBuffers:
        // the constant-attribute stub exists only to keep GL attribute-layout
        // permutations uniform (see VertexArray::AddConstantVertexBuffer).
        // The Vulkan pull path stubs optional streams in-shader, and recording
        // the 8-byte buffer here would make it stream 1 of the pull PAIR,
        // shadowing the bone-pull slot with garbage.
        void AddConstantVertexBuffer(const Ref<VertexBuffer>& /*vertexBuffer*/) override
        {
        }
        void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) override;

        [[nodiscard]] const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const override
        {
            return m_VertexBuffers;
        }
        [[nodiscard]] const Ref<IndexBuffer>& GetIndexBuffer() const override
        {
            return m_IndexBuffer;
        }

        // Diagnostics-only field: a native GL name does not exist here.
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 0;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }

        // Draw-path conveniences (backend-internal). Const: the draw assembly
        // only reads addresses/handles off the buffers.
        [[nodiscard]] const VulkanVertexBuffer* GetPullVertexBuffer() const;
        // Stream-indexed form for the reserved pull PAIR (ADR item A3):
        // stream 0 = SSBO_VERTEX_PULL (57), stream 1 = SSBO_BONE_PULL (63,
        // MeshSource's bone-influence VB). Returns null past the last stream —
        // the root-data writer maps that to the zero address (warn-once),
        // never an error.
        [[nodiscard]] const VulkanVertexBuffer* GetPullVertexBuffer(sizet streamIndex) const;
        [[nodiscard]] const VulkanIndexBuffer* GetVulkanIndexBuffer() const;

#ifdef OLO_DEBUG
        // Leak forensics: who built this VAO (see
        // VulkanRootObjectRegistry::LogSurvivingVertexArrays).
        [[nodiscard]] const std::stacktrace& GetCreationStack() const
        {
            return m_CreationStack;
        }
#endif

      private:
        std::vector<Ref<VertexBuffer>> m_VertexBuffers;
        Ref<IndexBuffer> m_IndexBuffer;
        RHI::ScopedResourceHandle m_RHIHandle;
#ifdef OLO_DEBUG
        std::stacktrace m_CreationStack;
#endif
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
