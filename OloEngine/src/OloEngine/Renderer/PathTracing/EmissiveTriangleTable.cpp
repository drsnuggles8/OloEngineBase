#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PathTracing/EmissiveTriangleTable.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MeshSource.h"

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

    f32 EmissiveTriangleTable::AppendTriangles(std::span<const Vertex> vertices, std::span<const u32> indices,
                                               u32 firstIndex, u32 indexCount, i32 baseVertex,
                                               const glm::mat4& worldTransform, const glm::vec3& renderOrigin,
                                               const glm::vec3& radiance, bool twoSided, f32 runningArea,
                                               std::vector<EmissiveTriangleRecord>& out)
    {
        const sizet indexEnd = static_cast<sizet>(firstIndex) + indexCount;
        if (indexEnd > indices.size() || indexCount < 3u)
            return runningArea;

        const sizet vertexCount = vertices.size();
        const auto resolveVertex = [&](sizet indexSlot, glm::vec3& outPosition) -> bool
        {
            const i64 vertexIndex = static_cast<i64>(indices[indexSlot]) + baseVertex;
            if (vertexIndex < 0 || static_cast<sizet>(vertexIndex) >= vertexCount)
                return false;
            const glm::vec3 local = vertices[static_cast<sizet>(vertexIndex)].Position;
            outPosition = glm::vec3(worldTransform * glm::vec4(local, 1.0f)) - renderOrigin;
            return true;
        };

        const sizet triangleCount = static_cast<sizet>(indexCount) / 3u;
        for (sizet triangle = 0; triangle < triangleCount; ++triangle)
        {
            const sizet base = static_cast<sizet>(firstIndex) + triangle * 3u;
            glm::vec3 p0, p1, p2;
            if (!resolveVertex(base + 0u, p0) || !resolveVertex(base + 1u, p1) || !resolveVertex(base + 2u, p2))
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
            record.V1 = glm::vec4(p1, 0.0f);
            record.V2 = glm::vec4(p2, 0.0f);
            record.NormalAndCdf = glm::vec4(crossProduct / crossLength, runningArea);
            record.RadianceAndFlags = glm::vec4(radiance, twoSided ? 1.0f : 0.0f);
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

    u32 EmissiveTriangleTable::EndFrame(const GPUScene& scene)
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

            const MeshSource& source = *pending.m_MeshSource;
            const auto& submeshes = source.GetSubmeshes();
            if (pending.m_SubmeshIndex >= static_cast<u32>(submeshes.Num()))
                continue;
            const Submesh& submesh = submeshes[static_cast<i32>(pending.m_SubmeshIndex)];
            const auto& vertices = source.GetVertices();
            const auto& indices = source.GetIndices();

            m_TotalArea = AppendTriangles(
                std::span<const Vertex>(vertices.GetData(), static_cast<sizet>(vertices.Num())),
                std::span<const u32>(indices.GetData(), static_cast<sizet>(indices.Num())), submesh.m_BaseIndex,
                submesh.m_IndexCount, static_cast<i32>(submesh.m_BaseVertex), pending.m_WorldTransform,
                m_RenderOrigin, radiance, twoSided, m_TotalArea, m_Records);
        }
        m_Pending.clear();

        Finalize(m_Records, m_TotalArea);
        if (m_Records.empty())
            return 0;

        const auto requiredBytes = static_cast<u32>(m_Records.size() * sizeof(EmissiveTriangleRecord));
        if (!m_Buffer || m_Buffer->GetSize() < requiredBytes)
        {
            // Grow geometrically so a scene that adds emitters one at a time
            // does not re-create the buffer every frame. Resize mints a new
            // device address, which is why the pass resolves it every frame.
            const u32 capacityRecords =
                std::max<u32>(kMinimumRecordCapacity, static_cast<u32>(m_Records.size()) * 2u);
            const auto capacityBytes = static_cast<u32>(capacityRecords * sizeof(EmissiveTriangleRecord));
            if (m_Buffer)
                m_Buffer->Resize(capacityBytes);
            else
            {
                // By device address only, never by slot: the shader reaches it
                // through GL_EXT_buffer_reference, so it publishes nowhere.
                m_Buffer = StorageBuffer::Create(capacityBytes, StorageBuffer::kNoBinding,
                                                 StorageBufferUsage::DynamicDraw);
            }
            if (!m_Buffer)
                return 0;
            // A new or resized buffer holds nothing the records could match.
            m_Uploaded.clear();
        }

        // Written only when the bytes changed. In a static scene the table is
        // identical every frame, and a write would race the previous frame's
        // draw, which is still reading the persistent buffer by address (the
        // in-flight caveat in VulkanStorageBuffer::SetData). When the bytes DO
        // change, the GPU Scene commit that changed them also reported a dirty
        // range, and the frame whose draw could read a torn record is the one
        // the SceneMutated invalidation discards.
        const bool unchanged = m_Uploaded.size() == m_Records.size() &&
                               std::memcmp(m_Uploaded.data(), m_Records.data(), requiredBytes) == 0;
        if (!unchanged)
        {
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
