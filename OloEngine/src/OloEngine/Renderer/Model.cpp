// TODO(OloEngine): When implementing the asset pipeline for animated models, ensure that
// AnimatedMeshComponent, AnimationStateComponent, and SkeletonComponent are assigned to entities
// upon import. This is required for ECS-driven animated mesh support.
#include <filesystem>

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Task/ParallelFor.h"

namespace OloEngine
{
    Model::Model(const std::string& path, const TextureOverride& textureOverride, bool flipUV)
        : m_TextureOverride(textureOverride.HasAnyTexture() ? std::optional<TextureOverride>(textureOverride) : std::nullopt),
          m_FlipUV(flipUV)
    {
        LoadModel(path, textureOverride, flipUV);
    }

    void Model::LoadModel(const std::string& path, const TextureOverride& textureOverride, bool flipUV)
    {
        OLO_PROFILE_FUNCTION();

        // Store texture override and UV flip setting for use in material processing
        m_TextureOverride = textureOverride.HasAnyTexture() ? std::optional<TextureOverride>(textureOverride) : std::nullopt;
        m_FlipUV = flipUV;

        // Create an instance of the Importer class
        Assimp::Importer importer;

        // And have it read the given file with some postprocessing
        const aiScene* scene = importer.ReadFile(path,
                                                 aiProcess_Triangulate |             // Make sure we get triangles
                                                     aiProcess_GenNormals |          // Create normals if not present
                                                     aiProcess_CalcTangentSpace |    // Calculate tangents and bitangents
                                                     aiProcess_FlipUVs |             // Flip texture coordinates
                                                     aiProcess_ValidateDataStructure // Validate the imported data structure
        );

        // Check for errors
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            OLO_CORE_ERROR("ASSIMP Error: {0}", importer.GetErrorString());
            return;
        }

        // Store the directory path
        m_Directory = std::filesystem::path(path).parent_path().string();

        OLO_CORE_INFO("Loading model: {0} ({1} meshes, {2} materials)", path, scene->mNumMeshes, scene->mNumMaterials);

        // Reserve space for expected number of meshes and materials to reduce allocations
        m_Meshes.reserve(scene->mNumMeshes);
        m_Materials.reserve(scene->mNumMaterials);

        // Pre-size the material index map to reduce rehashing overhead
        m_MaterialIndexMap.reserve(scene->mNumMaterials);

        // Process all the nodes recursively
        ProcessNode(scene->mRootNode, scene);

        // Calculate bounding volumes for the entire model
        CalculateBounds();

        OLO_CORE_INFO("Model loaded successfully: {0} meshes processed", m_Meshes.size());
    }

    void Model::ProcessNode(const aiNode* node, const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        // Process all the node's meshes (if any)
        for (u32 i = 0; i < node->mNumMeshes; i++)
        {
            const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            m_Meshes.push_back(ProcessMesh(mesh, scene));
        }

        // Then do the same for each of its children
        for (u32 i = 0; i < node->mNumChildren; i++)
        {
            ProcessNode(node->mChildren[i], scene);
        }
    }

    Ref<Mesh> Model::ProcessMesh(const aiMesh* mesh, const aiScene* scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        // Reserve space to reduce allocations during mesh processing
        vertices.resize(mesh->mNumVertices);  // Use resize so we can write in parallel
        indices.reserve(mesh->mNumFaces * 3); // Assuming triangulated mesh

        // Threshold for parallel vertex processing
        constexpr u32 PARALLEL_VERTEX_THRESHOLD = 1024;
        const bool flipUV = m_FlipUV;
        const bool hasNormals = mesh->HasNormals();
        const bool hasTexCoords = mesh->mTextureCoords[0] != nullptr;

        if (mesh->mNumVertices >= PARALLEL_VERTEX_THRESHOLD)
        {
            // Process vertices in parallel
            ParallelFor(
                "Model::ProcessMesh::Vertices",
                static_cast<i32>(mesh->mNumVertices),
                256, // MinBatchSize
                [&vertices, mesh, flipUV, hasNormals, hasTexCoords](i32 i)
                {
                    Vertex& vertex = vertices[i];

                    // Process vertex positions, normals and texture coordinates
                    vertex.Position = glm::vec3(
                        mesh->mVertices[i].x,
                        mesh->mVertices[i].y,
                        mesh->mVertices[i].z);

                    if (hasNormals)
                    {
                        vertex.Normal = glm::vec3(
                            mesh->mNormals[i].x,
                            mesh->mNormals[i].y,
                            mesh->mNormals[i].z);
                    }

                    if (hasTexCoords)
                    {
                        vertex.TexCoord = glm::vec2(
                            mesh->mTextureCoords[0][i].x,
                            mesh->mTextureCoords[0][i].y);

                        if (flipUV)
                        {
                            vertex.TexCoord.y = 1.0f - vertex.TexCoord.y;
                        }
                    }
                    else
                    {
                        vertex.TexCoord = glm::vec2(0.0f, 0.0f);
                    }
                });
        }
        else
        {
            // Process vertices sequentially for small meshes
            for (u32 i = 0; i < mesh->mNumVertices; i++)
            {
                Vertex& vertex = vertices[i];

                vertex.Position = glm::vec3(
                    mesh->mVertices[i].x,
                    mesh->mVertices[i].y,
                    mesh->mVertices[i].z);

                if (hasNormals)
                {
                    vertex.Normal = glm::vec3(
                        mesh->mNormals[i].x,
                        mesh->mNormals[i].y,
                        mesh->mNormals[i].z);
                }

                if (hasTexCoords)
                {
                    vertex.TexCoord = glm::vec2(
                        mesh->mTextureCoords[0][i].x,
                        mesh->mTextureCoords[0][i].y);

                    if (flipUV)
                    {
                        vertex.TexCoord.y = 1.0f - vertex.TexCoord.y;
                    }
                }
                else
                {
                    vertex.TexCoord = glm::vec2(0.0f, 0.0f);
                }
            }
        }

        for (u32 i = 0; i < mesh->mNumFaces; i++)
        {
            const aiFace face = mesh->mFaces[i];
            for (u32 j = 0; j < face.mNumIndices; j++)
            {
                indices.push_back(face.mIndices[j]);
            }
        }

        if (mesh->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

            // Check if we already processed this material
            auto mapIt = m_MaterialIndexMap.find(mesh->mMaterialIndex);
            if (mapIt == m_MaterialIndexMap.end())
            {
                // Check for potential overflow before casting to u32
                if (m_Materials.size() >= UINT32_MAX)
                {
                    OLO_CORE_ERROR("Model: Material count exceeds u32 maximum ({}), cannot add more materials", UINT32_MAX);
                    return nullptr; // Early return to prevent overflow
                }

                // Add new material and create mapping
                u32 newMaterialIndex = static_cast<u32>(m_Materials.size());
                m_Materials.push_back(ProcessMaterial(material));
                m_MaterialIndexMap[mesh->mMaterialIndex] = newMaterialIndex;
            }
        }

        // Store sizes before moving to avoid undefined behavior
        const sizet indexCount = indices.size();
        const sizet vertexCount = vertices.size();

        // Check for potential overflow before creating meshSource and moving data
        if (indexCount > UINT32_MAX)
        {
            OLO_CORE_ERROR("Model: Index count exceeds u32 maximum ({}), mesh too large", UINT32_MAX);
            return nullptr;
        }
        if (vertexCount > UINT32_MAX)
        {
            OLO_CORE_ERROR("Model: Vertex count exceeds u32 maximum ({}), mesh too large", UINT32_MAX);
            return nullptr;
        }

        auto meshSource = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;

        // Use stored sizes since vectors have been moved
        submesh.m_IndexCount = static_cast<u32>(indexCount);
        submesh.m_VertexCount = static_cast<u32>(vertexCount);

        // Use mapped material index with bounds checking
        auto mapIt = m_MaterialIndexMap.find(mesh->mMaterialIndex);
        if (mapIt != m_MaterialIndexMap.end() && mapIt->second < m_Materials.size())
        {
            submesh.m_MaterialIndex = mapIt->second;
        }
        else
        {
            // Always use sentinel value for missing/invalid materials
            submesh.m_MaterialIndex = UINT32_MAX;
            OLO_CORE_WARN("Model: Invalid or missing material mapping for mesh '{}', using fallback", mesh->mName.C_Str());
        }

        submesh.m_IsRigged = false;
        submesh.m_NodeName = mesh->mName.C_Str();
        meshSource->AddSubmesh(submesh);

        meshSource->Build();

        // Create Mesh objects for all submeshes in the MeshSource
        // Note: Currently each Assimp mesh creates one submesh, but this future-proofs
        // for cases where a MeshSource might have multiple submeshes
        const auto& submeshes = meshSource->GetSubmeshes();
        Ref<Mesh> primaryMesh = Ref<Mesh>::Create(meshSource, 0);

        // If there are additional submeshes, we should handle them
        // For now, we'll just return the primary mesh since the current Assimp processing
        // creates one submesh per Assimp mesh. Future enhancement: modify calling code
        // to handle multiple meshes per MeshSource
        if (submeshes.Num() > 1)
        {
            OLO_CORE_WARN("Model: MeshSource has {} submeshes but only returning first. Consider updating Model loading to handle multiple submeshes per MeshSource.", submeshes.Num());
        }

        return primaryMesh;
    }

    std::vector<Ref<Texture2D>> Model::LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Ref<Texture2D>> textures;

        for (u32 i = 0; i < mat->GetTextureCount(type); i++)
        {
            aiString str;
            mat->GetTexture(type, i, &str);

            std::filesystem::path relativePath = str.C_Str();
            std::filesystem::path texturePath = std::filesystem::path(m_Directory) / relativePath;
            std::string texturePathStr = texturePath.string();

            if (!m_LoadedTextures.contains(texturePathStr))
            {
                Ref<Texture2D> texture = Texture2D::Create(texturePathStr);

                if (texture && texture->IsLoaded())
                {
                    m_LoadedTextures[texturePathStr] = texture;
                    textures.push_back(texture);
                }
            }
            else
            {
                textures.push_back(m_LoadedTextures[texturePathStr]);
            }
        }

        return textures;
    }

    Ref<Material> Model::ProcessMaterial(const aiMaterial* mat)
    {
        aiString name;
        mat->Get(AI_MATKEY_NAME, name);
        std::string materialName = name.length > 0 ? name.C_Str() : "PBR Model Material";

        // Get base color factor
        aiColor3D baseColor(1.0f, 1.0f, 1.0f);
        mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor);

        // Get metallic and roughness factors
        float metallic = 0.0f;
        float roughness = 0.5f;
        mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);

        // If texture overrides are used, set base color to white so texture colors come through properly
        glm::vec3 finalBaseColor = glm::vec3(baseColor.r, baseColor.g, baseColor.b);
        if (m_TextureOverride && !m_TextureOverride->AlbedoPath.empty())
        {
            finalBaseColor = glm::vec3(1.0f, 1.0f, 1.0f);
        }

        auto materialRef = Material::CreatePBR(
            materialName,
            finalBaseColor,
            metallic,
            roughness);

        // Load PBR textures - prioritize overrides if provided

        // Albedo/Diffuse textures
        if (m_TextureOverride && !m_TextureOverride->AlbedoPath.empty())
        {
            auto overrideTexture = Texture2D::Create(m_TextureOverride->AlbedoPath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetAlbedoMap(overrideTexture);
            }
        }
        else
        {
            // Fall back to FBX textures
            auto albedoMaps = LoadMaterialTextures(mat, aiTextureType_DIFFUSE);
            if (albedoMaps.empty())
            {
                // Try base color for newer PBR materials
                albedoMaps = LoadMaterialTextures(mat, aiTextureType_BASE_COLOR);
            }
            if (!albedoMaps.empty())
            {
                materialRef->SetAlbedoMap(albedoMaps[0]);
            }
        }

        // Metallic/Roughness textures
        if (m_TextureOverride && !m_TextureOverride->MetallicPath.empty())
        {
            auto overrideTexture = Texture2D::Create(m_TextureOverride->MetallicPath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetMetallicRoughnessMap(overrideTexture);
            }
        }
        else
        {
            // Fall back to FBX textures
            auto metallicRoughnessMaps = LoadMaterialTextures(mat, aiTextureType_METALNESS);
            if (metallicRoughnessMaps.empty())
            {
                // Try alternative metallic texture types
                metallicRoughnessMaps = LoadMaterialTextures(mat, aiTextureType_REFLECTION);
            }
            if (!metallicRoughnessMaps.empty())
            {
                materialRef->SetMetallicRoughnessMap(metallicRoughnessMaps[0]);
            }
        }

        // Normal textures
        if (m_TextureOverride && !m_TextureOverride->NormalPath.empty())
        {
            auto overrideTexture = Texture2D::Create(m_TextureOverride->NormalPath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetNormalMap(overrideTexture);
            }
        }
        else
        {
            // Fall back to FBX textures
            auto normalMaps = LoadMaterialTextures(mat, aiTextureType_NORMALS);
            if (normalMaps.empty())
            {
                // Try height maps as normal maps
                normalMaps = LoadMaterialTextures(mat, aiTextureType_HEIGHT);
            }
            if (!normalMaps.empty())
            {
                materialRef->SetNormalMap(normalMaps[0]);
            }
        }

        // AO textures
        if (m_TextureOverride && !m_TextureOverride->AOPath.empty())
        {
            auto overrideTexture = Texture2D::Create(m_TextureOverride->AOPath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetAOMap(overrideTexture);
            }
        }
        else if (m_TextureOverride && !m_TextureOverride->RoughnessPath.empty())
        {
            // Use roughness texture as AO if no dedicated AO texture (common for Cerberus-style models)
            auto overrideTexture = Texture2D::Create(m_TextureOverride->RoughnessPath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetAOMap(overrideTexture);
            }
        }
        else
        {
            // Fall back to FBX textures
            auto aoMaps = LoadMaterialTextures(mat, aiTextureType_AMBIENT_OCCLUSION);
            if (aoMaps.empty())
            {
                // Try lightmap as AO
                aoMaps = LoadMaterialTextures(mat, aiTextureType_LIGHTMAP);
            }
            if (!aoMaps.empty())
            {
                materialRef->SetAOMap(aoMaps[0]);
            }
        }

        // Emissive textures
        if (m_TextureOverride && !m_TextureOverride->EmissivePath.empty())
        {
            auto overrideTexture = Texture2D::Create(m_TextureOverride->EmissivePath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetEmissiveMap(overrideTexture);
            }
        }
        else
        {
            // Fall back to FBX textures
            auto emissiveMaps = LoadMaterialTextures(mat, aiTextureType_EMISSIVE);
            if (!emissiveMaps.empty())
            {
                materialRef->SetEmissiveMap(emissiveMaps[0]);
            }
        }

        return materialRef;
    }

    void Model::CalculateBounds()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Meshes.empty())
        {
            // Default to a unit cube and sphere around origin if no meshes
            m_BoundingBox = BoundingBox(glm::vec3(-0.5f), glm::vec3(0.5f));
            m_BoundingSphere = BoundingSphere(glm::vec3(0.0f), 0.5f);
            return;
        }

        // Start with the first mesh's bounding volumes
        m_BoundingBox = m_Meshes[0]->GetBoundingBox();
        m_BoundingSphere = m_Meshes[0]->GetBoundingSphere();

        // Expand to include all other meshes
        for (sizet i = 1; i < m_Meshes.size(); i++)
        {
            const BoundingBox& meshBox = m_Meshes[i]->GetBoundingBox();

            // Expand the model's bounding box
            m_BoundingBox.Min = glm::min(m_BoundingBox.Min, meshBox.Min);
            m_BoundingBox.Max = glm::max(m_BoundingBox.Max, meshBox.Max);
        }

        // Recalculate the bounding sphere based on the final bounding box
        glm::vec3 center = (m_BoundingBox.Min + m_BoundingBox.Max) * 0.5f;
        f32 radius = glm::length(m_BoundingBox.Max - center);

        // Add a small margin (5%) to prevent edge cases
        radius *= 1.05f;

        m_BoundingSphere = BoundingSphere(center, radius);
    }

    void Model::GetDrawCommands(const glm::mat4& transform, const Material& material, std::vector<CommandPacket*>& outCommands) const
    {
        OLO_PROFILE_FUNCTION();
        outCommands.clear();
        outCommands.reserve(m_Meshes.size());

        for (sizet i = 0; i < m_Meshes.size(); i++)
        {
            // Get the submesh to access its material index
            const Submesh& submesh = m_Meshes[i]->GetSubmesh();

            // Use the submesh's material index to look up the correct material
            Material meshMaterial;
            if (submesh.m_MaterialIndex < m_Materials.size() && m_Materials[submesh.m_MaterialIndex])
            {
                meshMaterial = *m_Materials[submesh.m_MaterialIndex];
            }
            else
            {
                meshMaterial = material;
            }

            CommandPacket* cmd = OloEngine::Renderer3D::DrawMesh(m_Meshes[i], transform, meshMaterial);
            if (cmd)
                outCommands.push_back(cmd);
        }
    }

    void Model::GetDrawCommands(const glm::mat4& transform, std::vector<CommandPacket*>& outCommands) const
    {
        OLO_PROFILE_FUNCTION();
        outCommands.clear();
        outCommands.reserve(m_Meshes.size());

        for (sizet i = 0; i < m_Meshes.size(); i++)
        {
            // Get the submesh to access its material index
            const Submesh& submesh = m_Meshes[i]->GetSubmesh();

            // Static default material to avoid repeated allocations
            static Material s_DefaultMaterial = []() -> Material
            {
                auto defaultMaterialRef = Material::CreatePBR("Default PBR", glm::vec3(0.8f), 0.0f, 0.5f);
                if (defaultMaterialRef)
                {
                    return *defaultMaterialRef; // Copy to value type for struct-like access
                }
                else
                {
                    OLO_CORE_ERROR("Model: Failed to create default PBR material, using empty material");
                    return Material{}; // Default constructed material
                }
            }();

            // Use the submesh's material index to look up the correct material
            Material meshMaterial;
            if (submesh.m_MaterialIndex < m_Materials.size() && m_Materials[submesh.m_MaterialIndex])
            {
                meshMaterial = *m_Materials[submesh.m_MaterialIndex];
            }
            else
            {
                // Use the cached static default material
                meshMaterial = s_DefaultMaterial;
            }

            CommandPacket* cmd = OloEngine::Renderer3D::DrawMesh(m_Meshes[i], transform, meshMaterial);
            if (cmd)
                outCommands.push_back(cmd);
        }
    }

    void Model::Draw(const glm::mat4& transform, const Material& material) const
    {
        // Material reference is always valid since it's passed by reference
        std::vector<CommandPacket*> commands;
        GetDrawCommands(transform, material, commands);
        for (auto* cmd : commands)
        {
            OloEngine::Renderer3D::SubmitPacket(cmd);
        }
    }

    void Model::Draw(const glm::mat4& transform, const Ref<const Material>& material) const
    {
        if (material)
        {
            Draw(transform, *material);
        }
        else
        {
            // Fallback to default Draw behavior when material is null
            std::vector<CommandPacket*> commands;
            GetDrawCommands(transform, commands);
            for (auto* cmd : commands)
            {
                OloEngine::Renderer3D::SubmitPacket(cmd);
            }
        }
    }

    void Model::GetDrawCommands(const glm::mat4& transform, const Ref<const Material>& material, std::vector<CommandPacket*>& outCommands) const
    {
        if (material)
        {
            GetDrawCommands(transform, *material, outCommands);
        }
        else
        {
            // Log when a null Ref<Material> is received for diagnostics
            OLO_CORE_WARN("GetDrawCommands received null Ref<Material>, falling back to default material handling");
            GetDrawCommands(transform, outCommands);
        }
    }

    void Model::DrawParallel(const glm::mat4& transform, const Material& fallbackMaterial) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Meshes.empty())
            return;

        // Collect mesh descriptors for parallel submission
        std::vector<Renderer3D::MeshSubmitDesc> meshDescriptors;
        meshDescriptors.reserve(m_Meshes.size());

        for (sizet i = 0; i < m_Meshes.size(); i++)
        {
            const Submesh& submesh = m_Meshes[i]->GetSubmesh();

            // Determine material: use submesh material if valid, otherwise fallback
            Material meshMaterial = fallbackMaterial;
            if (submesh.m_MaterialIndex < m_Materials.size() && m_Materials[submesh.m_MaterialIndex])
            {
                meshMaterial = *m_Materials[submesh.m_MaterialIndex];
            }

            meshDescriptors.push_back({
                m_Meshes[i],
                transform,
                meshMaterial,
                true,   // IsStatic
                false,  // IsAnimated
                nullptr // BoneMatrices
            });
        }

        // Submit all meshes in parallel
        Renderer3D::SubmitMeshesParallel(meshDescriptors);
    }

    void Model::DrawParallel(const glm::mat4& transform) const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Meshes.empty())
            return;

        // Static default material
        static Material s_DefaultMaterial = []() -> Material
        {
            auto defaultMaterialRef = Material::CreatePBR("Default PBR", glm::vec3(0.8f), 0.0f, 0.5f);
            if (defaultMaterialRef)
            {
                return *defaultMaterialRef;
            }
            return Material{};
        }();

        // Collect mesh descriptors for parallel submission
        std::vector<Renderer3D::MeshSubmitDesc> meshDescriptors;
        meshDescriptors.reserve(m_Meshes.size());

        for (sizet i = 0; i < m_Meshes.size(); i++)
        {
            const Submesh& submesh = m_Meshes[i]->GetSubmesh();

            // Determine material
            Material meshMaterial = s_DefaultMaterial;
            if (submesh.m_MaterialIndex < m_Materials.size() && m_Materials[submesh.m_MaterialIndex])
            {
                meshMaterial = *m_Materials[submesh.m_MaterialIndex];
            }

            meshDescriptors.push_back({
                m_Meshes[i],
                transform,
                meshMaterial,
                true,   // IsStatic
                false,  // IsAnimated
                nullptr // BoneMatrices
            });
        }

        // Submit all meshes in parallel
        Renderer3D::SubmitMeshesParallel(meshDescriptors);
    }
} // namespace OloEngine
