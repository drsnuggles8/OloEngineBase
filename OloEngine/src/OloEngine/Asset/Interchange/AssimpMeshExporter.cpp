#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/AssimpMeshExporter.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"

#include <assimp/Exporter.hpp>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Add the source-file basename of `texture` (if any) as a glTF texture URI on `mat`.
        // We deliberately write only the filename: glTF references external images relatively
        // and the near-universal convention is textures sitting next to the .gltf. Copying the
        // image bytes next to the export target is a follow-up; the material factors below fully
        // define appearance for untextured materials regardless.
        void AddTexture(aiMaterial* mat, const Ref<Texture2D>& texture, aiTextureType type)
        {
            if (!texture)
                return;
            const std::string& srcPath = texture->GetPath();
            if (srcPath.empty())
                return;
            const std::string filename = std::filesystem::path(srcPath).filename().string();
            if (filename.empty())
                return;
            aiString aiPath(filename);
            mat->AddProperty(&aiPath, AI_MATKEY_TEXTURE(type, 0));
        }

        aiMaterial* BuildMaterial(const Material& material)
        {
            auto* mat = new aiMaterial();

            aiString name(material.GetName().empty() ? std::string("Material") : material.GetName());
            mat->AddProperty(&name, AI_MATKEY_NAME);

            const glm::vec4& baseColor = material.GetBaseColorFactor();
            aiColor4D aiBaseColor(baseColor.r, baseColor.g, baseColor.b, baseColor.a);
            mat->AddProperty(&aiBaseColor, 1, AI_MATKEY_BASE_COLOR);
            // Also emit legacy diffuse so importers that don't read PBR base-color still see a colour.
            aiColor4D aiDiffuse(baseColor.r, baseColor.g, baseColor.b, baseColor.a);
            mat->AddProperty(&aiDiffuse, 1, AI_MATKEY_COLOR_DIFFUSE);

            f32 metallic = material.GetMetallicFactor();
            f32 roughness = material.GetRoughnessFactor();
            mat->AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
            mat->AddProperty(&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);

            const glm::vec4& emissive = material.GetEmissiveFactor();
            aiColor3D aiEmissive(emissive.r, emissive.g, emissive.b);
            mat->AddProperty(&aiEmissive, 1, AI_MATKEY_COLOR_EMISSIVE);

            // glTF alphaMode: assimp exposes AI_MATKEY_GLTF_ALPHAMODE as a string ("OPAQUE"/"MASK"/"BLEND").
            const char* alphaMode = "OPAQUE";
            if (material.GetAlphaMode() == AlphaMode::Mask)
                alphaMode = "MASK";
            else if (material.GetAlphaMode() == AlphaMode::Blend)
                alphaMode = "BLEND";
            aiString aiAlphaMode(alphaMode);
            mat->AddProperty(&aiAlphaMode, "$mat.gltf.alphaMode", 0, 0);
            if (material.GetAlphaMode() == AlphaMode::Mask)
            {
                f32 cutoff = material.GetAlphaCutoff();
                mat->AddProperty(&cutoff, 1, "$mat.gltf.alphaCutoff", 0, 0);
            }

            AddTexture(mat, material.GetAlbedoMap(), aiTextureType_BASE_COLOR);
            AddTexture(mat, material.GetNormalMap(), aiTextureType_NORMALS);
            AddTexture(mat, material.GetEmissiveMap(), aiTextureType_EMISSIVE);
            AddTexture(mat, material.GetAOMap(), aiTextureType_AMBIENT_OCCLUSION);
            // glTF stores a combined metallic-roughness texture; assimp's glTF2 exporter reads
            // it from aiTextureType_METALNESS / aiTextureType_DIFFUSE_ROUGHNESS (same image).
            AddTexture(mat, material.GetMetallicRoughnessMap(), aiTextureType_METALNESS);
            AddTexture(mat, material.GetMetallicRoughnessMap(), aiTextureType_DIFFUSE_ROUGHNESS);

            return mat;
        }

        // Populate one aiMesh from the submesh's slice of the combined vertex/index buffers.
        aiMesh* BuildMesh(const MeshSource& source, const Submesh& submesh, u32 materialIndex, const std::string& name)
        {
            const auto& vertices = source.GetVertices();
            const auto& indices = source.GetIndices();

            const u32 baseVertex = submesh.m_BaseVertex;
            const u32 vertexCount = submesh.m_VertexCount;
            const u32 baseIndex = submesh.m_BaseIndex;
            const u32 indexCount = submesh.m_IndexCount;

            auto* mesh = new aiMesh();
            mesh->mName = aiString(name);
            mesh->mMaterialIndex = materialIndex;
            mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

            mesh->mNumVertices = vertexCount;
            mesh->mVertices = new aiVector3D[vertexCount];
            mesh->mNormals = new aiVector3D[vertexCount];
            mesh->mTextureCoords[0] = new aiVector3D[vertexCount];
            mesh->mNumUVComponents[0] = 2;

            for (u32 i = 0; i < vertexCount; ++i)
            {
                const Vertex& v = vertices[static_cast<i32>(baseVertex + i)];
                mesh->mVertices[i] = aiVector3D(v.Position.x, v.Position.y, v.Position.z);
                mesh->mNormals[i] = aiVector3D(v.Normal.x, v.Normal.y, v.Normal.z);
                mesh->mTextureCoords[0][i] = aiVector3D(v.TexCoord.x, v.TexCoord.y, 0.0f);
            }

            const u32 faceCount = indexCount / 3u;
            mesh->mNumFaces = faceCount;
            mesh->mFaces = new aiFace[faceCount];
            for (u32 f = 0; f < faceCount; ++f)
            {
                aiFace& face = mesh->mFaces[f];
                face.mNumIndices = 3;
                face.mIndices = new unsigned int[3];
                for (u32 k = 0; k < 3; ++k)
                {
                    // Combined indices are global (submesh-local + baseVertex); rebase to the
                    // per-mesh 0-based range assimp expects for this aiMesh.
                    const u32 globalIndex = indices[static_cast<i32>(baseIndex + f * 3u + k)];
                    face.mIndices[k] = globalIndex - baseVertex;
                }
            }

            return mesh;
        }
    } // namespace

    MeshExportResult AssimpMeshExporter::Export(const MeshSource& source, const std::filesystem::path& path,
                                                const MeshExportOptions& options)
    {
        (void)options;

        if (source.GetVertices().IsEmpty())
            return MeshExportResult::Failure("AssimpMeshExporter: MeshSource has no vertices");

        // Assemble the submesh list. A well-formed MeshSource always has >= 1 submesh; if it
        // somehow has none, synthesize a single submesh spanning the whole buffer so export
        // still succeeds.
        std::vector<Submesh> submeshes;
        if (source.GetSubmeshes().IsEmpty())
        {
            Submesh whole;
            whole.m_BaseVertex = 0;
            whole.m_BaseIndex = 0;
            whole.m_VertexCount = static_cast<u32>(source.GetVertices().Num());
            whole.m_IndexCount = static_cast<u32>(source.GetIndices().Num());
            whole.m_MaterialIndex = UINT32_MAX;
            submeshes.push_back(whole);
        }
        else
        {
            const auto& srcSubmeshes = source.GetSubmeshes();
            submeshes.reserve(static_cast<sizet>(srcSubmeshes.Num()));
            for (i32 i = 0; i < srcSubmeshes.Num(); ++i)
                submeshes.push_back(srcSubmeshes[i]);
        }

        // aiScene owns everything below and frees it in ~aiScene, so every array/element is
        // heap-allocated with new/new[]. Build on the stack and let RAII clean up.
        aiScene scene;
        scene.mRootNode = new aiNode();
        scene.mRootNode->mName = aiString("root");

        // Materials: one aiMaterial per imported material, plus a trailing default used by any
        // submesh whose material index is out of range / UINT32_MAX.
        const auto& importedMaterials = source.GetImportedMaterials();
        const u32 realMaterialCount = static_cast<u32>(importedMaterials.size());
        const u32 defaultMaterialIndex = realMaterialCount; // slot appended after the real ones
        const u32 totalMaterials = realMaterialCount + 1;

        scene.mNumMaterials = totalMaterials;
        scene.mMaterials = new aiMaterial*[totalMaterials];
        for (u32 i = 0; i < realMaterialCount; ++i)
        {
            scene.mMaterials[i] = importedMaterials[i] ? BuildMaterial(*importedMaterials[i]) : new aiMaterial();
        }
        {
            // Default material (index defaultMaterialIndex).
            auto* def = new aiMaterial();
            aiString defName("DefaultMaterial");
            def->AddProperty(&defName, AI_MATKEY_NAME);
            scene.mMaterials[defaultMaterialIndex] = def;
        }

        // Meshes: one per submesh.
        const auto meshCount = static_cast<u32>(submeshes.size());
        scene.mNumMeshes = meshCount;
        scene.mMeshes = new aiMesh*[meshCount];
        scene.mRootNode->mNumMeshes = meshCount;
        scene.mRootNode->mMeshes = new unsigned int[meshCount];

        for (u32 m = 0; m < meshCount; ++m)
        {
            const Submesh& submesh = submeshes[m];
            u32 materialIndex = submesh.m_MaterialIndex;
            if (materialIndex >= realMaterialCount)
                materialIndex = defaultMaterialIndex;

            std::string meshName = submesh.m_MeshName.empty()
                                       ? (submesh.m_NodeName.empty() ? ("Mesh_" + std::to_string(m)) : submesh.m_NodeName)
                                       : submesh.m_MeshName;
            scene.mMeshes[m] = BuildMesh(source, submesh, materialIndex, meshName);
            scene.mRootNode->mMeshes[m] = m;
        }

        // Choose the format from the extension: .glb -> binary, everything else -> text glTF.
        std::string ext = path.has_extension() ? path.extension().string() : std::string{};
        std::ranges::transform(ext, ext.begin(), [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
        const char* formatId = (ext == ".glb") ? "glb2" : "gltf2";

        Assimp::Exporter exporter;
        const aiReturn rc = exporter.Export(&scene, formatId, path.string(), 0u);
        if (rc != aiReturn_SUCCESS)
        {
            return MeshExportResult::Failure(std::string("AssimpMeshExporter: Assimp export failed: ") +
                                             exporter.GetErrorString());
        }

        OLO_CORE_INFO("AssimpMeshExporter: wrote {} ({} submeshes, {} materials) as {}", path.string(), meshCount,
                      realMaterialCount, formatId);
        return MeshExportResult::Ok();
    }
} // namespace OloEngine
