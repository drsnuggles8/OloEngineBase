#include "AssetManager.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Prefab.h"

#include <unordered_map>
#include <functional>

namespace OloEngine
{
    // Placeholder asset creation functions - similar to Hazel's approach
    static const std::unordered_map<AssetType, std::function<Ref<Asset>()>> s_AssetPlaceholderTable =
    {
        { AssetType::Texture2D, []() -> Ref<Asset> {
            // Cache the white texture to avoid recreating it on every call
            static Ref<Texture2D> s_WhiteTexture = []() {
                TextureSpecification spec;
                spec.Width = 1;
                spec.Height = 1;
                spec.Format = ImageFormat::RGBA8;
                spec.GenerateMips = false;
                auto texture = Texture2D::Create(spec);
                u32 whitePixel = 0xFFFFFFFF;
                texture->SetData(&whitePixel, sizeof(u32));
                return texture;
            }();
            return s_WhiteTexture;
        }},
        { AssetType::Font, []() -> Ref<Asset> {
            return Font::GetDefault();
        }},
        // TODO: Add EnvironmentMap placeholder when proper default creation is available
        // { AssetType::EnvMap, []() -> Ref<Asset> {
        //     // EnvironmentMap creation requires file path or complex setup
        //     return nullptr;
        // }},
        { AssetType::Material, []() -> Ref<Asset> {
            // Cache the default material to avoid recreating it
            static Ref<MaterialAsset> s_DefaultMaterial = []() {
                // Create a default non-transparent material
                return Ref<MaterialAsset>::Create(false);
            }();
            return s_DefaultMaterial;
        }},
        { AssetType::Mesh, []() -> Ref<Asset> {
            // Cache the unit cube mesh to avoid recreating it
            static Ref<Mesh> s_UnitCube = []() {
                return MeshPrimitives::CreateCube();
            }();
            return s_UnitCube;
        }},
        { AssetType::StaticMesh, []() -> Ref<Asset> {
            // Cache the unit cube mesh to avoid recreating it
            static Ref<Mesh> s_UnitCube = []() {
                return MeshPrimitives::CreateCube();
            }();
            return s_UnitCube;
        }},
        { AssetType::Scene, []() -> Ref<Asset> {
            // Create a new empty scene each time (scenes are typically not shared)
            return Scene::Create();
        }},
        { AssetType::Prefab, []() -> Ref<Asset> {
            // Create a new empty prefab each time
            return Ref<Prefab>::Create();
        }},
        { AssetType::Audio, []() -> Ref<Asset> {
            // Create a default silent audio file placeholder
            return Ref<AudioFile>::Create();
        }},
        // Add more placeholder types as needed
    };

    Ref<Asset> AssetManager::GetPlaceholderAsset(AssetType type)
    {
        auto it = s_AssetPlaceholderTable.find(type);
        if (it != s_AssetPlaceholderTable.end())
            return it->second();

        OLO_CORE_WARN("No placeholder asset available for type: {}", AssetUtils::AssetTypeToString(type));
        return nullptr;
    }

}
