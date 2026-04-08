#include "OloEnginePCH.h"
#include "AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Animation/MorphTargets/MorphTarget.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSet.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <ranges>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    namespace
    {
        // Combine multiple MeshSources into a single MeshSource for binary cache serialization.
        // Each original MeshSource becomes one submesh in the combined output.
        Ref<MeshSource> CombineMeshSourcesForCache(const std::vector<Ref<MeshSource>>& meshes, const Ref<Skeleton>& skeleton)
        {
            auto combined = Ref<MeshSource>::Create();
            u32 baseVertex = 0;
            u32 baseIndex = 0;

            for (const auto& src : meshes)
            {
                if (!src)
                {
                    continue;
                }

                const auto& srcVerts = src->GetVertices();
                const auto& srcIndices = src->GetIndices();

                for (i32 i = 0; i < srcVerts.Num(); ++i)
                {
                    combined->GetVertices().Add(srcVerts[i]);
                }

                for (i32 i = 0; i < srcIndices.Num(); ++i)
                {
                    combined->GetIndices().Add(srcIndices[i] + baseVertex);
                }

                for (i32 i = 0; i < src->GetBoneInfluences().Num(); ++i)
                {
                    combined->GetBoneInfluences().Add(src->GetBoneInfluences()[i]);
                }

                // Deduplicate bone info entries by bone index
                std::unordered_set<u32> existingBoneIndices;
                for (i32 j = 0; j < combined->GetBoneInfo().Num(); ++j)
                {
                    existingBoneIndices.insert(combined->GetBoneInfo()[j].m_BoneIndex);
                }
                for (i32 i = 0; i < src->GetBoneInfo().Num(); ++i)
                {
                    if (auto [_, inserted] = existingBoneIndices.insert(src->GetBoneInfo()[i].m_BoneIndex); inserted)
                    {
                        combined->GetBoneInfo().Add(src->GetBoneInfo()[i]);
                    }
                }

                // Create submesh entry for this original mesh
                Submesh submesh;
                submesh.m_BaseVertex = baseVertex;
                submesh.m_BaseIndex = baseIndex;
                submesh.m_VertexCount = static_cast<u32>(srcVerts.Num());
                submesh.m_IndexCount = static_cast<u32>(srcIndices.Num());
                submesh.m_IsRigged = src->HasBoneInfluences();

                if (!src->GetSubmeshes().IsEmpty())
                {
                    const auto& origSub = src->GetSubmeshes()[0];
                    submesh.m_MaterialIndex = origSub.m_MaterialIndex;
                    submesh.m_Transform = origSub.m_Transform;
                    submesh.m_LocalTransform = origSub.m_LocalTransform;
                    submesh.m_BoundingBox = origSub.m_BoundingBox;
                    submesh.m_NodeName = origSub.m_NodeName;
                    submesh.m_MeshName = origSub.m_MeshName;
                }

                combined->GetSubmeshes().Add(submesh);
                baseVertex += static_cast<u32>(srcVerts.Num());
                baseIndex += static_cast<u32>(srcIndices.Num());
            }

            if (skeleton)
            {
                combined->SetSkeleton(skeleton);
            }

            // Merge morph targets from all meshes into combined vertex space.
            // Each mesh's targets are expanded/remapped so vertex indices reference
            // the combined buffer. On split, the reverse extraction happens.
            {
                auto totalVertCount = static_cast<u32>(combined->GetVertices().Num());
                auto mergedSet = Ref<MorphTargetSet>::Create();
                std::unordered_map<std::string, sizet> nameToIndex; // target name -> index in mergedSet

                u32 meshBaseVertex = 0;
                for (const auto& src : meshes)
                {
                    if (!src)
                    {
                        continue;
                    }
                    auto srcVertCount = static_cast<u32>(src->GetVertices().Num());

                    if (src->HasMorphTargets())
                    {
                        for (const auto& target : src->GetMorphTargets()->Targets)
                        {
                            auto it = nameToIndex.find(target.Name);
                            if (it == nameToIndex.end())
                            {
                                // New target — create expanded dense array
                                MorphTarget combined_target;
                                combined_target.Name = target.Name;
                                combined_target.IsSparse = false;
                                combined_target.Vertices.resize(totalVertCount); // zero-initialized

                                if (target.IsSparse)
                                {
                                    for (const auto& sparse : target.SparseVertices)
                                    {
                                        if (sparse.VertexIndex < srcVertCount)
                                        {
                                            combined_target.Vertices[meshBaseVertex + sparse.VertexIndex] = sparse.Delta;
                                        }
                                    }
                                }
                                else
                                {
                                    auto copyCount = std::min(static_cast<u32>(target.Vertices.size()), srcVertCount);
                                    for (u32 v = 0; v < copyCount; ++v)
                                    {
                                        combined_target.Vertices[meshBaseVertex + v] = target.Vertices[v];
                                    }
                                }

                                nameToIndex[target.Name] = mergedSet->Targets.size();
                                mergedSet->Targets.push_back(MoveTemp(combined_target));
                            }
                            else
                            {
                                // Existing target — fill in this mesh's vertex range
                                auto& existing = mergedSet->Targets[it->second];
                                if (target.IsSparse)
                                {
                                    for (const auto& sparse : target.SparseVertices)
                                    {
                                        if (sparse.VertexIndex < srcVertCount)
                                        {
                                            existing.Vertices[meshBaseVertex + sparse.VertexIndex] = sparse.Delta;
                                        }
                                    }
                                }
                                else
                                {
                                    auto copyCount = std::min(static_cast<u32>(target.Vertices.size()), srcVertCount);
                                    for (u32 v = 0; v < copyCount; ++v)
                                    {
                                        existing.Vertices[meshBaseVertex + v] = target.Vertices[v];
                                    }
                                }
                            }
                        }
                    }

                    meshBaseVertex += srcVertCount;
                }

                if (!mergedSet->Targets.empty())
                {
                    combined->SetMorphTargets(mergedSet);
                }
            }

            // Preserve pre-optimized state from inputs so the cache round-trip
            // skips OptimizeMesh on reload (mirrors SplitCombinedMeshSource).
            for (const auto& src : meshes)
            {
                if (src && src->IsPreOptimized())
                {
                    combined->SetPreOptimized(true);
                    break;
                }
            }

            return combined;
        }

        // Split a combined MeshSource back into per-submesh MeshSources.
        bool SplitCombinedMeshSource(
            const Ref<MeshSource>& combined,
            std::vector<Ref<MeshSource>>& outMeshes,
            Ref<Skeleton>& outSkeleton)
        {
            if (!combined || combined->GetSubmeshes().IsEmpty())
            {
                return false;
            }

            // Build into locals so outMeshes/outSkeleton stay untouched on failure.
            Ref<Skeleton> localSkeleton;
            if (combined->HasSkeleton())
            {
                const auto* src = combined->GetSkeleton();

                // Verify all skeleton arrays share the same bone count
                auto const expectedBoneCount = src->m_ParentIndices.size();
                if (src->m_BoneNames.size() != expectedBoneCount ||
                    src->m_LocalTransforms.size() != expectedBoneCount ||
                    src->m_GlobalTransforms.size() != expectedBoneCount ||
                    src->m_FinalBoneMatrices.size() != expectedBoneCount ||
                    src->m_BindPoseMatrices.size() != expectedBoneCount ||
                    src->m_InverseBindPoses.size() != expectedBoneCount ||
                    src->m_BindPoseLocalTransforms.size() != expectedBoneCount ||
                    src->m_BonePreTransforms.size() != expectedBoneCount)
                {
                    OLO_CORE_ERROR("AnimatedModel::SplitCombinedMeshSource - Skeleton array size mismatch "
                                   "(expected {} bones), rejecting cache",
                                   expectedBoneCount);
                    return false;
                }

                // Validate all skeleton matrices for non-finite values before copying
                auto const validateMat4Array = [](const std::vector<glm::mat4>& arr, const char* name) -> bool
                {
                    for (sizet i = 0; i < arr.size(); ++i)
                    {
                        const auto& m = arr[i];
                        for (int c = 0; c < 4; ++c)
                        {
                            for (int r = 0; r < 4; ++r)
                            {
                                if (!std::isfinite(m[c][r]))
                                {
                                    OLO_CORE_ERROR("AnimatedModel::SplitCombinedMeshSource - Non-finite value in cached skeleton {} "
                                                   "at bone {} element [{},{}], rejecting cache",
                                                   name, i, c, r);
                                    return false;
                                }
                            }
                        }
                    }
                    return true;
                };

                if (!validateMat4Array(src->m_LocalTransforms, "LocalTransforms") ||
                    !validateMat4Array(src->m_GlobalTransforms, "GlobalTransforms") ||
                    !validateMat4Array(src->m_FinalBoneMatrices, "FinalBoneMatrices") ||
                    !validateMat4Array(src->m_BindPoseMatrices, "BindPoseMatrices") ||
                    !validateMat4Array(src->m_InverseBindPoses, "InverseBindPoses") ||
                    !validateMat4Array(src->m_BindPoseLocalTransforms, "BindPoseLocalTransforms") ||
                    !validateMat4Array(src->m_BonePreTransforms, "BonePreTransforms"))
                {
                    return false;
                }

                localSkeleton = Ref<Skeleton>::Create();
                localSkeleton->m_ParentIndices = src->m_ParentIndices;
                localSkeleton->m_BoneNames = src->m_BoneNames;
                localSkeleton->m_LocalTransforms = src->m_LocalTransforms;
                localSkeleton->m_GlobalTransforms = src->m_GlobalTransforms;
                localSkeleton->m_FinalBoneMatrices = src->m_FinalBoneMatrices;
                localSkeleton->m_BindPoseMatrices = src->m_BindPoseMatrices;
                localSkeleton->m_InverseBindPoses = src->m_InverseBindPoses;
                localSkeleton->m_BindPoseLocalTransforms = src->m_BindPoseLocalTransforms;
                localSkeleton->m_BonePreTransforms = src->m_BonePreTransforms;
            }

            const auto& allVerts = combined->GetVertices();
            const auto& allIndices = combined->GetIndices();
            const auto& allBones = combined->GetBoneInfluences();
            const auto& allBoneInfo = combined->GetBoneInfo();

            std::vector<Ref<MeshSource>> localMeshes;
            localMeshes.reserve(static_cast<sizet>(combined->GetSubmeshes().Num()));

            auto submeshCount = combined->GetSubmeshes().Num();
            for (i32 s = 0; s < submeshCount; ++s)
            {
                const auto& submesh = combined->GetSubmeshes()[s];

                // Validate submesh ranges against combined buffer sizes
                if (submesh.m_BaseVertex > static_cast<u32>(allVerts.Num()) ||
                    submesh.m_VertexCount > static_cast<u32>(allVerts.Num()) - submesh.m_BaseVertex ||
                    submesh.m_BaseIndex > static_cast<u32>(allIndices.Num()) ||
                    submesh.m_IndexCount > static_cast<u32>(allIndices.Num()) - submesh.m_BaseIndex)
                {
                    OLO_CORE_ERROR("AnimatedModel::SplitCombinedMeshSource - Submesh {} has out-of-range bounds, "
                                   "rejecting cache",
                                   s);
                    return false;
                }

                auto mesh = Ref<MeshSource>::Create();

                for (u32 i = 0; i < submesh.m_VertexCount; ++i)
                {
                    mesh->GetVertices().Add(allVerts[submesh.m_BaseVertex + i]);
                }

                for (u32 i = 0; i < submesh.m_IndexCount; ++i)
                {
                    u32 const orig = allIndices[submesh.m_BaseIndex + i];
                    if (orig < submesh.m_BaseVertex || orig >= submesh.m_BaseVertex + submesh.m_VertexCount)
                    {
                        OLO_CORE_ERROR("AnimatedModel::SplitCombinedMeshSource - Index {} out of submesh {} "
                                       "vertex range [{}..{}), rejecting cache",
                                       orig, s, submesh.m_BaseVertex,
                                       submesh.m_BaseVertex + submesh.m_VertexCount);
                        return false;
                    }
                    mesh->GetIndices().Add(orig - submesh.m_BaseVertex);
                }

                bool const hasBoneSlice = !allBones.IsEmpty() && submesh.m_BaseVertex + submesh.m_VertexCount <= static_cast<u32>(allBones.Num());
                if (localSkeleton && !hasBoneSlice)
                {
                    OLO_CORE_ERROR("AnimatedModel::SplitCombinedMeshSource - Bone influence data missing for rigged submesh {}, rejecting cache", s);
                    return false;
                }
                if (hasBoneSlice)
                {
                    for (u32 i = 0; i < submesh.m_VertexCount; ++i)
                    {
                        mesh->GetBoneInfluences().Add(allBones[submesh.m_BaseVertex + i]);
                    }
                }

                auto boneInfoCount = allBoneInfo.Num();
                for (i32 i = 0; i < boneInfoCount; ++i)
                {
                    mesh->GetBoneInfo().Add(allBoneInfo[i]);
                }

                Submesh newSub = submesh;
                newSub.m_BaseVertex = 0;
                newSub.m_BaseIndex = 0;
                mesh->GetSubmeshes().Add(newSub);

                mesh->SetSkeleton(localSkeleton);
                mesh->SetPreOptimized(combined->IsPreOptimized());

                // For pre-optimized meshes, generate shadow indices before Build
                // because Build skips OptimizeMesh (which normally handles it).
                if (mesh->IsPreOptimized())
                {
                    MeshOptimization::GenerateShadowIndices(*mesh);
                }
                mesh->Build();

                localMeshes.push_back(mesh);
            }

            // Split combined morph targets back into per-mesh sets.
            // Combined targets are dense arrays spanning the total vertex count;
            // extract each submesh's slice and skip targets with all-zero deltas.
            constexpr f32 kMorphEpsilon = 1e-6f;
            auto isNonZeroVec3 = [](const glm::vec3& v, f32 eps)
            {
                return std::abs(v.x) > eps || std::abs(v.y) > eps || std::abs(v.z) > eps;
            };

            if (combined->HasMorphTargets())
            {
                const auto& combinedMorphs = combined->GetMorphTargets();
                for (i32 s = 0; s < submeshCount && s < static_cast<i32>(localMeshes.size()); ++s)
                {
                    const auto& submesh = combined->GetSubmeshes()[s];
                    auto meshMorphs = Ref<MorphTargetSet>::Create();

                    for (const auto& target : combinedMorphs->Targets)
                    {
                        MorphTarget localTarget;
                        localTarget.Name = target.Name;
                        localTarget.IsSparse = false;
                        localTarget.Vertices.resize(submesh.m_VertexCount);

                        bool hasNonZero = false;
                        if (!target.IsSparse && submesh.m_BaseVertex + submesh.m_VertexCount <= static_cast<u32>(target.Vertices.size()))
                        {
                            for (u32 v = 0; v < submesh.m_VertexCount; ++v)
                            {
                                localTarget.Vertices[v] = target.Vertices[submesh.m_BaseVertex + v];
                                const auto& d = localTarget.Vertices[v];
                                if (isNonZeroVec3(d.DeltaPosition, kMorphEpsilon) || isNonZeroVec3(d.DeltaNormal, kMorphEpsilon) || isNonZeroVec3(d.DeltaTangent, kMorphEpsilon))
                                {
                                    hasNonZero = true;
                                }
                            }
                        }

                        if (hasNonZero)
                        {
                            meshMorphs->Targets.push_back(MoveTemp(localTarget));
                        }
                    }

                    if (!meshMorphs->Targets.empty())
                    {
                        localMeshes[s]->SetMorphTargets(meshMorphs);
                    }
                }
            }

            // Commit — no exceptions past this point.
            outSkeleton = MoveTemp(localSkeleton);
            outMeshes = MoveTemp(localMeshes);
            return true;
        }

        // Scan directory for a file whose stem matches (case-insensitive) with any common image extension.
        // Returns empty path if nothing found.
        std::filesystem::path FindTextureInDirectory(const std::filesystem::path& directory, const std::filesystem::path& filenameHint)
        {
            static constexpr std::array<std::string_view, 7> kImageExtensions = {
                ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds"
            };

            std::string targetStem = filenameHint.stem().string();
            std::ranges::transform(targetStem, targetStem.begin(),
                                   [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });

            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }

                const auto& entryPath = entry.path();
                std::string entryStem = entryPath.stem().string();
                std::ranges::transform(entryStem, entryStem.begin(),
                                       [](unsigned char c)
                                       { return static_cast<char>(std::tolower(c)); });

                if (entryStem != targetStem)
                {
                    continue;
                }

                std::string ext = entryPath.extension().string();
                std::ranges::transform(ext, ext.begin(),
                                       [](unsigned char c)
                                       { return static_cast<char>(std::tolower(c)); });

                if (std::ranges::find(kImageExtensions, ext) != kImageExtensions.end())
                {
                    return entryPath;
                }
            }

            return {};
        }
        // Collect material indices from the Assimp scene in DFS traversal
        // order to match the original ProcessNode order used to build m_Meshes.
        const auto CollectMaterialIndices = [](const aiNode* node, const aiScene* scene,
                                               std::vector<u32>& out, auto&& self) -> void
        {
            for (u32 i = 0; i < node->mNumMeshes; i++)
            {
                auto* mesh = scene->mMeshes[node->mMeshes[i]];
                out.push_back(mesh->mMaterialIndex);
            }
            for (u32 i = 0; i < node->mNumChildren; i++)
            {
                self(node->mChildren[i], scene, out, self);
            }
        };

    } // anonymous namespace

    AnimatedModel::AnimatedModel(const std::string& path)
    {
        LoadModel(path);
    }

    void AnimatedModel::LoadModel(const std::string& path)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_INFO("AnimatedModel::LoadModel: Loading animated model from {}", path);

        // Try loading from binary cache first (skip Assimp entirely)
        std::filesystem::path sourcePath(path);
        constexpr auto kAnimCachePrefix = "anim_";
        if (MeshCache::IsMeshCacheValid(sourcePath, kAnimCachePrefix))
        {
            auto cachedMesh = MeshCache::LoadMeshFromCache(sourcePath, kAnimCachePrefix);
            if (cachedMesh)
            {
                m_Directory = sourcePath.parent_path().string();

                Ref<Skeleton> skeleton;
                if (SplitCombinedMeshSource(cachedMesh, m_Meshes, skeleton))
                {
                    m_Skeleton = skeleton;

                    // Require animation cache to also be valid — an empty return is
                    // ambiguous (no clips vs. missing/corrupt .oanim).  If the animation
                    // sidecar is invalid, fall through to the full import so both halves
                    // are rebuilt together.
                    if (!MeshCache::IsAnimationCacheValid(sourcePath))
                    {
                        OLO_CORE_WARN("AnimatedModel::LoadModel: Mesh cache valid but animation cache invalid for '{}', re-importing", path);
                        m_Meshes.clear();
                        m_Skeleton = nullptr;
                        MeshCache::InvalidateCache(sourcePath, kAnimCachePrefix);
                    }
                    else
                    {
                        m_Animations = MeshCache::LoadAnimationsFromCache(sourcePath);

                        // Load materials from the source file (lightweight Assimp read — no geometry postprocessing).
                        // ProcessNode builds m_Meshes in DFS order, so we replay
                        // the same traversal to collect material indices in matching order.
                        {
                            Assimp::Importer matImporter;
                            const aiScene* matScene = matImporter.ReadFile(path, 0);
                            if (matScene && !(matScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) && matScene->mRootNode)
                            {
                                std::vector<u32> materialIndices;
                                materialIndices.reserve(m_Meshes.size());
                                CollectMaterialIndices(matScene->mRootNode, matScene, materialIndices, CollectMaterialIndices);

                                m_Materials.resize(m_Meshes.size());
                                auto numMaterials = std::min(materialIndices.size(), m_Meshes.size());
                                for (sizet i = 0; i < numMaterials; ++i)
                                {
                                    if (materialIndices[i] < matScene->mNumMaterials)
                                    {
                                        m_Materials[i] = ProcessMaterial(matScene->mMaterials[materialIndices[i]]);
                                    }
                                }
                            }
                            else
                            {
                                OLO_CORE_WARN("AnimatedModel::LoadModel: Failed to load materials from '{}' for cached geometry", path);
                                m_Materials.resize(m_Meshes.size());
                            }
                        }

                        CalculateBounds();

                        // Rebuild bone name→id map from cached skeleton for FindBoneIndex()
                        if (m_Skeleton)
                        {
                            m_BoneInfoMap.clear();
                            for (sizet i = 0; i < m_Skeleton->m_BoneNames.size(); ++i)
                            {
                                BoneInfo boneInfo;
                                boneInfo.Id = static_cast<u32>(i);
                                boneInfo.Offset = m_Skeleton->m_InverseBindPoses[i];
                                m_BoneInfoMap[m_Skeleton->m_BoneNames[i]] = boneInfo;
                            }
                        }

                        OLO_CORE_INFO("AnimatedModel::LoadModel: Loaded from cache - {} meshes, {} animations",
                                      m_Meshes.size(), m_Animations.size());
                        return;
                    } // end animation-cache-valid block
                }
                else
                {
                    // SplitCombinedMeshSource failed — invalidate corrupt cache so next load re-imports
                    OLO_CORE_WARN("AnimatedModel::LoadModel: SplitCombinedMeshSource failed for '{}', invalidating cache", path);
                    m_Meshes.clear();
                    m_Skeleton = nullptr;
                    MeshCache::InvalidateCache(sourcePath, kAnimCachePrefix);
                }
            }
        }

        // Create an instance of the Importer class
        Assimp::Importer importer;

        // Set import flags for skeletal animation
        u32 importFlags =
            aiProcess_Triangulate |           // Make sure we get triangles
            aiProcess_GenNormals |            // Create normals if not present
            aiProcess_CalcTangentSpace |      // Calculate tangents and bitangents
            aiProcess_ValidateDataStructure | // Validate the imported data structure
            aiProcess_LimitBoneWeights |      // Limit bone weights to 4 per vertex
            aiProcess_GlobalScale;            // Apply global scale
        // NOTE: Do NOT add aiProcess_FlipUVs here. Assimp's glTF2 importer already
        // flips V internally (glTF2Importer.cpp line ~648). Adding FlipUVs would
        // double-flip, returning UVs to the original glTF convention and breaking
        // texture mapping when stbi_set_flip_vertically_on_load is active.

        // Read the file
        const aiScene* scene = importer.ReadFile(path, importFlags);

        // Check for errors
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            OLO_CORE_ERROR("AnimatedModel::LoadModel: ASSIMP Error: {}", importer.GetErrorString());
            return;
        }

        // Store the directory path
        m_Directory = std::filesystem::path(path).parent_path().string();

        OLO_CORE_INFO("AnimatedModel::LoadModel: Scene loaded - Meshes: {}, Materials: {}, Animations: {}",
                      scene->mNumMeshes, scene->mNumMaterials, scene->mNumAnimations);

        // Process skeleton first if available
        ProcessSkeleton(scene);

        // Process all meshes (tracks mesh node's global transform for axis correction)
        ProcessNode(scene->mRootNode, scene, glm::mat4(1.0f));

        // Apply mesh node global transform correction for proper axis orientation.
        // Mesh vertices are stored in mesh-local space which may use a different
        // coordinate convention (e.g. Z-up from COLLADA). The mesh node's scene-graph
        // ancestors (like a "Z_UP" node) encode the conversion to the glTF/engine
        // Y-up system. By post-multiplying InverseBindPoses with meshNodeGlobal,
        // FinalBoneMatrices naturally includes this axis correction:
        //   FinalBone = GlobalBone * (InvBind * meshNodeGlobal)
        // At bind pose this yields meshNodeGlobal (instead of identity), correctly
        // transforming vertices from mesh-local to scene/world space.
        if (m_Skeleton && m_HasMeshNodeTransform)
        {
            for (sizet i = 0; i < m_Skeleton->m_InverseBindPoses.size(); ++i)
            {
                m_Skeleton->m_InverseBindPoses[i] = m_Skeleton->m_InverseBindPoses[i] * m_MeshNodeGlobalTransform;
                m_Skeleton->m_FinalBoneMatrices[i] = m_MeshNodeGlobalTransform;
            }
        }

        // Process animations
        ProcessAnimations(scene);

        // Calculate bounding volumes for the entire model
        CalculateBounds();

        // Save to binary cache for next load
        {
            auto combined = CombineMeshSourcesForCache(m_Meshes, m_Skeleton);
            MeshCache::SaveMeshToCache(sourcePath, *combined, kAnimCachePrefix);
            // Always write .oanim when skeleton exists (even if empty) so that
            // IsAnimationCacheValid succeeds on re-load.
            if (m_Skeleton || !m_Animations.empty())
            {
                MeshCache::SaveAnimationsToCache(sourcePath, m_Animations);
            }
        }

        OLO_CORE_INFO("AnimatedModel::LoadModel: Successfully loaded animated model with {} meshes, {} animations",
                      m_Meshes.size(), m_Animations.size());
    }

    void AnimatedModel::ProcessNode(const aiNode* node, const aiScene* scene, const glm::mat4& parentTransform)
    {
        OLO_PROFILE_FUNCTION();

        glm::mat4 nodeTransform = AssimpMatrixToGLM(node->mTransformation);
        glm::mat4 globalTransform = parentTransform * nodeTransform;

        // Process all the node's meshes
        for (u32 i = 0; i < node->mNumMeshes; i++)
        {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            auto meshSource = ProcessMesh(mesh, scene);
            if (meshSource)
            {
                m_Meshes.push_back(meshSource);

                // Store the first skinned mesh node's global transform for axis correction
                if (!m_HasMeshNodeTransform && mesh->mNumBones > 0)
                {
                    m_MeshNodeGlobalTransform = globalTransform;
                    m_HasMeshNodeTransform = true;
                }
                else if (m_HasMeshNodeTransform && mesh->mNumBones > 0)
                {
                    constexpr f32 kMatEps = 1e-4f;
                    bool same = true;
                    for (int r = 0; r < 4 && same; ++r)
                    {
                        for (int c = 0; c < 4 && same; ++c)
                        {
                            same = std::abs(m_MeshNodeGlobalTransform[r][c] - globalTransform[r][c]) < kMatEps;
                        }
                    }
                    OLO_CORE_ASSERT(same,
                                    "AnimatedModel: skinned meshes have different node transforms — axis correction may be wrong");
                }

                // Process the material for this mesh
                Material material;
                if (mesh->mMaterialIndex >= 0 && mesh->mMaterialIndex < scene->mNumMaterials)
                {
                    material = ProcessMaterial(scene->mMaterials[mesh->mMaterialIndex]);
                }
                else
                {
                    // Create default material if no material is found
                    auto defaultMaterialRef = Material::CreatePBR("Default Animated Material", glm::vec3(0.8f), 0.0f, 0.5f);
                    if (defaultMaterialRef)
                    {
                        material = *defaultMaterialRef; // Copy to value type for struct-like access
                    }
                    else
                    {
                        OLO_CORE_ERROR("AnimatedModel: Failed to create default PBR material");
                        // Use a minimal fallback material or skip this mesh
                        material = Material{}; // Default constructed material
                    }
                }
                m_Materials.push_back(material);
            }
        }

        // Process all the node's children
        for (u32 i = 0; i < node->mNumChildren; i++)
        {
            ProcessNode(node->mChildren[i], scene, globalTransform);
        }
    }

    Ref<MeshSource> AnimatedModel::ProcessMesh(const aiMesh* mesh, [[maybe_unused]] const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices; // Use regular Vertex instead of specialized vertex types
        std::vector<u32> indices;
        std::vector<BoneInfluence> boneInfluences; // Separate bone data

        // Reserve space to reduce allocations during mesh processing
        vertices.reserve(mesh->mNumVertices);
        indices.reserve(mesh->mNumFaces * 3);       // Assuming triangulated mesh
        boneInfluences.reserve(mesh->mNumVertices); // One per vertex for bone influences

        OLO_CORE_TRACE("AnimatedModel::ProcessMesh: Processing mesh with {} vertices, {} faces, {} bones",
                       mesh->mNumVertices, mesh->mNumFaces, mesh->mNumBones);

        // Process vertices (without bone data)
        for (u32 i = 0; i < mesh->mNumVertices; i++)
        {
            Vertex vertex;

            // Position
            vertex.Position = glm::vec3(
                mesh->mVertices[i].x,
                mesh->mVertices[i].y,
                mesh->mVertices[i].z);

            // Normal
            if (mesh->HasNormals())
            {
                vertex.Normal = glm::vec3(
                    mesh->mNormals[i].x,
                    mesh->mNormals[i].y,
                    mesh->mNormals[i].z);
            }
            else
            {
                vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            // Texture coordinates
            if (mesh->mTextureCoords[0])
            {
                vertex.TexCoord = glm::vec2(
                    mesh->mTextureCoords[0][i].x,
                    mesh->mTextureCoords[0][i].y);
            }
            else
            {
                vertex.TexCoord = glm::vec2(0.0f);
            }

            vertices.push_back(vertex);
        }

        // Initialize bone influences (one per vertex)
        boneInfluences.resize(vertices.size());

        // Process bone data if available
        if (mesh->mNumBones > 0)
        {
            ProcessBones(mesh, boneInfluences);
        }

        // Process indices
        for (u32 i = 0; i < mesh->mNumFaces; i++)
        {
            const aiFace& face = mesh->mFaces[i];
            for (u32 j = 0; j < face.mNumIndices; j++)
            {
                indices.push_back(face.mIndices[j]);
            }
        }

        // Store sizes before moving data to avoid use-after-move issues
        const u32 vertexCount = static_cast<u32>(vertices.size());
        const u32 indexCount = static_cast<u32>(indices.size());

        // Create MeshSource with separated data
        auto meshSource = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));

        // Set skeleton and bone data
        if (m_Skeleton)
        {
            meshSource->SetSkeleton(m_Skeleton);
        }

        // Copy bone influences
        meshSource->GetBoneInfluences().Empty();
        meshSource->GetBoneInfluences().Append(boneInfluences);

        OLO_CORE_TRACE("AnimatedModel::ProcessMesh: Set {} bone influences on MeshSource", meshSource->GetBoneInfluences().Num());

        // Copy bone info in correct skeleton order
        meshSource->GetBoneInfo().SetNum(static_cast<i32>(m_Skeleton->m_BoneNames.size()));

        // Initialize all entries with identity transforms and sequential IDs to ensure no uninitialized data
        for (sizet i = 0; i < m_Skeleton->m_BoneNames.size(); ++i)
        {
            meshSource->GetBoneInfo()[static_cast<i32>(i)] = { glm::mat4(1.0f), static_cast<u32>(i) };
        }

        // Overwrite entries with actual bone data from m_BoneInfoMap
        for (const auto& [boneName, boneInfo] : m_BoneInfoMap)
        {
            if (static_cast<i32>(boneInfo.Id) < meshSource->GetBoneInfo().Num())
            {
                meshSource->GetBoneInfo()[boneInfo.Id] = { boneInfo.Offset, boneInfo.Id };
            }
        }

        // Create a submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = indexCount;
        submesh.m_VertexCount = vertexCount;
        submesh.m_MaterialIndex = mesh->mMaterialIndex; // Use actual material index from Assimp
        submesh.m_IsRigged = mesh->mNumBones > 0;       // Set rigged flag based on bone presence
        submesh.m_NodeName = mesh->mName.C_Str();
        meshSource->AddSubmesh(submesh);

        // Extract morph targets (blend shapes) from Assimp anim meshes
        if (mesh->mNumAnimMeshes > 0)
        {
            auto morphTargets = Ref<MorphTargetSet>::Create();
            for (u32 i = 0; i < mesh->mNumAnimMeshes; ++i)
            {
                const aiAnimMesh* animMesh = mesh->mAnimMeshes[i];
                MorphTarget target;
                // Use index-based naming to match morph mesh channel keyframes
                // from ProcessAnimations which uses "MorphTarget_<index>"
                target.Name = "MorphTarget_" + std::to_string(i);

                target.Vertices.resize(animMesh->mNumVertices);
                for (u32 v = 0; v < animMesh->mNumVertices; ++v)
                {
                    MorphTargetVertex delta;
                    if (animMesh->mVertices && mesh->mVertices)
                    {
                        delta.DeltaPosition = glm::vec3(
                            animMesh->mVertices[v].x - mesh->mVertices[v].x,
                            animMesh->mVertices[v].y - mesh->mVertices[v].y,
                            animMesh->mVertices[v].z - mesh->mVertices[v].z);
                    }

                    if (animMesh->mNormals && mesh->mNormals)
                    {
                        delta.DeltaNormal = glm::vec3(
                            animMesh->mNormals[v].x - mesh->mNormals[v].x,
                            animMesh->mNormals[v].y - mesh->mNormals[v].y,
                            animMesh->mNormals[v].z - mesh->mNormals[v].z);
                    }

                    if (animMesh->mTangents && mesh->mTangents)
                    {
                        delta.DeltaTangent = glm::vec3(
                            animMesh->mTangents[v].x - mesh->mTangents[v].x,
                            animMesh->mTangents[v].y - mesh->mTangents[v].y,
                            animMesh->mTangents[v].z - mesh->mTangents[v].z);
                    }

                    target.Vertices[v] = delta;
                }
                morphTargets->AddTarget(std::move(target));
            }
            meshSource->SetMorphTargets(morphTargets);
            OLO_CORE_INFO("AnimatedModel::ProcessMesh: Loaded {} morph targets for mesh '{}'",
                          morphTargets->GetTargetCount(), mesh->mName.C_Str());
        }

        // Build() internally calls OptimizeMesh (which also remaps bone influences)
        // before uploading to GPU — do NOT call OptimizeMesh separately here.
        meshSource->Build();

        return meshSource;
    }

    void AnimatedModel::ProcessBones(const aiMesh* mesh, std::vector<BoneInfluence>& outBoneInfluences)
    {
        OLO_PROFILE_FUNCTION();

        for (u32 boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
        {
            std::string boneName = mesh->mBones[boneIndex]->mName.data;

            // Find the bone info using direct lookup from bone info map (populated during ProcessSkeleton)
            auto it = m_BoneInfoMap.find(boneName);
            if (it == m_BoneInfoMap.end())
            {
                OLO_CORE_WARN("AnimatedModel::ProcessBones: Bone '{}' not found in skeleton", boneName);
                continue;
            }

            u32 skeletonBoneId = it->second.Id;

            auto weights = mesh->mBones[boneIndex]->mWeights;
            u32 numWeights = mesh->mBones[boneIndex]->mNumWeights;

            for (u32 weightIndex = 0; weightIndex < numWeights; ++weightIndex)
            {
                u32 vertexId = weights[weightIndex].mVertexId;
                f32 weight = weights[weightIndex].mWeight;

                if (vertexId >= outBoneInfluences.size())
                {
                    OLO_CORE_WARN("AnimatedModel::ProcessBones: Invalid vertex ID: {}", vertexId);
                    continue;
                }

                // Find an empty slot in the bone influence data
                bool slotFound = false;
                for (u32 i = 0; i < 4; ++i)
                {
                    if (outBoneInfluences[vertexId].m_Weights[i] == 0.0f)
                    {
                        outBoneInfluences[vertexId].SetBoneData(i, skeletonBoneId, weight);
                        slotFound = true;
                        break;
                    }
                }

                if (!slotFound)
                {
                    OLO_CORE_WARN("AnimatedModel::ProcessBones: Vertex {} has more than 4 bone influences, ignoring extra bones", vertexId);
                }
            }
        }

        // Normalize bone weights for all vertices
        for (auto& influence : outBoneInfluences)
        {
            influence.Normalize();
        }
    }

    void AnimatedModel::ProcessSkeleton(const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene->mRootNode)
        {
            OLO_CORE_WARN("AnimatedModel::ProcessSkeleton: No root node found");
            return;
        }

        // Count total bones across all meshes and collect bone info
        std::unordered_set<std::string> uniqueBoneNames;
        std::unordered_map<std::string, glm::mat4> boneOffsetMatrices;

        for (u32 i = 0; i < scene->mNumMeshes; ++i)
        {
            const aiMesh* mesh = scene->mMeshes[i];
            for (u32 j = 0; j < mesh->mNumBones; ++j)
            {
                std::string boneName = mesh->mBones[j]->mName.data;
                uniqueBoneNames.insert(boneName);
                // Store the offset matrix for this bone
                boneOffsetMatrices[boneName] = AssimpMatrixToGLM(mesh->mBones[j]->mOffsetMatrix);
            }
        }

        if (uniqueBoneNames.empty())
        {
            OLO_CORE_INFO("AnimatedModel::ProcessSkeleton: No bones found, creating default skeleton");
            m_Skeleton = Ref<Skeleton>::Create(1);
            m_Skeleton->m_BoneNames = { "Root" };
            m_Skeleton->m_ParentIndices = { -1 };
            m_Skeleton->m_LocalTransforms = { glm::mat4(1.0f) };
            m_Skeleton->m_GlobalTransforms = { glm::mat4(1.0f) };
            m_Skeleton->m_FinalBoneMatrices = { glm::mat4(1.0f) };
            m_Skeleton->m_BindPoseMatrices = { glm::mat4(1.0f) };
            m_Skeleton->m_InverseBindPoses = { glm::mat4(1.0f) };
            return;
        }

        OLO_CORE_INFO("AnimatedModel::ProcessSkeleton: Found {} unique bones", uniqueBoneNames.size());

        // Create skeleton with the correct number of bones
        m_Skeleton = Ref<Skeleton>::Create(uniqueBoneNames.size());

        // Build bone hierarchy by traversing the node tree (DFS order).
        // Assigning indices during DFS guarantees topological ordering:
        // every parent bone receives a lower index than its children,
        // which is required by the iterative global-transform computation.
        // Non-bone nodes between bones have constant transforms that we
        // accumulate into m_BonePreTransforms for animation playback.
        std::unordered_map<std::string, u32> boneNameToIndex;
        u32 nextBoneIndex = 0;

        std::function<void(const aiNode*, i32, const glm::mat4&)> traverseNode =
            [&](const aiNode* node, i32 parentBoneIndex, const glm::mat4& accumulatedNonBoneTransform)
        {
            std::string nodeName = node->mName.data;

            i32 currentBoneIndex = -1;
            glm::mat4 nextAccumulated = glm::mat4(1.0f);

            // Is this node a bone that hasn't been assigned an index yet?
            if (uniqueBoneNames.count(nodeName) && !boneNameToIndex.count(nodeName))
            {
                currentBoneIndex = static_cast<i32>(nextBoneIndex);
                boneNameToIndex[nodeName] = nextBoneIndex;

                m_Skeleton->m_BoneNames[nextBoneIndex] = nodeName;
                m_Skeleton->m_ParentIndices[nextBoneIndex] = parentBoneIndex;
                m_Skeleton->m_LocalTransforms[nextBoneIndex] = AssimpMatrixToGLM(node->mTransformation);
                m_Skeleton->m_BonePreTransforms[nextBoneIndex] = accumulatedNonBoneTransform;
                m_Skeleton->m_GlobalTransforms[nextBoneIndex] = glm::mat4(1.0f);
                m_Skeleton->m_FinalBoneMatrices[nextBoneIndex] = glm::mat4(1.0f);

                ++nextBoneIndex;
                // Reset accumulation — child bones start fresh relative to this bone
                nextAccumulated = glm::mat4(1.0f);
            }
            else if (!uniqueBoneNames.count(nodeName))
            {
                // Non-bone node: accumulate its transform for descendant bones
                nextAccumulated = accumulatedNonBoneTransform * AssimpMatrixToGLM(node->mTransformation);
            }
            else
            {
                // Bone already visited (DAG guard) — just propagate as parent
                currentBoneIndex = static_cast<i32>(boneNameToIndex[nodeName]);
                nextAccumulated = glm::mat4(1.0f);
            }

            // Recursively process children
            for (u32 i = 0; i < node->mNumChildren; ++i)
            {
                traverseNode(node->mChildren[i],
                             (currentBoneIndex != -1) ? currentBoneIndex : parentBoneIndex,
                             nextAccumulated);
            }
        };

        // Start traversal from root node with identity accumulation
        traverseNode(scene->mRootNode, -1, glm::mat4(1.0f));

        // Compute initial global transforms based on hierarchy
        // Pre-transforms account for non-bone intermediate nodes
        for (sizet i = 0; i < m_Skeleton->m_LocalTransforms.size(); ++i)
        {
            i32 parent = m_Skeleton->m_ParentIndices[i];
            if (parent >= 0)
                m_Skeleton->m_GlobalTransforms[i] = m_Skeleton->m_GlobalTransforms[parent] * m_Skeleton->m_BonePreTransforms[i] * m_Skeleton->m_LocalTransforms[i];
            else
                m_Skeleton->m_GlobalTransforms[i] = m_Skeleton->m_BonePreTransforms[i] * m_Skeleton->m_LocalTransforms[i];
        }

        // Set up inverse bind poses and bone info map from bone offset matrices
        for (sizet i = 0; i < m_Skeleton->m_BoneNames.size(); ++i)
        {
            const std::string& boneName = m_Skeleton->m_BoneNames[i];

            // Initialize BoneInfo for efficient lookup during mesh processing
            BoneInfo boneInfo;
            boneInfo.Id = static_cast<u32>(i);

            auto it = boneOffsetMatrices.find(boneName);
            if (it != boneOffsetMatrices.end())
            {
                // Use the offset matrix from the mesh bone as the inverse bind pose
                boneInfo.Offset = it->second;
                m_Skeleton->m_InverseBindPoses[i] = it->second;
                m_Skeleton->m_BindPoseMatrices[i] = glm::inverse(it->second);
            }
            else
            {
                // Fallback: calculate from current global transform
                boneInfo.Offset = glm::inverse(m_Skeleton->m_GlobalTransforms[i]);
                m_Skeleton->m_BindPoseMatrices[i] = m_Skeleton->m_GlobalTransforms[i];
                m_Skeleton->m_InverseBindPoses[i] = glm::inverse(m_Skeleton->m_GlobalTransforms[i]);
            }

            // Store in bone info map for O(1) lookup during mesh processing
            m_BoneInfoMap[boneName] = boneInfo;
        }

        // Capture the bind pose now that all local/global transforms are set.
        // This enables AnimationSystem::Update() to reset bones to their rest
        // pose each frame so non-animated bones (e.g. b_Root_00 in fox.gltf
        // which carries a -90° X rotation) are never overwritten with identity.
        m_Skeleton->SetBindPose();

        OLO_CORE_INFO("AnimatedModel::ProcessSkeleton: Created skeleton with {} bones", m_Skeleton->m_BoneNames.size());
    }

    // Binary search helper function template for finding keyframe indices
    template<typename KeyType>
    u32 FindKeyframeIndex(f64 time, u32 numKeys, const KeyType* keys)
    {
        if (numKeys == 0)
            return 0;
        if (numKeys == 1)
            return 0;

        u32 left = 0;
        u32 right = numKeys - 1;

        // Handle time beyond last keyframe
        if (time >= keys[right].mTime)
            return right - 1;

        // Handle time before first keyframe
        if (time <= keys[0].mTime)
            return 0;

        // Binary search for the correct interval
        while (left < right)
        {
            u32 mid = left + (right - left) / 2;
            if (keys[mid + 1].mTime <= time)
                left = mid + 1;
            else
                right = mid;
        }

        return left;
    }

    // Specialized template for new keyframe structures that use .Time instead of .mTime
    template<typename KeyType>
    u32 FindKeyframeIndexForBoneKeys(f64 time, u32 numKeys, const KeyType* keys)
    {
        if (numKeys == 0)
            return 0;
        if (numKeys == 1)
            return 0;

        u32 left = 0;
        u32 right = numKeys - 1;

        // Handle time beyond last keyframe
        if (time >= keys[right].Time)
            return right - 1;

        // Handle time before first keyframe
        if (time <= keys[0].Time)
            return 0;

        // Binary search for the correct interval
        while (left < right)
        {
            u32 mid = left + (right - left) / 2;
            if (keys[mid + 1].Time <= time)
                left = mid + 1;
            else
                right = mid;
        }

        return left;
    }

    // Helper functions for sampling animation data
    glm::vec3 AnimatedModel::SamplePosition(const aiNodeAnim* nodeAnim, f64 time)
    {
        if (nodeAnim->mNumPositionKeys == 0)
            return glm::vec3(0.0f);

        if (nodeAnim->mNumPositionKeys == 1)
        {
            const aiVector3D& pos = nodeAnim->mPositionKeys[0].mValue;
            return glm::vec3(pos.x, pos.y, pos.z);
        }

        // Find the keyframe index using binary search
        u32 keyIndex = FindKeyframeIndex(time, nodeAnim->mNumPositionKeys, nodeAnim->mPositionKeys);

        // Handle edge case where we're at or beyond the last keyframe
        if (keyIndex >= nodeAnim->mNumPositionKeys - 1)
        {
            const aiVector3D& pos = nodeAnim->mPositionKeys[nodeAnim->mNumPositionKeys - 1].mValue;
            return glm::vec3(pos.x, pos.y, pos.z);
        }

        // Interpolate between the found keyframes
        f64 t = (time - nodeAnim->mPositionKeys[keyIndex].mTime) /
                (nodeAnim->mPositionKeys[keyIndex + 1].mTime - nodeAnim->mPositionKeys[keyIndex].mTime);

        const aiVector3D& pos1 = nodeAnim->mPositionKeys[keyIndex].mValue;
        const aiVector3D& pos2 = nodeAnim->mPositionKeys[keyIndex + 1].mValue;

        return glm::mix(glm::vec3(pos1.x, pos1.y, pos1.z),
                        glm::vec3(pos2.x, pos2.y, pos2.z), (f32)t);
    }

    glm::quat AnimatedModel::SampleRotation(const aiNodeAnim* nodeAnim, f64 time)
    {
        if (nodeAnim->mNumRotationKeys == 0)
            return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        if (nodeAnim->mNumRotationKeys == 1)
        {
            const aiQuaternion& rot = nodeAnim->mRotationKeys[0].mValue;
            return glm::quat(rot.w, rot.x, rot.y, rot.z);
        }

        // Find the keyframe index using binary search
        u32 keyIndex = FindKeyframeIndex(time, nodeAnim->mNumRotationKeys, nodeAnim->mRotationKeys);

        // Handle edge case where we're at or beyond the last keyframe
        if (keyIndex >= nodeAnim->mNumRotationKeys - 1)
        {
            const aiQuaternion& rot = nodeAnim->mRotationKeys[nodeAnim->mNumRotationKeys - 1].mValue;
            return glm::quat(rot.w, rot.x, rot.y, rot.z);
        }

        // Interpolate between the found keyframes
        f64 t = (time - nodeAnim->mRotationKeys[keyIndex].mTime) /
                (nodeAnim->mRotationKeys[keyIndex + 1].mTime - nodeAnim->mRotationKeys[keyIndex].mTime);

        const aiQuaternion& rot1 = nodeAnim->mRotationKeys[keyIndex].mValue;
        const aiQuaternion& rot2 = nodeAnim->mRotationKeys[keyIndex + 1].mValue;

        aiQuaternion result;
        aiQuaternion::Interpolate(result, rot1, rot2, (f32)t);
        return glm::quat(result.w, result.x, result.y, result.z);
    }

    glm::vec3 AnimatedModel::SampleScale(const aiNodeAnim* nodeAnim, f64 time)
    {
        if (nodeAnim->mNumScalingKeys == 0)
            return glm::vec3(1.0f);

        if (nodeAnim->mNumScalingKeys == 1)
        {
            const aiVector3D& scale = nodeAnim->mScalingKeys[0].mValue;
            return glm::vec3(scale.x, scale.y, scale.z);
        }

        // Find the keyframe index using binary search
        u32 keyIndex = FindKeyframeIndex(time, nodeAnim->mNumScalingKeys, nodeAnim->mScalingKeys);

        // Handle edge case where we're at or beyond the last keyframe
        if (keyIndex >= nodeAnim->mNumScalingKeys - 1)
        {
            const aiVector3D& scale = nodeAnim->mScalingKeys[nodeAnim->mNumScalingKeys - 1].mValue;
            return glm::vec3(scale.x, scale.y, scale.z);
        }

        // Interpolate between the found keyframes
        f64 t = (time - nodeAnim->mScalingKeys[keyIndex].mTime) /
                (nodeAnim->mScalingKeys[keyIndex + 1].mTime - nodeAnim->mScalingKeys[keyIndex].mTime);

        const aiVector3D& scale1 = nodeAnim->mScalingKeys[keyIndex].mValue;
        const aiVector3D& scale2 = nodeAnim->mScalingKeys[keyIndex + 1].mValue;

        return glm::mix(glm::vec3(scale1.x, scale1.y, scale1.z),
                        glm::vec3(scale2.x, scale2.y, scale2.z), (f32)t);
    }

    void AnimatedModel::ProcessAnimations(const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        for (u32 i = 0; i < scene->mNumAnimations; ++i)
        {
            const aiAnimation* anim = scene->mAnimations[i];

            auto animClip = Ref<AnimationClip>::Create();
            animClip->Name = anim->mName.data;
            animClip->Duration = static_cast<f32>(anim->mDuration / anim->mTicksPerSecond);

            OLO_CORE_INFO("AnimatedModel::ProcessAnimations: Processing animation [{}] '{}' - Duration: {:.2f}s, Channels: {}",
                          i, animClip->Name.empty() ? "(unnamed)" : animClip->Name, animClip->Duration, anim->mNumChannels);

            // Process animation channels (bone animations)
            for (u32 j = 0; j < anim->mNumChannels; ++j)
            {
                const aiNodeAnim* nodeAnim = anim->mChannels[j];

                BoneAnimation boneAnim;
                boneAnim.BoneName = nodeAnim->mNodeName.data;

                // Store position keyframes directly without merging timestamps
                boneAnim.PositionKeys.reserve(nodeAnim->mNumPositionKeys);
                for (u32 k = 0; k < nodeAnim->mNumPositionKeys; ++k)
                {
                    BonePositionKey posKey;
                    posKey.Time = static_cast<f64>(nodeAnim->mPositionKeys[k].mTime / anim->mTicksPerSecond);
                    const aiVector3D& pos = nodeAnim->mPositionKeys[k].mValue;
                    posKey.Position = glm::vec3(pos.x, pos.y, pos.z);
                    boneAnim.PositionKeys.push_back(posKey);
                }

                // Store rotation keyframes directly
                boneAnim.RotationKeys.reserve(nodeAnim->mNumRotationKeys);
                for (u32 k = 0; k < nodeAnim->mNumRotationKeys; ++k)
                {
                    BoneRotationKey rotKey;
                    rotKey.Time = static_cast<f64>(nodeAnim->mRotationKeys[k].mTime / anim->mTicksPerSecond);
                    const aiQuaternion& rot = nodeAnim->mRotationKeys[k].mValue;
                    rotKey.Rotation = glm::quat(rot.w, rot.x, rot.y, rot.z);
                    boneAnim.RotationKeys.push_back(rotKey);
                }

                // Store scale keyframes directly
                boneAnim.ScaleKeys.reserve(nodeAnim->mNumScalingKeys);
                for (u32 k = 0; k < nodeAnim->mNumScalingKeys; ++k)
                {
                    BoneScaleKey scaleKey;
                    scaleKey.Time = static_cast<f64>(nodeAnim->mScalingKeys[k].mTime / anim->mTicksPerSecond);
                    const aiVector3D& scale = nodeAnim->mScalingKeys[k].mValue;
                    scaleKey.Scale = glm::vec3(scale.x, scale.y, scale.z);
                    boneAnim.ScaleKeys.push_back(scaleKey);
                }

                animClip->BoneAnimations.push_back(boneAnim);
            }

            // Import morph mesh channels as morph target keyframes
            for (u32 j = 0; j < anim->mNumMorphMeshChannels; ++j)
            {
                const aiMeshMorphAnim* morphChannel = anim->mMorphMeshChannels[j];
                for (u32 k = 0; k < morphChannel->mNumKeys; ++k)
                {
                    const aiMeshMorphKey& key = morphChannel->mKeys[k];
                    f64 timeInSeconds = key.mTime / anim->mTicksPerSecond;
                    for (u32 w = 0; w < key.mNumValuesAndWeights; ++w)
                    {
                        MorphTargetKeyframe kf;
                        kf.Time = timeInSeconds;
                        kf.TargetName = "MorphTarget_" + std::to_string(key.mValues[w]);
                        kf.Weight = static_cast<f32>(key.mWeights[w]);
                        animClip->MorphKeyframes.push_back(kf);
                    }
                }
            }

            m_Animations.push_back(animClip);
        }

        OLO_CORE_INFO("AnimatedModel::ProcessAnimations: Successfully processed {} animations", m_Animations.size());
    }

    // New optimized sampling functions for separate keyframe channels
    glm::vec3 AnimatedModel::SampleBonePosition(const std::vector<BonePositionKey>& keys, f32 time)
    {
        if (keys.empty())
            return glm::vec3(0.0f);

        if (keys.size() == 1)
            return keys[0].Position;

        // Convert time to double for precision during search
        f64 searchTime = static_cast<f64>(time);

        // Find the keyframe index using binary search
        u32 keyIndex = FindKeyframeIndexForBoneKeys(searchTime, static_cast<u32>(keys.size()), keys.data());

        // Handle edge case where we're at or beyond the last keyframe
        if (keyIndex >= keys.size() - 1)
            return keys[keys.size() - 1].Position;

        // Interpolate between the found keyframes
        f64 t = (searchTime - keys[keyIndex].Time) /
                (keys[keyIndex + 1].Time - keys[keyIndex].Time);

        return glm::mix(keys[keyIndex].Position, keys[keyIndex + 1].Position, static_cast<f32>(t));
    }

    glm::quat AnimatedModel::SampleBoneRotation(const std::vector<BoneRotationKey>& keys, f32 time)
    {
        if (keys.empty())
            return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        if (keys.size() == 1)
            return keys[0].Rotation;

        // Convert time to double for precision during search
        f64 searchTime = static_cast<f64>(time);

        // Find the keyframe index using binary search
        u32 keyIndex = FindKeyframeIndexForBoneKeys(searchTime, static_cast<u32>(keys.size()), keys.data());

        // Handle edge case where we're at or beyond the last keyframe
        if (keyIndex >= keys.size() - 1)
            return keys[keys.size() - 1].Rotation;

        // Interpolate between the found keyframes using slerp
        f64 t = (searchTime - keys[keyIndex].Time) /
                (keys[keyIndex + 1].Time - keys[keyIndex].Time);

        return glm::slerp(keys[keyIndex].Rotation, keys[keyIndex + 1].Rotation, static_cast<f32>(t));
    }

    glm::vec3 AnimatedModel::SampleBoneScale(const std::vector<BoneScaleKey>& keys, f32 time)
    {
        if (keys.empty())
            return glm::vec3(1.0f);

        if (keys.size() == 1)
            return keys[0].Scale;

        // Convert time to double for precision during search
        f64 searchTime = static_cast<f64>(time);

        // Find the keyframe index using binary search
        u32 keyIndex = FindKeyframeIndexForBoneKeys(searchTime, static_cast<u32>(keys.size()), keys.data());

        // Handle edge case where we're at or beyond the last keyframe
        if (keyIndex >= keys.size() - 1)
            return keys[keys.size() - 1].Scale;

        // Interpolate between the found keyframes
        f64 t = (searchTime - keys[keyIndex].Time) /
                (keys[keyIndex + 1].Time - keys[keyIndex].Time);

        return glm::mix(keys[keyIndex].Scale, keys[keyIndex + 1].Scale, static_cast<f32>(t));
    }

    std::vector<Ref<Texture2D>> AnimatedModel::LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type)
    {
        std::vector<Ref<Texture2D>> textures;

        for (u32 i = 0; i < mat->GetTextureCount(type); i++)
        {
            aiString str;
            mat->GetTexture(type, i, &str);

            std::string filename = str.C_Str();
            std::filesystem::path path = std::filesystem::path(m_Directory) / filename;

            // Check if texture was loaded before
            if (m_LoadedTextures.find(path.string()) != m_LoadedTextures.end())
            {
                textures.push_back(m_LoadedTextures[path.string()]);
            }
            else
            {
                auto texture = Texture2D::Create(path.string());
                if (texture && texture->IsLoaded())
                {
                    textures.push_back(texture);
                    m_LoadedTextures[path.string()] = texture;
                }
                else
                {
                    // Fallback: try preserving the relative subdirectory from the texture path
                    std::filesystem::path filenamePath(filename);
                    std::filesystem::path filenameOnly = filenamePath.filename();
                    bool loaded = false;

                    // If the texture reference has a relative subdirectory (e.g. "textures/Fur.TGA"),
                    // try scanning that subdirectory first before falling back to model root.
                    if (filenamePath.has_parent_path() && filenamePath.parent_path() != ".")
                    {
                        auto subdir = std::filesystem::path(m_Directory) / filenamePath.parent_path();
                        auto discovered = FindTextureInDirectory(subdir, filenameOnly);
                        if (!discovered.empty())
                        {
                            std::string discoveredStr = discovered.string();
                            if (m_LoadedTextures.find(discoveredStr) != m_LoadedTextures.end())
                            {
                                textures.push_back(m_LoadedTextures[discoveredStr]);
                                loaded = true;
                            }
                            else
                            {
                                auto discoveredTexture = Texture2D::Create(discoveredStr);
                                if (discoveredTexture && discoveredTexture->IsLoaded())
                                {
                                    m_LoadedTextures[discoveredStr] = discoveredTexture;
                                    textures.push_back(discoveredTexture);
                                    loaded = true;
                                }
                            }
                        }
                    }

                    // Fallback: try just the filename in the model root directory
                    if (!loaded)
                    {
                        std::filesystem::path fallbackPath = std::filesystem::path(m_Directory) / filenameOnly;
                        std::string fallbackPathStr = fallbackPath.string();

                        if (fallbackPathStr != path.string() && m_LoadedTextures.find(fallbackPathStr) != m_LoadedTextures.end())
                        {
                            textures.push_back(m_LoadedTextures[fallbackPathStr]);
                            loaded = true;
                        }
                        else if (fallbackPathStr != path.string())
                        {
                            OLO_CORE_WARN("AnimatedModel::LoadMaterialTextures: '{}' not found, trying fallback '{}'",
                                          path.string(), fallbackPathStr);
                            auto fallbackTexture = Texture2D::Create(fallbackPathStr);
                            if (fallbackTexture && fallbackTexture->IsLoaded())
                            {
                                textures.push_back(fallbackTexture);
                                m_LoadedTextures[fallbackPathStr] = fallbackTexture;
                                loaded = true;
                            }
                        }
                    }

                    // Fallback: scan model directory for case-insensitive stem match with any image extension.
                    // Handles FBX referencing .tga when the actual file is .png, and case mismatches on Linux.
                    if (!loaded)
                    {
                        auto discovered = FindTextureInDirectory(std::filesystem::path(m_Directory), filenameOnly);
                        if (!discovered.empty())
                        {
                            std::string discoveredStr = discovered.string();
                            OLO_CORE_WARN("AnimatedModel::LoadMaterialTextures: Discovered '{}' via directory scan for '{}'",
                                          discoveredStr, filenameOnly.string());
                            if (m_LoadedTextures.find(discoveredStr) != m_LoadedTextures.end())
                            {
                                textures.push_back(m_LoadedTextures[discoveredStr]);
                                loaded = true;
                            }
                            else
                            {
                                auto discoveredTexture = Texture2D::Create(discoveredStr);
                                if (discoveredTexture && discoveredTexture->IsLoaded())
                                {
                                    m_LoadedTextures[discoveredStr] = discoveredTexture;
                                    textures.push_back(discoveredTexture);
                                    loaded = true;
                                }
                            }
                        }
                    }

                    if (!loaded)
                    {
                        OLO_CORE_WARN("AnimatedModel::LoadMaterialTextures: Failed to load texture '{}' (tried original, filename-only, and directory scan)",
                                      path.string());
                    }
                }
            }
        }

        return textures;
    }

    Material AnimatedModel::ProcessMaterial(const aiMaterial* mat)
    {
        // Get material name
        aiString name;
        mat->Get(AI_MATKEY_NAME, name);
        std::string materialName = name.length > 0 ? name.C_Str() : "Animated Model Material";

        // Get base color factor
        aiColor3D baseColor(1.0f, 1.0f, 1.0f);
        mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor);

        // Get metallic and roughness factors
        float metallic = 0.0f;
        float roughness = 0.5f;
        mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);

        // Create material with extracted properties
        auto materialRef = Material::CreatePBR(
            materialName,
            glm::vec3(baseColor.r, baseColor.g, baseColor.b),
            metallic,
            roughness);

        Material material;
        if (materialRef)
        {
            material = *materialRef; // Copy to value type for struct-like access
        }
        else
        {
            OLO_CORE_ERROR("AnimatedModel::ProcessMaterial: Failed to create PBR material '{}'", materialName);
            // Use default constructed material as fallback
            material = Material{};
        }

        // Load PBR textures
        auto albedoMaps = LoadMaterialTextures(mat, aiTextureType_DIFFUSE);
        if (albedoMaps.empty())
        {
            // Try base color for newer PBR/glTF materials
            albedoMaps = LoadMaterialTextures(mat, aiTextureType_BASE_COLOR);
        }
        if (!albedoMaps.empty())
        {
            material.SetAlbedoMap(albedoMaps[0]);
        }

        auto metallicRoughnessMaps = LoadMaterialTextures(mat, aiTextureType_METALNESS);
        if (!metallicRoughnessMaps.empty())
        {
            material.SetMetallicRoughnessMap(metallicRoughnessMaps[0]);
        }

        auto normalMaps = LoadMaterialTextures(mat, aiTextureType_NORMALS);
        if (!normalMaps.empty())
        {
            material.SetNormalMap(normalMaps[0]);
        }

        auto aoMaps = LoadMaterialTextures(mat, aiTextureType_AMBIENT_OCCLUSION);
        if (!aoMaps.empty())
        {
            material.SetAOMap(aoMaps[0]);
        }

        auto emissiveMaps = LoadMaterialTextures(mat, aiTextureType_EMISSIVE);
        if (!emissiveMaps.empty())
        {
            material.SetEmissiveMap(emissiveMaps[0]);
        }

        return material;
    }

    void AnimatedModel::CalculateBounds()
    {
        if (m_Meshes.empty())
        {
            m_BoundingBox = BoundingBox();
            m_BoundingSphere = BoundingSphere();
            return;
        }

        // Start with the first mesh bounds
        m_BoundingBox = m_Meshes[0]->GetBoundingBox();

        // Expand to include all meshes
        for (sizet i = 1; i < m_Meshes.size(); ++i)
        {
            m_BoundingBox = m_BoundingBox.Union(m_Meshes[i]->GetBoundingBox());
        }

        // Calculate bounding sphere from bounding box
        m_BoundingSphere = BoundingSphere(m_BoundingBox);
    }

    Ref<AnimationClip> AnimatedModel::GetAnimation(const std::string& name) const
    {
        for (const auto& animation : m_Animations)
        {
            if (animation->Name == name)
            {
                return animation;
            }
        }
        return nullptr;
    }

    glm::mat4 AnimatedModel::AssimpMatrixToGLM(const aiMatrix4x4& from)
    {
        glm::mat4 to;

        to[0][0] = from.a1;
        to[1][0] = from.a2;
        to[2][0] = from.a3;
        to[3][0] = from.a4;
        to[0][1] = from.b1;
        to[1][1] = from.b2;
        to[2][1] = from.b3;
        to[3][1] = from.b4;
        to[0][2] = from.c1;
        to[1][2] = from.c2;
        to[2][2] = from.c3;
        to[3][2] = from.c4;
        to[0][3] = from.d1;
        to[1][3] = from.d2;
        to[2][3] = from.d3;
        to[3][3] = from.d4;

        return to;
    }

    u32 AnimatedModel::FindBoneIndex(const std::string& boneName)
    {
        auto it = m_BoneInfoMap.find(boneName);
        if (it != m_BoneInfoMap.end())
        {
            return it->second.Id;
        }
        return 0; // Default to first bone if not found
    }
} // namespace OloEngine
