#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
	void LightProbeVolumeSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
	{
		auto probeVolume = asset.As<LightProbeVolumeAsset>();
		if (!probeVolume)
		{
			OLO_CORE_ERROR("LightProbeVolumeSerializer::Serialize - Invalid asset");
			return;
		}

		auto filepath = Project::GetAssetDirectory() / metadata.FilePath;

		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "LightProbeVolume" << YAML::Value;
		out << YAML::BeginMap;
		out << YAML::Key << "BoundsMin" << YAML::Value << probeVolume->BoundsMin;
		out << YAML::Key << "BoundsMax" << YAML::Value << probeVolume->BoundsMax;
		out << YAML::Key << "ResolutionX" << YAML::Value << probeVolume->Resolution.x;
		out << YAML::Key << "ResolutionY" << YAML::Value << probeVolume->Resolution.y;
		out << YAML::Key << "ResolutionZ" << YAML::Value << probeVolume->Resolution.z;
		out << YAML::Key << "Spacing" << YAML::Value << probeVolume->Spacing;
		out << YAML::Key << "ProbeCount" << YAML::Value << probeVolume->GetTotalProbeCount();
		out << YAML::Key << "CoefficientCount" << YAML::Value << static_cast<u64>(probeVolume->CoefficientData.size());
		out << YAML::EndMap;
		out << YAML::EndMap;

		std::string yamlStr = out.c_str();

		std::ofstream fout(filepath, std::ios::binary);
		if (!fout.is_open())
		{
			OLO_CORE_ERROR("Failed to open file for writing: {}", filepath.string());
			return;
		}

		// Write YAML length as u64, then YAML, then binary coefficient data
		auto yamlLen = static_cast<u64>(yamlStr.size());
		fout.write(reinterpret_cast<const char*>(&yamlLen), sizeof(u64));
		fout.write(yamlStr.data(), static_cast<std::streamsize>(yamlStr.size()));

		if (!probeVolume->CoefficientData.empty())
		{
			auto dataSize = static_cast<std::streamsize>(probeVolume->CoefficientData.size() * sizeof(glm::vec4));
			fout.write(reinterpret_cast<const char*>(probeVolume->CoefficientData.data()), dataSize);
		}

		fout.close();
		OLO_CORE_INFO("Serialized light probe volume: {}", filepath.string());
	}

	bool LightProbeVolumeSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
	{
		auto filepath = Project::GetAssetDirectory() / metadata.FilePath;

		std::ifstream fin(filepath, std::ios::binary);
		if (!fin.is_open())
		{
			OLO_CORE_ERROR("Failed to open light probe volume file: {}", filepath.string());
			return false;
		}

		// Read YAML length
		u64 yamlLen = 0;
		fin.read(reinterpret_cast<char*>(&yamlLen), sizeof(u64));
		if (!fin.good() || yamlLen == 0 || yamlLen > 1024 * 1024) // Sanity check: max 1MB YAML
		{
			OLO_CORE_ERROR("Invalid light probe volume file header: {}", filepath.string());
			return false;
		}

		// Read YAML
		std::string yamlStr(yamlLen, '\0');
		fin.read(yamlStr.data(), static_cast<std::streamsize>(yamlLen));
		if (!fin.good())
		{
			OLO_CORE_ERROR("Failed to read YAML from light probe volume file: {}", filepath.string());
			return false;
		}

		YAML::Node data = YAML::Load(yamlStr);
		auto volumeNode = data["LightProbeVolume"];
		if (!volumeNode)
		{
			OLO_CORE_ERROR("Invalid light probe volume YAML: {}", filepath.string());
			return false;
		}

		auto probeVolume = Ref<LightProbeVolumeAsset>::Create();
		probeVolume->BoundsMin = volumeNode["BoundsMin"].as<glm::vec3>(probeVolume->BoundsMin);
		probeVolume->BoundsMax = volumeNode["BoundsMax"].as<glm::vec3>(probeVolume->BoundsMax);
		probeVolume->Resolution.x = volumeNode["ResolutionX"].as<i32>(probeVolume->Resolution.x);
		probeVolume->Resolution.y = volumeNode["ResolutionY"].as<i32>(probeVolume->Resolution.y);
		probeVolume->Resolution.z = volumeNode["ResolutionZ"].as<i32>(probeVolume->Resolution.z);
		probeVolume->Spacing = volumeNode["Spacing"].as<f32>(probeVolume->Spacing);

		auto coeffCount = volumeNode["CoefficientCount"].as<u64>(0);
		if (coeffCount > 0)
		{
			probeVolume->CoefficientData.resize(static_cast<size_t>(coeffCount));
			auto dataSize = static_cast<std::streamsize>(coeffCount * sizeof(glm::vec4));
			fin.read(reinterpret_cast<char*>(probeVolume->CoefficientData.data()), dataSize);
			if (!fin.good())
			{
				OLO_CORE_ERROR("Failed to read coefficient data from: {}", filepath.string());
				return false;
			}
		}

		probeVolume->SetHandle(metadata.Handle);
		asset = probeVolume;
		return true;
	}

	bool LightProbeVolumeSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
	{
		auto asset = AssetManager::GetAsset<LightProbeVolumeAsset>(handle);
		if (!asset)
		{
			return false;
		}

		outInfo.Offset = stream.GetStreamPosition();

		// Write volume parameters
		stream.WriteData(reinterpret_cast<const char*>(&asset->BoundsMin), sizeof(glm::vec3));
		stream.WriteData(reinterpret_cast<const char*>(&asset->BoundsMax), sizeof(glm::vec3));
		stream.WriteData(reinterpret_cast<const char*>(&asset->Resolution), sizeof(glm::ivec3));
		stream.WriteData(reinterpret_cast<const char*>(&asset->Spacing), sizeof(f32));

		auto coeffCount = static_cast<u64>(asset->CoefficientData.size());
		stream.WriteData(reinterpret_cast<const char*>(&coeffCount), sizeof(u64));

		if (coeffCount > 0)
		{
			stream.WriteData(reinterpret_cast<const char*>(asset->CoefficientData.data()),
							 static_cast<size_t>(coeffCount * sizeof(glm::vec4)));
		}

		outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
		return true;
	}

	Ref<Asset> LightProbeVolumeSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
	{
		auto probeVolume = Ref<LightProbeVolumeAsset>::Create();

		stream.ReadData(reinterpret_cast<char*>(&probeVolume->BoundsMin), sizeof(glm::vec3));
		stream.ReadData(reinterpret_cast<char*>(&probeVolume->BoundsMax), sizeof(glm::vec3));
		stream.ReadData(reinterpret_cast<char*>(&probeVolume->Resolution), sizeof(glm::ivec3));
		stream.ReadData(reinterpret_cast<char*>(&probeVolume->Spacing), sizeof(f32));

		u64 coeffCount = 0;
		stream.ReadData(reinterpret_cast<char*>(&coeffCount), sizeof(u64));

		if (coeffCount > 0)
		{
			probeVolume->CoefficientData.resize(static_cast<size_t>(coeffCount));
			stream.ReadData(reinterpret_cast<char*>(probeVolume->CoefficientData.data()),
							static_cast<size_t>(coeffCount * sizeof(glm::vec4)));
		}

		probeVolume->SetHandle(assetInfo.Handle);
		return probeVolume;
	}

} // namespace OloEngine
