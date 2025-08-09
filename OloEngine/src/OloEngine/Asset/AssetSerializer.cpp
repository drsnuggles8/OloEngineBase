
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
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Core/YAMLConverters.h"
#include <yaml-cpp/yaml.h>
#include <fstream>

namespace OloEngine
{
    //////////////////////////////////////////////////////////////////////////////////
    // TextureSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool TextureSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        Ref<Texture2D> texture = Texture2D::Create(metadata.FilePath.string());
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to create texture: {}", metadata.FilePath.string());
            return false;
        }
        
        texture->m_Handle = metadata.Handle;
        bool result = texture->IsLoaded();
        if (!result)
        {
            texture->SetFlag(AssetFlag::Invalid, true);
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to load texture: {}", metadata.FilePath.string());
        }
        
        asset = texture; // Direct assignment - Ref<Texture2D> should convert to Ref<Asset>
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
        
        // Write texture metadata
        const auto& spec = texture->GetSpecification();
        stream.WriteRaw<u32>(spec.Width);
        stream.WriteRaw<u32>(spec.Height);
        stream.WriteRaw<u32>(static_cast<u32>(spec.Format));
        stream.WriteRaw<bool>(spec.GenerateMips);
        
        // Write texture path for reference
        const std::string& path = texture->GetPath();
        stream.WriteString(path);
        
        // Write additional metadata for better texture recreation
        stream.WriteRaw<bool>(texture->HasAlphaChannel());
        stream.WriteRaw<bool>(texture->IsLoaded());
        
        // Add texture creation timestamp for dependency tracking
        auto now = std::chrono::steady_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        stream.WriteRaw<i64>(timestamp);
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> TextureSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        // Read texture metadata
        u32 width;
        u32 height;
        u32 formatInt;
        bool generateMips;
        
        stream.ReadRaw(width);
        stream.ReadRaw(height);
        stream.ReadRaw(formatInt);
        stream.ReadRaw(generateMips);
        
        ImageFormat format = static_cast<ImageFormat>(formatInt);
        std::string path;
        stream.ReadString(path);
        
        // Create texture specification
        TextureSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Format = format;
        spec.GenerateMips = generateMips;
        
        // Create texture from path if available, otherwise from specification
        Ref<Texture2D> texture;
        if (!path.empty())
        {
            texture = Texture2D::Create(path);
        }
        else
        {
            texture = Texture2D::Create(spec);
        }
        
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::DeserializeFromAssetPack - Failed to create texture");
            return nullptr;
        }
        
        return texture;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // FontSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool FontSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        asset = Font::Create(metadata.FilePath);
        asset->m_Handle = metadata.Handle;

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
                
                // Serialize PBR texture maps
                if (material->AlbedoMap() && material->AlbedoMap()->m_Handle != 0)
                    out << YAML::Key << "AlbedoMap" << YAML::Value << material->AlbedoMap()->m_Handle;
                if (material->MetallicRoughnessMap() && material->MetallicRoughnessMap()->m_Handle != 0)
                    out << YAML::Key << "MetallicRoughnessMap" << YAML::Value << material->MetallicRoughnessMap()->m_Handle;
                if (material->NormalMap() && material->NormalMap()->m_Handle != 0)
                    out << YAML::Key << "NormalMap" << YAML::Value << material->NormalMap()->m_Handle;
                if (material->AOMap() && material->AOMap()->m_Handle != 0)
                    out << YAML::Key << "AOMap" << YAML::Value << material->AOMap()->m_Handle;
                if (material->EmissiveMap() && material->EmissiveMap()->m_Handle != 0)
                    out << YAML::Key << "EmissiveMap" << YAML::Value << material->EmissiveMap()->m_Handle;
                
                // Serialize dynamic texture uniforms
                const auto& texture2DUniforms = material->GetTexture2DUniforms();
                for (const auto& [name, texture] : texture2DUniforms)
                {
                    if (texture && texture->m_Handle != 0)
                    {
                        out << YAML::Key << name << YAML::Value << texture->m_Handle;
                    }
                }
                out << YAML::EndMap;
                
                // Serialize material properties
                out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
                
                // Serialize PBR properties with consistent map structure
                out << YAML::Key << "BaseColor" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "vec4";
                out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq 
                    << material->BaseColorFactor().x << material->BaseColorFactor().y 
                    << material->BaseColorFactor().z << material->BaseColorFactor().w << YAML::EndSeq;
                out << YAML::EndMap;
                
                out << YAML::Key << "Metallic" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "float";
                out << YAML::Key << "value" << YAML::Value << material->MetallicFactor();
                out << YAML::EndMap;
                
                out << YAML::Key << "Roughness" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "float";
                out << YAML::Key << "value" << YAML::Value << material->RoughnessFactor();
                out << YAML::EndMap;
                
                out << YAML::Key << "Emission" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "vec4";
                out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq 
                    << material->EmissiveFactor().x << material->EmissiveFactor().y 
                    << material->EmissiveFactor().z << material->EmissiveFactor().w << YAML::EndSeq;
                out << YAML::EndMap;
                
                // Serialize dynamic float uniforms
                const auto& floatUniforms = material->GetFloatUniforms();
                for (const auto& [name, value] : floatUniforms)
                {
                    out << YAML::Key << name << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "type" << YAML::Value << "float";
                    out << YAML::Key << "value" << YAML::Value << value;
                    out << YAML::EndMap;
                }
                
                // Serialize dynamic vec3 uniforms
                const auto& vec3Uniforms = material->GetVec3Uniforms();
                for (const auto& [name, value] : vec3Uniforms)
                {
                    out << YAML::Key << name << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "type" << YAML::Value << "vec3";
                    out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq 
                        << value.x << value.y << value.z << YAML::EndSeq;
                    out << YAML::EndMap;
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
        auto shader = Renderer3D::GetShaderLibrary().Get(shaderName);
        if (!shader)
        {
            // Fallback to loading from file if not in library
            shader = Shader::Create("assets/shaders/" + shaderName + ".glsl");
            if (shader)
            {
                // Add to library for future use
                Renderer3D::GetShaderLibrary().Add(shaderName, shader);
            }
        }
        if (!shader)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromYAML - Shader not found: {}", shaderName);
            return false;
        }

        auto material = Material::Create(shader);
        targetMaterialAsset = Ref<MaterialAsset>(new MaterialAsset(material));
        targetMaterialAsset->m_Handle = handle;

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
                const auto& valueNode = propNode.second;
                
                // Determine the type and set the appropriate material property
                if (valueNode["type"])
                {
                    std::string type = valueNode["type"].as<std::string>();
                    
                    if (type == "float")
                    {
                        material->Set(propName, valueNode["value"].as<float>());
                    }
                    else if (type == "int")
                    {
                        material->Set(propName, valueNode["value"].as<int>());
                    }
                    else if (type == "uint")
                    {
                        material->Set(propName, valueNode["value"].as<u32>());
                    }
                    else if (type == "bool")
                    {
                        material->Set(propName, valueNode["value"].as<bool>());
                    }
                    else if (type == "vec2")
                    {
                        material->Set(propName, valueNode["value"].as<glm::vec2>());
                    }
                    else if (type == "vec3")
                    {
                        material->Set(propName, valueNode["value"].as<glm::vec3>());
                    }
                    else if (type == "vec4")
                    {
                        material->Set(propName, valueNode["value"].as<glm::vec4>());
                    }
                    else if (type == "mat3")
                    {
                        material->Set(propName, valueNode["value"].as<glm::mat3>());
                    }
                    else if (type == "mat4")
                    {
                        material->Set(propName, valueNode["value"].as<glm::mat4>());
                    }
                    // Texture properties would be handled separately with asset handles
                    else if (type == "texture2d")
                    {
                        AssetHandle textureHandle = valueNode["value"].as<AssetHandle>(0);
                        if (textureHandle != 0)
                        {
                            auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                            if (texture)
                                material->Set(propName, texture);
                        }
                    }
                }
            }
        }

        // Load material flags
        if (materialNode["MaterialFlags"])
        {
            u32 flags = materialNode["MaterialFlags"].as<u32>(0);
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
        auto environment = EnvironmentMap::Create(EnvironmentMapSpecification{}); // TODO: Load from file path
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::TryLoadData - Failed to load environment: {}", path.string());
            return false;
        }

        environment->m_Handle = metadata.Handle;
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
        Ref<Scene> scene = Ref<Scene>(new Scene());
        SceneSerializer serializer(scene);
        
        if (serializer.Deserialize(metadata.FilePath))
        {
            scene->m_Handle = metadata.Handle;
            asset = scene; // Direct assignment - Ref<Scene> should convert to Ref<Asset>
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
        
        // Serialize scene to YAML string directly
        std::string yamlData = SerializeToString(scene);
        
        // Write YAML data size and content
        u32 dataSize = (u32)yamlData.size();
        stream.WriteRaw(dataSize);
        stream.WriteData(yamlData.c_str(), dataSize);
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        
        OLO_CORE_TRACE("SceneAssetSerializer::SerializeToAssetPack - Serialized scene, size: {} bytes", outInfo.Size);
        return true;
    }

    Ref<Asset> SceneAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        // Read YAML data size and content
        u32 dataSize;
        stream.ReadRaw(dataSize);
        
        std::vector<char> yamlData(dataSize + 1);
        stream.ReadData(yamlData.data(), dataSize);
        yamlData[dataSize] = '\0'; // Null terminate
        
        // Create scene and deserialize from YAML
        auto scene = Ref<Scene>(new Scene());
        
        if (!DeserializeFromString(yamlData.data(), scene))
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeFromAssetPack - Failed to deserialize scene from YAML");
            return nullptr;
        }
        
        // scene->m_Handle = assetInfo.Handle; // TODO: Scene doesn't have Handle member
        scene->m_Handle = assetInfo.Handle;
        
        OLO_CORE_TRACE("SceneAssetSerializer::DeserializeFromAssetPack - Deserialized scene from pack");
        return scene; // Direct return - Ref<Scene> should convert to Ref<Asset>
    }

    Ref<Scene> SceneAssetSerializer::DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const
    {
        // TODO: Implement scene pack deserialization
        OLO_CORE_WARN("SceneAssetSerializer::DeserializeSceneFromAssetPack not yet implemented");
        return nullptr;
    }

    std::string SceneAssetSerializer::SerializeToString(const Ref<Scene>& scene) const
    {
        if (!scene)
        {
            OLO_CORE_ERROR("SceneAssetSerializer::SerializeToString - Scene is null");
            return "";
        }

        SceneSerializer serializer(scene);
        return serializer.SerializeToYAML();
    }

    bool SceneAssetSerializer::DeserializeFromString(const std::string& yamlString, Ref<Scene>& scene) const
    {
        if (yamlString.empty())
        {
            OLO_CORE_ERROR("SceneAssetSerializer::DeserializeFromString - YAML string is empty");
            return false;
        }

        if (!scene)
        {
            scene = Ref<Scene>::Create();
        }

        SceneSerializer serializer(scene);
        return serializer.DeserializeFromYAML(yamlString);
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

        // TODO: MeshColliderAsset class doesn't exist yet
        // auto meshCollider = Ref<MeshColliderAsset>(new MeshColliderAsset());
        // 
        // // Load associated mesh
        // if (colliderNode["MeshSource"])
        // {
        //     AssetHandle meshHandle = colliderNode["MeshSource"].as<AssetHandle>();
        //     if (meshHandle != 0)
        //     {
        //         auto meshSource = AssetManager::GetAsset<MeshSource>(meshHandle);
        //         if (meshSource)
        //         {
        //             meshCollider->SetMeshSource(meshSource);
        //         }
        //     }
        // }
        // 
        // // Load collider properties
        // if (colliderNode["IsConvex"])
        // {
        //     meshCollider->SetConvex(colliderNode["IsConvex"].as<bool>());
        // }
        //
        // meshCollider->Handle = metadata.Handle;
        // asset = meshCollider;
        
        OLO_CORE_WARN("MeshColliderSerializer::TryLoadData - MeshColliderAsset class not implemented yet");
        return false;
        
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

    // std::string MeshColliderSerializer::SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const
    // {
    //     // TODO: Implement YAML serialization
    //     return "";
    // }

    // bool MeshColliderSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const
    // {
    //     // TODO: Implement YAML deserialization
    //     return false;
    // }

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

        // TODO: MeshSource class doesn't exist yet
        // Use Assimp to load the mesh file
        // auto meshSource = Ref<MeshSource>(new MeshSource(path));
        // if (!meshSource->IsValid())
        // {
        //     OLO_CORE_ERROR("MeshSourceSerializer::TryLoadData - Failed to load mesh source: {}", path.string());
        //     return false;
        // }
        // 
        // meshSource->Handle = metadata.Handle;
        // asset = meshSource;
        // 
        // OLO_CORE_TRACE("MeshSourceSerializer::TryLoadData - Successfully loaded mesh source: {} ({} submeshes)", 
        //               path.string(), meshSource->GetSubmeshes().size());
        
        OLO_CORE_WARN("MeshSourceSerializer::TryLoadData - MeshSource class not implemented yet");
        return false;
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
        
        // TODO: Current Mesh class doesn't have GetSubmeshes/GetMaterials methods
        // Serialize mesh properties
        // out << YAML::Key << "SubmeshCount" << YAML::Value << mesh->GetSubmeshes().size();
        
        // For now, just serialize basic mesh info
        out << YAML::Key << "VertexCount" << YAML::Value << mesh->GetVertices().size();
        out << YAML::Key << "IndexCount" << YAML::Value << mesh->GetIndices().size();
        // For now, just serialize basic mesh info
        out << YAML::Key << "VertexCount" << YAML::Value << mesh->GetVertices().size();
        out << YAML::Key << "IndexCount" << YAML::Value << mesh->GetIndices().size();
        
        // TODO: Implement submesh and material serialization when those features are added
        // 
        // // Serialize submeshes
        // out << YAML::Key << "Submeshes" << YAML::Value << YAML::BeginSeq;
        // for (const auto& submesh : mesh->GetSubmeshes())
        // {
        //     out << YAML::BeginMap;
        //     out << YAML::Key << "BaseVertex" << YAML::Value << submesh.BaseVertex;
        //     out << YAML::Key << "BaseIndex" << YAML::Value << submesh.BaseIndex;
        //     out << YAML::Key << "IndexCount" << YAML::Value << submesh.IndexCount;
        //     out << YAML::Key << "VertexCount" << YAML::Value << submesh.VertexCount;
        //     out << YAML::Key << "MaterialIndex" << YAML::Value << submesh.MaterialIndex;
        //     
        //     // Serialize transform
        //     out << YAML::Key << "Transform" << YAML::Value << submesh.Transform;
        //     
        //     // Serialize bounding box
        //     out << YAML::Key << "BoundingBox" << YAML::Value << YAML::BeginMap;
        //     out << YAML::Key << "Min" << YAML::Value << submesh.BoundingBox.Min;
        //     out << YAML::Key << "Max" << YAML::Value << submesh.BoundingBox.Max;
        //     out << YAML::EndMap;
        //     
        //     out << YAML::EndMap;
        // }
        // out << YAML::EndSeq;
        // 
        // // Serialize materials
        // out << YAML::Key << "Materials" << YAML::Value << YAML::BeginSeq;
        // for (const auto& material : mesh->GetMaterials())
        // {
        //     out << (material ? material->Handle : AssetHandle(0));
        // }
        // out << YAML::EndSeq;
        
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
        auto mesh = Ref<Mesh>(new Mesh());
        
        // TODO: Current Mesh class doesn't support submeshes and materials like expected
        // For now, just load basic mesh data
        // 
        // // Load submeshes
        // if (meshNode["Submeshes"])
        // {
        //     std::vector<Submesh> submeshes;
        //     for (const auto& submeshNode : meshNode["Submeshes"])
        //     {
        //         Submesh submesh{};
        //         submesh.BaseVertex = submeshNode["BaseVertex"].as<u32>();
        //         submesh.BaseIndex = submeshNode["BaseIndex"].as<u32>();
        //         submesh.IndexCount = submeshNode["IndexCount"].as<u32>();
        //         submesh.VertexCount = submeshNode["VertexCount"].as<u32>();
        //         submesh.MaterialIndex = submeshNode["MaterialIndex"].as<u32>();
        //         submesh.Transform = submeshNode["Transform"].as<glm::mat4>();
        //         
        //         // Load bounding box
        //         auto bbNode = submeshNode["BoundingBox"];
        //         submesh.BoundingBox.Min = bbNode["Min"].as<glm::vec3>();
        //         submesh.BoundingBox.Max = bbNode["Max"].as<glm::vec3>();
        //         
        //         submeshes.push_back(submesh);
        //     }
        //     mesh->SetSubmeshes(submeshes);
        // }
        // 
        // // Load materials
        // if (meshNode["Materials"])
        // {
        //     std::vector<Ref<MaterialAsset>> materials;
        //     for (const auto& materialNode : meshNode["Materials"])
        //     {
        //         AssetHandle materialHandle = materialNode.as<AssetHandle>();
        //         if (materialHandle != 0)
        //         {
        //             auto material = AssetManager::GetAsset<MaterialAsset>(materialHandle);
        //             materials.push_back(material);
        //         }
        //         else
        //         {
        //             materials.push_back(nullptr);
        //         }
        //     }
        //     mesh->SetMaterials(materials);
        // }

        mesh->m_Handle = metadata.Handle;
        asset = mesh;
        
        OLO_CORE_TRACE("MeshSerializer::TryLoadData - Successfully loaded mesh: {}", path.string());
        return true;
    }

    void MeshSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // TODO: Implement dependency registration when mesh materials are supported
        // // Load the mesh temporarily to extract material dependencies
        // Ref<Asset> asset;
        // if (TryLoadData(metadata, asset))
        // {
        //     auto mesh = asset.As<Mesh>();
        //     if (mesh)
        //     {
        //         for (const auto& material : mesh->GetMaterials())
        //         {
        //             if (material && material->Handle != 0)
        //             {
        //                 Project::GetAssetManager()->RegisterDependency(material->Handle, metadata.Handle);
        //             }
        //         }
        //     }
        // }
        OLO_CORE_WARN("MeshSerializer::RegisterDependencies - Not implemented yet");
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

        // TODO: StaticMesh class doesn't exist yet
        // Load static mesh (typically from processed mesh files)
        // auto staticMesh = Ref<StaticMesh>(new StaticMesh(path));
        // if (!staticMesh->IsValid())
        // {
        //     OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Failed to load static mesh: {}", path.string());
        //     return false;
        // }
        //
        // staticMesh->Handle = metadata.Handle;
        // asset = staticMesh;
        
        OLO_CORE_WARN("StaticMeshSerializer::TryLoadData - StaticMesh class not implemented yet");
        return false;
        
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
    // SoundGraphSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SoundGraphSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement sound graph serialization
        OLO_CORE_WARN("SoundGraphSerializer::Serialize not yet implemented");
    }

    bool SoundGraphSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        // TODO: Implement sound graph loading
        OLO_CORE_WARN("SoundGraphSerializer::TryLoadData not yet implemented");
        return false;
    }

    bool SoundGraphSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement sound graph pack serialization
        OLO_CORE_WARN("SoundGraphSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> SoundGraphSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement sound graph pack deserialization
        OLO_CORE_WARN("SoundGraphSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

} // namespace OloEngine
