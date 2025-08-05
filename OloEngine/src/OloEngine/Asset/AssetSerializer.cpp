#include "OloEnginePCH.h"
#include "AssetSerializer.h"
#include "MeshColliderAsset.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Core/Project.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UI/Font.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Scripting/ScriptAsset.h"
#include "OloEngine/Renderer/Environment.h"
#include "OloEngine/Audio/SoundConfig.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

namespace OloEngine
{
    ///////////////////////////////////
	bool TextureSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
    bool FontSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("FontSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        auto font = Font::Create(path.string());
        if (!font)
        {
            OLO_CORE_ERROR("FontSerializer::TryLoadData - Failed to load font: {}", path.string());
            return false;
        }

        font->Handle = metadata.Handle;
        asset = font;
        
        OLO_CORE_TRACE("FontSerializer::TryLoadData - Successfully loaded font: {}", path.string());
        return true;
    }  {
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // Load texture using the Renderer's texture creation system
        auto texture = Texture2D::Create(path.string());
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::TryLoadData - Failed to load texture: {}", path.string());
            return false;
        }

        asset = texture;
        asset->Handle = metadata.Handle;
        
        OLO_CORE_TRACE("TextureSerializer::TryLoadData - Successfully loaded texture: {}", path.string());
        return true;
    }
    //////////////////////////////////////////
    // TextureSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool TextureSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        asset = Texture2D::Create(metadata.FilePath);
        return asset != nullptr;
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
        
        // Write texture properties
        stream.WriteRaw(texture->GetWidth());
        stream.WriteRaw(texture->GetHeight());
        stream.WriteRaw(texture->GetChannels());
        stream.WriteRaw((uint32_t)texture->GetFormat());
        
        // Write texture data
        auto textureData = texture->GetTextureData();
        uint32_t dataSize = textureData.Size;
        stream.WriteRaw(dataSize);
        
        if (dataSize > 0 && textureData.Data)
        {
            stream.WriteBuffer(textureData.Data, dataSize);
        }
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        
        OLO_CORE_TRACE("TextureSerializer::SerializeToAssetPack - Serialized texture, size: {} bytes", outInfo.Size);
        return true;
    }

    Ref<Asset> TextureSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        // Read texture properties
        uint32_t width, height, channels;
        uint32_t formatRaw;
        stream.ReadRaw(width);
        stream.ReadRaw(height);
        stream.ReadRaw(channels);
        stream.ReadRaw(formatRaw);
        
        ImageFormat format = (ImageFormat)formatRaw;
        
        // Read texture data
        uint32_t dataSize;
        stream.ReadRaw(dataSize);
        
        Buffer textureData;
        if (dataSize > 0)
        {
            textureData.Allocate(dataSize);
            stream.ReadBuffer(textureData.Data, dataSize);
        }
        
        // Create texture from data
        TextureSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Format = format;
        
        auto texture = Texture2D::Create(spec, textureData);
        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::DeserializeFromAssetPack - Failed to create texture from packed data");
            return nullptr;
        }
        
        texture->Handle = assetInfo.Handle;
        
        OLO_CORE_TRACE("TextureSerializer::DeserializeFromAssetPack - Deserialized texture {}x{}", width, height);
        return texture;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // FontSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool FontSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        asset = Font::Create(metadata.FilePath);
        return asset != nullptr;
    }

    bool FontSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement font pack serialization
        OLO_CORE_WARN("FontSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> FontSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement font pack deserialization
        OLO_CORE_WARN("FontSerializer::DeserializeFromAssetPack not yet implemented");
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

        std::filesystem::path filepath = Project::GetAssetDirectory() / metadata.FilePath;
        
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Material" << YAML::Value << YAML::BeginMap;
        
        // Serialize material properties
        auto material = materialAsset->GetMaterial();
        if (material)
        {
            out << YAML::Key << "Shader" << YAML::Value << material->GetShader()->GetName();
            
            // Serialize material textures
            out << YAML::Key << "Textures" << YAML::Value << YAML::BeginMap;
            auto& textures = material->GetTextures();
            for (const auto& [name, texture] : textures)
            {
                if (texture)
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
        }
        
        out << YAML::EndMap;
        out << YAML::EndMap;
        
        std::ofstream fout(filepath);
        fout << out.c_str();
        fout.close();
        
        OLO_CORE_TRACE("MaterialAssetSerializer::Serialize - Serialized material to: {}", filepath.string());
    }

    bool MaterialAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - Failed to open file: {}", path.string());
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
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - YAML parsing error: {}", e.what());
            return false;
        }

        auto materialNode = data["Material"];
        if (!materialNode)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - No Material node found");
            return false;
        }

        // Load shader
        std::string shaderName = materialNode["Shader"].as<std::string>();
        auto shader = Renderer::GetShaderLibrary()->Get(shaderName);
        if (!shader)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::TryLoadData - Shader not found: {}", shaderName);
            return false;
        }

        auto material = Material::Create(shader);
        
        // Load textures
        if (materialNode["Textures"])
        {
            for (const auto& textureNode : materialNode["Textures"])
            {
                std::string textureName = textureNode.first.as<std::string>();
                AssetHandle textureHandle = textureNode.second.as<AssetHandle>();
                
                auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                if (texture)
                {
                    material->Set(textureName, texture);
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
            }
        }

        auto materialAsset = CreateRef<MaterialAsset>(material);
        materialAsset->Handle = metadata.Handle;
        asset = materialAsset;
        
        OLO_CORE_TRACE("MaterialAssetSerializer::TryLoadData - Successfully loaded material: {}", path.string());
        return true;
    }

    void MaterialAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        // TODO: Implement dependency registration
        OLO_CORE_WARN("MaterialAssetSerializer::RegisterDependencies not yet implemented");
    }

    bool MaterialAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        auto materialAsset = AssetManager::GetAsset<MaterialAsset>(handle);
        if (!materialAsset || !materialAsset->GetMaterial())
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::SerializeToAssetPack - Invalid material asset");
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();
        
        auto material = materialAsset->GetMaterial();
        
        // Write shader name
        std::string shaderName = material->GetShader()->GetName();
        stream.WriteString(shaderName);
        
        // Write texture count and texture handles
        auto& textures = material->GetTextures();
        uint32_t textureCount = (uint32_t)textures.size();
        stream.WriteRaw(textureCount);
        
        for (const auto& [name, texture] : textures)
        {
            stream.WriteString(name);
            stream.WriteRaw(texture ? texture->Handle : AssetHandle(0));
        }
        
        // Write uniform count and uniform data
        auto& uniforms = material->GetUniforms();
        uint32_t uniformCount = (uint32_t)uniforms.size();
        stream.WriteRaw(uniformCount);
        
        for (const auto& [name, uniform] : uniforms)
        {
            stream.WriteString(name);
            stream.WriteRaw((uint32_t)uniform.Type);
            
            // Write uniform data based on type
            switch (uniform.Type)
            {
                case ShaderUniformType::Float:
                    stream.WriteRaw(uniform.GetValue<float>());
                    break;
                case ShaderUniformType::Float2:
                    stream.WriteRaw(uniform.GetValue<glm::vec2>());
                    break;
                case ShaderUniformType::Float3:
                    stream.WriteRaw(uniform.GetValue<glm::vec3>());
                    break;
                case ShaderUniformType::Float4:
                    stream.WriteRaw(uniform.GetValue<glm::vec4>());
                    break;
                // Add more types as needed
            }
        }
        
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        
        OLO_CORE_TRACE("MaterialAssetSerializer::SerializeToAssetPack - Serialized material, size: {} bytes", outInfo.Size);
        return true;
    }

    Ref<Asset> MaterialAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        
        // Read shader name
        std::string shaderName = stream.ReadString();
        auto shader = Renderer::GetShaderLibrary()->Get(shaderName);
        if (!shader)
        {
            OLO_CORE_ERROR("MaterialAssetSerializer::DeserializeFromAssetPack - Shader not found: {}", shaderName);
            return nullptr;
        }
        
        auto material = Material::Create(shader);
        
        // Read textures
        uint32_t textureCount;
        stream.ReadRaw(textureCount);
        
        for (uint32_t i = 0; i < textureCount; i++)
        {
            std::string textureName = stream.ReadString();
            AssetHandle textureHandle;
            stream.ReadRaw(textureHandle);
            
            if (textureHandle != 0)
            {
                auto texture = AssetManager::GetAsset<Texture2D>(textureHandle);
                if (texture)
                {
                    material->Set(textureName, texture);
                }
            }
        }
        
        // Read uniforms
        uint32_t uniformCount;
        stream.ReadRaw(uniformCount);
        
        for (uint32_t i = 0; i < uniformCount; i++)
        {
            std::string uniformName = stream.ReadString();
            uint32_t uniformTypeRaw;
            stream.ReadRaw(uniformTypeRaw);
            
            ShaderUniformType uniformType = (ShaderUniformType)uniformTypeRaw;
            
            // Read uniform data based on type
            switch (uniformType)
            {
                case ShaderUniformType::Float:
                {
                    float value;
                    stream.ReadRaw(value);
                    material->Set(uniformName, value);
                    break;
                }
                case ShaderUniformType::Float2:
                {
                    glm::vec2 value;
                    stream.ReadRaw(value);
                    material->Set(uniformName, value);
                    break;
                }
                case ShaderUniformType::Float3:
                {
                    glm::vec3 value;
                    stream.ReadRaw(value);
                    material->Set(uniformName, value);
                    break;
                }
                case ShaderUniformType::Float4:
                {
                    glm::vec4 value;
                    stream.ReadRaw(value);
                    material->Set(uniformName, value);
                    break;
                }
                // Add more types as needed
            }
        }
        
        auto materialAsset = CreateRef<MaterialAsset>(material);
        materialAsset->Handle = assetInfo.Handle;
        
        OLO_CORE_TRACE("MaterialAssetSerializer::DeserializeFromAssetPack - Deserialized material: {}", shaderName);
        return materialAsset;
    }

    std::string MaterialAssetSerializer::SerializeToYAML(Ref<MaterialAsset> materialAsset) const
    {
        // TODO: Implement YAML serialization
        return "";
    }

    std::string MaterialAssetSerializer::GetYAML(const AssetMetadata& metadata) const
    {
        // TODO: Implement YAML getter
        return "";
    }

    void MaterialAssetSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        // TODO: Implement YAML dependency registration
    }

    bool MaterialAssetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MaterialAsset>& targetMaterialAsset, AssetHandle handle) const
    {
        // TODO: Implement YAML deserialization
        return false;
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
    // SoundConfigSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void SoundConfigSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<SoundConfig> soundConfig = asset.As<SoundConfig>();
        if (!soundConfig)
        {
            OLO_CORE_ERROR("SoundConfigSerializer::Serialize - Invalid sound config asset");
            return;
        }

        std::filesystem::path filepath = Project::GetAssetDirectory() / metadata.FilePath;
        
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "SoundConfig" << YAML::Value << YAML::BeginMap;
        
        // Serialize sound properties
        out << YAML::Key << "Volume" << YAML::Value << soundConfig->Volume;
        out << YAML::Key << "Pitch" << YAML::Value << soundConfig->Pitch;
        out << YAML::Key << "Looping" << YAML::Value << soundConfig->Looping;
        out << YAML::Key << "Spatialization" << YAML::Value << soundConfig->Spatialization;
        out << YAML::Key << "AttenuationModel" << YAML::Value << (int)soundConfig->AttenuationModel;
        out << YAML::Key << "RollOffFactor" << YAML::Value << soundConfig->RollOffFactor;
        out << YAML::Key << "MinDistance" << YAML::Value << soundConfig->MinDistance;
        out << YAML::Key << "MaxDistance" << YAML::Value << soundConfig->MaxDistance;
        out << YAML::Key << "ConeInnerAngle" << YAML::Value << soundConfig->ConeInnerAngle;
        out << YAML::Key << "ConeOuterAngle" << YAML::Value << soundConfig->ConeOuterAngle;
        out << YAML::Key << "ConeOuterGain" << YAML::Value << soundConfig->ConeOuterGain;
        out << YAML::Key << "DopplerFactor" << YAML::Value << soundConfig->DopplerFactor;
        
        // Serialize audio source handle if present
        if (soundConfig->AudioSource)
        {
            out << YAML::Key << "AudioSource" << YAML::Value << soundConfig->AudioSource->Handle;
        }
        
        out << YAML::EndMap;
        out << YAML::EndMap;
        
        std::ofstream fout(filepath);
        fout << out.c_str();
        fout.close();
        
        OLO_CORE_TRACE("SoundConfigSerializer::Serialize - Serialized sound config to: {}", filepath.string());
    }

    bool SoundConfigSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - Failed to open file: {}", path.string());
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
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - YAML parsing error: {}", e.what());
            return false;
        }

        auto configNode = data["SoundConfig"];
        if (!configNode)
        {
            OLO_CORE_ERROR("SoundConfigSerializer::TryLoadData - No SoundConfig node found");
            return false;
        }

        auto soundConfig = CreateRef<SoundConfig>();
        
        // Load sound properties
        soundConfig->Volume = configNode["Volume"].as<float>(1.0f);
        soundConfig->Pitch = configNode["Pitch"].as<float>(1.0f);
        soundConfig->Looping = configNode["Looping"].as<bool>(false);
        soundConfig->Spatialization = configNode["Spatialization"].as<bool>(false);
        soundConfig->AttenuationModel = (AttenuationModel)configNode["AttenuationModel"].as<int>(0);
        soundConfig->RollOffFactor = configNode["RollOffFactor"].as<float>(1.0f);
        soundConfig->MinDistance = configNode["MinDistance"].as<float>(1.0f);
        soundConfig->MaxDistance = configNode["MaxDistance"].as<float>(100.0f);
        soundConfig->ConeInnerAngle = configNode["ConeInnerAngle"].as<float>(360.0f);
        soundConfig->ConeOuterAngle = configNode["ConeOuterAngle"].as<float>(360.0f);
        soundConfig->ConeOuterGain = configNode["ConeOuterGain"].as<float>(0.0f);
        soundConfig->DopplerFactor = configNode["DopplerFactor"].as<float>(1.0f);
        
        // Load audio source if present
        if (configNode["AudioSource"])
        {
            AssetHandle audioSourceHandle = configNode["AudioSource"].as<AssetHandle>();
            if (audioSourceHandle != 0)
            {
                soundConfig->AudioSource = AssetManager::GetAsset<AudioSource>(audioSourceHandle);
            }
        }

        soundConfig->Handle = metadata.Handle;
        asset = soundConfig;
        
        OLO_CORE_TRACE("SoundConfigSerializer::TryLoadData - Successfully loaded sound config: {}", path.string());
        return true;
    }

    bool SoundConfigSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement sound config pack serialization
        OLO_CORE_WARN("SoundConfigSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> SoundConfigSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement sound config pack deserialization
        OLO_CORE_WARN("SoundConfigSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

    std::string SoundConfigSerializer::SerializeToYAML(Ref<SoundConfig> soundConfig) const
    {
        // TODO: Implement YAML serialization
        return "";
    }

    bool SoundConfigSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<SoundConfig> targetSoundConfig) const
    {
        // TODO: Implement YAML deserialization
        return false;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // PrefabSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void PrefabSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Prefab> prefab = asset.As<Prefab>();
        if (!prefab)
        {
            OLO_CORE_ERROR("PrefabSerializer::Serialize - Invalid prefab asset");
            return;
        }

        std::filesystem::path filepath = Project::GetAssetDirectory() / metadata.FilePath;
        
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Prefab" << YAML::Value << YAML::BeginMap;
        
        // Serialize the entity hierarchy
        Entity rootEntity = prefab->GetRootEntity();
        if (rootEntity)
        {
            out << YAML::Key << "RootEntity" << YAML::Value;
            SerializeEntity(out, rootEntity);
        }
        
        out << YAML::EndMap;
        out << YAML::EndMap;
        
        std::ofstream fout(filepath);
        fout << out.c_str();
        fout.close();
        
        OLO_CORE_TRACE("PrefabSerializer::Serialize - Serialized prefab to: {}", filepath.string());
    }

    bool PrefabSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream stream(path);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - Failed to open file: {}", path.string());
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
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - YAML parsing error: {}", e.what());
            return false;
        }

        auto prefabNode = data["Prefab"];
        if (!prefabNode)
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - No Prefab node found");
            return false;
        }

        // Create a temporary scene to deserialize the entity
        auto tempScene = CreateRef<Scene>();
        Entity rootEntity;
        
        if (prefabNode["RootEntity"])
        {
            rootEntity = DeserializeEntity(prefabNode["RootEntity"], tempScene);
        }

        auto prefab = CreateRef<Prefab>(rootEntity);
        prefab->Handle = metadata.Handle;
        asset = prefab;
        
        OLO_CORE_TRACE("PrefabSerializer::TryLoadData - Successfully loaded prefab: {}", path.string());
        return true;
    }

    bool PrefabSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement prefab pack serialization
        OLO_CORE_WARN("PrefabSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> PrefabSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement prefab pack deserialization
        OLO_CORE_WARN("PrefabSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

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
    // ScriptFileSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void ScriptFileSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        // TODO: Implement script file serialization
        OLO_CORE_WARN("ScriptFileSerializer::Serialize not yet implemented");
    }

    bool ScriptFileSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        // Read script file content
        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - Failed to open script file: {}", path.string());
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        auto scriptAsset = CreateRef<ScriptAsset>();
        scriptAsset->SetSourceCode(buffer.str());
        scriptAsset->SetFilePath(metadata.FilePath);
        scriptAsset->Handle = metadata.Handle;
        
        asset = scriptAsset;
        
        OLO_CORE_TRACE("ScriptFileSerializer::TryLoadData - Successfully loaded script: {}", path.string());
        return true;
    }

    bool ScriptFileSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement script file pack serialization
        OLO_CORE_WARN("ScriptFileSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> ScriptFileSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        // TODO: Implement script file pack deserialization
        OLO_CORE_WARN("ScriptFileSerializer::DeserializeFromAssetPack not yet implemented");
        return nullptr;
    }

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
