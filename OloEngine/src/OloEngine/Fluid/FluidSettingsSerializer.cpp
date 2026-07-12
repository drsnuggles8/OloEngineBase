#include "OloEnginePCH.h"
#include "OloEngine/Fluid/FluidSettingsSerializer.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Fluid/FluidSettings.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        constexpr i32 kFluidSettingsVersion = 1;

        /// Read a finite float clamped to [lo, hi]; keeps `current` when the
        /// key is missing or malformed (CLAMP semantics, not reject).
        void ReadClamped(const YAML::Node& node, f32& current, f32 lo, f32 hi)
        {
            if (!node)
            {
                return;
            }
            const f32 v = node.as<f32>(current);
            if (std::isfinite(v))
            {
                current = std::clamp(v, lo, hi);
            }
        }

        void ReadClampedU32(const YAML::Node& node, u32& current, u32 lo, u32 hi)
        {
            if (!node)
            {
                return;
            }
            const u32 v = node.as<u32>(current);
            current = std::clamp(v, lo, hi);
        }

        void ReadColor(const YAML::Node& node, glm::vec3& current)
        {
            if (!node || !node.IsSequence() || node.size() < 3)
            {
                return;
            }
            const glm::vec3 v{ node[0].as<f32>(current.x), node[1].as<f32>(current.y), node[2].as<f32>(current.z) };
            if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z))
            {
                current = glm::clamp(v, glm::vec3(0.0f), glm::vec3(10.0f));
            }
        }

        void EmitVec3(YAML::Emitter& out, const glm::vec3& v)
        {
            out << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
        }
    } // namespace

    std::string FluidSettingsSerializer::SerializeToString(const Ref<FluidSettings>& settings)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "FluidSettings" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Version" << YAML::Value << kFluidSettingsVersion;

        out << YAML::Key << "RestDensity" << YAML::Value << settings->m_RestDensity;
        out << YAML::Key << "ParticleRadius" << YAML::Value << settings->m_ParticleRadius;
        out << YAML::Key << "SmoothingRadiusScale" << YAML::Value << settings->m_SmoothingRadiusScale;
        out << YAML::Key << "SolverIterations" << YAML::Value << settings->m_SolverIterations;
        out << YAML::Key << "CfmEpsilon" << YAML::Value << settings->m_CfmEpsilon;
        out << YAML::Key << "SCorrK" << YAML::Value << settings->m_SCorrK;
        out << YAML::Key << "SCorrN" << YAML::Value << settings->m_SCorrN;
        out << YAML::Key << "SCorrDeltaQ" << YAML::Value << settings->m_SCorrDeltaQ;
        out << YAML::Key << "XsphViscosity" << YAML::Value << settings->m_XsphViscosity;
        out << YAML::Key << "VorticityEpsilon" << YAML::Value << settings->m_VorticityEpsilon;
        out << YAML::Key << "MaxSpeed" << YAML::Value << settings->m_MaxSpeed;
        out << YAML::Key << "GravityScale" << YAML::Value << settings->m_GravityScale;
        out << YAML::Key << "CouplingStiffness" << YAML::Value << settings->m_CouplingStiffness;

        out << YAML::Key << "Tint" << YAML::Value;
        EmitVec3(out, settings->m_Tint);
        out << YAML::Key << "AbsorptionColor" << YAML::Value;
        EmitVec3(out, settings->m_AbsorptionColor);
        out << YAML::Key << "AbsorptionScale" << YAML::Value << settings->m_AbsorptionScale;
        out << YAML::Key << "FoamVorticityThreshold" << YAML::Value << settings->m_FoamVorticityThreshold;

        out << YAML::EndMap;
        out << YAML::EndMap;
        return out.c_str();
    }

    Ref<FluidSettings> FluidSettingsSerializer::DeserializeFromString(const std::string& yamlString)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node root;
        try
        {
            root = YAML::Load(yamlString);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("FluidSettingsSerializer: YAML parse error: {}", e.what());
            return nullptr;
        }

        const YAML::Node node = root["FluidSettings"];
        if (!node || !node.IsMap())
        {
            OLO_CORE_ERROR("FluidSettingsSerializer: missing 'FluidSettings' root map");
            return nullptr;
        }

        const i32 version = node["Version"] ? node["Version"].as<i32>(0) : 0;
        if (version > kFluidSettingsVersion)
        {
            OLO_CORE_WARN("FluidSettingsSerializer: file version {} is newer than supported {}; "
                          "unknown fields will be ignored",
                          version, kFluidSettingsVersion);
        }

        Ref<FluidSettings> settings = Ref<FluidSettings>::Create();
        ReadClamped(node["RestDensity"], settings->m_RestDensity, 50.0f, 20000.0f);
        ReadClamped(node["ParticleRadius"], settings->m_ParticleRadius, 0.01f, 1.0f);
        ReadClamped(node["SmoothingRadiusScale"], settings->m_SmoothingRadiusScale, 1.2f, 4.0f);
        ReadClampedU32(node["SolverIterations"], settings->m_SolverIterations, 1u, 16u);
        ReadClamped(node["CfmEpsilon"], settings->m_CfmEpsilon, 1.0e-4f, 1.0e6f);
        ReadClamped(node["SCorrK"], settings->m_SCorrK, 0.0f, 1.0f);
        ReadClamped(node["SCorrN"], settings->m_SCorrN, 1.0f, 8.0f);
        ReadClamped(node["SCorrDeltaQ"], settings->m_SCorrDeltaQ, 0.0f, 0.99f);
        ReadClamped(node["XsphViscosity"], settings->m_XsphViscosity, 0.0f, 1.0f);
        ReadClamped(node["VorticityEpsilon"], settings->m_VorticityEpsilon, 0.0f, 5.0f);
        ReadClamped(node["MaxSpeed"], settings->m_MaxSpeed, 1.0f, 1000.0f);
        ReadClamped(node["GravityScale"], settings->m_GravityScale, -10.0f, 10.0f);
        ReadClamped(node["CouplingStiffness"], settings->m_CouplingStiffness, 0.0f, 10.0f);
        ReadColor(node["Tint"], settings->m_Tint);
        ReadColor(node["AbsorptionColor"], settings->m_AbsorptionColor);
        ReadClamped(node["AbsorptionScale"], settings->m_AbsorptionScale, 0.0f, 100.0f);
        ReadClamped(node["FoamVorticityThreshold"], settings->m_FoamVorticityThreshold, 0.0f, 100.0f);
        return settings;
    }

    bool FluidSettingsSerializer::Serialize(const Ref<FluidSettings>& settings, const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        if (!settings)
        {
            return false;
        }

        std::ofstream stream(filepath);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("FluidSettingsSerializer: cannot open '{}' for writing", filepath);
            return false;
        }
        stream << SerializeToString(settings);
        return stream.good();
    }

    Ref<FluidSettings> FluidSettingsSerializer::DeserializeAsset(const std::string& filepath)
    {
        OLO_PROFILE_FUNCTION();

        std::ifstream stream(filepath);
        if (!stream.is_open())
        {
            OLO_CORE_ERROR("FluidSettingsSerializer: cannot open '{}'", filepath);
            return nullptr;
        }
        std::stringstream buffer;
        buffer << stream.rdbuf();
        return DeserializeFromString(buffer.str());
    }
} // namespace OloEngine
