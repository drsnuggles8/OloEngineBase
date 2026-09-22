#include "OloEnginePCH.h"
#include "OloEngine/Renderer/MaterialShaderHeapTable.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include <cstring>
#include <limits>

namespace OloEngine
{
    void MaterialShaderHeapTable::Update(const GPUScene& scene)
    {
        OLO_PROFILE_FUNCTION();
        m_Unresolved = 0u;
        if (!HeapBinding::ShaderHeapIndexingSupported())
        {
            Shutdown();
            return;
        }

        const auto nullTexture = HeapBinding::ResolveShaderHeapNullTexture();
        const auto sampler = HeapBinding::ResolveShaderHeapSampler(HeapBinding::MaterialTexture2DSampler());
        const u32 count = scene.GetMaterialSlotCount();
        if (!nullTexture.IsValid() || !sampler.IsValid() || count == 0u ||
            count > std::numeric_limits<u32>::max() / sizeof(MaterialShaderHeapRecord))
        {
            Shutdown();
            return;
        }

        TArray<MaterialShaderHeapRecord> records;
        records.SetNum(static_cast<i32>(count));
        const u32 instanceCount = scene.GetInstanceSlotCount();
        for (u32 slot = 0; slot < instanceCount; ++slot)
        {
            const auto* instance = scene.GetLiveInstanceRecordBySlot(slot);
            if (!instance || instance->MaterialIndex >= count || records[instance->MaterialIndex].Generation != 0u)
                continue;
            const auto* material = scene.GetLiveMaterialRecordBySlot(instance->MaterialIndex, instance->MaterialGeneration);
            if (!material)
                continue;

            auto& record = records[instance->MaterialIndex];
            record.Generation = material->Generation;
            record.Flags = material->Flags;
            const auto resolve = [&](u32 index, u32 generation, u32 flag)
            {
                if ((record.Flags & flag) == 0u)
                    return nullTexture.Value;
                const auto offset = HeapBinding::ResolveShaderHeapTexture({ index, generation });
                if (offset.IsValid())
                    return offset.Value;
                ++m_Unresolved;
                record.Flags &= ~flag;
                return nullTexture.Value;
            };
            record.Textures = {
                resolve(material->AlbedoTextureIndex, material->AlbedoTextureGeneration, GPUSceneMaterialFlagAlbedoMap),
                resolve(material->MetallicRoughnessTextureIndex, material->MetallicRoughnessTextureGeneration, GPUSceneMaterialFlagMetallicRoughnessMap),
                resolve(material->NormalTextureIndex, material->NormalTextureGeneration, GPUSceneMaterialFlagNormalMap),
                resolve(material->OcclusionTextureIndex, material->OcclusionTextureGeneration, GPUSceneMaterialFlagOcclusionMap)
            };
            record.Emissive = resolve(material->EmissiveTextureIndex, material->EmissiveTextureGeneration, GPUSceneMaterialFlagEmissiveMap);
        }

        if (m_Unresolved > 0u)
            OLO_CORE_WARN("MaterialShaderHeapTable: {} material maps could not resolve; using their factors", m_Unresolved);

        const auto bytes = static_cast<u32>(records.Num() * sizeof(MaterialShaderHeapRecord));
        if (m_Buffer && records.Num() == m_Uploaded.Num() && std::memcmp(records.GetData(), m_Uploaded.GetData(), bytes) == 0)
            return;

        auto buffer = StorageBuffer::Create(bytes, StorageBuffer::kNoBinding);
        if (!buffer)
        {
            Shutdown();
            return;
        }
        buffer->SetData(records.GetData(), bytes);
        m_Buffer = std::move(buffer);
        m_Uploaded = std::move(records);
    }

    glm::uvec3 MaterialShaderHeapTable::GetAddressAndCount() const
    {
        if (!m_Buffer)
            return glm::uvec3(0u);
        const u64 address = m_Buffer->GetDeviceAddress();
        if (address == 0u)
            return glm::uvec3(0u);
        return { static_cast<u32>(address), static_cast<u32>(address >> 32u), static_cast<u32>(m_Uploaded.Num()) };
    }

    void MaterialShaderHeapTable::Shutdown()
    {
        m_Buffer.Reset();
        m_Uploaded.Reset();
        m_Unresolved = 0u;
    }
} // namespace OloEngine
