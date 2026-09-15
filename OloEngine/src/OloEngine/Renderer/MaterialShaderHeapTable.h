#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <glm/glm.hpp>
#include <cstddef>
#include <vector>

namespace OloEngine
{
    class GPUScene;

    // std430 twin of OloMaterialShaderHeapRecord. Byte offsets belong to the
    // Vulkan backend heap, not GPUSceneMaterial's engine-heap slot fields.
    struct alignas(16) MaterialShaderHeapRecord
    {
        glm::uvec4 Textures{ 0u }; // albedo, metallic-roughness, normal, AO
        u32 Emissive = 0u;
        u32 Generation = 0u;
        u32 Flags = 0u;
        u32 Pad0 = 0u;
    };
    static_assert(sizeof(MaterialShaderHeapRecord) == 32);
    static_assert(offsetof(MaterialShaderHeapRecord, Emissive) == 16);
    static_assert(offsetof(MaterialShaderHeapRecord, Generation) == 20);
    static_assert(offsetof(MaterialShaderHeapRecord, Flags) == 24);

    // Re-resolved at scene commit, before command recording. Changed data gets
    // a fresh buffer so previously submitted draws keep their own snapshot.
    class MaterialShaderHeapTable
    {
      public:
        void Update(const GPUScene& scene);
        void Shutdown();

        // PBRMaterialUBO HeapOffsets[1].yzw on Vulkan: address low/high, count.
        [[nodiscard]] glm::uvec3 GetAddressAndCount() const;
        [[nodiscard]] u32 GetUnresolvedCount() const
        {
            return m_Unresolved;
        }
        [[nodiscard]] bool HasGPUResources() const
        {
            return m_Buffer != nullptr;
        }

      private:
        Ref<StorageBuffer> m_Buffer;
        std::vector<MaterialShaderHeapRecord> m_Uploaded;
        u32 m_Unresolved = 0u;
    };
} // namespace OloEngine
