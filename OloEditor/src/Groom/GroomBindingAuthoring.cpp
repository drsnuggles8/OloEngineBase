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
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
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
        // The stem carries two free-form names -- an asset name and an entity
        // name, both typed by a person -- and a path is not a place to put
        // either one raw. `Coat / v2` makes a directory nobody asked for and an
        // ImportAsset of a path that is not where the bytes went; on Windows a
        // `:` makes an NTFS alternate data stream, which writes successfully,
        // reads back empty, and is invisible in every file listing.
        //
        // Sanitised rather than rejected, because the name is an authoring
        // convenience here and refusing a bind over a slash in a body's name
        // would be the tail wagging the dog.
        const auto sanitise = [](std::string_view name, std::string_view fallback)
        {
            std::string safe;
            safe.reserve(name.size());
            for (const char ch : name)
            {
                const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                                  ch == '-' || ch == '_';
                safe.push_back(keep ? ch : '-');
            }
            // A name that was ALL separators collapses to a row of dashes, which
            // is a legal but meaningless filename; treat it as absent.
            if (safe.empty() || safe.find_first_not_of('-') == std::string::npos)
            {
                return std::string(fallback);
            }
            return safe;
        };

        // The names are for a human reading the content browser. The IDENTITIES
        // are what make the filename unique, and they have to be there: two
        // bodies in one scene may both be called "Body" (duplicating an entity
        // is one Ctrl-D), and without a discriminator the second bind would
        // truncate the first binding's file and then ImportAsset would hand back
        // the FIRST one's handle -- so the second groom would silently wear the
        // first groom's binding, which is the exact failure this whole feature
        // is built to make impossible.
        //
        // Hashed to 16 hex digits rather than spelled out, because two 20-digit
        // decimal ids in a filename is not a name anyone can read, and the
        // discriminator only has to distinguish.
        u64 identity = 1469598103934665603ull; // FNV-1a offset basis
        const auto mix = [&identity](u64 value)
        {
            for (i32 byte = 0; byte < 8; ++byte)
            {
                identity ^= (value >> (byte * 8)) & 0xFFull;
                identity *= 1099511628211ull;
            }
        };
        mix(static_cast<u64>(groomComponent.m_Groom));
        mix(static_cast<u64>(targetEntity.GetUUID()));

        const std::string stem =
            std::format("{}-{}-{:016x}", sanitise(groom->GetName(), "groom"), sanitise(targetSourceName, "body"),
                        identity);
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
            // Written to a TEMPORARY and renamed over the target, because the
            // target may be a binding that currently works. Truncating it first
            // means a disk that fills up, a process killed mid-write or a
            // network share that drops leaves a half-file where a valid asset
            // was -- and the groom that was using it is broken by an authoring
            // action that FAILED. The rename is the only step that touches the
            // real path, and on both platforms it is atomic within a directory.
            //
            // The failure check also had to move: `out.fail()` was read while
            // the stream was still open, so it saw the buffered write and not
            // the flush, and a write that failed at close() -- which is where a
            // full disk reports -- was recorded as a success.
            const std::filesystem::path temporaryPath = absolutePath.string() + ".tmp";
            {
                std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
                if (!out.is_open())
                {
                    outcome.m_Reason = std::format("could not open '{}' for writing", temporaryPath.string());
                    return outcome;
                }
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out.close();
                if (out.fail())
                {
                    std::filesystem::remove(temporaryPath, ec);
                    outcome.m_Reason = std::format("failed while writing '{}'", temporaryPath.string());
                    return outcome;
                }
            }

            // rename(), not copy-then-delete: std::filesystem::rename replaces an
            // existing file on both Windows and POSIX, and the replacement is
            // what makes a REBIND of the same pair -- the common case, since the
            // filename is deterministic -- safe to repeat.
            std::filesystem::rename(temporaryPath, absolutePath, ec);
            if (ec)
            {
                std::error_code removeError;
                std::filesystem::remove(temporaryPath, removeError);
                outcome.m_Reason =
                    std::format("wrote '{}' but could not move it into place: {}", temporaryPath.string(),
                                ec.message());
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
