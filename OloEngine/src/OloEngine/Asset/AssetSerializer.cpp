
#include "OloEnginePCH.h"
#include "AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Animation/AnimationAsset.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Core/YAMLConverters.h"
#include <yaml-cpp/yaml.h>
#include <stb_image/stb_image.h>
#include <fstream>
#include <algorithm>

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

    bool TextureSerializer::TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData) const
    {
        OLO_PROFILE_FUNCTION();

        // This method is safe to call from any thread - no GPU/GL calls here

        std::string path = metadata.FilePath.string();

        int width = 0;
        int height = 0;
        int channels = 0;

        // Load image data using stb_image (thread-safe)
        ::stbi_set_flip_vertically_on_load(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - TextureSerializer::TryLoadRawData");
            data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
        }

        if (!data)
        {
            OLO_CORE_ERROR("TextureSerializer::TryLoadRawData - Failed to load image: {}", path);
            return false;
        }

        // Copy pixel data to RawTextureData
        RawTextureData rawData;
        rawData.Width = static_cast<u32>(width);
        rawData.Height = static_cast<u32>(height);
        rawData.Channels = static_cast<u32>(channels);
        rawData.Handle = metadata.Handle;
        rawData.DebugName = metadata.FilePath.filename().string();
        rawData.GenerateMipmaps = true;
        rawData.SRGB = false; // TODO: Could be determined from asset metadata

        // Copy the pixel data
        sizet dataSize = static_cast<sizet>(width) * height * channels;
        rawData.PixelData.resize(dataSize);
        std::memcpy(rawData.PixelData.data(), data, dataSize);

        // Free stb_image data
        ::stbi_image_free(data);

        OLO_CORE_TRACE("TextureSerializer::TryLoadRawData - Loaded raw texture data: {} ({}x{}, {} channels)",
                       rawData.DebugName, width, height, channels);

        outRawData = std::move(rawData);
        return true;
    }

    bool TextureSerializer::FinalizeFromRawData(const RawAssetData& rawData, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        // This method MUST be called from the main thread - creates GPU resources

        if (!std::holds_alternative<RawTextureData>(rawData))
        {
            OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Invalid raw data type");
            return false;
        }

        const auto& texData = std::get<RawTextureData>(rawData);

        if (!texData.IsValid())
        {
            OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Invalid texture data");
            return false;
        }

        // Create texture specification
        TextureSpecification spec;
        spec.Width = texData.Width;
        spec.Height = texData.Height;
        spec.GenerateMips = texData.GenerateMipmaps;

        // Determine format based on channel count
        // Note: The engine currently only supports R8, RGB8, RGBA8 and a few other formats
        switch (texData.Channels)
        {
            case 1:
                spec.Format = ImageFormat::R8;
                break;
            case 3:
                spec.Format = ImageFormat::RGB8;
                break;
            case 4:
            default:
                spec.Format = ImageFormat::RGBA8;
                break;
        }

        // Create the texture on the main thread (GL calls happen here)
        Ref<Texture2D> texture = Texture2D::Create(spec);

        if (!texture)
        {
            OLO_CORE_ERROR("TextureSerializer::FinalizeFromRawData - Failed to create texture: {}", texData.DebugName);
            return false;
        }

        // Set the pixel data on the texture
        // Note: SetData expects size in bytes
        u32 dataSize = static_cast<u32>(texData.PixelData.size());
        texture->SetData(const_cast<u8*>(texData.PixelData.data()), dataSize);

        texture->m_Handle = texData.Handle;

        OLO_CORE_TRACE("TextureSerializer::FinalizeFromRawData - Created texture: {} ({}x{}, {} channels)",
                       texData.DebugName, texData.Width, texData.Height, texData.Channels);

        asset = texture;
        return true;
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

        // Read additional metadata to maintain cursor consistency
        bool hasAlphaChannel;
        bool isLoaded;
        i64 timestamp;

        stream.ReadRaw(hasAlphaChannel);
        stream.ReadRaw(isLoaded);
        stream.ReadRaw(timestamp);

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
                if (material->GetAlbedoMap() && material->GetAlbedoMap()->m_Handle != 0)
                    out << YAML::Key << "AlbedoMap" << YAML::Value << material->GetAlbedoMap()->m_Handle;
                if (material->GetMetallicRoughnessMap() && material->GetMetallicRoughnessMap()->m_Handle != 0)
                    out << YAML::Key << "MetallicRoughnessMap" << YAML::Value << material->GetMetallicRoughnessMap()->m_Handle;
                if (material->GetNormalMap() && material->GetNormalMap()->m_Handle != 0)
                    out << YAML::Key << "NormalMap" << YAML::Value << material->GetNormalMap()->m_Handle;
                if (material->GetAOMap() && material->GetAOMap()->m_Handle != 0)
                    out << YAML::Key << "AOMap" << YAML::Value << material->GetAOMap()->m_Handle;
                if (material->GetEmissiveMap() && material->GetEmissiveMap()->m_Handle != 0)
                    out << YAML::Key << "EmissiveMap" << YAML::Value << material->GetEmissiveMap()->m_Handle;

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
                    << material->GetBaseColorFactor().x << material->GetBaseColorFactor().y
                    << material->GetBaseColorFactor().z << material->GetBaseColorFactor().w << YAML::EndSeq;
                out << YAML::EndMap;

                out << YAML::Key << "Metallic" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "float";
                out << YAML::Key << "value" << YAML::Value << material->GetMetallicFactor();
                out << YAML::EndMap;

                out << YAML::Key << "Roughness" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "float";
                out << YAML::Key << "value" << YAML::Value << material->GetRoughnessFactor();
                out << YAML::EndMap;

                out << YAML::Key << "Emission" << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "type" << YAML::Value << "vec4";
                out << YAML::Key << "value" << YAML::Value << YAML::Flow << YAML::BeginSeq
                    << material->GetEmissiveFactor().x << material->GetEmissiveFactor().y
                    << material->GetEmissiveFactor().z << material->GetEmissiveFactor().w << YAML::EndSeq;
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
        outInfo.Offset = stream.GetStreamPosition();

        Ref<EnvironmentMap> environment = AssetManager::GetAsset<EnvironmentMap>(handle);
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::SerializeToAssetPack - Failed to get environment asset");
            return false;
        }

        // Serialize environment specification for recreation
        const auto& spec = environment->GetSpecification();
        stream.WriteString(spec.FilePath);
        stream.WriteRaw(spec.Resolution);
        stream.WriteRaw(static_cast<u32>(spec.Format));
        stream.WriteRaw(spec.GenerateIBL);
        stream.WriteRaw(spec.GenerateMipmaps);

        // Serialize IBL configuration
        const auto& iblConfig = spec.IBLConfig;
        stream.WriteRaw(static_cast<u32>(iblConfig.Quality));
        stream.WriteRaw(iblConfig.UseImportanceSampling);
        stream.WriteRaw(iblConfig.UseSphericalHarmonics);
        stream.WriteRaw(iblConfig.IrradianceResolution);
        stream.WriteRaw(iblConfig.PrefilterResolution);
        stream.WriteRaw(iblConfig.BRDFLutResolution);
        stream.WriteRaw(iblConfig.IrradianceSamples);
        stream.WriteRaw(iblConfig.PrefilterSamples);
        stream.WriteRaw(iblConfig.EnableMultithreading);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        OLO_CORE_TRACE("EnvironmentSerializer::SerializeToAssetPack - Serialized environment: {}", spec.FilePath);
        return true;
    }

    Ref<Asset> EnvironmentSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read environment specification
        EnvironmentMapSpecification spec;
        stream.ReadString(spec.FilePath);
        stream.ReadRaw(spec.Resolution);

        u32 formatValue;
        stream.ReadRaw(formatValue);
        spec.Format = static_cast<ImageFormat>(formatValue);

        stream.ReadRaw(spec.GenerateIBL);
        stream.ReadRaw(spec.GenerateMipmaps);

        // Read IBL configuration
        u32 qualityValue;
        stream.ReadRaw(qualityValue);
        spec.IBLConfig.Quality = static_cast<IBLQuality>(qualityValue);

        stream.ReadRaw(spec.IBLConfig.UseImportanceSampling);
        stream.ReadRaw(spec.IBLConfig.UseSphericalHarmonics);
        stream.ReadRaw(spec.IBLConfig.IrradianceResolution);
        stream.ReadRaw(spec.IBLConfig.PrefilterResolution);
        stream.ReadRaw(spec.IBLConfig.BRDFLutResolution);
        stream.ReadRaw(spec.IBLConfig.IrradianceSamples);
        stream.ReadRaw(spec.IBLConfig.PrefilterSamples);
        stream.ReadRaw(spec.IBLConfig.EnableMultithreading);

        // Recreate environment map from specification
        auto environment = EnvironmentMap::Create(spec);
        if (!environment)
        {
            OLO_CORE_ERROR("EnvironmentSerializer::DeserializeFromAssetPack - Failed to create environment from: {}", spec.FilePath);
            return nullptr;
        }

        environment->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("EnvironmentSerializer::DeserializeFromAssetPack - Deserialized environment: {}", spec.FilePath);
        return environment;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AudioFileSourceSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AudioFileSourceSerializer::Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const
    {
        // AudioFile assets don't require explicit serialization to file
        // as they're loaded based on metadata analysis of the source file
    }

    bool AudioFileSourceSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        // Get the file path for analysis
        std::filesystem::path filePath = Project::GetAssetDirectory() / metadata.FilePath;

        // Initialize default values
        double duration = 0.0;
        u32 samplingRate = 44100;
        u16 bitDepth = 16;
        u16 numChannels = 2;
        u64 fileSize = 0;

        // Get file size
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec) && !ec)
        {
            fileSize = std::filesystem::file_size(filePath, ec);
            if (ec)
                fileSize = 0;
        }

        // Basic audio file format detection and analysis
        std::string extension = filePath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        if (extension == ".wav")
        {
            // Basic WAV header analysis
            if (GetWavFileInfo(filePath, duration, samplingRate, bitDepth, numChannels))
            {
                OLO_CORE_TRACE("AudioFileSourceSerializer: Analyzed WAV file - Duration: {:.2f}s, Rate: {}Hz, Depth: {}bit, Channels: {}",
                               duration, samplingRate, bitDepth, numChannels);
            }
        }
        else if (extension == ".mp3" || extension == ".ogg" || extension == ".flac")
        {
            // For other formats, use estimated values based on file size
            // These are rough estimates - in the future, proper audio decoding should be implemented
            if (fileSize > 0)
            {
                // Estimate duration based on average bitrate assumptions
                double estimatedBitrate = 128000.0; // 128 kbps average for compressed audio
                if (extension == ".flac")
                    estimatedBitrate = 1000000.0; // 1 Mbps for FLAC

                duration = (fileSize * 8.0) / estimatedBitrate; // Convert bytes to duration
                samplingRate = 44100;                           // Standard CD quality
                bitDepth = 16;                                  // Standard for compressed formats
                numChannels = 2;                                // Assume stereo
            }

            OLO_CORE_TRACE("AudioFileSourceSerializer: Estimated audio properties for {} - Duration: {:.2f}s (estimated)",
                           extension, duration);
        }

        // Create AudioFile asset with extracted/estimated metadata
        asset = Ref<AudioFile>::Create(duration, samplingRate, bitDepth, numChannels, fileSize);
        asset->SetHandle(metadata.Handle);

        OLO_CORE_TRACE("AudioFileSourceSerializer: Loaded AudioFile asset {} - {}MB",
                       metadata.Handle, fileSize / (1024 * 1024));
        return true;
    }

    bool AudioFileSourceSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        Ref<AudioFile> audioFile = AssetManager::GetAsset<AudioFile>(handle);
        if (!audioFile)
        {
            OLO_CORE_ERROR("AudioFileSourceSerializer: Failed to get AudioFile asset for handle {0}", handle);
            return false;
        }

        // Get the file path for this asset
        auto path = Project::GetAssetDirectory() / Project::GetAssetManager()->GetAssetMetadata(handle).FilePath;
        auto relativePath = std::filesystem::relative(path, Project::GetAssetDirectory());

        std::string filePath;
        if (relativePath.empty())
            filePath = path.string();
        else
            filePath = relativePath.string();

        // Serialize the file path so runtime can load the audio file
        stream.WriteString(filePath);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("AudioFileSourceSerializer: Serialized AudioFile to pack - Handle: {0}, Path: {1}, Size: {2}",
                       handle, filePath, outInfo.Size);
        return true;
    }

    Ref<Asset> AudioFileSourceSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string filePath;
        stream.ReadString(filePath);

        // Create AudioFile asset with file path information
        // TODO: In runtime, analyze the audio file to get proper metadata
        Ref<AudioFile> audioFile = Ref<AudioFile>::Create();
        audioFile->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("AudioFileSourceSerializer: Deserialized AudioFile from pack - Handle: {0}, Path: {1}",
                       assetInfo.Handle, filePath);
        return audioFile;
    }

    bool AudioFileSourceSerializer::GetWavFileInfo(const std::filesystem::path& filePath, double& duration, u32& samplingRate, u16& bitDepth, u16& numChannels) const
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Failed to open WAV file: {}", filePath.string());
            return false;
        }

        // Read RIFF header
        char riffHeader[4];
        file.read(riffHeader, 4);
        if (std::memcmp(riffHeader, "RIFF", 4) != 0)
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Invalid RIFF header in WAV file: {}", filePath.string());
            return false;
        }

        // Skip chunk size (4 bytes)
        file.seekg(4, std::ios::cur);

        // Read WAVE format
        char waveHeader[4];
        file.read(waveHeader, 4);
        if (std::memcmp(waveHeader, "WAVE", 4) != 0)
        {
            OLO_CORE_WARN("AudioFileSourceSerializer: Invalid WAVE header in WAV file: {}", filePath.string());
            return false;
        }

        // Find fmt chunk
        bool fmtFound = false;
        u32 dataSize = 0;

        while (!file.eof() && (!fmtFound || dataSize == 0))
        {
            char chunkId[4];
            u32 chunkSize;

            file.read(chunkId, 4);
            file.read(reinterpret_cast<char*>(&chunkSize), 4);

            if (std::strncmp(chunkId, "fmt ", 4) == 0)
            {
                // Read format chunk
                u16 audioFormat, channels, blockAlign, bitsPerSample;
                u32 sampleRate, byteRate;

                file.read(reinterpret_cast<char*>(&audioFormat), 2);
                file.read(reinterpret_cast<char*>(&channels), 2);
                file.read(reinterpret_cast<char*>(&sampleRate), 4);
                file.read(reinterpret_cast<char*>(&byteRate), 4);
                file.read(reinterpret_cast<char*>(&blockAlign), 2);
                file.read(reinterpret_cast<char*>(&bitsPerSample), 2);

                // Store values
                numChannels = channels;
                samplingRate = sampleRate;
                bitDepth = bitsPerSample;
                fmtFound = true;

                // Skip any extra fmt data
                if (chunkSize > 16)
                {
                    file.seekg(chunkSize - 16, std::ios::cur);
                }
            }
            else if (std::strncmp(chunkId, "data", 4) == 0)
            {
                dataSize = chunkSize;
                // Skip the data chunk content
                file.seekg(chunkSize, std::ios::cur);
            }
            else
            {
                // Skip unknown chunk
                file.seekg(chunkSize, std::ios::cur);
            }
        }

        if (fmtFound && dataSize > 0)
        {
            // Calculate duration: dataSize / (sampleRate * channels * (bitDepth/8))
            u32 bytesPerSample = (bitDepth / 8) * numChannels;
            if (bytesPerSample > 0 && samplingRate > 0)
            {
                duration = static_cast<double>(dataSize) / (samplingRate * bytesPerSample);
            }

            OLO_CORE_TRACE("AudioFileSourceSerializer: WAV analysis complete - {}Hz, {}bit, {} channels, {:.2f}s",
                           samplingRate, bitDepth, numChannels, duration);
            return true;
        }

        OLO_CORE_WARN("AudioFileSourceSerializer: Failed to find required chunks in WAV file: {}", filePath.string());
        return false;
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
    // PrefabSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void PrefabSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<Prefab> prefab = asset.As<Prefab>();
        if (!prefab)
        {
            OLO_CORE_ERROR("PrefabSerializer::Serialize - Asset is not a Prefab");
            return;
        }

        std::string yamlString = SerializeToYAML(prefab);

        std::ofstream fout(metadata.FilePath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("PrefabSerializer::Serialize - Failed to open file for writing: {}", metadata.FilePath.string());
            return;
        }

        fout << yamlString;
        fout.close();
    }

    bool PrefabSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::ifstream stream(metadata.FilePath);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - Failed to open file: {}", metadata.FilePath.string());
            return false;
        }

        std::stringstream strStream;
        strStream << stream.rdbuf();
        stream.close();

        Ref<Prefab> prefab = Ref<Prefab>::Create();
        bool success = DeserializeFromYAML(strStream.str(), prefab);
        if (!success)
        {
            OLO_CORE_ERROR("PrefabSerializer::TryLoadData - Failed to deserialize prefab from YAML");
            return false;
        }

        asset = prefab;
        asset->m_Handle = metadata.Handle;
        return true;
    }

    bool PrefabSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(handle);
        if (!prefab)
        {
            OLO_CORE_ERROR("PrefabSerializer::SerializeToAssetPack - Failed to get prefab asset");
            return false;
        }

        std::string yamlString = SerializeToYAML(prefab);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> PrefabSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<Prefab> prefab = Ref<Prefab>::Create();
        bool result = DeserializeFromYAML(yamlString, prefab);
        if (!result)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromAssetPack - Failed to deserialize prefab from YAML");
            return nullptr;
        }

        prefab->m_Handle = assetInfo.Handle;
        return prefab;
    }

    std::string PrefabSerializer::SerializeToYAML(const Ref<Prefab>& prefab) const
    {
        if (!prefab || !prefab->GetScene())
        {
            OLO_CORE_ERROR("PrefabSerializer::SerializeToYAML - Invalid prefab or scene");
            return "";
        }

        // Use SceneSerializer to serialize the entire scene
        SceneSerializer sceneSerializer(prefab->GetScene());
        std::string sceneYaml = sceneSerializer.SerializeToYAML();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Prefab";
        out << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Handle" << YAML::Value << prefab->GetHandle();
        out << YAML::Key << "Scene" << YAML::Value << sceneYaml;
        out << YAML::EndMap;
        out << YAML::EndMap;

        return std::string(out.c_str());
    }

    bool PrefabSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<Prefab>& prefab) const
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Failed to parse YAML: {}", e.what());
            return false;
        }

        auto prefabNode = data["Prefab"];
        if (!prefabNode)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Missing Prefab node");
            return false;
        }

        // Create a new scene for the prefab
        Ref<Scene> scene = Scene::Create();
        if (!scene)
        {
            OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Failed to create scene");
            return false;
        }

        // Deserialize the scene content
        auto sceneNode = prefabNode["Scene"];
        if (sceneNode)
        {
            std::string sceneYamlString = sceneNode.as<std::string>();
            SceneSerializer sceneSerializer(scene);
            bool result = sceneSerializer.DeserializeFromYAML(sceneYamlString);
            if (!result)
            {
                OLO_CORE_ERROR("PrefabSerializer::DeserializeFromYAML - Failed to deserialize scene from YAML");
                return false;
            }
        }

        // Set up the prefab with the deserialized scene
        prefab->m_Scene = scene;

        // Find the root entity (assuming it's the first entity in the scene)
        auto entities = scene->GetAllEntitiesWith<IDComponent>();
        if (!entities.empty())
        {
            auto firstEntity = entities.front();
            prefab->m_Entity = Entity{ firstEntity, scene.get() };
        }

        return true;
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

    Ref<Scene> SceneAssetSerializer::DeserializeSceneFromAssetPack([[maybe_unused]] FileStreamReader& stream, [[maybe_unused]] const AssetPackFile::SceneInfo& sceneInfo) const
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
        Ref<MeshColliderAsset> meshCollider = asset.As<MeshColliderAsset>();
        if (!meshCollider)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Invalid mesh collider asset");
            return;
        }

        std::string yamlString = SerializeToYAML(meshCollider);

        std::filesystem::path filepath = Project::GetAssetDirectory() / metadata.FilePath;

        // Ensure parent directory exists
        std::filesystem::path parentDir = filepath.parent_path();
        if (!parentDir.empty())
        {
            std::error_code ec;
            if (!std::filesystem::create_directories(parentDir, ec) && ec)
            {
                OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Failed to create parent directories for: {}, error: {}", filepath.string(), ec.message());
                return;
            }
        }

        // Create temporary file for atomic write
        std::filesystem::path tempFilepath = parentDir / (filepath.filename().string() + ".tmp");

        // Write to temporary file
        std::ofstream fout(tempFilepath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Failed to open temporary file for writing: {}", tempFilepath.string());
            return;
        }

        fout << yamlString;
        fout.flush(); // Ensure data is written to the file
        fout.close();

        // Atomically rename temp file to final file
        std::error_code ec;
        std::filesystem::rename(tempFilepath, filepath, ec);
        if (ec)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::Serialize - Failed to rename temporary file {} to {}, error: {}", tempFilepath.string(), filepath.string(), ec.message());
            // Clean up temporary file on failure
            std::filesystem::remove(tempFilepath, ec);
            return;
        }

        OLO_CORE_TRACE("MeshColliderSerializer::Serialize - Successfully serialized MeshCollider to: {}", filepath.string());
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

        auto meshCollider = Ref<MeshColliderAsset>::Create();

        // Use the YAML deserializer to load the data
        bool success = DeserializeFromYAML(strStream.str(), meshCollider);
        if (!success)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::TryLoadData - Failed to deserialize from YAML");
            return false;
        }

        meshCollider->SetHandle(metadata.Handle);
        asset = meshCollider;

        OLO_CORE_TRACE("MeshColliderSerializer::TryLoadData - Successfully loaded mesh collider: {}", path.string());
        return true;
    }

    bool MeshColliderSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<MeshColliderAsset> meshCollider = AssetManager::GetAsset<MeshColliderAsset>(handle);
        if (!meshCollider)
        {
            OLO_CORE_ERROR("MeshColliderSerializer: Failed to get MeshColliderAsset for handle {0}", handle);
            return false;
        }

        std::string yamlString = SerializeToYAML(meshCollider);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("MeshColliderSerializer: Serialized MeshCollider to pack - Handle: {0}, Size: {1}",
                       handle, outInfo.Size);
        return true;
    }

    Ref<Asset> MeshColliderSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<MeshColliderAsset> meshCollider = Ref<MeshColliderAsset>::Create();
        bool success = DeserializeFromYAML(yamlString, meshCollider);

        if (!success)
        {
            OLO_CORE_ERROR("MeshColliderSerializer: Failed to deserialize MeshCollider from YAML - Handle: {0}", assetInfo.Handle);
            return nullptr;
        }

        meshCollider->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("MeshColliderSerializer: Deserialized MeshCollider from pack - Handle: {0}", assetInfo.Handle);
        return meshCollider;
    }

    std::string MeshColliderSerializer::SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // MeshCollider

        out << YAML::Key << "MeshCollider" << YAML::Value;
        out << YAML::BeginMap; // Asset Data

        // Serialize ColliderMesh asset reference
        out << YAML::Key << "ColliderMesh" << YAML::Value << meshCollider->m_ColliderMesh;

        // Serialize Material properties
        out << YAML::Key << "Material" << YAML::Value;
        out << YAML::BeginMap; // Material
        out << YAML::Key << "StaticFriction" << YAML::Value << meshCollider->m_Material.GetStaticFriction();
        out << YAML::Key << "DynamicFriction" << YAML::Value << meshCollider->m_Material.GetDynamicFriction();
        out << YAML::Key << "Restitution" << YAML::Value << meshCollider->m_Material.GetRestitution();
        out << YAML::Key << "Density" << YAML::Value << meshCollider->m_Material.GetDensity();
        out << YAML::EndMap; // Material

        // Serialize other properties
        out << YAML::Key << "EnableVertexWelding" << YAML::Value << meshCollider->m_EnableVertexWelding;
        out << YAML::Key << "VertexWeldTolerance" << YAML::Value << meshCollider->m_VertexWeldTolerance;
        out << YAML::Key << "FlipNormals" << YAML::Value << meshCollider->m_FlipNormals;
        out << YAML::Key << "CheckZeroAreaTriangles" << YAML::Value << meshCollider->m_CheckZeroAreaTriangles;
        out << YAML::Key << "AreaTestEpsilon" << YAML::Value << meshCollider->m_AreaTestEpsilon;
        out << YAML::Key << "ShiftVerticesToOrigin" << YAML::Value << meshCollider->m_ShiftVerticesToOrigin;
        out << YAML::Key << "AlwaysShareShape" << YAML::Value << meshCollider->m_AlwaysShareShape;

        // Serialize collision complexity
        out << YAML::Key << "CollisionComplexity" << YAML::Value << (int)meshCollider->m_CollisionComplexity;

        // Serialize scale
        out << YAML::Key << "ColliderScale" << YAML::Value << meshCollider->m_ColliderScale;

        out << YAML::EndMap; // Asset Data
        out << YAML::EndMap; // MeshCollider

        return std::string(out.c_str());
    }

    bool MeshColliderSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const
    {
        try
        {
            YAML::Node data = YAML::Load(yamlString);
            if (!data["MeshCollider"])
            {
                OLO_CORE_ERROR("MeshColliderSerializer: No MeshCollider node found in YAML");
                return false;
            }

            YAML::Node meshColliderNode = data["MeshCollider"];

            // Deserialize ColliderMesh asset reference
            if (meshColliderNode["ColliderMesh"])
                targetMeshCollider->m_ColliderMesh = meshColliderNode["ColliderMesh"].as<AssetHandle>();

            // Deserialize Material properties
            if (meshColliderNode["Material"])
            {
                YAML::Node materialNode = meshColliderNode["Material"];
                ColliderMaterial material;
                // Handle both old and new material formats for backward compatibility
                if (materialNode["Friction"])
                {
                    float friction = materialNode["Friction"].as<float>(0.5f);
                    // Clamp friction values to valid range [0.0, 1.0]
                    friction = std::clamp(friction, 0.0f, 1.0f);
                    material.SetStaticFriction(friction);
                    material.SetDynamicFriction(friction);
                }
                else
                {
                    float staticFriction = materialNode["StaticFriction"].as<float>(0.6f);
                    float dynamicFriction = materialNode["DynamicFriction"].as<float>(0.6f);
                    // Clamp friction values to valid range [0.0, 1.0]
                    material.SetStaticFriction(std::clamp(staticFriction, 0.0f, 1.0f));
                    material.SetDynamicFriction(std::clamp(dynamicFriction, 0.0f, 1.0f));
                }

                float restitution = materialNode["Restitution"].as<float>(0.0f);
                float density = materialNode["Density"].as<float>(1000.0f);

                // Clamp restitution to valid range [0.0, 1.0]
                material.SetRestitution(std::clamp(restitution, 0.0f, 1.0f));

                // Clamp density to sensible positive range [MIN_DENSITY, 1e6]
                material.SetDensity(std::clamp(density, ColliderMaterial::MIN_DENSITY, 1e6f));

                targetMeshCollider->m_Material = material;
            }

            // Deserialize other properties
            if (meshColliderNode["EnableVertexWelding"])
                targetMeshCollider->m_EnableVertexWelding = meshColliderNode["EnableVertexWelding"].as<bool>();
            if (meshColliderNode["VertexWeldTolerance"])
                targetMeshCollider->m_VertexWeldTolerance = meshColliderNode["VertexWeldTolerance"].as<float>();
            if (meshColliderNode["FlipNormals"])
                targetMeshCollider->m_FlipNormals = meshColliderNode["FlipNormals"].as<bool>();
            if (meshColliderNode["CheckZeroAreaTriangles"])
                targetMeshCollider->m_CheckZeroAreaTriangles = meshColliderNode["CheckZeroAreaTriangles"].as<bool>();
            if (meshColliderNode["AreaTestEpsilon"])
                targetMeshCollider->m_AreaTestEpsilon = meshColliderNode["AreaTestEpsilon"].as<float>();
            if (meshColliderNode["ShiftVerticesToOrigin"])
                targetMeshCollider->m_ShiftVerticesToOrigin = meshColliderNode["ShiftVerticesToOrigin"].as<bool>();
            if (meshColliderNode["AlwaysShareShape"])
                targetMeshCollider->m_AlwaysShareShape = meshColliderNode["AlwaysShareShape"].as<bool>();

            // Deserialize collision complexity
            if (meshColliderNode["CollisionComplexity"])
                targetMeshCollider->m_CollisionComplexity = (ECollisionComplexity)meshColliderNode["CollisionComplexity"].as<int>();

            // Deserialize scale
            if (meshColliderNode["ColliderScale"])
                targetMeshCollider->m_ColliderScale = meshColliderNode["ColliderScale"].as<glm::vec3>();

            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshColliderSerializer: YAML parsing error: {0}", e.what());
            return false;
        }
    }

    void MeshColliderSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("MeshColliderSerializer::RegisterDependencies - File does not exist: {}", path.string());
            return;
        }

        std::ifstream fin(path);
        if (!fin.good())
        {
            OLO_CORE_WARN("MeshColliderSerializer::RegisterDependencies - Failed to open file: {}", path.string());
            return;
        }

        std::stringstream ss;
        ss << fin.rdbuf();
        fin.close();

        RegisterDependenciesFromYAML(ss.str(), metadata.Handle);
    }

    void MeshColliderSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        // Deregister existing dependencies first
        AssetManager::DeregisterDependencies(handle);

        try
        {
            YAML::Node root = YAML::Load(yamlString);
            YAML::Node meshColliderNode = root["MeshCollider"];
            if (!meshColliderNode)
                return;

            // Register ColliderMesh dependency
            if (meshColliderNode["ColliderMesh"])
            {
                AssetHandle colliderMeshHandle = meshColliderNode["ColliderMesh"].as<AssetHandle>(0);
                if (colliderMeshHandle != 0)
                {
                    AssetManager::RegisterDependency(colliderMeshHandle, handle);
                    OLO_CORE_TRACE("MeshColliderSerializer: Registered dependency - MeshCollider {0} depends on ColliderMesh {1}", handle, colliderMeshHandle);
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("MeshColliderSerializer::RegisterDependenciesFromYAML - YAML parsing error: {0}", e.what());
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////
    // ScriptFileSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void ScriptFileSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<ScriptFileAsset> scriptAsset = asset.As<ScriptFileAsset>();
        std::string yamlString = SerializeToYAML(scriptAsset);

        std::ofstream fout(Project::GetAssetDirectory() / metadata.FilePath);
        fout << yamlString;

        OLO_CORE_TRACE("ScriptFileSerializer: Serialized ScriptFile to YAML - Handle: {0}", metadata.Handle);
    }

    bool ScriptFileSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetAssetDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - Failed to open file: {}", path.string());
            return false;
        }

        std::stringstream strStream;
        strStream << file.rdbuf();

        Ref<ScriptFileAsset> scriptAsset = Ref<ScriptFileAsset>::Create();
        bool success = DeserializeFromYAML(strStream.str(), scriptAsset);

        if (!success)
        {
            OLO_CORE_ERROR("ScriptFileSerializer::TryLoadData - Failed to deserialize from YAML");
            return false;
        }

        scriptAsset->SetHandle(metadata.Handle);
        asset = scriptAsset;

        OLO_CORE_TRACE("ScriptFileSerializer::TryLoadData - Successfully loaded script file: {}", path.string());
        return true;
    }

    bool ScriptFileSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<ScriptFileAsset> scriptAsset = AssetManager::GetAsset<ScriptFileAsset>(handle);
        if (!scriptAsset)
        {
            OLO_CORE_ERROR("ScriptFileSerializer: Failed to get ScriptFileAsset for handle {0}", handle);
            return false;
        }

        std::string yamlString = SerializeToYAML(scriptAsset);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("ScriptFileSerializer: Serialized ScriptFile to pack - Handle: {0}, Size: {1}",
                       handle, outInfo.Size);
        return true;
    }

    Ref<Asset> ScriptFileSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<ScriptFileAsset> scriptAsset = Ref<ScriptFileAsset>::Create();
        bool success = DeserializeFromYAML(yamlString, scriptAsset);

        if (!success)
        {
            OLO_CORE_ERROR("ScriptFileSerializer: Failed to deserialize ScriptFile from YAML - Handle: {0}", assetInfo.Handle);
            return nullptr;
        }

        scriptAsset->SetHandle(assetInfo.Handle);
        OLO_CORE_TRACE("ScriptFileSerializer: Deserialized ScriptFile from pack - Handle: {0}", assetInfo.Handle);
        return scriptAsset;
    }

    std::string ScriptFileSerializer::SerializeToYAML(Ref<ScriptFileAsset> scriptAsset) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap; // ScriptFile

        out << YAML::Key << "ScriptFile" << YAML::Value;
        out << YAML::BeginMap; // Asset Data

        out << YAML::Key << "ClassNamespace" << YAML::Value << scriptAsset->GetClassNamespace();
        out << YAML::Key << "ClassName" << YAML::Value << scriptAsset->GetClassName();

        out << YAML::EndMap; // Asset Data
        out << YAML::EndMap; // ScriptFile

        return std::string(out.c_str());
    }

    bool ScriptFileSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<ScriptFileAsset> targetScriptAsset) const
    {
        try
        {
            YAML::Node data = YAML::Load(yamlString);
            if (!data["ScriptFile"])
            {
                OLO_CORE_ERROR("ScriptFileSerializer: No ScriptFile node found in YAML");
                return false;
            }

            YAML::Node scriptNode = data["ScriptFile"];

            if (scriptNode["ClassNamespace"])
                targetScriptAsset->SetClassNamespace(scriptNode["ClassNamespace"].as<std::string>());
            if (scriptNode["ClassName"])
                targetScriptAsset->SetClassName(scriptNode["ClassName"].as<std::string>());

            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("ScriptFileSerializer: YAML parsing error: {0}", e.what());
            return false;
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    // MeshSourceSerializer
    //////////////////////////////////////////////////////////////////////////////////

    bool MeshSourceSerializer::TryLoadData(const AssetMetadata& metadata, [[maybe_unused]] Ref<Asset>& asset) const
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
        // meshSource->SetHandle(metadata.Handle);
        // asset = meshSource;
        //
        // OLO_CORE_TRACE("MeshSourceSerializer::TryLoadData - Successfully loaded mesh source: {} ({} submeshes)",
        //               path.string(), meshSource->GetSubmeshes().size());

        OLO_CORE_WARN("MeshSourceSerializer::TryLoadData - MeshSource class not implemented yet");
        return false;
    }

    bool MeshSourceSerializer::SerializeToAssetPack([[maybe_unused]] AssetHandle handle, [[maybe_unused]] FileStreamWriter& stream, [[maybe_unused]] AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement mesh source pack serialization
        OLO_CORE_WARN("MeshSourceSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> MeshSourceSerializer::DeserializeFromAssetPack([[maybe_unused]] FileStreamReader& stream, [[maybe_unused]] const AssetPackFile::AssetInfo& assetInfo) const
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
        // out << YAML::Key << "SubmeshCount" << YAML::Value << mesh->GetSubmeshes().Num();

        // For now, just serialize basic mesh info
        out << YAML::Key << "VertexCount" << YAML::Value << mesh->GetVertices().Num();
        out << YAML::Key << "IndexCount" << YAML::Value << mesh->GetIndices().Num();

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
        // For StaticMesh, register material dependencies from MaterialTable
        // Note: Regular Mesh assets don't currently have material dependencies in OloEngine
        // This is mainly for StaticMesh assets that have MaterialTable
        if (metadata.Type == AssetType::StaticMesh)
        {
            Ref<Asset> asset;
            if (TryLoadData(metadata, asset))
            {
                auto staticMesh = asset.As<StaticMesh>();
                if (staticMesh)
                {
                    AssetManager::DeregisterDependencies(metadata.Handle);

                    // Register MeshSource dependency
                    AssetHandle meshSourceHandle = staticMesh->GetMeshSource();
                    if (meshSourceHandle != 0)
                    {
                        AssetManager::RegisterDependency(meshSourceHandle, metadata.Handle);
                        OLO_CORE_TRACE("MeshSerializer: Registered MeshSource dependency - StaticMesh {0} depends on MeshSource {1}", metadata.Handle, meshSourceHandle);
                    }

                    // Register material dependencies
                    auto materialTable = staticMesh->GetMaterials();
                    if (materialTable)
                    {
                        const auto& materials = materialTable->GetMaterials();
                        for (const auto& [index, materialHandle] : materials)
                        {
                            if (materialHandle != 0)
                            {
                                AssetManager::RegisterDependency(materialHandle, metadata.Handle);
                                OLO_CORE_TRACE("MeshSerializer: Registered material dependency - StaticMesh {0} depends on Material {1} at index {2}", metadata.Handle, materialHandle, index);
                            }
                        }
                    }
                }
            }
        }
        else if (metadata.Type == AssetType::Mesh)
        {
            // Regular Mesh assets in OloEngine don't currently have material dependencies
            // They only reference the MeshSource, which is handled by the asset loading system
            OLO_CORE_TRACE("MeshSerializer::RegisterDependencies - Mesh assets don't have material dependencies in current implementation");
        }
        else
        {
            OLO_CORE_WARN("MeshSerializer::RegisterDependencies - Unexpected asset type: {}", (int)metadata.Type);
        }
    }

    bool MeshSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(handle);
        if (!mesh)
        {
            OLO_CORE_ERROR("MeshSerializer: Failed to get Mesh asset for handle {0}", handle);
            return false;
        }

        // Serialize mesh properties
        // For basic Mesh, we store the MeshSource handle and submesh index
        AssetHandle meshSourceHandle = 0;
        if (mesh->GetMeshSource())
        {
            meshSourceHandle = mesh->GetMeshSource()->GetHandle();
        }

        stream.WriteRaw<AssetHandle>(meshSourceHandle);
        stream.WriteRaw<u32>(mesh->GetSubmeshIndex());

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("MeshSerializer: Serialized Mesh to pack - Handle: {0}, MeshSource: {1}, SubmeshIndex: {2}, Size: {3}",
                       handle, meshSourceHandle, mesh->GetSubmeshIndex(), outInfo.Size);
        return true;
    }

    Ref<Asset> MeshSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read mesh properties
        AssetHandle meshSourceHandle;
        stream.ReadRaw<AssetHandle>(meshSourceHandle);

        u32 submeshIndex;
        stream.ReadRaw<u32>(submeshIndex);

        // Create Mesh asset
        Ref<MeshSource> meshSource = nullptr;
        if (meshSourceHandle != 0)
        {
            meshSource = AssetManager::GetAsset<MeshSource>(meshSourceHandle);
        }

        Ref<Mesh> mesh = Ref<Mesh>(new Mesh(meshSource, submeshIndex));
        mesh->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("MeshSerializer: Deserialized Mesh from pack - Handle: {0}, MeshSource: {1}, SubmeshIndex: {2}",
                       assetInfo.Handle, meshSourceHandle, submeshIndex);
        return mesh;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // StaticMeshSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void StaticMeshSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<StaticMesh> staticMesh = asset.As<StaticMesh>();
        if (!staticMesh)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::Serialize - Asset is not a valid StaticMesh");
            return;
        }

        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;

        try
        {
            YAML::Emitter out;
            out << YAML::BeginMap;
            out << YAML::Key << "StaticMesh" << YAML::Value;
            out << YAML::BeginMap;

            // Serialize mesh source handle
            out << YAML::Key << "MeshSource" << YAML::Value << staticMesh->GetMeshSource();

            // Serialize collider generation flag
            out << YAML::Key << "GenerateColliders" << YAML::Value << staticMesh->ShouldGenerateColliders();

            // Serialize submesh indices if not using all submeshes
            const auto& submeshIndices = staticMesh->GetSubmeshes();
            if (!submeshIndices.IsEmpty())
            {
                out << YAML::Key << "Submeshes" << YAML::Value;
                out << YAML::BeginSeq;
                for (u32 index : submeshIndices)
                {
                    out << index;
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap;
            out << YAML::EndMap;

            std::ofstream fout(path);
            fout << out.c_str();
            fout.close();

            OLO_CORE_TRACE("StaticMeshSerializer::Serialize - Successfully serialized static mesh to: {}", path.string());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::Serialize - Error serializing static mesh {}: {}", path.string(), e.what());
        }
    }

    bool StaticMeshSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - File does not exist: {}", path.string());
            return false;
        }

        try
        {
            // Load YAML file with static mesh configuration
            YAML::Node yamlData = YAML::LoadFile(path.string());

            if (!yamlData["StaticMesh"])
            {
                OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Invalid static mesh file (missing StaticMesh node): {}", path.string());
                return false;
            }

            YAML::Node meshNode = yamlData["StaticMesh"];

            // Get the mesh source handle
            AssetHandle meshSourceHandle = meshNode["MeshSource"].as<u64>(0);
            if (meshSourceHandle == 0)
            {
                OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Invalid mesh source handle in: {}", path.string());
                return false;
            }

            // Get optional settings
            bool generateColliders = meshNode["GenerateColliders"].as<bool>(false);

            // Get submesh indices (optional)
            TArray<u32> submeshIndices;
            if (meshNode["Submeshes"])
            {
                for (const auto& submeshNode : meshNode["Submeshes"])
                {
                    submeshIndices.Add(submeshNode.as<u32>());
                }
            }

            // Create the static mesh
            Ref<StaticMesh> staticMesh;
            if (submeshIndices.IsEmpty())
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, generateColliders);
            }
            else
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, submeshIndices, generateColliders);
            }

            staticMesh->SetHandle(metadata.Handle);
            asset = staticMesh;

            OLO_CORE_TRACE("StaticMeshSerializer::TryLoadData - Successfully loaded static mesh: {}", path.string());
            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - YAML parsing error in {}: {}", path.string(), e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::TryLoadData - Error loading static mesh {}: {}", path.string(), e.what());
            return false;
        }
    }

    void StaticMeshSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        std::filesystem::path path = Project::GetAssetDirectory() / metadata.FilePath;

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("StaticMeshSerializer::RegisterDependencies - File does not exist: {}", path.string());
            return;
        }

        try
        {
            // Deregister existing dependencies first
            AssetManager::DeregisterDependencies(metadata.Handle);

            YAML::Node yamlData = YAML::LoadFile(path.string());

            if (!yamlData["StaticMesh"])
            {
                OLO_CORE_WARN("StaticMeshSerializer::RegisterDependencies - Invalid static mesh file: {}", path.string());
                return;
            }

            YAML::Node meshNode = yamlData["StaticMesh"];

            // Register mesh source dependency
            AssetHandle meshSourceHandle = meshNode["MeshSource"].as<u64>(0);
            if (meshSourceHandle != 0)
            {
                AssetManager::RegisterDependency(meshSourceHandle, metadata.Handle);
                OLO_CORE_TRACE("StaticMeshSerializer: Registered MeshSource dependency - StaticMesh {0} depends on MeshSource {1}", metadata.Handle, meshSourceHandle);
            }

            // Register material dependencies from MaterialTable
            if (meshNode["MaterialTable"])
            {
                YAML::Node materialTable = meshNode["MaterialTable"];

                if (materialTable["Materials"] && materialTable["Materials"].IsMap())
                {
                    for (const auto& materialEntry : materialTable["Materials"])
                    {
                        u32 materialIndex = materialEntry.first.as<u32>();
                        AssetHandle materialHandle = materialEntry.second.as<AssetHandle>(0);

                        if (materialHandle != 0)
                        {
                            AssetManager::RegisterDependency(materialHandle, metadata.Handle);
                            OLO_CORE_TRACE("StaticMeshSerializer: Registered material dependency - StaticMesh {0} depends on Material {1} at index {2}", metadata.Handle, materialHandle, materialIndex);
                        }
                    }
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::RegisterDependencies - YAML parsing error in {}: {}", path.string(), e.what());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::RegisterDependencies - Error in {}: {}", path.string(), e.what());
        }
    }

    bool StaticMeshSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<StaticMesh> staticMesh = AssetManager::GetAsset<StaticMesh>(handle);
        if (!staticMesh)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::SerializeToAssetPack - Failed to load static mesh asset {}", handle);
            return false;
        }

        outInfo.Offset = stream.GetStreamPosition();

        try
        {
            // Write mesh source handle
            stream.WriteRaw<AssetHandle>(staticMesh->GetMeshSource());

            // Write collider generation flag
            stream.WriteRaw<bool>(staticMesh->ShouldGenerateColliders());

            // Write submesh indices
            const auto& submeshIndices = staticMesh->GetSubmeshes();
            stream.WriteRaw<u32>(static_cast<u32>(submeshIndices.Num()));
            for (u32 index : submeshIndices)
            {
                stream.WriteRaw<u32>(index);
            }

            outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::SerializeToAssetPack - Error serializing static mesh {}: {}", handle, e.what());
            return false;
        }
    }

    Ref<Asset> StaticMeshSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        try
        {
            // Read mesh source handle
            AssetHandle meshSourceHandle;
            stream.ReadRaw(meshSourceHandle);

            // Read collider generation flag
            bool generateColliders;
            stream.ReadRaw(generateColliders);

            // Read submesh indices
            u32 submeshCount;
            stream.ReadRaw(submeshCount);
            TArray<u32> submeshIndices;
            submeshIndices.Reserve(static_cast<i32>(submeshCount));

            for (u32 i = 0; i < submeshCount; ++i)
            {
                u32 submeshIndex;
                stream.ReadRaw(submeshIndex);
                submeshIndices.Add(submeshIndex);
            }

            // Create static mesh
            Ref<StaticMesh> staticMesh;
            if (submeshIndices.IsEmpty())
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, generateColliders);
            }
            else
            {
                staticMesh = Ref<StaticMesh>::Create(meshSourceHandle, submeshIndices, generateColliders);
            }

            staticMesh->SetHandle(assetInfo.Handle);
            return staticMesh;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("StaticMeshSerializer::DeserializeFromAssetPack - Error deserializing static mesh: {}", e.what());
            return nullptr;
        }
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AnimationAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    std::string AnimationAssetSerializer::SerializeToYAML(Ref<AnimationAsset> animationAsset) const
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        {
            out << YAML::Key << "Animation" << YAML::Value;
            out << YAML::BeginMap;
            {
                out << YAML::Key << "AnimationSource" << YAML::Value << animationAsset->GetAnimationSource();
                out << YAML::Key << "Mesh" << YAML::Value << animationAsset->GetMeshHandle();
                out << YAML::Key << "AnimationName" << YAML::Value << animationAsset->GetAnimationName();
                out << YAML::Key << "ExtractRootMotion" << YAML::Value << animationAsset->IsExtractRootMotion();
                out << YAML::Key << "RootBoneIndex" << YAML::Value << animationAsset->GetRootBoneIndex();
                out << YAML::Key << "RootTranslationMask" << YAML::Value << animationAsset->GetRootTranslationMask();
                out << YAML::Key << "RootRotationMask" << YAML::Value << animationAsset->GetRootRotationMask();
                out << YAML::Key << "DiscardRootMotion" << YAML::Value << animationAsset->IsDiscardRootMotion();
            }
            out << YAML::EndMap;
        }
        out << YAML::EndMap;

        return std::string(out.c_str());
    }

    bool AnimationAssetSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<AnimationAsset>& animationAsset) const
    {
        YAML::Node data = YAML::Load(yamlString);
        return DeserializeFromYAML(data, animationAsset);
    }

    bool AnimationAssetSerializer::DeserializeFromYAML(const YAML::Node& data, Ref<AnimationAsset>& animationAsset) const
    {
        auto animationNode = data["Animation"];
        if (!animationNode)
            return false;

        AssetHandle animationSource = animationNode["AnimationSource"].as<AssetHandle>();
        AssetHandle mesh = animationNode["Mesh"].as<AssetHandle>();
        std::string animationName = animationNode["AnimationName"].as<std::string>();
        bool extractRootMotion = animationNode["ExtractRootMotion"] ? animationNode["ExtractRootMotion"].as<bool>() : false;
        u32 rootBoneIndex = animationNode["RootBoneIndex"] ? animationNode["RootBoneIndex"].as<u32>() : 0;

        glm::vec3 rootTranslationMask = glm::vec3(1.0f);
        if (animationNode["RootTranslationMask"])
        {
            auto maskNode = animationNode["RootTranslationMask"];
            if (maskNode.IsSequence() && maskNode.size() == 3)
            {
                rootTranslationMask.x = maskNode[0].as<float>();
                rootTranslationMask.y = maskNode[1].as<float>();
                rootTranslationMask.z = maskNode[2].as<float>();
            }
        }

        glm::vec3 rootRotationMask = glm::vec3(1.0f);
        if (animationNode["RootRotationMask"])
        {
            auto maskNode = animationNode["RootRotationMask"];
            if (maskNode.IsSequence() && maskNode.size() == 3)
            {
                rootRotationMask.x = maskNode[0].as<float>();
                rootRotationMask.y = maskNode[1].as<float>();
                rootRotationMask.z = maskNode[2].as<float>();
            }
        }

        bool discardRootMotion = animationNode["DiscardRootMotion"] ? animationNode["DiscardRootMotion"].as<bool>() : false;

        animationAsset = Ref<AnimationAsset>(new AnimationAsset(animationSource, mesh, animationName,
                                                                extractRootMotion, rootBoneIndex,
                                                                rootTranslationMask, rootRotationMask,
                                                                discardRootMotion));
        return true;
    }

    void AnimationAssetSerializer::RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const
    {
        YAML::Node data = YAML::Load(yamlString);
        auto animationNode = data["Animation"];
        if (!animationNode)
            return;

        // Register dependencies on animation source and mesh
        if (animationNode["AnimationSource"])
        {
            AssetHandle animationSource = animationNode["AnimationSource"].as<AssetHandle>();
            if (animationSource != 0)
                AssetManager::RegisterDependency(animationSource, handle);
        }

        if (animationNode["Mesh"])
        {
            AssetHandle mesh = animationNode["Mesh"].as<AssetHandle>();
            if (mesh != 0)
                AssetManager::RegisterDependency(mesh, handle);
        }
    }

    void AnimationAssetSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        Ref<AnimationAsset> animationAsset = asset.As<AnimationAsset>();
        if (!animationAsset)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::Serialize - Asset is not an AnimationAsset");
            return;
        }

        std::ofstream fout(Project::GetAssetDirectory() / metadata.FilePath);
        if (!fout.good())
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::Serialize - Failed to open file for writing: {}", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(animationAsset);
        fout << yamlString;
    }

    bool AnimationAssetSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        std::ifstream fin(Project::GetAssetDirectory() / metadata.FilePath);
        if (!fin.good())
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::TryLoadData - Failed to open file: {}", metadata.FilePath.string());
            return false;
        }

        std::stringstream buffer;
        buffer << fin.rdbuf();
        std::string yamlString = buffer.str();

        Ref<AnimationAsset> animationAsset;
        bool result = DeserializeFromYAML(yamlString, animationAsset);
        if (!result)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::TryLoadData - Failed to deserialize animation asset");
            return false;
        }

        asset = animationAsset;
        asset->m_Handle = metadata.Handle;
        RegisterDependenciesFromYAML(yamlString, asset->m_Handle);
        return true;
    }

    void AnimationAssetSerializer::RegisterDependencies(const AssetMetadata& metadata) const
    {
        std::ifstream fin(Project::GetAssetDirectory() / metadata.FilePath);
        if (!fin.good())
        {
            OLO_CORE_WARN("AnimationAssetSerializer::RegisterDependencies - Failed to open file: {}", metadata.FilePath.string());
            return;
        }

        std::stringstream buffer;
        buffer << fin.rdbuf();
        std::string yamlString = buffer.str();

        RegisterDependenciesFromYAML(yamlString, metadata.Handle);
    }

    bool AnimationAssetSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        Ref<AnimationAsset> animationAsset = AssetManager::GetAsset<AnimationAsset>(handle);
        if (!animationAsset)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::SerializeToAssetPack - Failed to get animation asset");
            return false;
        }

        std::string yamlString = SerializeToYAML(animationAsset);

        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> AnimationAssetSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        Ref<AnimationAsset> animationAsset;
        bool result = DeserializeFromYAML(yamlString, animationAsset);
        if (!result)
        {
            OLO_CORE_ERROR("AnimationAssetSerializer::DeserializeFromAssetPack - Failed to deserialize animation asset");
            return nullptr;
        }

        return animationAsset;
    }

    //////////////////////////////////////////////////////////////////////////////////
    // AnimationGraphAssetSerializer
    //////////////////////////////////////////////////////////////////////////////////

    void AnimationGraphAssetSerializer::Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const
    {
        // TODO: Implement animation graph serialization
        OLO_CORE_WARN("AnimationGraphAssetSerializer::Serialize not yet implemented");
    }

    bool AnimationGraphAssetSerializer::TryLoadData([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] Ref<Asset>& asset) const
    {
        // TODO: Implement animation graph loading
        OLO_CORE_WARN("AnimationGraphAssetSerializer::TryLoadData not yet implemented");
        return false;
    }

    void AnimationGraphAssetSerializer::RegisterDependencies([[maybe_unused]] const AssetMetadata& metadata) const
    {
        // TODO: Implement dependency registration
        OLO_CORE_WARN("AnimationGraphAssetSerializer::RegisterDependencies not yet implemented");
    }

    bool AnimationGraphAssetSerializer::SerializeToAssetPack([[maybe_unused]] AssetHandle handle, [[maybe_unused]] FileStreamWriter& stream, [[maybe_unused]] AssetSerializationInfo& outInfo) const
    {
        // TODO: Implement animation graph pack serialization
        OLO_CORE_WARN("AnimationGraphAssetSerializer::SerializeToAssetPack not yet implemented");
        return false;
    }

    Ref<Asset> AnimationGraphAssetSerializer::DeserializeFromAssetPack([[maybe_unused]] FileStreamReader& stream, [[maybe_unused]] const AssetPackFile::AssetInfo& assetInfo) const
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
        OLO_PROFILE_FUNCTION();

        Ref<SoundGraphAsset> soundGraphAsset = asset.As<SoundGraphAsset>();
        if (!soundGraphAsset)
        {
            OLO_CORE_ERROR("SoundGraphSerializer::Serialize - asset is not a SoundGraphAsset");
            return;
        }

        // Resolve absolute path by anchoring to project asset directory
        std::filesystem::path absolutePath = Project::GetAssetDirectory() / metadata.FilePath;

        std::filesystem::path parentDir = absolutePath.parent_path();
        if (!parentDir.empty())
        {
            std::error_code ec;
            if (!std::filesystem::create_directories(parentDir, ec) && ec)
            {
                OLO_CORE_ERROR("SoundGraphSerializer::Serialize - Failed to create parent directories for: {}, error: {}", absolutePath.string(), ec.message());
                return;
            }
        }

        if (!Audio::SoundGraph::SoundGraphSerializer::Serialize(*soundGraphAsset, absolutePath))
        {
            OLO_CORE_ERROR("SoundGraphSerializer::Serialize - Failed to serialize sound graph to file: {}", absolutePath.string());
            return;
        }
    }

    bool SoundGraphSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        Ref<SoundGraphAsset> soundGraphAsset = Ref<SoundGraphAsset>::Create();

        // Resolve absolute path by anchoring to project asset directory
        std::filesystem::path absolutePath = Project::GetAssetDirectory() / metadata.FilePath;

        if (!Audio::SoundGraph::SoundGraphSerializer::Deserialize(*soundGraphAsset, absolutePath))
        {
            OLO_CORE_ERROR("SoundGraphSerializer::TryLoadData - Failed to deserialize SoundGraph from '{}'", absolutePath.string());
            return false;
        }

        // Set the asset handle from metadata
        soundGraphAsset->SetHandle(metadata.Handle);

        asset = soundGraphAsset;
        return true;
    }

    bool SoundGraphSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        outInfo.Offset = stream.GetStreamPosition();

        // Get the SoundGraphAsset
        Ref<SoundGraphAsset> soundGraphAsset = AssetManager::GetAsset<SoundGraphAsset>(handle);
        if (!soundGraphAsset)
        {
            OLO_CORE_ERROR("SoundGraphSerializer::SerializeToAssetPack - Failed to get SoundGraphAsset for handle {}", handle);
            return false;
        }

        // Serialize the SoundGraphAsset to YAML string
        std::string yamlData = Audio::SoundGraph::SoundGraphSerializer::SerializeToString(*soundGraphAsset);

        // Write the YAML data as a string to the pack
        stream.WriteString(yamlData);

        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;

        OLO_CORE_TRACE("SoundGraphSerializer::SerializeToAssetPack - Serialized SoundGraph '{}' to pack, Size: {} bytes",
                       soundGraphAsset->GetName(), outInfo.Size);
        return true;
    }

    Ref<Asset> SoundGraphSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        // Set stream position to the asset's offset in the pack
        stream.SetStreamPosition(assetInfo.PackedOffset);

        // Read the YAML data as a string
        std::string yamlData;
        stream.ReadString(yamlData);

        // Deserialize from YAML string
        Ref<SoundGraphAsset> soundGraphAsset = Ref<SoundGraphAsset>::Create();
        if (!Audio::SoundGraph::SoundGraphSerializer::DeserializeFromString(*soundGraphAsset, yamlData))
        {
            OLO_CORE_ERROR("SoundGraphSerializer::DeserializeFromAssetPack - Failed to deserialize SoundGraph from pack");
            return nullptr;
        }

        // Set the handle
        soundGraphAsset->SetHandle(assetInfo.Handle);

        OLO_CORE_TRACE("SoundGraphSerializer::DeserializeFromAssetPack - Deserialized SoundGraph '{}' from pack, Handle: {}",
                       soundGraphAsset->GetName(), assetInfo.Handle);
        return soundGraphAsset;
    }

} // namespace OloEngine
