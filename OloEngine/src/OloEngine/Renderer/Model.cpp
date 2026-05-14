// TODO(OloEngine): When implementing the asset pipeline for animated models, ensure that
// AnimatedMeshComponent, AnimationStateComponent, and SkeletonComponent are assigned to entities
// upon import. This is required for ECS-driven animated mesh support.
#include "OloEnginePCH.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Task/ParallelFor.h"

#include <stb_image/stb_image.h>

namespace OloEngine
{
    namespace
    {
        std::string ToLowerCopy(std::string value)
        {
            std::ranges::transform(value, value.begin(),
                                   [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            if (value == nullptr)
                return false;

            const std::string normalized = ToLowerCopy(value);
            return !normalized.empty() && normalized != "0" && normalized != "false" &&
                   normalized != "off" && normalized != "no";
        }

        bool IsModelImportDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_MODEL_IMPORT_DIAGNOSTICS");
            return enabled;
        }

        bool IsObjModelPath(const std::filesystem::path& path)
        {
            return ToLowerCopy(path.extension().string()) == ".obj";
        }

        std::string GetStaticMeshCachePrefix(bool effectiveFlipUV)
        {
            // Vertex UVs are baked into the .omesh cache. Keep the flipped-UV
            // variant separate from older/default caches so reload/reimport
            // cannot silently reuse geometry imported with different UVs.
            return effectiveFlipUV ? "static_uvflip_v1" : std::string{};
        }

        // Scan directory for a file whose stem matches (case-insensitive) with any common image extension.
        // Returns empty path if nothing found.
        std::filesystem::path FindTextureInDirectory(const std::filesystem::path& directory, const std::filesystem::path& filenameHint)
        {
            static constexpr std::array<std::string_view, 7> kImageExtensions = {
                ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds"
            };

            std::string targetStem = ToLowerCopy(filenameHint.stem().string());

            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }

                const auto& entryPath = entry.path();
                std::string entryStem = ToLowerCopy(entryPath.stem().string());

                if (entryStem != targetStem)
                {
                    continue;
                }

                // Stem matches — check if the extension is a known image format
                std::string ext = ToLowerCopy(entryPath.extension().string());

                if (std::ranges::find(kImageExtensions, ext) != kImageExtensions.end())
                {
                    return entryPath;
                }
            }

            return {};
        }

        // Try to discover a PBR companion texture by replacing an albedo-like suffix
        // with the given PBR suffix. E.g., "cerberus_A.png" + "_N" → "cerberus_N.png".
        // Uses FindTextureInDirectory for case-insensitive matching.
        std::filesystem::path DiscoverPBRCompanion(
            const std::filesystem::path& directory,
            const std::filesystem::path& albedoPath,
            std::string_view pbrSuffix)
        {
            static constexpr std::array<std::string_view, 5> kAlbedoSuffixes = {
                "_A", "_a", "_albedo", "_Albedo", "_diffuse"
            };

            std::string stem = albedoPath.stem().string();

            for (auto alSuffix : kAlbedoSuffixes)
            {
                if (stem.size() > alSuffix.size() && stem.ends_with(alSuffix))
                {
                    std::string baseName = stem.substr(0, stem.size() - alSuffix.size());
                    std::string candidateStem = baseName + std::string(pbrSuffix);
                    // Use any extension — FindTextureInDirectory checks all image extensions
                    std::filesystem::path hint = candidateStem + ".png";
                    auto result = FindTextureInDirectory(directory, hint);
                    if (!result.empty())
                    {
                        return result;
                    }
                }
            }

            return {};
        }

        std::filesystem::path FindFirstTextureByStem(
            const std::filesystem::path& directory,
            std::initializer_list<std::string_view> stems)
        {
            for (std::string_view stem : stems)
            {
                auto result = FindTextureInDirectory(directory, std::filesystem::path(std::string(stem) + ".png"));
                if (!result.empty())
                    return result;
            }

            return {};
        }

        u8 FloatFactorToByte(f32 value)
        {
            if (!std::isfinite(value))
                value = 0.0f;

            value = std::clamp(value, 0.0f, 1.0f);
            return static_cast<u8>(std::lround(value * 255.0f));
        }

        u8 ScaleByteByFactor(u8 value, f32 factor)
        {
            if (!std::isfinite(factor))
                factor = 1.0f;

            const auto normalized = static_cast<f32>(value) / 255.0f;
            return FloatFactorToByte(normalized * factor);
        }

        struct StbiImageDeleter
        {
            void operator()(stbi_uc* data) const noexcept
            {
                ::stbi_image_free(data);
            }
        };

        struct SingleChannelImage
        {
            i32 Width = 0;
            i32 Height = 0;
            std::unique_ptr<stbi_uc, StbiImageDeleter> Pixels;

            [[nodiscard]] bool IsValid() const noexcept
            {
                return Pixels && Width > 0 && Height > 0;
            }
        };

        SingleChannelImage LoadSingleChannelImage(const std::filesystem::path& path)
        {
            if (path.empty())
                return {};

            i32 width = 0;
            i32 height = 0;
            i32 channels = 0;

            // Match Texture2D::Create(path), which flips file-backed images for OpenGL.
            ::stbi_set_flip_vertically_on_load_thread(1);
            stbi_uc* pixels = ::stbi_load(path.string().c_str(), &width, &height, &channels, 1);
            ::stbi_set_flip_vertically_on_load_thread(0); // reset thread-local flag to avoid polluting later stbi calls
            if (!pixels)
            {
                OLO_CORE_WARN("Model: Failed to load single-channel material texture '{}'", path.string());
                return {};
            }

            SingleChannelImage image;
            image.Width = width;
            image.Height = height;
            image.Pixels.reset(pixels);
            return image;
        }

        u8 SampleSingleChannelNearest(const SingleChannelImage& image, u32 x, u32 y, u32 width, u32 height, u8 fallback)
        {
            if (!image.IsValid() || width == 0 || height == 0)
                return fallback;

            const auto sourceX = std::min(static_cast<u32>(image.Width - 1), (x * static_cast<u32>(image.Width)) / width);
            const auto sourceY = std::min(static_cast<u32>(image.Height - 1), (y * static_cast<u32>(image.Height)) / height);
            return image.Pixels.get()[static_cast<sizet>(sourceY) * static_cast<sizet>(image.Width) + sourceX];
        }

        Ref<Texture2D> CreatePackedMetallicRoughnessTexture(
            const std::filesystem::path& metallicPath,
            const std::filesystem::path& roughnessPath,
            f32 metallicFallback,
            f32 roughnessFallback,
            f32 metallicScale,
            f32 roughnessScale)
        {
            auto metallicImage = LoadSingleChannelImage(metallicPath);
            auto roughnessImage = LoadSingleChannelImage(roughnessPath);
            if (!metallicImage.IsValid() && !roughnessImage.IsValid())
                return nullptr;

            const auto width = static_cast<u32>(metallicImage.IsValid() ? metallicImage.Width : roughnessImage.Width);
            const auto height = static_cast<u32>(metallicImage.IsValid() ? metallicImage.Height : roughnessImage.Height);
            if (width == 0 || height == 0)
                return nullptr;

            const auto pixelCount = static_cast<sizet>(width) * static_cast<sizet>(height);
            if (pixelCount > (std::numeric_limits<u32>::max() / 4u))
            {
                OLO_CORE_WARN("Model: Refusing to pack oversized metallic-roughness texture ({}x{})", width, height);
                return nullptr;
            }

            std::vector<u8> packed(pixelCount * 4u);
            const auto metallicFallbackByte = FloatFactorToByte(metallicFallback);
            const auto roughnessFallbackByte = FloatFactorToByte(roughnessFallback);

            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const auto metallicSample = SampleSingleChannelNearest(metallicImage, x, y, width, height, metallicFallbackByte);
                    const auto roughnessSample = SampleSingleChannelNearest(roughnessImage, x, y, width, height, roughnessFallbackByte);
                    const auto offset = (static_cast<sizet>(y) * width + x) * 4u;

                    // glTF/PBR convention used by PBRCommon.glsl: G = roughness, B = metallic.
                    packed[offset + 0] = 0;
                    packed[offset + 1] = roughnessImage.IsValid() ? ScaleByteByFactor(roughnessSample, roughnessScale) : roughnessSample;
                    packed[offset + 2] = metallicImage.IsValid() ? ScaleByteByFactor(metallicSample, metallicScale) : metallicSample;
                    packed[offset + 3] = 255;
                }
            }

            TextureSpecification spec;
            spec.Width = width;
            spec.Height = height;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            spec.MipLevels = 1;

            auto texture = Texture2D::Create(spec);
            if (!texture || !texture->IsLoaded())
                return nullptr;

            texture->SetData(packed.data(), static_cast<u32>(packed.size()));
            return texture;
        }
    } // namespace

    Model::Model(const std::string& path, const TextureOverride& textureOverride, bool flipUV)
        : m_TextureOverride(textureOverride.HasAnyTexture() ? std::optional<TextureOverride>(textureOverride) : std::nullopt),
          m_FlipUV(flipUV)
    {
        LoadModel(path, textureOverride, flipUV);
    }

    void Model::LoadModel(const std::string& path, const TextureOverride& textureOverride, bool flipUV)
    {
        OLO_PROFILE_FUNCTION();

        // Store texture override and UV flip setting for use in material processing.
        // File-backed Texture2D uploads flip image rows for OpenGL. Legacy OBJ
        // atlases (including the LearnOpenGL backpack fixture) are authored for
        // the opposite V origin, so flip OBJ UVs by default to keep atlas regions
        // aligned with the flipped texture data.
        std::filesystem::path sourcePath(path);
        const bool isObjModel = IsObjModelPath(sourcePath);
        const bool effectiveFlipUV = flipUV || isObjModel;
        const std::string cachePrefix = GetStaticMeshCachePrefix(effectiveFlipUV);

        m_TextureOverride = textureOverride.HasAnyTexture() ? std::optional<TextureOverride>(textureOverride) : std::nullopt;
        m_FlipUV = effectiveFlipUV;

        if (IsModelImportDiagnosticsEnabled())
        {
            OLO_CORE_INFO("Model import diagnostics: path='{}', extension='{}', requestedFlipUV={}, effectiveFlipUV={}, meshCachePrefix='{}'",
                          path,
                          sourcePath.extension().string(),
                          flipUV,
                          effectiveFlipUV,
                          cachePrefix.empty() ? "<default>" : cachePrefix);
        }

        // Try loading geometry from binary cache (skip Assimp for vertex data)
        if (MeshCache::IsMeshCacheValid(sourcePath, cachePrefix))
        {
            auto cachedMesh = MeshCache::LoadMeshFromCache(sourcePath, cachePrefix);
            if (cachedMesh)
            {
                m_Directory = sourcePath.parent_path().string();

                // Create individual Mesh objects from submeshes
                cachedMesh->Build();
                for (i32 i = 0; i < cachedMesh->GetSubmeshes().Num(); ++i)
                {
                    m_Meshes.push_back(Ref<Mesh>::Create(cachedMesh, static_cast<u32>(i)));
                }

                // Load materials from the source file (lightweight Assimp read — no geometry postprocessing)
                // CreateCombinedMeshSource sets materialIndex = meshIdx, so m_Materials[i]
                // must hold the material for the i-th Assimp mesh.
                {
                    Assimp::Importer importer;
                    const aiScene* scene = importer.ReadFile(path, 0);
                    if (scene)
                    {
                        m_Materials.resize(m_Meshes.size());
                        auto numSceneMeshes = std::min(scene->mNumMeshes, static_cast<u32>(m_Meshes.size()));
                        for (u32 i = 0; i < numSceneMeshes; ++i)
                        {
                            auto matIdx = scene->mMeshes[i]->mMaterialIndex;
                            if (matIdx < scene->mNumMaterials)
                            {
                                m_Materials[i] = ProcessMaterial(scene->mMaterials[matIdx]);
                            }
                        }
                        // Fill any unfilled slots with a default material so accesses never see null
                        if (numSceneMeshes < static_cast<u32>(m_Meshes.size()))
                        {
                            OLO_CORE_WARN("Model::LoadModel: Scene has {} meshes but cache has {} — "
                                          "assigning default material to trailing entries",
                                          numSceneMeshes, m_Meshes.size());
                            for (auto i = numSceneMeshes; i < static_cast<u32>(m_Meshes.size()); ++i)
                            {
                                if (!m_Materials[i])
                                {
                                    m_Materials[i] = Ref<Material>::Create();
                                }
                            }
                        }
                    }
                    else
                    {
                        OLO_CORE_WARN("Model::LoadModel: Failed to load materials from '{}' for cached geometry", path);
                        m_Materials.resize(m_Meshes.size());
                    }
                }

                CalculateBounds();

                OLO_CORE_TRACE("Model::LoadModel: Loaded {} meshes from cache '{}'", m_Meshes.size(), cachePrefix.empty() ? "<default>" : cachePrefix);
                return;
            }
        }

        // Create an instance of the Importer class
        Assimp::Importer importer;

        // And have it read the given file with some postprocessing
        const aiScene* scene = importer.ReadFile(path,
                                                 aiProcess_Triangulate |               // Make sure we get triangles
                                                     aiProcess_GenNormals |            // Create normals if not present
                                                     aiProcess_CalcTangentSpace |      // Calculate tangents and bitangents
                                                     aiProcess_JoinIdenticalVertices | // Deduplicate identical vertices for smaller buffers
                                                     aiProcess_ValidateDataStructure | // Validate the imported data structure
                                                     aiProcess_PreTransformVertices    // Bake node transforms into vertices (safe for static meshes)
        );

        // Check for errors
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            OLO_CORE_ERROR("ASSIMP Error: {0}", importer.GetErrorString());
            return;
        }

        // Store the directory path
        m_Directory = std::filesystem::path(path).parent_path().string();

        OLO_CORE_TRACE("Loading model: {0} ({1} meshes, {2} materials, flipUV={3})", path, scene->mNumMeshes, scene->mNumMaterials, m_FlipUV);

        // Reserve space for expected number of meshes and materials to reduce allocations
        m_Meshes.reserve(scene->mNumMeshes);
        m_Materials.reserve(scene->mNumMaterials);

        // Pre-size the material index map to reduce rehashing overhead
        m_MaterialIndexMap.reserve(scene->mNumMaterials);

        // Process all the nodes recursively
        ProcessNode(scene->mRootNode, scene);

        // Calculate bounding volumes for the entire model
        CalculateBounds();

        // Save geometry to binary cache for next load
        {
            auto combinedMeshSource = CreateCombinedMeshSource();
            if (combinedMeshSource)
            {
                MeshCache::SaveMeshToCache(sourcePath, *combinedMeshSource, cachePrefix);
            }
        }

        OLO_CORE_TRACE("Model loaded successfully: {0} meshes processed", m_Meshes.size());
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

        // Build() internally calls OptimizeMesh before uploading to GPU
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
                else
                {
                    // Fallback: FBX files often embed absolute or nested relative paths
                    // that don't match the filesystem. Try just the filename in the model directory.
                    std::filesystem::path filenameOnly = relativePath.filename();
                    std::filesystem::path fallbackPath = std::filesystem::path(m_Directory) / filenameOnly;
                    std::string fallbackPathStr = fallbackPath.string();

                    bool loaded = false;

                    if (fallbackPathStr != texturePathStr && !m_LoadedTextures.contains(fallbackPathStr))
                    {
                        OLO_CORE_WARN("Model::LoadMaterialTextures: '{}' not found, trying fallback '{}'",
                                      texturePathStr, fallbackPathStr);
                        auto fallbackTexture = Texture2D::Create(fallbackPathStr);
                        if (fallbackTexture && fallbackTexture->IsLoaded())
                        {
                            m_LoadedTextures[fallbackPathStr] = fallbackTexture;
                            textures.push_back(fallbackTexture);
                            loaded = true;
                        }
                    }
                    else if (m_LoadedTextures.contains(fallbackPathStr))
                    {
                        textures.push_back(m_LoadedTextures[fallbackPathStr]);
                        loaded = true;
                    }

                    // Fallback: scan model directory for case-insensitive stem match with any image extension.
                    // Handles FBX referencing .tga when the actual file is .png, and case mismatches on Linux.
                    if (!loaded)
                    {
                        auto discovered = FindTextureInDirectory(std::filesystem::path(m_Directory), filenameOnly);
                        if (!discovered.empty())
                        {
                            std::string discoveredStr = discovered.string();
                            OLO_CORE_WARN("Model::LoadMaterialTextures: Discovered '{}' via directory scan for '{}'",
                                          discoveredStr, filenameOnly.string());
                            if (m_LoadedTextures.contains(discoveredStr))
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
                        OLO_CORE_WARN("Model::LoadMaterialTextures: Failed to load texture '{}' (tried original, filename-only, and directory scan)",
                                      texturePathStr);
                    }
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
        const bool hasMetallicFactor = mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS;
        const bool hasRoughnessFactor = mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS;

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
        // Track the albedo filename for PBR companion texture discovery
        std::filesystem::path albedoFilename;
        const auto modelDirectory = std::filesystem::path(m_Directory);

        const auto loadTextureFromPath = [this](const std::filesystem::path& texturePath) -> Ref<Texture2D>
        {
            if (texturePath.empty())
                return nullptr;

            const auto texturePathStr = texturePath.string();
            if (auto it = m_LoadedTextures.find(texturePathStr); it != m_LoadedTextures.end())
                return it->second;

            auto texture = Texture2D::Create(texturePathStr);
            if (texture && texture->IsLoaded())
            {
                m_LoadedTextures[texturePathStr] = texture;
                return texture;
            }

            OLO_CORE_WARN("Model: Failed to load discovered material texture '{}'", texturePathStr);
            return nullptr;
        };

        const auto getFirstTexturePath = [](const std::vector<Ref<Texture2D>>& textures) -> std::filesystem::path
        {
            if (!textures.empty() && textures[0])
                return textures[0]->GetPath();
            return {};
        };

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
                // Remember albedo filename for companion discovery below
                albedoFilename = albedoMaps[0]->GetPath();
            }
        }

        // Metallic/Roughness textures
        if (m_TextureOverride && !m_TextureOverride->MetallicPath.empty())
        {
            auto overrideTexture = Texture2D::Create(m_TextureOverride->MetallicPath);
            if (overrideTexture && overrideTexture->IsLoaded())
            {
                materialRef->SetMetallicRoughnessMap(overrideTexture);
                if (!hasMetallicFactor)
                    materialRef->SetMetallicFactor(1.0f);
                if (!hasRoughnessFactor)
                    materialRef->SetRoughnessFactor(1.0f);
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
                if (!hasMetallicFactor)
                    materialRef->SetMetallicFactor(1.0f);
                if (!hasRoughnessFactor)
                    materialRef->SetRoughnessFactor(1.0f);
            }
            else
            {
                std::filesystem::path metallicSourcePath;
                std::filesystem::path roughnessSourcePath;

                if (!albedoFilename.empty())
                {
                    // Auto-discover PBR companions by naming convention (e.g., cerberus_A -> cerberus_M/R).
                    metallicSourcePath = DiscoverPBRCompanion(modelDirectory, albedoFilename.filename(), "_M");
                    roughnessSourcePath = DiscoverPBRCompanion(modelDirectory, albedoFilename.filename(), "_R");
                }

                if (metallicSourcePath.empty())
                    metallicSourcePath = FindFirstTextureByStem(modelDirectory, { "metallic", "metalness", "metal" });
                if (roughnessSourcePath.empty())
                    roughnessSourcePath = FindFirstTextureByStem(modelDirectory, { "roughness", "rough" });

                // Legacy Phong/OBJ materials commonly provide map_Ks instead of metalness.
                // Use it as a best-effort metalness mask so atlas regions such as axe heads
                // and buckles retain high specular/metallic response in the PBR shader.
                if (metallicSourcePath.empty())
                {
                    auto specularMaps = LoadMaterialTextures(mat, aiTextureType_SPECULAR);
                    metallicSourcePath = getFirstTexturePath(specularMaps);
                }

                if (!metallicSourcePath.empty() || !roughnessSourcePath.empty())
                {
                    const auto metallicScale = metallicSourcePath.empty() ? 1.0f : (hasMetallicFactor ? metallic : 1.0f);
                    const auto roughnessScale = roughnessSourcePath.empty() ? 1.0f : (hasRoughnessFactor ? roughness : 1.0f);
                    const auto cacheKey = std::string("packed_mr|") + metallicSourcePath.string() + "|" + roughnessSourcePath.string() + "|" +
                                          std::to_string(metallic) + "|" + std::to_string(roughness);

                    Ref<Texture2D> packedTexture;
                    if (auto it = m_LoadedTextures.find(cacheKey); it != m_LoadedTextures.end())
                    {
                        packedTexture = it->second;
                    }
                    else
                    {
                        packedTexture = CreatePackedMetallicRoughnessTexture(
                            metallicSourcePath,
                            roughnessSourcePath,
                            metallic,
                            roughness,
                            metallicScale,
                            roughnessScale);
                        if (packedTexture && packedTexture->IsLoaded())
                        {
                            m_LoadedTextures[cacheKey] = packedTexture;
                        }
                    }

                    if (packedTexture && packedTexture->IsLoaded())
                    {
                        OLO_CORE_TRACE("Model: Packed metallic-roughness map for material '{}' (metallic='{}', roughness='{}')",
                                       materialName,
                                       metallicSourcePath.empty() ? "<scalar>" : metallicSourcePath.string(),
                                       roughnessSourcePath.empty() ? "<scalar>" : roughnessSourcePath.string());
                        materialRef->SetMetallicRoughnessMap(packedTexture);
                        materialRef->SetMetallicFactor(1.0f);
                        materialRef->SetRoughnessFactor(1.0f);
                    }
                }
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
            else if (!albedoFilename.empty())
            {
                // Auto-discover normal companion by naming convention (e.g., cerberus_A → cerberus_N)
                auto discovered = DiscoverPBRCompanion(std::filesystem::path(m_Directory), albedoFilename.filename(), "_N");
                if (discovered.empty())
                    discovered = FindFirstTextureByStem(modelDirectory, { "normal", "normals", "normalmap", "normal_map" });
                if (!discovered.empty())
                {
                    OLO_CORE_TRACE("Model: Auto-discovered normal map '{}' from albedo '{}'", discovered.string(), albedoFilename.string());
                    auto tex = loadTextureFromPath(discovered);
                    if (tex && tex->IsLoaded())
                    {
                        materialRef->SetNormalMap(tex);
                    }
                }
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
            else if (!albedoFilename.empty())
            {
                auto discovered = DiscoverPBRCompanion(std::filesystem::path(m_Directory), albedoFilename.filename(), "_AO");
                if (discovered.empty())
                    discovered = FindFirstTextureByStem(modelDirectory, { "ao", "ambient_occlusion", "ambientocclusion", "occlusion" });

                // If no dedicated AO map exists, keep the older roughness-as-AO fallback
                // for assets that only ship a single monochrome detail map.
                if (discovered.empty())
                    discovered = DiscoverPBRCompanion(std::filesystem::path(m_Directory), albedoFilename.filename(), "_R");
                if (discovered.empty())
                    discovered = FindFirstTextureByStem(modelDirectory, { "roughness", "rough" });

                if (!discovered.empty())
                {
                    OLO_CORE_TRACE("Model: Auto-discovered AO map '{}' from albedo '{}'", discovered.string(), albedoFilename.string());
                    auto tex = loadTextureFromPath(discovered);
                    if (tex && tex->IsLoaded())
                    {
                        materialRef->SetAOMap(tex);
                    }
                }
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

    void Model::DrawParallel(const glm::mat4& transform, const Material& fallbackMaterial, i32 entityID) const
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
                true,     // IsStatic
                entityID, // EntityID for picking
                false,    // IsAnimated
                nullptr   // BoneMatrices
            });
        }

        // Submit all meshes in parallel
        Renderer3D::SubmitMeshesParallel(meshDescriptors);
    }

    void Model::DrawParallel(const glm::mat4& transform, i32 entityID) const
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
                true,     // IsStatic
                entityID, // EntityID for picking
                false,    // IsAnimated
                nullptr   // BoneMatrices
            });
        }

        // Submit all meshes in parallel
        Renderer3D::SubmitMeshesParallel(meshDescriptors);
    }

    Ref<MeshSource> Model::CreateCombinedMeshSource() const
    {
        OLO_PROFILE_FUNCTION();

        if (m_Meshes.empty())
        {
            OLO_CORE_WARN("Model::CreateCombinedMeshSource: No meshes to combine");
            return nullptr;
        }

        // Calculate total vertex and index counts
        sizet totalVertices = 0;
        sizet totalIndices = 0;
        for (const auto& mesh : m_Meshes)
        {
            if (mesh && mesh->GetMeshSource())
            {
                totalVertices += mesh->GetMeshSource()->GetVertices().Num();
                totalIndices += mesh->GetMeshSource()->GetIndices().Num();
            }
        }

        if (totalVertices == 0)
        {
            OLO_CORE_WARN("Model::CreateCombinedMeshSource: No vertices found in meshes");
            return nullptr;
        }

        // Create combined vertex and index arrays
        TArray<Vertex> combinedVertices;
        TArray<u32> combinedIndices;
        combinedVertices.Reserve(static_cast<i32>(totalVertices));
        combinedIndices.Reserve(static_cast<i32>(totalIndices));

        // Create the combined MeshSource
        auto combinedMeshSource = Ref<MeshSource>::Create();

        u32 baseVertex = 0;
        u32 baseIndex = 0;

        for (sizet meshIdx = 0; meshIdx < m_Meshes.size(); ++meshIdx)
        {
            const auto& mesh = m_Meshes[meshIdx];
            if (!mesh || !mesh->GetMeshSource())
            {
                continue;
            }

            const auto& srcMeshSource = mesh->GetMeshSource();
            const auto& srcVertices = srcMeshSource->GetVertices();
            const auto& srcIndices = srcMeshSource->GetIndices();

            // Copy vertices
            for (i32 i = 0; i < srcVertices.Num(); ++i)
            {
                combinedVertices.Add(srcVertices[i]);
            }

            // Copy indices with offset
            for (i32 i = 0; i < srcIndices.Num(); ++i)
            {
                combinedIndices.Add(srcIndices[i] + baseVertex);
            }

            // Create a submesh for this mesh
            Submesh submesh;
            submesh.m_BaseVertex = baseVertex;
            submesh.m_BaseIndex = baseIndex;
            submesh.m_VertexCount = static_cast<u32>(srcVertices.Num());
            submesh.m_IndexCount = static_cast<u32>(srcIndices.Num());
            submesh.m_MaterialIndex = static_cast<u32>(meshIdx); // Use mesh index as material index
            submesh.m_IsRigged = false;
            submesh.m_NodeName = "Mesh_" + std::to_string(meshIdx);

            // Copy submesh bounding box if available
            if (!srcMeshSource->GetSubmeshes().IsEmpty())
            {
                submesh.m_BoundingBox = srcMeshSource->GetSubmeshes()[0].m_BoundingBox;
            }

            combinedMeshSource->GetSubmeshes().Add(submesh);

            baseVertex += static_cast<u32>(srcVertices.Num());
            baseIndex += static_cast<u32>(srcIndices.Num());
        }

        // Set the combined vertex and index data
        combinedMeshSource->GetVertices() = std::move(combinedVertices);
        combinedMeshSource->GetIndices() = std::move(combinedIndices);

        // NOTE: Do NOT call Build() here — this MeshSource is only used for cache serialization,
        // not rendering. Build() would re-run OptimizeMesh on already-optimized data, corrupting
        // submesh base vertex/index offsets.

        OLO_CORE_INFO("Model::CreateCombinedMeshSource: Combined {} meshes into {} vertices, {} indices, {} submeshes",
                      m_Meshes.size(), combinedMeshSource->GetVertices().Num(),
                      combinedMeshSource->GetIndices().Num(), combinedMeshSource->GetSubmeshes().Num());

        return combinedMeshSource;
    }
} // namespace OloEngine
