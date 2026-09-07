#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PathTracing/MaterialTextureTable.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include <algorithm>
#include <cstring>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kMinimumRecordCapacity = 16u;
    } // namespace

    void MaterialTextureTable::BeginFrame(bool gather)
    {
        m_Gathering = gather;
        m_ChangedThisFrame = false;
        m_Records.clear();
        m_Unresolved = 0;
    }

    u32 MaterialTextureTable::ResolveTexture(u32 handleIndex, u32 handleGeneration)
    {
        const RHI::ResourceHandle handle{ handleIndex, handleGeneration };
        if (!handle.IsValid())
            return RHI::HeapOffset::Invalid;
        // Resolved EVERY frame, never memoised by handle: an in-place reload
        // keeps the handle and replaces the image and its slot. The backend's
        // slot cache is the memo; this is a registry lookup and a hash probe.
        const u32 offset = HeapBinding::ResolveShaderHeapTexture(handle).Value;
        if (offset == RHI::HeapOffset::Invalid)
            ++m_Unresolved;
        return offset;
    }

    u32 MaterialTextureTable::EndFrame(const GPUScene& scene)
    {
        m_Records.clear();
        m_UploadedCount = 0;
        if (!m_Gathering)
            return 0;

        if (!HeapBinding::ShaderHeapIndexingSupported())
        {
            // Not an error: the OpenGL backend cannot index a heap from a
            // shader, and the pass reports the untextured shading it does
            // instead (GpuPathTracerStats::TexturesAvailable).
            return 0;
        }
        if (!m_SamplerResolved)
        {
            m_SamplerOffset = HeapBinding::ResolveShaderHeapSampler(HeapBinding::MaterialTexture2DSampler()).Value;
            m_SamplerResolved = m_SamplerOffset != RHI::HeapOffset::Invalid;
        }

        // One record per material SLOT, reached through the live instances:
        // the slot is what the shader indexes with (instance.MaterialIndex),
        // and an instance carries the generation the slot lookup needs.
        const u32 materialSlots = scene.GetMaterialSlotCount();
        m_Records.assign(materialSlots, MaterialTextureRecord{});
        std::vector<bool> filled(materialSlots, false);
        const u32 instanceSlots = scene.GetInstanceSlotCount();
        for (u32 slot = 0; slot < instanceSlots; ++slot)
        {
            const GPUSceneInstance* instance = scene.GetLiveInstanceRecordBySlot(slot);
            if (instance == nullptr || instance->MaterialIndex >= materialSlots || filled[instance->MaterialIndex])
                continue;
            const GPUSceneMaterial* material =
                scene.GetLiveMaterialRecordBySlot(instance->MaterialIndex, instance->MaterialGeneration);
            if (material == nullptr)
                continue;
            filled[instance->MaterialIndex] = true;
            // Only the maps the material declares: the *_MAP flags follow the
            // handle's validity, and they are the whole gate the raster
            // GPU-scene path applies (PBR_GBuffer.glsl reads them, not the
            // legacy UseTextureMaps bit, which a PBR material need not set).
            MaterialTextureRecord& record = m_Records[instance->MaterialIndex];
            if ((material->Flags & GPUSceneMaterialFlagAlbedoMap) != 0u)
                record.Albedo = ResolveTexture(material->AlbedoTextureIndex, material->AlbedoTextureGeneration);
            if ((material->Flags & GPUSceneMaterialFlagMetallicRoughnessMap) != 0u)
                record.MetallicRoughness =
                    ResolveTexture(material->MetallicRoughnessTextureIndex, material->MetallicRoughnessTextureGeneration);
            if ((material->Flags & GPUSceneMaterialFlagNormalMap) != 0u)
                record.Normal = ResolveTexture(material->NormalTextureIndex, material->NormalTextureGeneration);
            if ((material->Flags & GPUSceneMaterialFlagEmissiveMap) != 0u)
                record.Emissive = ResolveTexture(material->EmissiveTextureIndex, material->EmissiveTextureGeneration);
        }
        if (m_Records.empty())
            return 0;

        const auto requiredBytes = static_cast<u32>(m_Records.size() * sizeof(MaterialTextureRecord));
        // Same write discipline as the emissive table: only when the bytes
        // changed, and then into a fresh buffer, because the previous frame's
        // draw reads the old one by address and an in-place write would race
        // it (EmissiveTriangleTable::EndFrame says why dropping the old Ref
        // is safe).
        const bool unchanged = m_Buffer && m_Uploaded.size() == m_Records.size() &&
                               std::memcmp(m_Uploaded.data(), m_Records.data(), requiredBytes) == 0;
        if (!unchanged)
        {
            const u32 capacityRecords = std::max<u32>(kMinimumRecordCapacity, static_cast<u32>(m_Records.size()));
            const auto capacityBytes = static_cast<u32>(capacityRecords * sizeof(MaterialTextureRecord));
            m_Buffer = StorageBuffer::Create(capacityBytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicDraw);
            if (!m_Buffer)
            {
                m_Uploaded.clear();
                m_UploadedCount = 0;
                return 0;
            }
            m_Buffer->SetData(m_Records.data(), requiredBytes);
            m_Uploaded = m_Records;
            m_ChangedThisFrame = true;
        }
        m_UploadedCount = static_cast<u32>(m_Records.size());
        return m_UploadedCount;
    }

    MaterialTextureRecord MaterialTextureTable::GetRecord(u32 materialSlot) const noexcept
    {
        if (materialSlot < m_Records.size())
            return m_Records[materialSlot];
        return MaterialTextureRecord{};
    }

    u64 MaterialTextureTable::GetDeviceAddress() const noexcept
    {
        if (!m_Buffer || m_UploadedCount == 0)
            return 0;
        return m_Buffer->GetDeviceAddress();
    }

    void MaterialTextureTable::Shutdown()
    {
        m_Gathering = false;
        m_Records.clear();
        m_Records.shrink_to_fit();
        m_Uploaded.clear();
        m_Uploaded.shrink_to_fit();
        m_ChangedThisFrame = false;
        m_SamplerOffset = RHI::HeapOffset::Invalid;
        m_SamplerResolved = false;
        m_Unresolved = 0;
        m_UploadedCount = 0;
        m_Buffer.Reset();
    }
} // namespace OloEngine
