#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <glm/glm.hpp>

#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class GPUScene;

    // Mirrors OloPtMaterialTextures' record in GpuPathTracer.glsl (std430, 16
    // bytes): the BYTE offset of each material texture's descriptor in the
    // resource heap, or kUnresolved. Indexed by GPU Scene material SLOT.
    struct alignas(16) MaterialTextureRecord
    {
        u32 Albedo = RHI::HeapOffset::Invalid;
        u32 MetallicRoughness = RHI::HeapOffset::Invalid;
        u32 Normal = RHI::HeapOffset::Invalid;
        u32 Emissive = RHI::HeapOffset::Invalid;

        [[nodiscard]] auto operator==(const MaterialTextureRecord&) const -> bool = default;
    };
    static_assert(sizeof(MaterialTextureRecord) == 16, "MaterialTextureRecord must match the 16-byte GLSL record");

    // @brief The GPU path tracer's per-material texture table (issue #1055,
    // the #805 capability scoped to ray-query shaders).
    //
    // WHY IT EXISTS. A ray-query hit can land on any material, so shading it
    // from textures needs the shader to pick a descriptor at runtime — which
    // GL_EXT_descriptor_heap gives a Vulkan-only shader directly (ADR 0011
    // amendment (95)). The GPU Scene material records carry each texture's
    // RHI handle; this table turns those handles into heap BYTE offsets
    // through the backend's own slot cache (HeapBinding::
    // ResolveShaderHeapTexture), one record per material slot, uploaded by
    // device address like the emissive table. It does not touch the records'
    // own `*HeapOffset` fields: those are the engine heap's slot indices,
    // resolved only under the OLO_RHI_BINDLESS lever and owned by GPUScene.
    //
    // WHEN IT RUNS. EndFrame after the GPU Scene commit, before the emissive
    // table resolves (an emitter's texture offset comes from here). Records
    // are rebuilt every frame the tracer gathers and re-uploaded only when
    // their bytes changed, for the reason EmissiveTriangleTable gives.
    class MaterialTextureTable
    {
      public:
        void BeginFrame(bool gather);

        // Resolve every live material's textures. Returns the record count.
        u32 EndFrame(const GPUScene& scene);

        [[nodiscard]] bool IsGathering() const noexcept
        {
            return m_Gathering;
        }
        [[nodiscard]] u32 GetRecordCount() const noexcept
        {
            return static_cast<u32>(m_Records.size());
        }
        // A material slot's record, or an all-Invalid one past the end.
        [[nodiscard]] MaterialTextureRecord GetRecord(u32 materialSlot) const noexcept;
        // Textures a live material referenced that the backend could not
        // resolve this frame (dead handle, foreign image, no heap). Counted,
        // never silently shaded flat.
        [[nodiscard]] u32 GetUnresolvedCount() const noexcept
        {
            return m_Unresolved;
        }
        // The material sampler's heap byte offset (HeapBinding::
        // MaterialTexture2DSampler), Invalid where the backend cannot index.
        [[nodiscard]] u32 GetSamplerHeapOffset() const noexcept
        {
            return m_SamplerOffset;
        }
        // Device address of the uploaded table; 0 with nothing uploaded or on
        // a backend without buffer addresses.
        [[nodiscard]] u64 GetDeviceAddress() const noexcept;

        void Shutdown();

      private:
        [[nodiscard]] u32 ResolveTexture(u32 handleIndex, u32 handleGeneration);

        bool m_Gathering = false;
        std::vector<MaterialTextureRecord> m_Records;
        std::vector<MaterialTextureRecord> m_Uploaded;
        // Handle -> byte offset, kept across frames: the slot cache is
        // persistent and a resolve is a registry lookup plus a mutex.
        std::unordered_map<u64, u32> m_ResolvedByHandle;
        u32 m_SamplerOffset = RHI::HeapOffset::Invalid;
        bool m_SamplerResolved = false;
        u32 m_Unresolved = 0;
        u32 m_UploadedCount = 0;
        Ref<StorageBuffer> m_Buffer;
    };
} // namespace OloEngine
