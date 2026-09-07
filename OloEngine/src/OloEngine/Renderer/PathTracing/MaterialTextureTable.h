#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class GPUScene;

    // Mirrors OloPtMaterialTextures' record in GpuPathTracer.glsl (std430, 16
    // bytes): the BYTE offset of each material texture's descriptor in the
    // resource heap, or RHI::HeapOffset::Invalid. Indexed by GPU Scene
    // material SLOT.
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
    // are rebuilt AND re-resolved every frame the tracer gathers — never
    // memoised across frames, because a texture reloaded in place keeps its
    // RHI handle while its image and heap slot change (VulkanTexture2D::
    // Invalidate); RHITypes.h's rule for a heap offset is "fetch it, do not
    // store it". Uploaded only when the bytes changed, into a fresh buffer
    // for the reason EmissiveTriangleTable gives, and that change is reported through
    // ChangedThisFrame so the accumulation restarts: a map that resolves a
    // frame late changes what every hit shades with.
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
        // The uploaded bytes differ from last frame's: a texture resolved or
        // changed slot, so the integrand changed and the sum must restart.
        [[nodiscard]] bool ChangedThisFrame() const noexcept
        {
            return m_ChangedThisFrame;
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

        [[nodiscard]] bool HasGPUResources() const noexcept
        {
            return m_Buffer != nullptr;
        }

        void Shutdown();

      private:
        [[nodiscard]] u32 ResolveTexture(u32 handleIndex, u32 handleGeneration);

        bool m_Gathering = false;
        bool m_ChangedThisFrame = false;
        std::vector<MaterialTextureRecord> m_Records;
        std::vector<MaterialTextureRecord> m_Uploaded;
        u32 m_SamplerOffset = RHI::HeapOffset::Invalid;
        bool m_SamplerResolved = false;
        u32 m_Unresolved = 0;
        u32 m_UploadedCount = 0;
        Ref<StorageBuffer> m_Buffer;
    };
} // namespace OloEngine
