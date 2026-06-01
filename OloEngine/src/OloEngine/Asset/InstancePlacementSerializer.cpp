#include "OloEnginePCH.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/InstancePlacementAsset.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h" // AssetSerializer.h forward-declares Scene; pulling it in here resolves Ref<Scene>::~Ref instantiation triggered by template instantiation chains.

#include <fstream>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    namespace
    {
        // Per-instance YAML schema. Mirrors the inline form used by
        // SceneSerializer for InstancedMeshComponent so authoring tools can
        // produce either form interchangeably.
        void EmitInstance(YAML::Emitter& out, const InstanceData& inst)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Transform" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (sizet i = 0; i < 16; ++i)
                out << glm::value_ptr(inst.Transform)[i];
            out << YAML::EndSeq;
            out << YAML::Key << "Color" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << inst.Color.x << inst.Color.y << inst.Color.z << inst.Color.w << YAML::EndSeq;
            if (inst.EntityID != -1)
                out << YAML::Key << "EntityID" << YAML::Value << inst.EntityID;
            // Skip emitting Custom when it's exactly the default — bit-exact
            // sentinel comparison per cpp-coding-quality §2a.
            {
                constexpr f32 defaultCustom = 0.0f;
                if (!Math::BitwiseEqual(inst.Custom, defaultCustom))
                    out << YAML::Key << "Custom" << YAML::Value << inst.Custom;
            }
            out << YAML::EndMap;
        }

        bool ParseInstance(const YAML::Node& node, InstanceData& out)
        {
            if (auto t = node["Transform"]; t && t.IsSequence() && t.size() == 16)
            {
                for (sizet i = 0; i < 16; ++i)
                    glm::value_ptr(out.Transform)[i] = t[i].as<f32>();
            }
            else
            {
                return false; // Transform is mandatory; without it the entry is meaningless.
            }
            out.Normal = glm::transpose(glm::inverse(out.Transform));
            out.PrevTransform = out.Transform;
            if (auto c = node["Color"]; c && c.IsSequence() && c.size() == 4)
            {
                for (sizet i = 0; i < 4; ++i)
                    glm::value_ptr(out.Color)[i] = c[i].as<f32>();
            }
            if (node["EntityID"])
                out.EntityID = node["EntityID"].as<i32>();
            if (node["Custom"])
                out.Custom = node["Custom"].as<f32>();
            return true;
        }
    } // namespace

    void InstancePlacementSerializer::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto placement = asset.As<InstancePlacementAsset>();
        if (!placement)
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::Serialize - asset cast failed ({})", metadata.FilePath.string());
            return;
        }

        std::string yamlString = SerializeToYAML(placement);
        auto fullPath = Project::GetAssetDirectory() / metadata.FilePath;

        std::error_code ec;
        std::filesystem::create_directories(fullPath.parent_path(), ec);
        if (ec)
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::Serialize - mkdir failed ({}): {}", fullPath.string(), ec.message());
            return;
        }

        std::ofstream fout(fullPath);
        if (!fout.is_open())
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::Serialize - open-for-write failed ({})", fullPath.string());
            return;
        }
        fout << yamlString;
    }

    bool InstancePlacementSerializer::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        auto path = Project::GetAssetDirectory() / metadata.FilePath;
        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("InstancePlacementSerializer::TryLoadData - file missing ({})", path.string());
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::TryLoadData - open failed ({})", path.string());
            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();

        auto placement = Ref<InstancePlacementAsset>::Create();
        if (!DeserializeFromYAML(ss.str(), placement))
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::TryLoadData - YAML parse failed ({})", path.string());
            return false;
        }

        placement->SetHandle(metadata.Handle);
        asset = placement;
        return true;
    }

    bool InstancePlacementSerializer::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const
    {
        OLO_PROFILE_FUNCTION();

        auto placement = AssetManager::GetAsset<InstancePlacementAsset>(handle);
        if (!placement)
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::SerializeToAssetPack - get-asset failed ({})", static_cast<u64>(handle));
            return false;
        }
        std::string yamlString = SerializeToYAML(placement);
        outInfo.Offset = stream.GetStreamPosition();
        stream.WriteString(yamlString);
        outInfo.Size = stream.GetStreamPosition() - outInfo.Offset;
        return true;
    }

    Ref<Asset> InstancePlacementSerializer::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const
    {
        OLO_PROFILE_FUNCTION();

        stream.SetStreamPosition(assetInfo.PackedOffset);
        std::string yamlString;
        stream.ReadString(yamlString);

        auto placement = Ref<InstancePlacementAsset>::Create();
        if (!DeserializeFromYAML(yamlString, placement))
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::DeserializeFromAssetPack - YAML parse failed (handle: {})", static_cast<u64>(assetInfo.Handle));
            return nullptr;
        }
        placement->SetHandle(assetInfo.Handle);
        return placement;
    }

    std::string InstancePlacementSerializer::SerializeToYAML(const Ref<InstancePlacementAsset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "InstancePlacement" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Instances" << YAML::Value << YAML::BeginSeq;
        for (const auto& inst : asset->GetInstances())
            EmitInstance(out, inst);
        out << YAML::EndSeq;
        out << YAML::EndMap;
        out << YAML::EndMap;
        return std::string(out.c_str());
    }

    bool InstancePlacementSerializer::DeserializeFromYAML(const std::string& yamlString, Ref<InstancePlacementAsset>& asset) const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            YAML::Node root = YAML::Load(yamlString);
            auto section = root["InstancePlacement"];
            if (!section)
                return false;
            auto instances = section["Instances"];
            if (!instances || !instances.IsSequence())
                return true; // Empty placement is valid.
            asset->GetInstances().reserve(instances.size());
            for (const auto& node : instances)
            {
                InstanceData inst;
                if (ParseInstance(node, inst))
                    asset->AddInstance(inst);
            }
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("InstancePlacementSerializer::DeserializeFromYAML - {}", e.what());
            return false;
        }
    }
} // namespace OloEngine
