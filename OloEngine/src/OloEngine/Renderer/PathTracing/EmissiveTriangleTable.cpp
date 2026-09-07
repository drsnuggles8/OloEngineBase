#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PathTracing/MaterialTextureTable.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kMinimumRecordCapacity = 16u;
    } // namespace

    void EmissiveTriangleTable::BeginFrame(const glm::vec3& renderOrigin, bool gather)
    {
        m_Pending.clear();
        m_EmissiveMaterials.clear();
        m_Records.clear();
        m_RenderOrigin = renderOrigin;
        m_TotalArea = 0.0f;
        m_Gathering = gather;
    }

    void EmissiveTriangleTable::NoteEmissiveMaterial(const GPUSceneMaterialKey& materialKey)
    {
        if (!m_Gathering)
            return;
        m_EmissiveMaterials.insert(materialKey);
    }

    void EmissiveTriangleTable::QueueSubmesh(const Ref<MeshSource>& meshSource, u32 submeshIndex,
                                             const glm::mat4& worldTransform, const GPUSceneMaterialKey& materialKey)
    {
        if (!m_Gathering || !meshSource || !m_EmissiveMaterials.contains(materialKey))
            return;
        m_Pending.push_back(PendingSubmesh{
            .m_MeshSource = meshSource,
            .m_SubmeshIndex = submeshIndex,
            .m_WorldTransform = worldTransform,
            .m_MaterialKey = materialKey,
        });
    }

    f32 EmissiveTriangleTable::AppendTriangles(const TriangleRange& range, const Emitter& emitter, f32 runningArea,
                                               std::vector<EmissiveTriangleRecord>& out)
    {
        const std::span<const Vertex> vertices = range.Vertices;
        const std::span<const u32> indices = range.Indices;
        const u32 firstIndex = range.FirstIndex;
        const u32 indexCount = range.IndexCount;
        const i32 baseVertex = range.BaseVertex;
        const glm::mat4& worldTransform = emitter.WorldTransform;
        const glm::vec3& renderOrigin = emitter.RenderOrigin;
        const glm::vec3& radiance = emitter.Radiance;
        const bool twoSided = emitter.TwoSided;
        const u32 emissiveTexture = emitter.EmissiveTexture;

        const sizet indexEnd = static_cast<sizet>(firstIndex) + indexCount;
        if (indexEnd > indices.size() || indexCount < 3u)
            return runningArea;

        const sizet vertexCount = vertices.size();
        const auto resolveVertex = [&](sizet indexSlot, glm::vec3& outPosition, glm::vec2& outUv) -> bool
        {
            const i64 vertexIndex = static_cast<i64>(indices[indexSlot]) + baseVertex;
            if (vertexIndex < 0 || static_cast<sizet>(vertexIndex) >= vertexCount)
                return false;
            const Vertex& vertex = vertices[static_cast<sizet>(vertexIndex)];
            outPosition = glm::vec3(worldTransform * glm::vec4(vertex.Position, 1.0f)) - renderOrigin;
            outUv = vertex.TexCoord;
            return true;
        };

        const sizet triangleCount = static_cast<sizet>(indexCount) / 3u;
        for (sizet triangle = 0; triangle < triangleCount; ++triangle)
        {
            const sizet base = static_cast<sizet>(firstIndex) + triangle * 3u;
            glm::vec3 p0, p1, p2;
            glm::vec2 uv0, uv1, uv2;
            if (!resolveVertex(base + 0u, p0, uv0) || !resolveVertex(base + 1u, p1, uv1) ||
                !resolveVertex(base + 2u, p2, uv2))
                continue;
            if (!std::isfinite(p0.x + p0.y + p0.z + p1.x + p1.y + p1.z + p2.x + p2.y + p2.z))
                continue;

            // The same winding cross product ReferenceScene::BuildEmissiveList
            // takes, so the two tables hold the same normals and the same
            // areas for the same triangles.
            const glm::vec3 crossProduct = glm::cross(p1 - p0, p2 - p0);
            const f32 crossLength = glm::length(crossProduct);
            if (!(crossLength > 1e-12f))
                continue; // degenerate

            const f32 area = 0.5f * crossLength;
            runningArea += area;

            EmissiveTriangleRecord record;
            record.V0 = glm::vec4(p0, area);
            record.V1 = glm::vec4(p1, uv2.x);
            record.V2 = glm::vec4(p2, uv2.y);
            record.NormalAndCdf = glm::vec4(crossProduct / crossLength, runningArea);
            record.RadianceAndFlags = glm::vec4(radiance, twoSided ? 1.0f : 0.0f);
            record.Uv01 = glm::vec4(uv0, uv1);
            record.Texture = glm::uvec4(emissiveTexture, 0u, 0u, 0u);
            out.push_back(record);
        }
        return runningArea;
    }

    void EmissiveTriangleTable::Finalize(std::vector<EmissiveTriangleRecord>& records, f32 totalArea)
    {
        if (records.empty())
            return;
        const f32 invTotal = totalArea > 0.0f ? 1.0f / totalArea : 0.0f;
        for (auto& record : records)
            record.NormalAndCdf.w *= invTotal;
        // Forced exactly, as the CPU reference does: rounding can leave the
        // last running fraction a few ulps under 1, and a selection value in
        // that gap would otherwise index past the end.
        records.back().NormalAndCdf.w = 1.0f;
    }

    u32 EmissiveTriangleTable::EndFrame(const GPUScene& scene, const MaterialTextureTable& textures)
    {
        m_Records.clear();
        m_TotalArea = 0.0f;
        m_UploadedCount = 0;

        for (const PendingSubmesh& pending : m_Pending)
        {
            const GPUSceneHandle handle = scene.FindMaterial(pending.m_MaterialKey);
            const GPUSceneMaterial* material = scene.IsMaterialHandleLive(handle) ? scene.GetMaterialRecord(handle) : nullptr;
            if (material == nullptr || (material->Flags & GPUSceneMaterialFlagActive) == 0u)
                continue;
            // The record's own emissive factor: the same bytes the shader reads
            // for a direct hit, so an emitter is one that emits on BOTH paths.
            const glm::vec3 radiance(material->EmissiveFactor);
            if (!(std::max({ radiance.x, radiance.y, radiance.z }) > 0.0f))
                continue;
            const bool twoSided = (material->Flags & GPUSceneMaterialFlagTwoSided) != 0u;
            // The emissive map the hit path multiplies the factor by, so NEE
            // samples the same emitter. Invalid where the material has none
            // or the backend cannot index the heap.
            const u32 emissiveTexture = textures.GetRecord(handle.m_Index).Emissive;

            const MeshSource& source = *pending.m_MeshSource;
            const auto& submeshes = source.GetSubmeshes();
            if (pending.m_SubmeshIndex >= static_cast<u32>(submeshes.Num()))
                continue;
            const Submesh& submesh = submeshes[static_cast<i32>(pending.m_SubmeshIndex)];
            const auto& vertices = source.GetVertices();
            const auto& indices = source.GetIndices();

            m_TotalArea = AppendTriangles(
                TriangleRange{
                    .Vertices = std::span<const Vertex>(vertices.GetData(), static_cast<sizet>(vertices.Num())),
                    .Indices = std::span<const u32>(indices.GetData(), static_cast<sizet>(indices.Num())),
                    .FirstIndex = submesh.m_BaseIndex,
                    .IndexCount = submesh.m_IndexCount,
                    .BaseVertex = static_cast<i32>(submesh.m_BaseVertex),
                },
                Emitter{
                    .WorldTransform = pending.m_WorldTransform,
                    .RenderOrigin = m_RenderOrigin,
                    .Radiance = radiance,
                    .TwoSided = twoSided,
                    .EmissiveTexture = emissiveTexture,
                },
                m_TotalArea, m_Records);
        }
        m_Pending.clear();

        Finalize(m_Records, m_TotalArea);
        if (m_Records.empty())
            return 0;

        const auto requiredBytes = static_cast<u32>(m_Records.size() * sizeof(EmissiveTriangleRecord));

        // Written only when the bytes changed, and then into a FRESH buffer.
        // The previous frame's draw may still be reading the old allocation
        // by device address, and a SetData on Vulkan writes that persistent
        // allocation in place (the in-flight caveat in VulkanStorageBuffer::
        // SetData; the snapshot mechanism serves bound SSBOs, not addresses).
        // Dropping the old Ref hands its allocation to the backend's deferred
        // reclaim, which destroys it only once the GPU is past every frame
        // that could reference it, so the old address stays valid for the
        // frame that holds it and the new one is written before any use. A
        // new address every change is why the pass resolves it every frame.
        // In a static scene the table is identical every frame and nothing is
        // allocated or written.
        const bool unchanged = m_Buffer && m_Uploaded.size() == m_Records.size() &&
                               std::memcmp(m_Uploaded.data(), m_Records.data(), requiredBytes) == 0;
        if (!unchanged)
        {
            const u32 capacityRecords = std::max<u32>(kMinimumRecordCapacity, static_cast<u32>(m_Records.size()));
            const auto capacityBytes = static_cast<u32>(capacityRecords * sizeof(EmissiveTriangleRecord));
            // By device address only, never by slot: the shader reaches it
            // through GL_EXT_buffer_reference, so it publishes nowhere.
            m_Buffer = StorageBuffer::Create(capacityBytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicDraw);
            if (!m_Buffer)
            {
                m_Uploaded.clear();
                m_UploadedCount = 0;
                return 0;
            }
            m_Buffer->SetData(m_Records.data(), requiredBytes);
            m_Uploaded = m_Records;
        }
        m_UploadedCount = static_cast<u32>(m_Records.size());
        return m_UploadedCount;
    }

    u64 EmissiveTriangleTable::GetDeviceAddress() const noexcept
    {
        if (!m_Buffer || m_UploadedCount == 0)
            return 0;
        return m_Buffer->GetDeviceAddress();
    }

    void EmissiveTriangleTable::Shutdown()
    {
        m_Pending.clear();
        m_Pending.shrink_to_fit();
        m_EmissiveMaterials.clear();
        m_Records.clear();
        m_Records.shrink_to_fit();
        m_Uploaded.clear();
        m_Uploaded.shrink_to_fit();
        m_TotalArea = 0.0f;
        m_UploadedCount = 0;
        m_Gathering = false;
        m_Buffer.Reset();
    }
} // namespace OloEngine
