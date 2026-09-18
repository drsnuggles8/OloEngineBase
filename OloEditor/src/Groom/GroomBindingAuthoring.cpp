#include "OloEnginePCH.h"
#include "Groom/GroomBindingAuthoring.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBindingCooker.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <cstddef>
#include <format>
#include <fstream>
#include <vector>

namespace OloEngine::GroomAuthoring
{
    ResolvedTarget ResolveTarget(Entity targetEntity)
    {
        ResolvedTarget target;
        if (!targetEntity || !targetEntity.HasComponent<MeshComponent>())
        {
            return target;
        }
        const auto* lodGroup = targetEntity.HasComponent<LODGroupComponent>()
                                   ? &targetEntity.GetComponent<LODGroupComponent>()
                                   : nullptr;
        target.m_Surface = Scene::ResolveAnimatedSurface(lodGroup, targetEntity.GetComponent<MeshComponent>());
        target.m_Skeleton = Scene::ResolveSurfaceSkeleton(targetEntity.HasComponent<SkeletonComponent>()
                                                              ? &targetEntity.GetComponent<SkeletonComponent>()
                                                              : nullptr,
                                                          target.m_Surface);
        return target;
    }

    GroomSurfaceView MakeSurfaceView(const MeshSource& surface, const Skeleton* skeleton)
    {
        GroomSurfaceView view;
        const auto& vertices = surface.GetVertices();
        const auto& indices = surface.GetIndices();
        if (vertices.IsEmpty() || indices.IsEmpty())
        {
            return view;
        }

        static_assert(offsetof(Vertex, Position) == 0,
                      "GroomSurfaceView addresses positions at the start of a Vertex");
        view.PositionData = reinterpret_cast<const std::byte*>(vertices.GetData());
        view.PositionStride = static_cast<u32>(sizeof(Vertex));
        view.VertexCount = static_cast<u32>(vertices.Num());
        view.Indices = indices.GetData();
        view.IndexCount = static_cast<u32>(indices.Num());
        view.BoneCount = skeleton != nullptr ? static_cast<u32>(skeleton->m_FinalBoneMatrices.size()) : 0u;
        view.SkeletonNameHash = skeleton != nullptr ? HashGroomSkeletonNames(skeleton->m_BoneNames) : 0u;
        return view;
    }

    glm::mat4 SurfaceToGroomMatrix(Scene& scene, Entity groomEntity, Entity targetEntity)
    {
        if (!groomEntity || !targetEntity || groomEntity == targetEntity)
        {
            return glm::mat4(1.0f);
        }
        // The one implementation, shared with the runtime (GroomSurfaceFrame.h):
        // a binder and a deformer that disagreed about this matrix by so much as
        // a scale factor would cook a coat the renderer then draws in the wrong
        // place, with every signature still matching.
        return MakeGroomSurfaceToGroomMatrix(scene.GetWorldTransform(groomEntity),
                                             scene.GetWorldTransform(targetEntity));
    }

    BindOutcome BuildAndImportBinding(Scene& scene, Entity groomEntity, Entity targetEntity, f32 searchRadius)
    {
        BindOutcome outcome;

        if (!groomEntity || !groomEntity.HasComponent<GroomComponent>())
        {
            outcome.m_Reason = "the entity has no Groom component.";
            return outcome;
        }
        const auto& groomComponent = groomEntity.GetComponent<GroomComponent>();
        Ref<GroomAsset> groom = AssetManager::GetAsset<GroomAsset>(groomComponent.m_Groom);
        if (!groom)
        {
            outcome.m_Reason = "the entity's Groom component has no loaded groom asset.";
            return outcome;
        }

        const ResolvedTarget target = ResolveTarget(targetEntity);
        if (!target)
        {
            outcome.m_Reason = "the target entity has no mesh to bind to, or its source is not loaded.";
            return outcome;
        }

        const GroomSurfaceView view = MakeSurfaceView(*target.m_Surface, target.m_Skeleton);
        if (!view.IsUsable())
        {
            outcome.m_Reason = std::format("the target mesh has {} vertices and {} indices, which is not a surface.",
                                           view.VertexCount, view.IndexCount);
            return outcome;
        }

        GroomBindingBuildSettings settings;
        settings.SearchRadius = searchRadius;
        settings.SurfaceToGroom = SurfaceToGroomMatrix(scene, groomEntity, targetEntity);

        // The target's own NAME is what the file records as its source, not a
        // path: a MeshSource reached through a MeshComponent may have been built
        // at runtime and have no file at all, and an absolute path would make
        // two machines cook different bytes.
        const std::string targetSourceName = targetEntity.GetName();

        std::vector<u8> bytes;
        Ref<GroomBindingAsset> binding;
        if (!GroomBindingCooker::CookPair(*groom, view, targetSourceName, settings, bytes, binding,
                                          outcome.m_Stats, outcome.m_Reason))
        {
            return outcome;
        }

        auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
        if (!assetManager)
        {
            outcome.m_Reason = "no editor asset manager, so the cooked binding has nowhere to go.";
            return outcome;
        }

        // Written NEXT TO THE GROOM rather than into a bindings folder, so the
        // pair travels together in the content browser.
        std::filesystem::path relativeDirectory = "Grooms";
        if (const auto& metadata = assetManager->GetMetadata(groomComponent.m_Groom);
            metadata.Handle != 0 && metadata.FilePath.has_parent_path())
        {
            relativeDirectory = metadata.FilePath.parent_path();
        }
        const std::string stem = (groom->GetName().empty() ? std::string("groom") : groom->GetName()) + "-" +
                                 (targetSourceName.empty() ? std::string("body") : targetSourceName);
        outcome.m_RelativePath = relativeDirectory / (stem + ".ologroombinding");
        const std::filesystem::path absolutePath = Project::GetProjectDirectory() / outcome.m_RelativePath;

        {
            std::error_code ec;
            std::filesystem::create_directories(absolutePath.parent_path(), ec);
            if (ec)
            {
                outcome.m_Reason =
                    std::format("could not create '{}': {}", absolutePath.parent_path().string(), ec.message());
                return outcome;
            }
            std::ofstream out(absolutePath, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                outcome.m_Reason = std::format("could not open '{}' for writing", absolutePath.string());
                return outcome;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (out.fail())
            {
                outcome.m_Reason = std::format("failed while writing '{}'", absolutePath.string());
                return outcome;
            }
        }

        outcome.m_Binding = assetManager->ImportAsset(outcome.m_RelativePath);
        if (outcome.m_Binding == 0)
        {
            outcome.m_Reason = std::format("wrote '{}' but the asset manager refused to import it",
                                           outcome.m_RelativePath.string());
            return outcome;
        }

        outcome.m_Ok = true;
        return outcome;
    }
} // namespace OloEngine::GroomAuthoring
