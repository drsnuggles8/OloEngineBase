
#include "OloEnginePCH.h"
#include "AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include <yaml-cpp/yaml.h>
#include <fstream>


namespace OloEngine
{
    //////////////////////////////////////////////////////////////////////////////////
    // TextureSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool TextureSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        asset = Texture2D::Create(metadata.FilePath);
        asset->Handle = metadata.Handle;

        Ref<Texture2D> texture = asset.As<Texture2D>();
        bool result = texture && texture->IsLoaded();
        if (!result)
        {
            asset->SetFlag(AssetFlag::Invalid, true);
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to load texture: {}", metadata.FilePath.string());
        }

        return result;
    }

    bool TextureSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto texture = AssetManager::GetAsset<Texture2D>(handle);
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::SerializeToAssetPack - Invalid texture asset");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        
        // Write texture data (implementation needed based on OloEngine's texture format)
        // TODO: Implement actual texture serialization to asset pack
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> TextureSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        // TODO: Implement actual texture deserialization from asset pack
        // This should read the texture data and create a Texture2D
        
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // FontSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool FontSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        asset = Font::Create(metadata.FilePath);
        asset->Handle = metadata.Handle;

        // Note: Font loading validation could be added here if needed
        // bool result = asset.As<Font>()->Loaded();
        // if (!result) { ... }

        return true;
    }

    bool FontSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        outInfo.Offset = stream.GetStreamPosition();

        Ref<Font> font = AssetManager::GetAsset<Font>(handle);
        if (!font)
        {
            OLO_CORE_ERROR("FontSerializer::SerializeToAssetPack - Invalid font asset");
            return false;
        }
        
        // Write font name and data
        stream.WriteString(font->GetName());
        
        // TODO: Read font file data and write to stream
        // This should read the original font file and write its contents
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> FontSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string name;
        stream.ReadString(name);
        
        // TODO: Read font data buffer and create font
        // Buffer fontData;
        // stream.ReadBuffer(fontData);
        // return Font::Create(name, fontData);
        
        OLO_CORE_WARN("FontSerializer::DeserializeFromAssetPack not yet fully implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MaterialAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void MaterialAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<MaterialAsset> materialAsset = asset.As<MaterialAsset>();
        if (!materialAsset)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::Serialize - Invalid material asset");
            return;
        }

        std::string yamlString = SerializeToYAML(materialAsset);

        std::filesystem::path filepath = Project::GetAssetDirectory() / metadata.FilePath;
        std::ofstream fout(filepath);
        fout << yamlString;
        fout.close();
    }

    bool MaterialAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        Ref<MaterialAsset> materialAsset;
        if (!DeserializeFromYAML(GetYAML(metadata), materialAsset, metadata.Handle))
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - Failed to deserialize material: {}", metadata.FilePath.string());
            // Note: Could set asset->SetFlag(AssetFlag::Invalid, true) here, but asset is not created yet
            return false;
        }
        asset = materialAsset;
        return true;
    }

    void MaterialAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        RegisterDependenciesFromYAML(GetYAML(metadata), metadata.Handle);
    }

    bool MaterialAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<MaterialAsset> materialAsset = AssetManager::GetAsset<MaterialAsset>(handle);
        if (!materialAsset)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::SerializeToAssetPack - Invalid material asset");
            return false;
        }

        std::string yamlString = SerializeToYAML(materialAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> MaterialAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<MaterialAsset> materialAsset;
        bool result = DeserializeFromYAML(yamlString, materialAsset, assetInfo.Handle);
        if (!result)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromAssetPack - Failed to deserialize material from YAML");
            return nullptr;
        }

        return materialAsset;
    }

    std::string MaterialAssetSerializer::SerializeToYAML(Ref<MaterialAsset> materialAsset) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // Material
        out << YAML::Key << "Material" << YAML::Value;
        {
            out << YAML::BeginMap;

            auto material = materialAsset->GetMaterial();
            if (material)
            {
                // Serialize shader name
                out << YAML::Key << "Shader" << YAML::Value << material->GetShader()->GetName();
                
                // Serialize material textures
                out << YAML::Key << "Textures" << YAML::Value << YAML::BeginMap;
                auto& textures = material->GetTextures();
                for (const auto& [name, texture] : textures)
                {
                    if (texture && texture->Handle != 0)
                    {
                        out << YAML::Key << name << YAML::Value << texture->Handle;
                    }
                }
                out << YAML::EndMap;
                
                // Serialize material uniforms/properties
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
                auto& uniforms = material->GetUniforms();
                for (const auto& [name, uniform] : uniforms)
                {
                    out << YAML::Key << name << YAML::Value;
                    // Serialize based on uniform type
                    switch (uniform.Type)
                    {
                        case ShaderUniformType::Float:
                            out << uniform.GetValue<float>();
                            break;
                        case ShaderUniformType::Float2:
                            out << uniform.GetValue<glm::vec2>();
                            break;
                        case ShaderUniformType::Float3:
                            out << uniform.GetValue<glm::vec3>();
                            break;
                        case ShaderUniformType::Float4:
                            out << uniform.GetValue<glm::vec4>();
                            break;
                        // Add more types as needed
                    }
                }
                out << YAML::EndMap;

                // Serialize material flags
                out << YAML::Key << "MaterialFlags" << YAML::Value << material->GetFlags();
            }

            out << YAML::EndMap;
        }
        out << YAML::EndMap; // Material

        return std::string(out.c_str());
    }

    std::string MaterialAssetSerializer::GetYAML(const AssetMetadata& metadata) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::GetYAML - Failed to open file: {}", path.string());
            return "";
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        return strStream.str();
    }

    void MaterialAssetSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        // Deregister existing dependencies first
        AssetManager::DeregisterDependencies(handle);

        YAML::Node root = YAML::Load(yamlString);
        YAML::Node materialNode = root["Material"];
        if (!materialNode)
            return;

        // Register texture dependencies
        if (materialNode["Textures"])
        {
            for (const auto& textureNode : materialNode["Textures"])
            {
                AssetHandle textureHandle = textureNode.second.as<AssetHandle>(0);
                if (textureHandle != 0)
                {
                    AssetManager::RegisterDependency(textureHandle, handle);
                }
            }
        }
    }

    bool MaterialAssetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MaterialAsset>& targetMaterialAsset, AssetHandle handle) const
    {
        RegisterDependenciesFromYAML(yamlString, handle);

        YAML::Node root = YAML::Load(yamlString);
        YAML::Node materialNode = root["Material"];
        if (!materialNode)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromYAML - No Material node found");
            return false;
        }

        // Load shader
        std::string shaderName = materialNode["Shader"].as<std::string>("DefaultPBR");
        auto shader = Renderer::GetShaderLibrary()->Get(shaderName);
        if (!shader)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromYAML - Shader not found: {}", shaderName);
            return false;
        }

        auto material = Material::Create(shader);
        targetMaterialAsset = CreateRef<MaterialAsset>(material);
        targetMaterialAsset->Handle = handle;

        // Load textures
        if (materialNode["Textures"])
        {
            for (const auto& textureNode : materialNode["Textures"])
            {
                std::string textureName = textureNode.first.as<std::string>();
                AssetHandle textureHandle = textureNode.second.as<AssetHandle>(0);
                
                if (textureHandle != 0)
                {
                    auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                    if (texture)
                    {
                        material->Set(textureName, texture);
                    }
                }
            }
        }
        
        // Load properties/uniforms
        if (materialNode["Properties"])
        {
            for (const auto& propNode : materialNode["Properties"])
            {
                std::string propName = propNode.first.as<std::string>();
                // Set material properties based on shader uniform types
                // This would need to be expanded based on the actual uniform system
                // For now, we'll skip this as it requires more complex type handling
            }
        }

        // Load material flags
        if (materialNode["MaterialFlags"])
        {
            uint32_t flags = materialNode["MaterialFlags"].as<uint32_t>(0);
            material->SetFlags(flags);
        }

        return true;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // EnvironmentSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool EnvironmentSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("EnvironmentSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // Load HDR environment map
        auto environment = Environment::Create(path.string());
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::TryLoadData - Failed to load environment: {}", path.string());
            return false;
        }

        environment->Handle = metadata.Handle;
        asset = environment;
        
        OLO_CORE_TRACE("EnvironmentSerializer::TryLoadData - Successfully loaded environment: {}", path.string());
        return true;
    }

    bool EnvironmentSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement environment pack serialization
        OLO_CORE_WARN("EnvironmentSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> EnvironmentSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement environment pack deserialization
        OLO_CORE_WARN("EnvironmentSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AudioFileSourceSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AudioFileSourceSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement audio serialization
        OLO_CORE_WARN("AudioFileSourceSerializer::Serialize not yet implemented");
    }

    bool AudioFileSourceSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // TODO: Implement audio loading
        OLO_CORE_WARN("AudioFileSourceSerializer::TryLoadData not yet implemented");
        return false;
    }

    bool AudioFileSourceSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement audio pack serialization
        OLO_CORE_WARN("AudioFileSourceSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> AudioFileSourceSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement audio pack deserialization
        OLO_CORE_WARN("AudioFileSourceSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // SoundConfigSerializer - DISABLED (SoundConfig class not implemented)
    //////////////////////////////////////////////////////////////////////////////////

    /*
    void SoundConfigSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // Implementation commented out - SoundConfig class not available
        OLO_CORE_WARN("SoundConfigSerializer::Serialize - SoundConfig class not implemented");
    }

    bool SoundConfigSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // Implementation commented out - SoundConfig class not available
        OLO_CORE_WARN("SoundConfigSerializer::TryLoadData - SoundConfig class not implemented");
        return false;
    }

    bool SoundConfigSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_CORE_WARN("SoundConfigSerializer::SerializeToAssetPack - SoundConfig class not implemented");
        return false;
    }

    Ref<Asset> SoundConfigSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_CORE_WARN("SoundConfigSerializer::DeserializeFromAssetPack - SoundConfig class not implemented");
        return nullptr;
    }

    std::string SoundConfigSerializer::SerializeToYAML(Ref<SoundConfig> soundConfig) const
    {
        OLO_CORE_WARN("SoundConfigSerializer::SerializeToYAML - SoundConfig class not implemented");
        return "";
    }

    bool SoundConfigSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<SoundConfig> targetSoundConfig) const
    {
        OLO_CORE_WARN("SoundConfigSerializer::DeserializeFromYAML - SoundConfig class not implemented");
        return false;
    }
    */

    //////////////////////////////////////////////////////////////////////////////////
    // PrefabSerializer - DISABLED (Prefab class not implemented)
    //////////////////////////////////////////////////////////////////////////////////

    /*
    void PrefabSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // Implementation commented out - Prefab class not available
        OLO_CORE_WARN("PrefabSerializer::Serialize - Prefab class not implemented");
    }

    bool PrefabSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // Implementation commented out - Prefab class not available
        OLO_CORE_WARN("PrefabSerializer::TryLoadData - Prefab class not implemented");
        return false;
    }

    bool PrefabSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_CORE_WARN("PrefabSerializer::SerializeToAssetPack - Prefab class not implemented");
        return false;
    }

    Ref<Asset> PrefabSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_CORE_WARN("PrefabSerializer::DeserializeFromAssetPack - Prefab class not implemented");
        return nullptr;
    }
    */

    //////////////////////////////////////////////////////////////////////////////////
    // SceneAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SceneAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Scene> scene = asset.As<Scene>();
        if (!scene)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::Serialize - Asset is not a Scene");
            return;
        }

        SceneSerializer serializer(scene);
        serializer.Serialize(metadata.FilePath);
    }

    bool SceneAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        Ref<Scene> scene = CreateRef<Scene>();
        SceneSerializer serializer(scene);
        
        if (serializer.Deserialize(metadata.FilePath))
        {
            asset = scene;
            return true;
        }
        
        return false;
    }

    bool SceneAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto scene = AssetManager::GetAsset<Scene>(handle);
        if (!scene)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::SerializeToAssetPack - Invalid scene asset");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        
        // Serialize scene to YAML string first
        SceneSerializer serializer(scene);
        std::string yamlData = serializer.SerializeToString();
        
        // Write YAML data size and content
        uint32_t dataSize = (uint32_t)yamlData.size();
        stream.WriteRaw(dataSize);
        stream.WriteBuffer((void*)yamlData.c_str(), dataSize);
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        
        OLO_CORE_TRACE("SceneAssetSerializer::SerializeToAssetPack - Serialized scene, size: {} bytes", outInfo.Size);
        return true;
    }

    Ref<Asset> SceneAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        // Read YAML data size and content
        uint32_t dataSize;
        stream.ReadRaw(dataSize);
        
        std::vector<char> yamlData(dataSize + 1);
        stream.ReadBuffer(yamlData.data(), dataSize);
        yamlData[dataSize] = '\0'; // Null terminate
        
        // Create scene and deserialize from YAML
        auto scene = CreateRef<Scene>();
        SceneSerializer serializer(scene);
        
        if (!serializer.DeserializeFromString(yamlData.data()))
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeFromAssetPack - Failed to deserialize scene from YAML");
            return nullptr;
        }
        
        scene->Handle = assetInfo.Handle;
        
        OLO_CORE_TRACE("SceneAssetSerializer::DeserializeFromAssetPack - Deserialized scene from pack");
        return scene;
    }

    Ref<Scene> SceneAssetSerializer::DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const
    {
        // TODO: Implement scene pack deserialization
        OLO_CORE_WARN("SceneAssetSerializer::DeserializeSceneFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MeshColliderSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void MeshColliderSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement mesh collider serialization
        OLO_CORE_WARN("MeshColliderSerializer::Serialize not yet implemented");
    }

    bool MeshColliderSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - Failed to open file: {}", path.string());
            return false;
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        stream.close();

        YAML::Node data;
        try
        {
            data = YAML::Load(strStream.str());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - YAML parsing error: {}", e.what());
            return false;
        }

        auto colliderNode = data["MeshCollider"];
        if (!colliderNode)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - No MeshCollider node found");
            return false;
        }

        auto meshCollider = CreateRef<MeshColliderAsset>();
        
        // Load associated mesh
        if (colliderNode["MeshSource"])
        {
            AssetHandle meshHandle = colliderNode["MeshSource"].as<AssetHandle>();
            if (meshHandle != 0)
            {
                auto meshSource = AssetManager::GetAsset<MeshSource>(meshHandle);
                if (meshSource)
                {
                    meshCollider->SetMeshSource(meshSource);
                }
            }
        }
        
        // Load collider properties
        if (colliderNode["IsConvex"])
        {
            meshCollider->SetConvex(colliderNode["IsConvex"].as<bool>());
        }

        meshCollider->Handle = metadata.Handle;
        asset = meshCollider;
        
        OLO_CORE_TRACE("MeshColliderSerializer::TryLoadData - Successfully loaded mesh collider: {}", path.string());
        return true;
    }

    bool MeshColliderSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement mesh collider pack serialization
        OLO_CORE_WARN("MeshColliderSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> MeshColliderSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement mesh collider pack deserialization
        OLO_CORE_WARN("MeshColliderSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    std::string MeshColliderSerializer::SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const
    {
        // TODO: Implement YAML serialization
        return "";
    }

    bool MeshColliderSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const
    {
        // TODO: Implement YAML deserialization
        return false;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // ScriptFileSerializer - DISABLED (ScriptAsset class not implemented)
    //////////////////////////////////////////////////////////////////////////////////

    /*
    void ScriptFileSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // Implementation commented out - ScriptAsset class not available
        OLO_CORE_WARN("ScriptFileSerializer::Serialize - ScriptAsset class not implemented");
    }

    bool ScriptFileSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // Implementation commented out - ScriptAsset class not available
        OLO_CORE_WARN("ScriptFileSerializer::TryLoadData - ScriptAsset class not implemented");
        return false;
    }

    bool ScriptFileSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_CORE_WARN("ScriptFileSerializer::SerializeToAssetPack - ScriptAsset class not implemented");
        return false;
    }

    Ref<Asset> ScriptFileSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_CORE_WARN("ScriptFileSerializer::DeserializeFromAssetPack - ScriptAsset class not implemented");
        return nullptr;
    }
    */

    //////////////////////////////////////////////////////////////////////////////////
    // MeshSourceSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool MeshSourceSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MeshSourceSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // Use Assimp to load the mesh file
        auto meshSource = CreateRef<MeshSource>(path);
        if (!meshSource->IsValid())
        {
            OLO_CORE_ERROR("MeshSourceSerializer::TryLoadData - Failed to load mesh source: {}", path.string());
            return false;
        }
        
        meshSource->Handle = metadata.Handle;
        asset = meshSource;
        
        OLO_CORE_TRACE("MeshSourceSerializer::TryLoadData - Successfully loaded mesh source: {} ({} submeshes)", 
                      path.string(), meshSource->GetSubmeshes().size());
        return true;
    }

    bool MeshSourceSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement mesh source pack serialization
        OLO_CORE_WARN("MeshSourceSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> MeshSourceSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement mesh source pack deserialization
        OLO_CORE_WARN("MeshSourceSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MeshSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void MeshSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Mesh> mesh = asset.As<Mesh>();
        if (!mesh)
        {
            OLO_CORE_ERROR("MeshSerializer::Serialize - Invalid mesh asset");
            return;
        }

        std::filesystem::path filepath = Project::GetAssetDirectory() / metadata.FilePath;
        
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Mesh" << YAML::Value << YAML::BeginMap;
        
        // Serialize mesh properties
        out << YAML::Key << "SubmeshCount" << YAML::Value << mesh->GetSubmeshes().size();
        
        // Serialize submeshes
        out << YAML::Key << "Submeshes" << YAML::Value << YAML::BeginSeq;
        for (const auto& submesh : mesh->GetSubmeshes())
        {
            out << YAML::BeginMap;
            out << YAML::Key << "BaseVertex" << YAML::Value << submesh.BaseVertex;
            out << YAML::Key << "BaseIndex" << YAML::Value << submesh.BaseIndex;
            out << YAML::Key << "IndexCount" << YAML::Value << submesh.IndexCount;
            out << YAML::Key << "VertexCount" << YAML::Value << submesh.VertexCount;
            out << YAML::Key << "MaterialIndex" << YAML::Value << submesh.MaterialIndex;
            
            // Serialize transform
            out << YAML::Key << "Transform" << YAML::Value << submesh.Transform;
            
            // Serialize bounding box
            out << YAML::Key << "BoundingBox" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "Min" << YAML::Value << submesh.BoundingBox.Min;
            out << YAML::Key << "Max" << YAML::Value << submesh.BoundingBox.Max;
            out << YAML::EndMap;
            
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        
        // Serialize materials
        out << YAML::Key << "Materials" << YAML::Value << YAML::BeginSeq;
        for (const auto& material : mesh->GetMaterials())
        {
            out << (material ? material->Handle : AssetHandle(0));
        }
        out << YAML::EndSeq;
        
        out << YAML::EndMap;
        out << YAML::EndMap;
        
        std::ofstream fout(filepath);
        fout << out.c_str();
        fout.close();
        
        OLO_CORE_TRACE("MeshSerializer::Serialize - Serialized mesh to: {}", filepath.string());
    }

    bool MeshSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - Failed to open file: {}", path.string());
            return false;
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        stream.close();

        YAML::Node data;
        try
        {
            data = YAML::Load(strStream.str());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - YAML parsing error: {}", e.what());
            return false;
        }

        auto meshNode = data["Mesh"];
        if (!meshNode)
        {
            OLO_CORE_ERROR("MeshSerializer::TryLoadData - No Mesh node found");
            return false;
        }

        // Create mesh
        auto mesh = CreateRef<Mesh>();
        
        // Load submeshes
        if (meshNode["Submeshes"])
        {
            std::vector<Submesh> submeshes;
            for (const auto& submeshNode : meshNode["Submeshes"])
            {
                Submesh submesh{};
                submesh.BaseVertex = submeshNode["BaseVertex"].as<uint32_t>();
                submesh.BaseIndex = submeshNode["BaseIndex"].as<uint32_t>();
                submesh.IndexCount = submeshNode["IndexCount"].as<uint32_t>();
                submesh.VertexCount = submeshNode["VertexCount"].as<uint32_t>();
                submesh.MaterialIndex = submeshNode["MaterialIndex"].as<uint32_t>();
                submesh.Transform = submeshNode["Transform"].as<glm::mat4>();
                
                // Load bounding box
                auto bbNode = submeshNode["BoundingBox"];
                submesh.BoundingBox.Min = bbNode["Min"].as<glm::vec3>();
                submesh.BoundingBox.Max = bbNode["Max"].as<glm::vec3>();
                
                submeshes.push_back(submesh);
            }
            mesh->SetSubmeshes(submeshes);
        }
        
        // Load materials
        if (meshNode["Materials"])
        {
            std::vector<Ref<MaterialAsset>> materials;
            for (const auto& materialNode : meshNode["Materials"])
            {
                AssetHandle materialHandle = materialNode.as<AssetHandle>();
                if (materialHandle != 0)
                {
                    auto material = AssetManager::GetAsset<MaterialAsset>(materialHandle);
                    materials.push_back(material);
                }
                else
                {
                    materials.push_back(nullptr);
                }
            }
            mesh->SetMaterials(materials);
        }

        mesh->Handle = metadata.Handle;
        asset = mesh;
        
        OLO_CORE_TRACE("MeshSerializer::TryLoadData - Successfully loaded mesh: {}", path.string());
        return true;
    }

    void MeshSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // Load the mesh temporarily to extract material dependencies
        Ref<Asset> asset;
        if (TryLoadData(metadata, asset))
        {
            auto mesh = asset.As<Mesh>();
            if (mesh)
            {
                for (const auto& material : mesh->GetMaterials())
                {
                    if (material && material->Handle != 0)
                    {
                        Project::GetAssetManager()->RegisterDependency(material->Handle, metadata.Handle);
                    }
                }
            }
        }
    }

    bool MeshSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement mesh pack serialization
        OLO_CORE_WARN("MeshSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> MeshSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement mesh pack deserialization
        OLO_CORE_WARN("MeshSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // StaticMeshSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void StaticMeshSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement static mesh serialization
        OLO_CORE_WARN("StaticMeshSerializer::Serialize not yet implemented");
    }

    bool StaticMeshSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // Load static mesh (typically from processed mesh files)
        auto staticMesh = CreateRef<StaticMesh>(path);
        if (!staticMesh->IsValid())
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Failed to load static mesh: {}", path.string());
            return false;
        }

        staticMesh->Handle = metadata.Handle;
        asset = staticMesh;
        
        OLO_CORE_TRACE("StaticMeshSerializer::TryLoadData - Successfully loaded static mesh: {}", path.string());
        return true;
    }

    void StaticMeshSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // TODO: Implement dependency registration
        OLO_CORE_WARN("StaticMeshSerializer::RegisterDependencies not yet implemented");
    }

    bool StaticMeshSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement static mesh pack serialization
        OLO_CORE_WARN("StaticMeshSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> StaticMeshSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement static mesh pack deserialization
        OLO_CORE_WARN("StaticMeshSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AnimationAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AnimationAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement animation serialization
        OLO_CORE_WARN("AnimationAssetSerializer::Serialize not yet implemented");
    }

    bool AnimationAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // TODO: Implement animation loading
        OLO_CORE_WARN("AnimationAssetSerializer::TryLoadData not yet implemented");
        return false;
    }

    void AnimationAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // TODO: Implement dependency registration
        OLO_CORE_WARN("AnimationAssetSerializer::RegisterDependencies not yet implemented");
    }

    bool AnimationAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement animation pack serialization
        OLO_CORE_WARN("AnimationAssetSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> AnimationAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement animation pack deserialization
        OLO_CORE_WARN("AnimationAssetSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AnimationGraphAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AnimationGraphAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement animation graph serialization
        OLO_CORE_WARN("AnimationGraphAssetSerializer::Serialize not yet implemented");
    }

    bool AnimationGraphAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // TODO: Implement animation graph loading
        OLO_CORE_WARN("AnimationGraphAssetSerializer::TryLoadData not yet implemented");
        return false;
    }

    void AnimationGraphAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // TODO: Implement dependency registration
        OLO_CORE_WARN("AnimationGraphAssetSerializer::RegisterDependencies not yet implemented");
    }

    bool AnimationGraphAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement animation graph pack serialization
        OLO_CORE_WARN("AnimationGraphAssetSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> AnimationGraphAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement animation graph pack deserialization
        OLO_CORE_WARN("AnimationGraphAssetSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // SoundGraphGraphSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SoundGraphGraphSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement sound graph serialization
        OLO_CORE_WARN("SoundGraphGraphSerializer::Serialize not yet implemented");
    }

    bool SoundGraphGraphSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // TODO: Implement sound graph loading
        OLO_CORE_WARN("SoundGraphGraphSerializer::TryLoadData not yet implemented");
        return false;
    }

    bool SoundGraphGraphSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement sound graph pack serialization
        OLO_CORE_WARN("SoundGraphGraphSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> SoundGraphGraphSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement sound graph pack deserialization
        OLO_CORE_WARN("SoundGraphGraphSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

} // namespace OloEngine
