#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/AssimpMeshImporter.h"

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Model.h"

#include <exception>
#include <filesystem>
#include <system_error>

namespace OloEngine
{
    namespace
    {
        // Import through AnimatedModel and flatten to one MeshSource.
        //
        // AnimatedModel is the only importer in the engine that extracts bone influences,
        // bone info and the skeleton, and (since #1278) merges morph targets into a
        // combined source. Routing rigged files here rather than teaching Model to read
        // bones is deliberate: two importers that must agree about what a mesh is are how
        // they come to disagree (issue #1272).
        MeshImportResult ImportAnimated(const std::filesystem::path& path)
        {
            AnimatedModel model(path.string());
            auto meshSource = model.CreateCombinedMeshSource();
            if (!meshSource || meshSource->GetVertices().IsEmpty())
            {
                return MeshImportResult::Failure("AssimpMeshImporter: animated import produced no geometry: " +
                                                 path.string());
            }

            return MeshImportResult::Ok(std::move(meshSource));
        }
    } // namespace

    MeshImportResult AssimpMeshImporter::Import(const std::filesystem::path& path, const MeshImportOptions& options)
    {
        // Non-throwing existence check — the throwing overload can throw on a filesystem
        // error (permissions, etc.), which would breach Import's no-throw boundary.
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            return MeshImportResult::Failure("AssimpMeshImporter: file does not exist: " + path.string());

        // Model construction / assimp import can throw (bad file, allocation); convert any
        // exception into a Failure result so Import stays no-throw for its callers.
        try
        {
            // A rigged file already imported once has a warm ANIMATED cache, and its header
            // says whether the source really had bones. Both halves matter: validity alone
            // would send a STATIC mesh that someone loaded through AnimationStateComponent
            // down the animated path and cost it its cooked DAG and its cached materials.
            // This branch is what keeps the steady state at ONE import — see the re-route
            // below for the cold case.
            if (MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix) &&
                MeshCache::IsCachedSourceRigged(path, AnimatedModel::kCachePrefix))
            {
                return ImportAnimated(path);
            }

            // Otherwise import as a static mesh, which is what this function always did:
            // build a Model (which reads the .omesh geometry cache when warm and only
            // re-imports the source for materials a pre-v4 cache cannot supply) and combine
            // its meshes into a single MeshSource. CreateCombinedMeshSource deliberately
            // does NOT Build() — the caller does — and marks the result pre-optimized.
            //
            // FlipUV is passed straight through (no per-format origin XOR like the
            // USD/Alembic importers): the assimp import path already normalizes the UV
            // origin, so the flag here carries the "invert relative to the format's default"
            // meaning directly. See the MeshImportOptions::FlipUV note in MeshImporter.h.
            Model model(path.string(), TextureOverride{}, options.FlipUV);

            // Model does not import bones, but it now NOTICES them — on a cold Assimp walk
            // from aiMesh::mNumBones, on a warm load from the cache's FlagSourceRigged. A
            // rigged source has to be re-imported through AnimatedModel or the MeshSource
            // asset ships with no influences and every handle-addressed consumer
            // (VirtualMeshComponent, MeshComponent) renders it in its bind pose while the
            // skeleton animates (issue #1272).
            //
            // This costs a SECOND parse, but only on the load that first discovers the rig:
            // AnimatedModel writes its own cache, and the branch above takes over from then
            // on. The static .omesh Model just wrote is left in place on purpose — it is
            // keyed by a different prefix, it records FlagSourceRigged, and deleting it
            // would only cost a re-parse if the anim cache is ever invalidated.
            if (model.IsSourceRigged())
            {
                if (options.FlipUV)
                {
                    // AnimatedModel has no flipUV switch: its import flags are fixed (and
                    // deliberately exclude aiProcess_FlipUVs, because Assimp's glTF2 reader
                    // already flips V). Say so rather than silently importing with the
                    // opposite UV origin from the one that was asked for.
                    OLO_CORE_WARN("AssimpMeshImporter: '{}' is rigged and routes to AnimatedModel, which does "
                                  "not support FlipUV — importing with the format's default UV origin",
                                  path.string());
                }

                MeshImportResult animated = ImportAnimated(path);
                if (animated.Succeeded())
                {
                    return animated;
                }

                // Loud, and NOT silent: the static result below is geometrically correct but
                // unskinnable, so a character loaded through here stands in its bind pose.
                // Failing outright would make a file that used to load stop loading, which
                // is worse; saying nothing would reproduce exactly the bug #1272 is about.
                OLO_CORE_ERROR("AssimpMeshImporter: '{}' has bones but the animated import failed ({}). "
                               "Falling back to the STATIC import — the mesh will load with NO bone "
                               "influences and will render in its bind pose.",
                               path.string(), animated.Error);
            }

            auto meshSource = model.CreateCombinedMeshSource();
            if (!meshSource || meshSource->GetVertices().IsEmpty())
                return MeshImportResult::Failure("AssimpMeshImporter: import produced no geometry: " + path.string());

            return MeshImportResult::Ok(std::move(meshSource));
        }
        catch (const std::exception& e)
        {
            return MeshImportResult::Failure(std::string("AssimpMeshImporter: import failed: ") + e.what());
        }
    }
} // namespace OloEngine
