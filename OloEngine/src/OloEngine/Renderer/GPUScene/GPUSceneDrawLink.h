#pragma once

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <glm/glm.hpp>

#include <limits>

// The seam between GPU Scene extraction and the raster consumer (issue #994).
//
// Extraction and submission are interleaved: Scene walks the registry once and,
// per submesh, stages a GPU Scene instance AND builds the draw packet. Slots are
// not assigned until EndExtraction commits in key order, so at submit time the
// draw cannot know which record it will become. It records a LINK INDEX instead
// — a plain position in this frame's link table — and the table is resolved in
// one pass immediately after the commit, before any pass configuration can look
// at the buffers.
//
// That ordering is the ownership boundary the epic asks for, stated as code:
//
//   BeginGPUSceneExtraction  -> the frame's link table is cleared
//   ExtractGPUSceneMesh      -> stages a record, appends its key, returns an index
//   DrawMesh                 -> carries that index in the command packet
//   EndScene: EndExtraction  -> slots and generations become final
//            Upload
//            ResolveGPUSceneDrawLinks -> every link gets its record, once
//   command dispatch         -> reads resolved links only, never the registry
//
// A consumer therefore never reads a staged record, and never resolves a key
// per draw. A link that does not resolve (its record was rejected at extraction,
// or retired between staging and commit) stays Unresolved, and the draw falls
// back to exactly the data it used before the migration. Geometry never
// disappears because its link is missing.
namespace OloEngine
{
    inline constexpr u32 GPUSceneDrawLinkNone = std::numeric_limits<u32>::max();

    // Whether an extraction call wants a link back. A caller that cannot
    // consume one — the explicitly-instanced path, which draws N sources in one
    // instanced call and has no per-instance link lane — says so, and no link
    // is staged. Otherwise a 20k-instance scatter scene would append 20k links
    // per frame and resolve every one of them for nothing.
    enum class GPUSceneDrawLinkRequest
    {
        None,
        Link,
    };

    // One frame's link: the key staged at submit time, and — after
    // ResolveGPUSceneDrawLinks — the committed record it names.
    struct GPUSceneDrawLink
    {
        GPUSceneInstanceKey m_InstanceKey;

        // Resolved half. m_Resolved is the only thing a consumer may test; the
        // transforms are render-origin-relative because the record encodes them
        // that way, so a consumer must NOT shift them again.
        bool m_Resolved = false;
        GPUSceneHandle m_Instance;
        GPUSceneHandle m_Material;
        glm::mat4 m_CurrentTransform{ 1.0f };
        glm::mat4 m_PreviousTransform{ 1.0f };

        // The value InstanceData::GPUSceneRef carries to the shader:
        // (instance slot, instance generation, material slot, material
        // generation). Both generation lanes are zero while unresolved, which
        // is the invalid generation, so a shader cannot read an unlinked draw
        // as a linked one.
        [[nodiscard]] glm::uvec4 Ref() const
        {
            return m_Resolved ? glm::uvec4(m_Instance.m_Index, m_Instance.m_Generation, m_Material.m_Index,
                                           m_Material.m_Generation)
                              : glm::uvec4(0u);
        }
    };

    // A GPUSceneTransform is three affine ROWS of the render-relative matrix
    // (GPUScene.cpp, EncodeTransform); the fourth row is the invariant
    // (0,0,0,1). This is the one place the rows are turned back into the
    // column-major glm matrix the raster path uploads, so a transposition
    // mistake has exactly one home.
    [[nodiscard]] inline glm::mat4 DecodeGPUSceneTransform(const GPUSceneTransform& transform)
    {
        glm::mat4 matrix(1.0f);
        matrix[0] = glm::vec4(transform.Row0.x, transform.Row1.x, transform.Row2.x, 0.0f);
        matrix[1] = glm::vec4(transform.Row0.y, transform.Row1.y, transform.Row2.y, 0.0f);
        matrix[2] = glm::vec4(transform.Row0.z, transform.Row1.z, transform.Row2.z, 0.0f);
        matrix[3] = glm::vec4(transform.Row0.w, transform.Row1.w, transform.Row2.w, 1.0f);
        return matrix;
    }
} // namespace OloEngine
