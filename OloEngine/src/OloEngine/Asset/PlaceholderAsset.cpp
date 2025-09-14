#include "OloEnginePCH.h"
#include "PlaceholderAsset.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Material.h" 
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Asset/AssetManager.h"

namespace OloEngine
{
    // Static member definitions
    std::unordered_map<AssetType, Ref<Asset>> PlaceholderAssetManager::s_PlaceholderAssets;
    std::mutex PlaceholderAssetManager::s_PlaceholderMutex;
    bool PlaceholderAssetManager::s_Initialized = false;

    //////////////////////////////////////////////////////////////////////////////////
    // PlaceholderTexture
    //////////////////////////////////////////////////////////////////////////////////

    PlaceholderTexture::PlaceholderTexture() : PlaceholderAsset(AssetType::Texture2D)
    {
        CreatePlaceholderTexture();
    }

    void PlaceholderTexture::CreatePlaceholderTexture()
    {
        // Create a 64x64 checkerboard pattern in magenta/black
        constexpr u32 size = 64;
        constexpr u32 checkerSize = 8;
        
        std::vector<u8> pixels(size * size * 4); // RGBA
        
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                u32 index = (y * size + x) * 4;
                
                // Create checkerboard pattern
                bool isChecker = ((x / checkerSize) + (y / checkerSize)) % 2 == 0;
                
                if (isChecker)
                {
                    // Magenta
                    pixels[index + 0] = 255; // R
                    pixels[index + 1] = 0;   // G
                    pixels[index + 2] = 255; // B
                    pixels[index + 3] = 255; // A
                }
                else
                {
                    // Black
                    pixels[index + 0] = 0;   // R
                    pixels[index + 1] = 0;   // G
                    pixels[index + 2] = 0;   // B
                    pixels[index + 3] = 255; // A
                }
            }
        }

        TextureSpecification spec;
        spec.Width = size;
        spec.Height = size;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        
        m_Texture = Texture2D::Create(spec);
        // Note: Data upload may require additional API not available here
        
        OLO_CORE_TRACE("PlaceholderTexture: Created {}x{} checkerboard texture", size, size);
    }

    //////////////////////////////////////////////////////////////////////////////////
    // PlaceholderMaterial
    //////////////////////////////////////////////////////////////////////////////////

    PlaceholderMaterial::PlaceholderMaterial() : PlaceholderAsset(AssetType::Material)
    {
        CreatePlaceholderMaterial();
    }

    void PlaceholderMaterial::CreatePlaceholderMaterial()
    {
        m_Material = Ref<MaterialAsset>::Create();
        
        // Set distinctive placeholder material properties
        m_Material->SetAlbedoColor(glm::vec3(1.0f, 0.0f, 1.0f)); // Magenta
        m_Material->SetMetalness(0.0f);
        m_Material->SetRoughness(0.8f);
        m_Material->SetEmission(0.1f); // Slight glow to make it obvious
        
        // Use placeholder texture if available
        auto placeholderTexture = PlaceholderAssetManager::GetPlaceholderAsset(AssetType::Texture2D);
        if (auto texPlaceholder = placeholderTexture.As<PlaceholderTexture>())
        {
            // Note: SetAlbedoMap may need AssetHandle instead of Ref<Texture2D>
            // m_Material->SetAlbedoMap(texPlaceholder->GetTexture());
        }
        
        OLO_CORE_TRACE("PlaceholderMaterial: Created placeholder material with magenta color");
    }

    //////////////////////////////////////////////////////////////////////////////////
    // PlaceholderMesh
    //////////////////////////////////////////////////////////////////////////////////

    PlaceholderMesh::PlaceholderMesh() : PlaceholderAsset(AssetType::Mesh)
    {
        CreatePlaceholderMesh();
    }

    void PlaceholderMesh::CreatePlaceholderMesh()
    {
        // Create a simple cube mesh as placeholder
        std::vector<Vertex> vertices = {
            // Front face
            {{ -0.5f, -0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 0.0f, 0.0f }},
            {{  0.5f, -0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 1.0f, 0.0f }},
            {{  0.5f,  0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 1.0f, 1.0f }},
            {{ -0.5f,  0.5f,  0.5f }, {  0.0f,  0.0f,  1.0f }, { 0.0f, 1.0f }},
            
            // Back face
            {{ -0.5f, -0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 1.0f, 0.0f }},
            {{ -0.5f,  0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 1.0f, 1.0f }},
            {{  0.5f,  0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 0.0f, 1.0f }},
            {{  0.5f, -0.5f, -0.5f }, {  0.0f,  0.0f, -1.0f }, { 0.0f, 0.0f }},
        };

        std::vector<u32> indices = {
            // Front face
            0, 1, 2, 2, 3, 0,
            // Back face
            4, 5, 6, 6, 7, 4,
            // Left face
            4, 0, 3, 3, 5, 4,
            // Right face
            1, 7, 6, 6, 2, 1,
            // Top face
            3, 2, 6, 6, 5, 3,
            // Bottom face
            4, 7, 1, 1, 0, 4
        };

        // Create MeshSource first, then create Mesh from it
        auto meshSource = Ref<MeshSource>::Create(vertices, indices);
        m_Mesh = Ref<Mesh>::Create(meshSource);
        
        OLO_CORE_TRACE("PlaceholderMesh: Created placeholder cube mesh with {} vertices, {} indices", 
                       vertices.size(), indices.size());
    }

    //////////////////////////////////////////////////////////////////////////////////
    // PlaceholderAudio
    //////////////////////////////////////////////////////////////////////////////////

    PlaceholderAudio::PlaceholderAudio() : PlaceholderAsset(AssetType::Audio)
    {
        CreatePlaceholderAudio();
    }

    void PlaceholderAudio::CreatePlaceholderAudio()
    {
        // For placeholder audio, we don't create an actual AudioSource since it requires a file path
        // Instead, we leave m_AudioSource as nullptr to indicate "no audio"
        // When a system tries to use this, it should check for nullptr and handle gracefully
        m_AudioSource = nullptr;
        
        OLO_CORE_TRACE("PlaceholderAudio: Created placeholder audio (null - no audio)");
    }

    //////////////////////////////////////////////////////////////////////////////////
    // PlaceholderAssetManager
    //////////////////////////////////////////////////////////////////////////////////

    void PlaceholderAssetManager::Initialize()
    {
        std::scoped_lock lock(s_PlaceholderMutex);
        
        if (s_Initialized)
        {
            OLO_CORE_WARN("PlaceholderAssetManager::Initialize - Already initialized");
            return;
        }

        s_PlaceholderAssets.clear();
        s_Initialized = true;
        
        OLO_CORE_INFO("PlaceholderAssetManager: Initialized");
    }

    void PlaceholderAssetManager::Shutdown()
    {
        std::scoped_lock lock(s_PlaceholderMutex);
        
        if (!s_Initialized)
            return;
        
        s_PlaceholderAssets.clear();
        s_Initialized = false;
    }

    Ref<Asset> PlaceholderAssetManager::GetPlaceholderAsset(AssetType type)
    {
        std::scoped_lock lock(s_PlaceholderMutex);
        
        if (!s_Initialized)
        {
            OLO_CORE_ERROR("PlaceholderAssetManager::GetPlaceholderAsset - Not initialized");
            return nullptr;
        }

        // Check if we already have a placeholder for this type
        auto it = s_PlaceholderAssets.find(type);
        if (it != s_PlaceholderAssets.end())
        {
            return it->second;
        }

        // Create new placeholder
        Ref<Asset> placeholder = CreatePlaceholderAsset(type);
        if (placeholder)
        {
            s_PlaceholderAssets[type] = placeholder;
            OLO_CORE_TRACE("PlaceholderAssetManager: Created new placeholder for asset type {}",
                           AssetUtils::AssetTypeToString(type));
        }

        return placeholder;
    }

    bool PlaceholderAssetManager::IsPlaceholderAsset(const Ref<Asset>& asset)
    {
        if (!asset)
            return false;

        // Check if it's derived from PlaceholderAsset
        return asset.As<PlaceholderAsset>() != nullptr;
    }

    Ref<Asset> PlaceholderAssetManager::CreatePlaceholderAsset(AssetType type)
    {
        switch (type)
        {
            case AssetType::Texture2D:
            case AssetType::TextureCube:
                return Ref<PlaceholderTexture>::Create();
                
            case AssetType::Material:
                return Ref<PlaceholderMaterial>::Create();
                
            case AssetType::Mesh:
            case AssetType::StaticMesh:
            case AssetType::MeshSource:
                return Ref<PlaceholderMesh>::Create();
                
            case AssetType::Audio:
                return Ref<PlaceholderAudio>::Create();
                
            case AssetType::Scene:
            case AssetType::Prefab:
            case AssetType::EnvMap:
            case AssetType::Environment:
            case AssetType::Font:
            case AssetType::Script:
            case AssetType::ScriptFile:
            case AssetType::Shader:
            case AssetType::MeshCollider:
            case AssetType::AnimationClip:
            case AssetType::AnimationGraph:
            case AssetType::Model:
            default:
                return Ref<GenericPlaceholder>::Create(type);
        }
    }

} // namespace OloEngine