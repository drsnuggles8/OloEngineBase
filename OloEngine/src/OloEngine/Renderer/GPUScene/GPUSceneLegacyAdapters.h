#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <array>
#include <optional>
#include <string_view>

// The anti-duplication ratchet's allowlist (issue #994).
//
// One raster path — the classic ordinary-mesh path, Scene::SubmitMeshSourceClassic
// through Renderer3D::DrawMesh — renders through the canonical GPU Scene
// records. Every OTHER path still assembles its own copy of the two things the
// records own: a draw's world transform and its previous-frame transform. That
// is the duplication the epic exists to stop spreading, so each remaining site
// is NAMED here, with what it is and what has to happen before it can go.
//
// GPUSceneAntiDuplicationRatchetTest enforces the list in both directions:
//
//   * a source file that assembles previous-frame transform truth and is NOT
//     on this list fails the test. Adding one is then a deliberate act — you
//     either migrate the path or write down why it is still an adapter.
//   * an entry whose file no longer duplicates anything fails too, so the list
//     shrinks as paths migrate instead of accumulating dead entries.
//
// The markers the test scans for are the mechanism, not the type name: the
// per-entity previous-transform cache (Renderer3D::GetAndRecordPrevTransform
// and its instanced sibling) and any assignment into a PrevTransform /
// prevTransform lane. A path that invents a third way to carry previous-frame
// truth is a new mechanism and belongs in the marker list, not around it.
//
// Diagnostics, separately: an adapter that submits geometry the records cannot
// represent reports its GPUSceneUnsupportedCategory, so the geometry shows up
// in the renderer profiler instead of vanishing from the scene's inventory. An
// adapter with no category submits geometry that IS extracted — it just draws
// it without consuming the link.
namespace OloEngine::GPUSceneLegacyAdapters
{
    struct Adapter
    {
        // Repo-relative source path, forward slashes.
        std::string_view m_File;
        // What the path is, in the vocabulary the renderer uses elsewhere.
        std::string_view m_Name;
        // The diagnostics category this path reports for geometry the records
        // cannot represent, or nullopt when it submits extractable geometry
        // and merely does not consume the link yet.
        std::optional<GPUSceneUnsupportedCategory> m_UnsupportedCategory;
        // What has to be true before the entry can be deleted.
        std::string_view m_Exit;
    };

    inline constexpr std::array kAdapters{
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Renderer3D.h",
            .m_Name = "Per-entity previous-transform cache",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "The cache itself. It is keyed on entity id, so a multi-submesh mesh "
                      "overwrites its own history between submeshes; the instance record is "
                      "keyed per (entity, geometry, submesh) and does not. Deleted once every "
                      "caller below consumes a draw link instead.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Renderer3DMeshSubmission.cpp",
            .m_Name = "Renderer3D submission (skinned, parallel, instanced, quad)",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "DrawMesh already takes a link. The skinned and parallel-worker variants "
                      "need the same parameter, and DrawMeshInstanced needs a per-instance link "
                      "lane in InstanceData before N sources can each name their own record. The "
                      "same lane is what CommandBucket's auto-batching needs: it collapses N "
                      "linked draws into one instanced call and drops their links today, so a "
                      "batched draw falls back even though its link resolved.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Commands/CommandDispatch.cpp",
            .m_Name = "Single-instance upload choke point",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "Half migrated: a draw with a resolved link takes the record's transforms, "
                      "everything else keeps the camera-relative shift. The fallback branch goes "
                      "when every submission path carries a link.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Instancing/GPUFrustumCuller.cpp",
            .m_Name = "GPU per-instance frustum cull",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "The cull compute rewrites the InstanceData stream, so it must preserve the "
                      "link lane once instanced draws carry one.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Occlusion/OcclusionCuller.cpp",
            .m_Name = "Occlusion proxy draws",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "Proxy boxes are render-side geometry with no authored identity; they alias "
                      "prev to current on purpose. Likely stays an adapter.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Passes/ShadowRenderPass.cpp",
            .m_Name = "Shadow caster submission",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "Shadow casters have no motion-vector consumer, so they alias prev to "
                      "current. They can read the record's current transform once the caster list "
                      "carries links.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/Renderer3DUtilityDraws.cpp",
            .m_Name = "Debug visualisation draws",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "Debug geometry is not authored scene content and has no record. Stays an "
                      "adapter.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.cpp",
            .m_Name = "Virtualized geometry (cluster-LOD DAG)",
            .m_UnsupportedCategory = GPUSceneUnsupportedCategory::Virtualized,
            .m_Exit = "Virtual geometry has no per-submesh triangle identity in the records yet; "
                      "#977 defines the policy and hook, a later issue gives it a representation.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Terrain/Foliage/FoliageRenderer.cpp",
            .m_Name = "Foliage instancing",
            .m_UnsupportedCategory = GPUSceneUnsupportedCategory::Foliage,
            .m_Exit = "Foliage instancing rides its own vertex stream (OLO_INSTANCE_SINGLE) and "
                      "never indexes the instance SSBO per instance.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Asset/InstancePlacementSerializer.cpp",
            .m_Name = "Baked instance placement asset",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "Deserialisation seeds prev from current for a freshly loaded placement, "
                      "which is the correct 'no history' start. Stays an adapter.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp",
            .m_Name = "C# instance authoring",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "A script that adds an instance seeds prev from current so the new instance "
                      "starts static. Stays an adapter.",
        },
        Adapter{
            .m_File = "OloEngine/src/OloEngine/Scripting/Lua/LuaScriptGlue.cpp",
            .m_Name = "Lua instance authoring",
            .m_UnsupportedCategory = std::nullopt,
            .m_Exit = "Same as the C# glue: seeding prev from current is the correct no-history "
                      "start for an instance a script just created.",
        },
    };
} // namespace OloEngine::GPUSceneLegacyAdapters
