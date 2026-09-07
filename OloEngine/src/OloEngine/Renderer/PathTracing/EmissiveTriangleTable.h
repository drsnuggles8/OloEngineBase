#pragma once

// =============================================================================
// EmissiveTriangleTable.h — the GPU path tracer's area-light sampling table
// (issue #1055).
//
// The CPU reference tracer samples emissive geometry uniformly BY AREA over
// every emissive triangle in the scene (ReferenceScene::BuildEmissiveList /
// SampleEmissive), which makes the light density one constant, 1 / total
// area, that the BSDF-hit side of the MIS weight can reuse without knowing
// which triangle it hit. The GPU tracer needs the same set, in the same
// form, on the device. This table is that set.
//
// WHY IT IS BUILT ON THE CPU. The GPU Scene records carry vertex data only as
// device addresses; the CPU never sees the triangles through them. The
// staging seam (Renderer3D::ExtractGPUSceneMesh) does have the MeshSource in
// hand — the same CPU vertex and index arrays the raster draw indexes — so
// the gather hangs off it. Deliberately not a GPU prefix-sum: the emissive
// set of a scene is small (a Cornell box has two triangles) and a CPU build is
// the thing a test can check against ReferenceScene::GetEmissiveTriangles()
// exactly.
//
// TWO PHASES, because of when the material records exist. Staging only
// QUEUES a material for commit; the record — the encoded EmissiveFactor and
// the TwoSided flag the shader will read — exists after
// GPUScene::EndExtraction. So ExtractGPUSceneMesh queues the submesh here,
// and EndFrame, called after the commit, resolves each queued submesh against
// the committed record and walks the triangles of the ones that emit. The
// table therefore describes exactly the emitters the shader shades, from the
// same bytes.
//
// THE RECORD IS THE GLSL STRUCT. Five vec4, uploaded verbatim; the shader
// reaches it through a device address in the ray-tracing UBO because the
// buffer-binding namespace is full (#978). The cumulative area fraction rides
// each record's NormalAndCdf.w, with the last entry forced to exactly 1 so a
// selection value of 1.0 cannot walk off the end.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <set>
#include <span>
#include <vector>

namespace OloEngine
{
    class GPUScene;
    class MeshSource;

    // Mirrors OloPtEmissiveTriangle in GpuPathTracer.glsl (std430, 80 bytes).
    struct alignas(16) EmissiveTriangleRecord
    {
        glm::vec4 V0{ 0.0f };               // xyz vertex 0, w = area
        glm::vec4 V1{ 0.0f };               // xyz vertex 1, w unused
        glm::vec4 V2{ 0.0f };               // xyz vertex 2, w unused
        glm::vec4 NormalAndCdf{ 0.0f };     // xyz winding normal, w = cumulative area fraction
        glm::vec4 RadianceAndFlags{ 0.0f }; // rgb radiance, w = 1 when two-sided
    };
    static_assert(sizeof(EmissiveTriangleRecord) == 80, "EmissiveTriangleRecord must match the 80-byte GLSL struct");

    class EmissiveTriangleTable
    {
      public:
        // Start a frame's gather. `renderOrigin` is the frame's render origin:
        // vertices are stored render-relative, like the GPU Scene transforms
        // the TLAS is built from, so a ray origin in that frame lands on them.
        // `gather` false makes every Queue call a no-op — the table costs
        // nothing while the tracer is off.
        void BeginFrame(const glm::vec3& renderOrigin, bool gather);

        // Whether THIS frame gathered — true from BeginFrame(gather = true)
        // until the next BeginFrame. The pass reads it after EndFrame: a
        // tracer that is live on a frame whose gather was decided off (the
        // setting flipped mid-frame) must not trace without its emitters.
        [[nodiscard]] bool IsGathering() const noexcept
        {
            return m_Gathering;
        }

        // Tell the table, before its submeshes are queued, that this material
        // emits. Called where the Material is in hand (the material
        // extraction); QueueSubmesh drops every key not noted, so the gather
        // costs O(emitting submeshes) rather than O(submeshes).
        void NoteEmissiveMaterial(const GPUSceneMaterialKey& materialKey);

        // Record a staged submesh for EndFrame to resolve. Cheap: a Ref copy
        // and a few POD fields for an emitting material, nothing for any other.
        // The triangles are walked at EndFrame against the COMMITTED material
        // record, so an emitter is one that emits on both paths.
        void QueueSubmesh(const Ref<MeshSource>& meshSource, u32 submeshIndex, const glm::mat4& worldTransform,
                          const GPUSceneMaterialKey& materialKey);

        // Close the frame AFTER the GPU Scene commit: resolve every queued
        // submesh against its committed material record, walk the emitting
        // ones, normalise the running area into the CDF and upload. Returns
        // the triangle count. Safe with nothing queued — the table then reports
        // zero triangles and a zero address, which the shader treats as "no
        // emitters".
        u32 EndFrame(const GPUScene& scene);

        [[nodiscard]] u32 GetTriangleCount() const noexcept
        {
            return static_cast<u32>(m_Records.size());
        }
        [[nodiscard]] f32 GetTotalArea() const noexcept
        {
            return m_TotalArea;
        }
        // 1 / total area, or 0 with no emitters — ReferenceScene::EmissivePdfArea's twin.
        [[nodiscard]] f32 GetPdfArea() const noexcept
        {
            return m_TotalArea > 0.0f ? 1.0f / m_TotalArea : 0.0f;
        }
        // The persistent buffer's device address after EndFrame; 0 on a
        // backend without buffer addresses or with nothing uploaded.
        [[nodiscard]] u64 GetDeviceAddress() const noexcept;

        void Shutdown();

        // The pure half, exposed for the headless test: append the triangles
        // of `indices[first, first + count)` (with `baseVertex` applied,
        // exactly as the raster draw does) that have a non-degenerate area,
        // transformed by `worldTransform` into the render-relative frame, and
        // return the running area. Records carry the raw running area in
        // NormalAndCdf.w until Finalize normalises it.
        static f32 AppendTriangles(std::span<const Vertex> vertices, std::span<const u32> indices, u32 firstIndex,
                                   u32 indexCount, i32 baseVertex, const glm::mat4& worldTransform,
                                   const glm::vec3& renderOrigin, const glm::vec3& radiance, bool twoSided,
                                   f32 runningArea, std::vector<EmissiveTriangleRecord>& out);
        // Turn the running area sums into cumulative fractions; the last
        // entry is forced to exactly 1.
        static void Finalize(std::vector<EmissiveTriangleRecord>& records, f32 totalArea);

      private:
        struct PendingSubmesh
        {
            Ref<MeshSource> m_MeshSource;
            u32 m_SubmeshIndex = 0;
            glm::mat4 m_WorldTransform{ 1.0f };
            GPUSceneMaterialKey m_MaterialKey{};
        };

        std::vector<PendingSubmesh> m_Pending;
        std::set<GPUSceneMaterialKey> m_EmissiveMaterials;
        std::vector<EmissiveTriangleRecord> m_Records;
        // What the device buffer holds, so a frame whose table is byte-identical
        // to the last one (the common case: a static scene converging) issues
        // no write. The write would race the previous frame's in-flight draw,
        // which reads the persistent buffer by device address.
        std::vector<EmissiveTriangleRecord> m_Uploaded;
        glm::vec3 m_RenderOrigin{ 0.0f };
        f32 m_TotalArea = 0.0f;
        u32 m_UploadedCount = 0;
        bool m_Gathering = false;
        Ref<StorageBuffer> m_Buffer;
    };
} // namespace OloEngine
