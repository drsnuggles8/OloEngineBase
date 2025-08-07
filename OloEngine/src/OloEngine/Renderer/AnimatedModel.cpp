#include "OloEnginePCH.h"
#include "AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include <filesystem>
#include <set>

namespace OloEngine
{
    AnimatedModel::AnimatedModel(const std::string& path)
    {
        LoadModel(path);
    }

    void AnimatedModel::LoadModel(const std::string& path)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_INFO("AnimatedModel::LoadModel: Loading animated model from {}", path);

        // Create an instance of the Importer class
        Assimp::Importer importer;

        // Set import flags for skeletal animation
        u32 importFlags = 
            aiProcess_Triangulate |           // Make sure we get triangles
            aiProcess_GenNormals |           // Create normals if not present
            aiProcess_CalcTangentSpace |     // Calculate tangents and bitangents
            aiProcess_FlipUVs |             // Flip texture coordinates
            aiProcess_ValidateDataStructure | // Validate the imported data structure
            aiProcess_LimitBoneWeights |     // Limit bone weights to 4 per vertex
            aiProcess_GlobalScale;           // Apply global scale

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

        // Process all meshes
        ProcessNode(scene->mRootNode, scene);

        // Process animations
        ProcessAnimations(scene);

        // Calculate bounding volumes for the entire model
        CalculateBounds();

        OLO_CORE_INFO("AnimatedModel::LoadModel: Successfully loaded animated model with {} meshes, {} animations", 
                     m_Meshes.size(), m_Animations.size());
    }

    void AnimatedModel::ProcessNode(const aiNode* node, const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        // Process all the node's meshes
        for (u32 i = 0; i < node->mNumMeshes; i++)
        {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            auto skinnedMesh = ProcessMesh(mesh, scene);
            if (skinnedMesh)
            {
                m_Meshes.push_back(skinnedMesh);
                
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
                    material = *defaultMaterialRef; // Copy to value type for struct-like access
                }
                m_Materials.push_back(material);
            }
        }

        // Process all the node's children
        for (u32 i = 0; i < node->mNumChildren; i++)
        {
            ProcessNode(node->mChildren[i], scene);
        }
    }

    Ref<SkinnedMesh> AnimatedModel::ProcessMesh(const aiMesh* mesh, const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<SkinnedVertex> vertices;
        std::vector<u32> indices;

        OLO_CORE_TRACE("AnimatedModel::ProcessMesh: Processing mesh with {} vertices, {} faces, {} bones", 
                       mesh->mNumVertices, mesh->mNumFaces, mesh->mNumBones);

        // Process vertices
        for (u32 i = 0; i < mesh->mNumVertices; i++)
        {
            SkinnedVertex vertex;

            // Position
            vertex.Position = glm::vec3(
                mesh->mVertices[i].x,
                mesh->mVertices[i].y,
                mesh->mVertices[i].z
            );

            // Normal
            if (mesh->HasNormals())
            {
                vertex.Normal = glm::vec3(
                    mesh->mNormals[i].x,
                    mesh->mNormals[i].y,
                    mesh->mNormals[i].z
                );
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
                    mesh->mTextureCoords[0][i].y
                );
            }
            else
            {
                vertex.TexCoord = glm::vec2(0.0f);
            }

            // Initialize bone data (will be filled by ProcessBones)
            vertex.BoneIndices = glm::ivec4(-1);
            vertex.BoneWeights = glm::vec4(0.0f);

            vertices.push_back(vertex);
        }

        // Process bone data if available
        if (mesh->mNumBones > 0)
        {
            ProcessBones(mesh, vertices);
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

        // Create and return the skinned mesh
        auto skinnedMesh = Ref<SkinnedMesh>::Create(std::move(vertices), std::move(indices));
        skinnedMesh->Build();

        return skinnedMesh;
    }

    void AnimatedModel::ProcessBones(const aiMesh* mesh, std::vector<SkinnedVertex>& vertices)
    {
        OLO_PROFILE_FUNCTION();

        for (u32 boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
        {
            std::string boneName = mesh->mBones[boneIndex]->mName.data;
            
            // Find the bone index in the skeleton
            u32 skeletonBoneId = 0;
            bool foundBone = false;
            if (m_Skeleton)
            {
                for (size_t i = 0; i < m_Skeleton->m_BoneNames.size(); ++i)
                {
                    if (m_Skeleton->m_BoneNames[i] == boneName)
                    {
                        skeletonBoneId = static_cast<u32>(i);
                        foundBone = true;
                        break;
                    }
                }
            }
            
            if (!foundBone)
            {
                OLO_CORE_WARN("AnimatedModel::ProcessBones: Bone '{}' not found in skeleton", boneName);
                continue;
            }

            // Update the bone info map with the correct skeleton index
            if (m_BoneInfoMap.find(boneName) == m_BoneInfoMap.end())
            {
                BoneInfo newBoneInfo;
                newBoneInfo.Id = skeletonBoneId;
                newBoneInfo.Offset = AssimpMatrixToGLM(mesh->mBones[boneIndex]->mOffsetMatrix);
                m_BoneInfoMap[boneName] = newBoneInfo;
            }

            auto weights = mesh->mBones[boneIndex]->mWeights;
            u32 numWeights = mesh->mBones[boneIndex]->mNumWeights;

            for (u32 weightIndex = 0; weightIndex < numWeights; ++weightIndex)
            {
                u32 vertexId = weights[weightIndex].mVertexId;
                f32 weight = weights[weightIndex].mWeight;

                if (vertexId >= vertices.size())
                {
                    OLO_CORE_WARN("AnimatedModel::ProcessBones: Invalid vertex ID: {}", vertexId);
                    continue;
                }

                // Find an empty slot in the bone data
                for (u32 i = 0; i < 4; ++i)
                {
                    if (vertices[vertexId].BoneIndices[i] == -1)
                    {
                        vertices[vertexId].BoneIndices[i] = static_cast<i32>(skeletonBoneId);
                        vertices[vertexId].BoneWeights[i] = weight;
                        break;
                    }
                }
            }
        }

        // Normalize bone weights
        for (auto& vertex : vertices)
        {
            f32 totalWeight = vertex.BoneWeights.x + vertex.BoneWeights.y + 
                             vertex.BoneWeights.z + vertex.BoneWeights.w;
            if (totalWeight > 0.0f)
            {
                vertex.BoneWeights /= totalWeight;
            }
            else
            {
                // If no bone weights, assign to bone 0 with full weight
                if (m_Skeleton && !m_Skeleton->m_BoneNames.empty())
                {
                    vertex.BoneIndices = glm::ivec4(0, -1, -1, -1);
                    vertex.BoneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                }
                else
                {
                    // Keep default values (-1 indices, 0 weights)
                    OLO_CORE_WARN("No skeleton available for skinning vertex without bone weights");
                }
            }
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

        // Create ordered list of bone names and build name-to-index mapping
        std::vector<std::string> orderedBoneNames(uniqueBoneNames.begin(), uniqueBoneNames.end());
        std::unordered_map<std::string, u32> boneNameToIndex;
        for (u32 i = 0; i < orderedBoneNames.size(); ++i)
        {
            boneNameToIndex[orderedBoneNames[i]] = i;
            m_Skeleton->m_BoneNames[i] = orderedBoneNames[i];
            m_Skeleton->m_ParentIndices[i] = -1; // Initialize to no parent
            m_Skeleton->m_LocalTransforms[i] = glm::mat4(1.0f);
            m_Skeleton->m_GlobalTransforms[i] = glm::mat4(1.0f);
            m_Skeleton->m_FinalBoneMatrices[i] = glm::mat4(1.0f);
        }

        // Build proper bone hierarchy by traversing the node tree
        std::function<void(const aiNode*, i32)> traverseNode = [&](const aiNode* node, i32 parentBoneIndex)
        {
            std::string nodeName = node->mName.data;
            
            // Check if this node is a bone
            auto it = boneNameToIndex.find(nodeName);
            i32 currentBoneIndex = -1;
            if (it != boneNameToIndex.end())
            {
                currentBoneIndex = static_cast<i32>(it->second);
                m_Skeleton->m_ParentIndices[currentBoneIndex] = parentBoneIndex;
                
                // Set the node's transformation as the local transform
                m_Skeleton->m_LocalTransforms[currentBoneIndex] = AssimpMatrixToGLM(node->mTransformation);
            }

            // Recursively process children
            for (u32 i = 0; i < node->mNumChildren; ++i)
            {
                traverseNode(node->mChildren[i], (currentBoneIndex != -1) ? currentBoneIndex : parentBoneIndex);
            }
        };

        // Start traversal from root node
        traverseNode(scene->mRootNode, -1);

        // Compute initial global transforms based on hierarchy
        for (size_t i = 0; i < m_Skeleton->m_LocalTransforms.size(); ++i)
        {
            i32 parent = m_Skeleton->m_ParentIndices[i];
            if (parent >= 0)
                m_Skeleton->m_GlobalTransforms[i] = m_Skeleton->m_GlobalTransforms[parent] * m_Skeleton->m_LocalTransforms[i];
            else
                m_Skeleton->m_GlobalTransforms[i] = m_Skeleton->m_LocalTransforms[i];
        }

        // Set up inverse bind poses from bone offset matrices
        for (size_t i = 0; i < m_Skeleton->m_BoneNames.size(); ++i)
        {
            const std::string& boneName = m_Skeleton->m_BoneNames[i];
            auto it = boneOffsetMatrices.find(boneName);
            if (it != boneOffsetMatrices.end())
            {
                // Use the offset matrix from the mesh bone as the inverse bind pose
                m_Skeleton->m_InverseBindPoses[i] = it->second;
                m_Skeleton->m_BindPoseMatrices[i] = glm::inverse(it->second);
            }
            else
            {
                // Fallback: calculate from current global transform
                m_Skeleton->m_BindPoseMatrices[i] = m_Skeleton->m_GlobalTransforms[i];
                m_Skeleton->m_InverseBindPoses[i] = glm::inverse(m_Skeleton->m_GlobalTransforms[i]);
            }
        }

        OLO_CORE_INFO("AnimatedModel::ProcessSkeleton: Created skeleton with {} bones", m_Skeleton->m_BoneNames.size());
    }

    // Binary search helper function template for finding keyframe indices
    template<typename KeyType>
    u32 FindKeyframeIndex(f64 time, u32 numKeys, const KeyType* keys)
    {
        if (numKeys == 0) return 0;
        if (numKeys == 1) return 0;
        
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
        if (numKeys == 0) return 0;
        if (numKeys == 1) return 0;
        
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

            OLO_CORE_INFO("AnimatedModel::ProcessAnimations: Processing animation '{}' - Duration: {:.2f}s, Channels: {}", 
                         animClip->Name, animClip->Duration, anim->mNumChannels);

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
                if (texture)
                {
                    textures.push_back(texture);
                    m_LoadedTextures[path.string()] = texture;
                }
                else
                {
                    OLO_CORE_WARN("AnimatedModel::LoadMaterialTextures: Failed to load texture: {}", path.string());
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
            roughness
        );
        Material material = *materialRef; // Copy to value type for struct-like access
        
        // Load PBR textures
        auto albedoMaps = LoadMaterialTextures(mat, aiTextureType_DIFFUSE);
        if (!albedoMaps.empty())
        {
            material.AlbedoMap = albedoMaps[0];
        }
        
        auto metallicRoughnessMaps = LoadMaterialTextures(mat, aiTextureType_METALNESS);
        if (!metallicRoughnessMaps.empty())
        {
            material.MetallicRoughnessMap = metallicRoughnessMaps[0];
        }
        
        auto normalMaps = LoadMaterialTextures(mat, aiTextureType_NORMALS);
        if (!normalMaps.empty())
        {
            material.NormalMap = normalMaps[0];
        }
        
        auto aoMaps = LoadMaterialTextures(mat, aiTextureType_AMBIENT_OCCLUSION);
        if (!aoMaps.empty())
        {
            material.AOMap = aoMaps[0];
        }
        
        auto emissiveMaps = LoadMaterialTextures(mat, aiTextureType_EMISSIVE);
        if (!emissiveMaps.empty())
        {
            material.EmissiveMap = emissiveMaps[0];
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
        
        to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
        to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
        to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
        to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
        
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
}
