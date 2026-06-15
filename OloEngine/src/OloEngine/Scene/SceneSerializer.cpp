#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Core/YAMLConverters.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Particle/EmissionShapeUtils.h"
#include "OloEngine/Particle/ParticleCurveSerializer.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"

#include <fstream>
#include <cmath>

#include <glm/gtc/type_ptr.hpp>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
#define WRITE_SCRIPT_FIELD(FieldType, Type)  \
    case ScriptFieldType::FieldType:         \
        out << scriptField.GetValue<Type>(); \
        break

#define READ_SCRIPT_FIELD(FieldType, Type)               \
    case ScriptFieldType::FieldType:                     \
    {                                                    \
        Type fieldData = scriptField["Data"].as<Type>(); \
        fieldInstance.SetValue(fieldData);               \
        break;                                           \
    }

    template<typename T>
    inline T TrySet(T& value, const YAML::Node& node)
    {
        if (node)
        {
            value = node.as<T>(value);
        }
        return value;
    }

    template<typename T>
    inline T TrySetEnum(T& value, const YAML::Node& node)
    {
        if (node)
        {
            value = (T)node.as<int>((int)value);
        }
        return value;
    }

    // Load a texture referenced by scene YAML through the asset registry so
    // it participates in hot-reload and is de-duplicated across the scene
    // graph. Falls back to a raw Texture2D::Create() when no EditorAssetManager
    // is active (e.g. the runtime path loading a v0 scene with raw paths) --
    // matches the legacy behaviour exactly in that case.
    static Ref<Texture2D> LoadSceneTexture(const std::string& texPath)
    {
        if (texPath.empty())
            return nullptr;

        if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
        {
            // ImportAsset returns an AssetHandle, registers the path for hot-
            // reload watchers and dedupes repeated imports of the same file.
            // We then go through the standard AssetManager::GetAsset<T>(handle)
            // flow so the returned Ref participates in AssetReloadedEvent
            // invalidation.
            const AssetHandle imported = assetManager->ImportAsset(texPath);
            if (imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Texture2D)
                return AssetManager::GetAsset<Texture2D>(imported);
        }

        // Runtime path (no EditorAssetManager): legacy raw load. The runtime
        // asset manager works off handles, not paths, so a scene that still
        // carries raw paths can only be loaded via Texture2D::Create(). This
        // bypass is intentional and scoped to the runtime fallback.
        return Texture2D::Create(texPath);
    }

    // ---------- Sanitization helpers (shared across all Deserialize* functions) ----------

    /// Replace non-finite values with \p fallback, then clamp to [lo, hi].
    static void SanitizeFloat(f32& v, f32 lo, f32 hi, f32 fallback)
    {
        if (!std::isfinite(v))
        {
            v = fallback;
            return;
        }
        v = std::clamp(v, lo, hi);
    }

    /// Replace any vec3 that contains a non-finite component with \p fallback.
    static void SanitizeVec3(glm::vec3& v, const glm::vec3& fallback)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(v[i]))
            {
                v = fallback;
                return;
            }
        }
    }

    /// Replace any vec2 that contains a non-finite component with \p fallback.
    static void SanitizeVec2(glm::vec2& v, const glm::vec2& fallback)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (!std::isfinite(v[i]))
            {
                v = fallback;
                return;
            }
        }
    }

    /// Per-component: replace non-finite with fallback component, then clamp to [lo, hi].
    static void SanitizeVec3Clamped(glm::vec3& v, f32 lo, f32 hi, const glm::vec3& fallback)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(v[i]))
            {
                v[i] = fallback[i];
            }
            else
            {
                v[i] = std::clamp(v[i], lo, hi);
            }
        }
    }

    static Ref<Mesh> CreateMeshFromPrimitive(MeshPrimitive primitive)
    {
        switch (primitive)
        {
            case MeshPrimitive::Cube:
                return MeshPrimitives::CreateCube();
            case MeshPrimitive::Sphere:
                return MeshPrimitives::CreateSphere();
            case MeshPrimitive::Plane:
                return MeshPrimitives::CreatePlane();
            case MeshPrimitive::Cylinder:
                return MeshPrimitives::CreateCylinder();
            case MeshPrimitive::Cone:
                return MeshPrimitives::CreateCone();
            case MeshPrimitive::Icosphere:
                return MeshPrimitives::CreateIcosphere();
            case MeshPrimitive::Torus:
                return MeshPrimitives::CreateTorus();
            default:
                return nullptr;
        }
    }

    static void SerializeEffectList(YAML::Emitter& out, const char* key, const std::vector<GameplayEffect>& effects)
    {
        out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
        for (auto const& effect : effects)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << effect.Name;

            std::string durType = "Instant";
            if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::HasDuration)
                durType = "HasDuration";
            else if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::Infinite)
                durType = "Infinite";
            else
            {
                // No additional handling required.
            }
            out << YAML::Key << "DurationType" << YAML::Value << durType;
            out << YAML::Key << "DurationSeconds" << YAML::Value << effect.Policy.DurationSeconds;
            out << YAML::Key << "IsPeriodic" << YAML::Value << effect.Policy.IsPeriodic;
            out << YAML::Key << "PeriodSeconds" << YAML::Value << effect.Policy.PeriodSeconds;

            out << YAML::Key << "Modifiers" << YAML::Value << YAML::BeginSeq;
            for (auto const& mod : effect.Modifiers)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Attribute" << YAML::Value << mod.AttributeName;
                std::string op = "Add";
                if (mod.Op == AttributeModifier::Operation::Multiply)
                    op = "Multiply";
                else if (mod.Op == AttributeModifier::Operation::Override)
                    op = "Override";
                else
                {
                    // No additional handling required.
                }
                out << YAML::Key << "Operation" << YAML::Value << op;
                out << YAML::Key << "Magnitude" << YAML::Value << mod.Magnitude;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "MaxStacks" << YAML::Value << effect.MaxStacks;
            out << YAML::Key << "RefreshDurationOnStack" << YAML::Value << effect.RefreshDurationOnStack;

            out << YAML::Key << "GrantedTags" << YAML::Value << YAML::BeginSeq;
            for (auto const& t : effect.GrantedTags.GetTags())
            {
                out << t.GetTagString();
            }
            out << YAML::EndSeq;

            out << YAML::Key << "RequiredTags" << YAML::Value << YAML::BeginSeq;
            for (auto const& t : effect.RequiredTags.GetTags())
            {
                out << t.GetTagString();
            }
            out << YAML::EndSeq;

            out << YAML::Key << "BlockedTags" << YAML::Value << YAML::BeginSeq;
            for (auto const& t : effect.BlockedTags.GetTags())
            {
                out << t.GetTagString();
            }
            out << YAML::EndSeq;

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }

    static void DeserializeEffectList(const YAML::Node& node, const char* key, std::vector<GameplayEffect>& effects)
    {
        auto effectsNode = node[key];
        if (!effectsNode || !effectsNode.IsSequence())
            return;

        for (auto const& effectNode : effectsNode)
        {
            GameplayEffect ge;
            ge.Name = effectNode["Name"].as<std::string>("");

            if (std::string durType = effectNode["DurationType"].as<std::string>("Instant"); durType == "HasDuration")
                ge.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
            else if (durType == "Infinite")
                ge.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;
            else
            {
                // No additional handling required.
            }

            ge.Policy.DurationSeconds = effectNode["DurationSeconds"].as<f32>(0.0f);
            ge.Policy.IsPeriodic = effectNode["IsPeriodic"].as<bool>(false);
            ge.Policy.PeriodSeconds = effectNode["PeriodSeconds"].as<f32>(1.0f);
            SanitizeFloat(ge.Policy.DurationSeconds, 0.0f, 600.0f, 0.0f);
            SanitizeFloat(ge.Policy.PeriodSeconds, 0.01f, 60.0f, 1.0f);

            if (auto mods = effectNode["Modifiers"]; mods && mods.IsSequence())
            {
                for (auto const& modNode : mods)
                {
                    GameplayEffect::AttributeMod mod;
                    mod.AttributeName = modNode["Attribute"].as<std::string>("");
                    if (std::string op = modNode["Operation"].as<std::string>("Add"); op == "Multiply")
                        mod.Op = AttributeModifier::Operation::Multiply;
                    else if (op == "Override")
                        mod.Op = AttributeModifier::Operation::Override;
                    else
                    {
                        // No additional handling required.
                    }
                    mod.Magnitude = modNode["Magnitude"].as<f32>(0.0f);
                    ge.Modifiers.push_back(mod);
                }
            }

            ge.MaxStacks = effectNode["MaxStacks"].as<i32>(1);
            ge.RefreshDurationOnStack = effectNode["RefreshDurationOnStack"].as<bool>(true);

            if (auto grantedTags = effectNode["GrantedTags"]; grantedTags && grantedTags.IsSequence())
            {
                for (auto const& t : grantedTags)
                {
                    ge.GrantedTags.AddTag(GameplayTag(t.as<std::string>("")));
                }
            }
            if (auto reqTags = effectNode["RequiredTags"]; reqTags && reqTags.IsSequence())
            {
                for (auto const& t : reqTags)
                {
                    ge.RequiredTags.AddTag(GameplayTag(t.as<std::string>("")));
                }
            }
            if (auto blkTags = effectNode["BlockedTags"]; blkTags && blkTags.IsSequence())
            {
                for (auto const& t : blkTags)
                {
                    ge.BlockedTags.AddTag(GameplayTag(t.as<std::string>("")));
                }
            }

            effects.push_back(std::move(ge));
        }
    }

    static void SerializeSnowSettings(YAML::Emitter& out, const SnowSettings& snow)
    {
        out << YAML::Key << "SnowSettings";
        out << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << snow.Enabled;
        out << YAML::Key << "HeightStart" << YAML::Value << snow.HeightStart;
        out << YAML::Key << "HeightFull" << YAML::Value << snow.HeightFull;
        out << YAML::Key << "SlopeStart" << YAML::Value << snow.SlopeStart;
        out << YAML::Key << "SlopeFull" << YAML::Value << snow.SlopeFull;
        out << YAML::Key << "Albedo" << YAML::Value << snow.Albedo;
        out << YAML::Key << "Roughness" << YAML::Value << snow.Roughness;
        out << YAML::Key << "SSSColor" << YAML::Value << snow.SSSColor;
        out << YAML::Key << "SSSIntensity" << YAML::Value << snow.SSSIntensity;
        out << YAML::Key << "SparkleIntensity" << YAML::Value << snow.SparkleIntensity;
        out << YAML::Key << "SparkleDensity" << YAML::Value << snow.SparkleDensity;
        out << YAML::Key << "SparkleScale" << YAML::Value << snow.SparkleScale;
        out << YAML::Key << "NormalPerturbStrength" << YAML::Value << snow.NormalPerturbStrength;
        out << YAML::Key << "WindDriftFactor" << YAML::Value << snow.WindDriftFactor;
        out << YAML::Key << "SSSBlurEnabled" << YAML::Value << snow.SSSBlurEnabled;
        out << YAML::Key << "SSSBlurRadius" << YAML::Value << snow.SSSBlurRadius;
        out << YAML::Key << "SSSBlurFalloff" << YAML::Value << snow.SSSBlurFalloff;
        out << YAML::EndMap;
    }

    static void DeserializeSnowSettings(const YAML::Node& data, SnowSettings& snow)
    {
        if (auto snowNode = data["SnowSettings"]; snowNode)
        {
            TrySet(snow.Enabled, snowNode["Enabled"]);
            TrySet(snow.HeightStart, snowNode["HeightStart"]);
            TrySet(snow.HeightFull, snowNode["HeightFull"]);
            TrySet(snow.SlopeStart, snowNode["SlopeStart"]);
            TrySet(snow.SlopeFull, snowNode["SlopeFull"]);
            TrySet(snow.Albedo, snowNode["Albedo"]);
            TrySet(snow.Roughness, snowNode["Roughness"]);
            TrySet(snow.SSSColor, snowNode["SSSColor"]);
            TrySet(snow.SSSIntensity, snowNode["SSSIntensity"]);
            TrySet(snow.SparkleIntensity, snowNode["SparkleIntensity"]);
            TrySet(snow.SparkleDensity, snowNode["SparkleDensity"]);
            TrySet(snow.SparkleScale, snowNode["SparkleScale"]);
            TrySet(snow.NormalPerturbStrength, snowNode["NormalPerturbStrength"]);
            TrySet(snow.WindDriftFactor, snowNode["WindDriftFactor"]);
            TrySet(snow.SSSBlurEnabled, snowNode["SSSBlurEnabled"]);
            TrySet(snow.SSSBlurRadius, snowNode["SSSBlurRadius"]);
            TrySet(snow.SSSBlurFalloff, snowNode["SSSBlurFalloff"]);
        }
    }

    static void SerializeFogSettings(YAML::Emitter& out, const FogSettings& fog)
    {
        OLO_PROFILE_FUNCTION();

        out << YAML::Key << "FogSettings";
        out << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << fog.Enabled;
        out << YAML::Key << "Mode" << YAML::Value << std::to_underlying(fog.Mode);
        out << YAML::Key << "Color" << YAML::Value << fog.Color;
        out << YAML::Key << "Density" << YAML::Value << fog.Density;
        out << YAML::Key << "Start" << YAML::Value << fog.Start;
        out << YAML::Key << "End" << YAML::Value << fog.End;
        out << YAML::Key << "HeightFalloff" << YAML::Value << fog.HeightFalloff;
        out << YAML::Key << "HeightOffset" << YAML::Value << fog.HeightOffset;
        out << YAML::Key << "MaxOpacity" << YAML::Value << fog.MaxOpacity;
        out << YAML::Key << "EnableScattering" << YAML::Value << fog.EnableScattering;
        out << YAML::Key << "RayleighStrength" << YAML::Value << fog.RayleighStrength;
        out << YAML::Key << "MieStrength" << YAML::Value << fog.MieStrength;
        out << YAML::Key << "MieDirectionality" << YAML::Value << fog.MieDirectionality;
        out << YAML::Key << "RayleighColor" << YAML::Value << fog.RayleighColor;
        out << YAML::Key << "SunIntensity" << YAML::Value << fog.SunIntensity;
        out << YAML::Key << "EnableVolumetric" << YAML::Value << fog.EnableVolumetric;
        out << YAML::Key << "VolumetricSamples" << YAML::Value << fog.VolumetricSamples;
        out << YAML::Key << "AbsorptionCoefficient" << YAML::Value << fog.AbsorptionCoefficient;
        out << YAML::Key << "EnableNoise" << YAML::Value << fog.EnableNoise;
        out << YAML::Key << "NoiseScale" << YAML::Value << fog.NoiseScale;
        out << YAML::Key << "NoiseSpeed" << YAML::Value << fog.NoiseSpeed;
        out << YAML::Key << "NoiseIntensity" << YAML::Value << fog.NoiseIntensity;
        out << YAML::Key << "EnableLightShafts" << YAML::Value << fog.EnableLightShafts;
        out << YAML::Key << "LightShaftIntensity" << YAML::Value << fog.LightShaftIntensity;
        out << YAML::EndMap;
    }

    static void DeserializeFogSettings(const YAML::Node& data, FogSettings& fog)
    {
        OLO_PROFILE_FUNCTION();

        if (auto fogNode = data["FogSettings"]; fogNode)
        {
            TrySet(fog.Enabled, fogNode["Enabled"]);
            i32 mode = std::to_underlying(fog.Mode);
            TrySet(mode, fogNode["Mode"]);
            mode = std::clamp(mode, std::to_underlying(FogMode::Linear), std::to_underlying(FogMode::ExponentialSquared));
            fog.Mode = static_cast<FogMode>(mode);
            TrySet(fog.Color, fogNode["Color"]);
            TrySet(fog.Density, fogNode["Density"]);
            TrySet(fog.Start, fogNode["Start"]);
            TrySet(fog.End, fogNode["End"]);
            TrySet(fog.HeightFalloff, fogNode["HeightFalloff"]);
            TrySet(fog.HeightOffset, fogNode["HeightOffset"]);
            TrySet(fog.MaxOpacity, fogNode["MaxOpacity"]);
            TrySet(fog.EnableScattering, fogNode["EnableScattering"]);
            TrySet(fog.RayleighStrength, fogNode["RayleighStrength"]);
            TrySet(fog.MieStrength, fogNode["MieStrength"]);
            TrySet(fog.MieDirectionality, fogNode["MieDirectionality"]);
            TrySet(fog.RayleighColor, fogNode["RayleighColor"]);
            TrySet(fog.SunIntensity, fogNode["SunIntensity"]);
            TrySet(fog.EnableVolumetric, fogNode["EnableVolumetric"]);
            TrySet(fog.VolumetricSamples, fogNode["VolumetricSamples"]);
            TrySet(fog.AbsorptionCoefficient, fogNode["AbsorptionCoefficient"]);
            TrySet(fog.EnableNoise, fogNode["EnableNoise"]);
            TrySet(fog.NoiseScale, fogNode["NoiseScale"]);
            TrySet(fog.NoiseSpeed, fogNode["NoiseSpeed"]);
            TrySet(fog.NoiseIntensity, fogNode["NoiseIntensity"]);
            TrySet(fog.EnableLightShafts, fogNode["EnableLightShafts"]);
            TrySet(fog.LightShaftIntensity, fogNode["LightShaftIntensity"]);

            // Validate and sanitize deserialized fog parameters
            SanitizeFloat(fog.Density, 0.0f, 10.0f, 0.02f);
            SanitizeFloat(fog.Start, 0.0f, 1e6f, 10.0f);
            SanitizeFloat(fog.End, 0.0f, 1e6f, 300.0f);
            if (fog.Start >= fog.End)
            {
                fog.End = fog.Start + 1.0f;
            }
            SanitizeFloat(fog.HeightFalloff, 0.0f, 100.0f, 0.1f);
            SanitizeFloat(fog.HeightOffset, -1e5f, 1e5f, 0.0f);
            SanitizeFloat(fog.MaxOpacity, 0.0f, 1.0f, 1.0f);
            SanitizeFloat(fog.RayleighStrength, 0.0f, 100.0f, 1.0f);
            SanitizeFloat(fog.MieStrength, 0.0f, 10.0f, 0.005f);
            SanitizeFloat(fog.MieDirectionality, -1.0f, 1.0f, 0.76f);
            SanitizeFloat(fog.SunIntensity, 0.0f, 1000.0f, 22.0f);
            SanitizeFloat(fog.AbsorptionCoefficient, 0.0f, 10.0f, 0.02f);
            SanitizeFloat(fog.NoiseScale, 0.0f, 100.0f, 0.01f);
            SanitizeFloat(fog.NoiseSpeed, 0.0f, 100.0f, 0.1f);
            SanitizeFloat(fog.NoiseIntensity, 0.0f, 10.0f, 0.3f);
            SanitizeFloat(fog.LightShaftIntensity, 0.0f, 100.0f, 1.0f);
            fog.VolumetricSamples = std::clamp(fog.VolumetricSamples, 4, 128);
            SanitizeVec3(fog.Color, glm::vec3(0.5f, 0.6f, 0.7f));
            SanitizeVec3(fog.RayleighColor, glm::vec3(0.27f, 0.51f, 0.83f));
        }
    }

    static void SerializeWindSettings(YAML::Emitter& out, const WindSettings& wind)
    {
        OLO_PROFILE_FUNCTION();

        out << YAML::Key << "WindSettings";
        out << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << wind.Enabled;
        out << YAML::Key << "Direction" << YAML::Value << wind.Direction;
        out << YAML::Key << "Speed" << YAML::Value << wind.Speed;
        out << YAML::Key << "GustStrength" << YAML::Value << wind.GustStrength;
        out << YAML::Key << "GustFrequency" << YAML::Value << wind.GustFrequency;
        out << YAML::Key << "TurbulenceIntensity" << YAML::Value << wind.TurbulenceIntensity;
        out << YAML::Key << "TurbulenceScale" << YAML::Value << wind.TurbulenceScale;
        out << YAML::Key << "GridWorldSize" << YAML::Value << wind.GridWorldSize;
        out << YAML::Key << "GridResolution" << YAML::Value << wind.GridResolution;
        out << YAML::EndMap;
    }

    static void DeserializeWindSettings(const YAML::Node& data, WindSettings& wind)
    {
        OLO_PROFILE_FUNCTION();

        if (auto windNode = data["WindSettings"]; windNode)
        {
            TrySet(wind.Enabled, windNode["Enabled"]);
            TrySet(wind.Direction, windNode["Direction"]);
            TrySet(wind.Speed, windNode["Speed"]);
            TrySet(wind.GustStrength, windNode["GustStrength"]);
            TrySet(wind.GustFrequency, windNode["GustFrequency"]);
            TrySet(wind.TurbulenceIntensity, windNode["TurbulenceIntensity"]);
            TrySet(wind.TurbulenceScale, windNode["TurbulenceScale"]);
            TrySet(wind.GridWorldSize, windNode["GridWorldSize"]);
            TrySet(wind.GridResolution, windNode["GridResolution"]);

            // Validate and clamp wind scalar fields
            SanitizeFloat(wind.Speed, 0.0f, 1e4f, 0.0f);
            SanitizeFloat(wind.GustStrength, 0.0f, 1e4f, 0.0f);
            SanitizeFloat(wind.GustFrequency, 0.0f, 1e3f, 0.0f);
            SanitizeFloat(wind.TurbulenceIntensity, 0.0f, 1.0f, 0.0f);
            SanitizeFloat(wind.TurbulenceScale, 1e-6f, 1e6f, 1.0f);

            // Clamp grid bounds
            wind.GridWorldSize = std::clamp(wind.GridWorldSize, 0.1f, 10000.0f);

            // Normalize GridResolution to supported values {64, 128}
            if (wind.GridResolution <= 96)
                wind.GridResolution = 64;
            else
                wind.GridResolution = 128;

            // Validate direction: reject NaN/Inf/zero, normalize to unit length
            bool dirInvalid = !std::isfinite(wind.Direction.x) || !std::isfinite(wind.Direction.y) || !std::isfinite(wind.Direction.z);
            if (!dirInvalid)
            {
                f32 len2 = glm::dot(wind.Direction, wind.Direction);
                dirInvalid = !std::isfinite(len2) || (len2 < 1e-8f);
            }
            if (dirInvalid)
            {
                wind.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            else
            {
                wind.Direction = glm::normalize(wind.Direction);
            }
        }
    }

    static void SerializeSnowAccumulationSettings(YAML::Emitter& out, const SnowAccumulationSettings& sa)
    {
        OLO_PROFILE_FUNCTION();

        out << YAML::Key << "SnowAccumulationSettings";
        out << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << sa.Enabled;
        out << YAML::Key << "AccumulationRate" << YAML::Value << sa.AccumulationRate;
        out << YAML::Key << "MaxDepth" << YAML::Value << sa.MaxDepth;
        out << YAML::Key << "MeltRate" << YAML::Value << sa.MeltRate;
        out << YAML::Key << "RestorationRate" << YAML::Value << sa.RestorationRate;
        out << YAML::Key << "DisplacementScale" << YAML::Value << sa.DisplacementScale;
        out << YAML::Key << "ClipmapResolution" << YAML::Value << sa.ClipmapResolution;
        out << YAML::Key << "ClipmapExtent" << YAML::Value << sa.ClipmapExtent;
        out << YAML::Key << "NumClipmapRings" << YAML::Value << sa.NumClipmapRings;
        out << YAML::Key << "SnowDensity" << YAML::Value << sa.SnowDensity;
        out << YAML::EndMap;
    }

    static void DeserializeSnowAccumulationSettings(const YAML::Node& data, SnowAccumulationSettings& sa)
    {
        OLO_PROFILE_FUNCTION();

        if (auto saNode = data["SnowAccumulationSettings"]; saNode)
        {
            TrySet(sa.Enabled, saNode["Enabled"]);
            TrySet(sa.AccumulationRate, saNode["AccumulationRate"]);
            TrySet(sa.MaxDepth, saNode["MaxDepth"]);
            TrySet(sa.MeltRate, saNode["MeltRate"]);
            TrySet(sa.RestorationRate, saNode["RestorationRate"]);
            TrySet(sa.DisplacementScale, saNode["DisplacementScale"]);
            TrySet(sa.ClipmapResolution, saNode["ClipmapResolution"]);
            TrySet(sa.ClipmapExtent, saNode["ClipmapExtent"]);
            TrySet(sa.NumClipmapRings, saNode["NumClipmapRings"]);
            TrySet(sa.SnowDensity, saNode["SnowDensity"]);

            // Validate — sanitize NaN/Inf then clamp
            SanitizeFloat(sa.AccumulationRate, 0.0f, 10.0f, 0.02f);
            SanitizeFloat(sa.MaxDepth, 0.01f, 10.0f, 0.5f);
            SanitizeFloat(sa.MeltRate, 0.0f, 10.0f, 0.005f);
            SanitizeFloat(sa.RestorationRate, 0.0f, 10.0f, 0.01f);
            SanitizeFloat(sa.DisplacementScale, 0.0f, 10.0f, 1.0f);
            SanitizeFloat(sa.ClipmapExtent, 1.0f, 1000.0f, 128.0f);
            SanitizeFloat(sa.SnowDensity, 0.0f, 1.0f, 0.3f);
            sa.ClipmapResolution = std::clamp(sa.ClipmapResolution, 256u, 4096u);
            sa.NumClipmapRings = std::clamp(sa.NumClipmapRings, 1u, 3u);
        }
    }

    static void SerializeSnowEjectaSettings(YAML::Emitter& out, const SnowEjectaSettings& se)
    {
        OLO_PROFILE_FUNCTION();

        out << YAML::Key << "SnowEjectaSettings";
        out << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << se.Enabled;
        out << YAML::Key << "ParticlesPerDeform" << YAML::Value << se.ParticlesPerDeform;
        out << YAML::Key << "EjectaSpeed" << YAML::Value << se.EjectaSpeed;
        out << YAML::Key << "SpeedVariance" << YAML::Value << se.SpeedVariance;
        out << YAML::Key << "UpwardBias" << YAML::Value << se.UpwardBias;
        out << YAML::Key << "LifetimeMin" << YAML::Value << se.LifetimeMin;
        out << YAML::Key << "LifetimeMax" << YAML::Value << se.LifetimeMax;
        out << YAML::Key << "InitialSize" << YAML::Value << se.InitialSize;
        out << YAML::Key << "SizeVariance" << YAML::Value << se.SizeVariance;
        out << YAML::Key << "GravityScale" << YAML::Value << se.GravityScale;
        out << YAML::Key << "DragCoefficient" << YAML::Value << se.DragCoefficient;
        out << YAML::Key << "Color" << YAML::Value << se.Color;
        out << YAML::Key << "VelocityThreshold" << YAML::Value << se.VelocityThreshold;
        out << YAML::Key << "MaxParticles" << YAML::Value << se.MaxParticles;
        out << YAML::Key << "WindInfluence" << YAML::Value << se.WindInfluence;
        out << YAML::Key << "NoiseStrength" << YAML::Value << se.NoiseStrength;
        out << YAML::Key << "NoiseFrequency" << YAML::Value << se.NoiseFrequency;
        out << YAML::Key << "GroundY" << YAML::Value << se.GroundY;
        out << YAML::Key << "CollisionBounce" << YAML::Value << se.CollisionBounce;
        out << YAML::Key << "CollisionFriction" << YAML::Value << se.CollisionFriction;
        out << YAML::EndMap;
    }

    static void DeserializeSnowEjectaSettings(const YAML::Node& data, SnowEjectaSettings& se)
    {
        OLO_PROFILE_FUNCTION();

        if (auto seNode = data["SnowEjectaSettings"]; seNode)
        {
            TrySet(se.Enabled, seNode["Enabled"]);
            TrySet(se.ParticlesPerDeform, seNode["ParticlesPerDeform"]);
            TrySet(se.EjectaSpeed, seNode["EjectaSpeed"]);
            TrySet(se.SpeedVariance, seNode["SpeedVariance"]);
            TrySet(se.UpwardBias, seNode["UpwardBias"]);
            TrySet(se.LifetimeMin, seNode["LifetimeMin"]);
            TrySet(se.LifetimeMax, seNode["LifetimeMax"]);
            TrySet(se.InitialSize, seNode["InitialSize"]);
            TrySet(se.SizeVariance, seNode["SizeVariance"]);
            TrySet(se.GravityScale, seNode["GravityScale"]);
            TrySet(se.DragCoefficient, seNode["DragCoefficient"]);
            TrySet(se.Color, seNode["Color"]);
            TrySet(se.VelocityThreshold, seNode["VelocityThreshold"]);
            TrySet(se.MaxParticles, seNode["MaxParticles"]);
            TrySet(se.WindInfluence, seNode["WindInfluence"]);
            TrySet(se.NoiseStrength, seNode["NoiseStrength"]);
            TrySet(se.NoiseFrequency, seNode["NoiseFrequency"]);
            TrySet(se.GroundY, seNode["GroundY"]);
            TrySet(se.CollisionBounce, seNode["CollisionBounce"]);
            TrySet(se.CollisionFriction, seNode["CollisionFriction"]);

            // Validate — sanitize NaN/Inf then clamp
            se.ParticlesPerDeform = std::clamp(se.ParticlesPerDeform, 1u, 128u);
            SanitizeFloat(se.EjectaSpeed, 0.0f, 50.0f, 2.5f);
            SanitizeFloat(se.SpeedVariance, 0.0f, 1.0f, 0.8f);
            SanitizeFloat(se.UpwardBias, 0.0f, 1.0f, 0.6f);
            SanitizeFloat(se.LifetimeMin, 0.01f, 10.0f, 0.4f);
            SanitizeFloat(se.LifetimeMax, se.LifetimeMin, 10.0f, std::max(se.LifetimeMin, 1.2f));
            SanitizeFloat(se.InitialSize, 0.001f, 1.0f, 0.04f);
            SanitizeFloat(se.SizeVariance, 0.0f, 0.5f, 0.02f);
            SanitizeFloat(se.GravityScale, 0.0f, 5.0f, 0.3f);
            SanitizeFloat(se.DragCoefficient, 0.0f, 20.0f, 2.0f);
            SanitizeFloat(se.VelocityThreshold, 0.0f, 10.0f, 0.1f);
            se.MaxParticles = std::clamp(se.MaxParticles, 256u, 65536u);
            SanitizeFloat(se.WindInfluence, 0.0f, 1.0f, 0.5f);
            SanitizeFloat(se.NoiseStrength, 0.0f, 5.0f, 0.3f);
            SanitizeFloat(se.NoiseFrequency, 0.0f, 20.0f, 2.0f);
            SanitizeFloat(se.GroundY, -1000.0f, 1000.0f, 0.0f);
            SanitizeFloat(se.CollisionBounce, 0.0f, 1.0f, 0.0f);
            SanitizeFloat(se.CollisionFriction, 0.0f, 1.0f, 1.0f);
        }
    }

    static void SerializePrecipitationSettings(YAML::Emitter& out, const PrecipitationSettings& ps)
    {
        OLO_PROFILE_FUNCTION();

        out << YAML::Key << "PrecipitationSettings";
        out << YAML::BeginMap;
        out << YAML::Key << "Enabled" << YAML::Value << ps.Enabled;
        out << YAML::Key << "Type" << YAML::Value << std::to_underlying(ps.Type);
        out << YAML::Key << "Intensity" << YAML::Value << ps.Intensity;
        out << YAML::Key << "TransitionSpeed" << YAML::Value << ps.TransitionSpeed;
        out << YAML::Key << "BaseEmissionRate" << YAML::Value << ps.BaseEmissionRate;
        out << YAML::Key << "MaxParticlesNearField" << YAML::Value << ps.MaxParticlesNearField;
        out << YAML::Key << "MaxParticlesFarField" << YAML::Value << ps.MaxParticlesFarField;
        out << YAML::Key << "NearFieldExtent" << YAML::Value << ps.NearFieldExtent;
        out << YAML::Key << "NearFieldParticleSize" << YAML::Value << ps.NearFieldParticleSize;
        out << YAML::Key << "NearFieldSizeVariance" << YAML::Value << ps.NearFieldSizeVariance;
        out << YAML::Key << "NearFieldSpeedMin" << YAML::Value << ps.NearFieldSpeedMin;
        out << YAML::Key << "NearFieldSpeedMax" << YAML::Value << ps.NearFieldSpeedMax;
        out << YAML::Key << "NearFieldLifetime" << YAML::Value << ps.NearFieldLifetime;
        out << YAML::Key << "FarFieldExtent" << YAML::Value << ps.FarFieldExtent;
        out << YAML::Key << "FarFieldParticleSize" << YAML::Value << ps.FarFieldParticleSize;
        out << YAML::Key << "FarFieldSpeedMin" << YAML::Value << ps.FarFieldSpeedMin;
        out << YAML::Key << "FarFieldSpeedMax" << YAML::Value << ps.FarFieldSpeedMax;
        out << YAML::Key << "FarFieldLifetime" << YAML::Value << ps.FarFieldLifetime;
        out << YAML::Key << "FarFieldAlphaMultiplier" << YAML::Value << ps.FarFieldAlphaMultiplier;
        out << YAML::Key << "GravityScale" << YAML::Value << ps.GravityScale;
        out << YAML::Key << "WindInfluence" << YAML::Value << ps.WindInfluence;
        out << YAML::Key << "DragCoefficient" << YAML::Value << ps.DragCoefficient;
        out << YAML::Key << "TurbulenceStrength" << YAML::Value << ps.TurbulenceStrength;
        out << YAML::Key << "TurbulenceFrequency" << YAML::Value << ps.TurbulenceFrequency;
        out << YAML::Key << "GroundCollisionEnabled" << YAML::Value << ps.GroundCollisionEnabled;
        out << YAML::Key << "GroundY" << YAML::Value << ps.GroundY;
        out << YAML::Key << "CollisionBounce" << YAML::Value << ps.CollisionBounce;
        out << YAML::Key << "CollisionFriction" << YAML::Value << ps.CollisionFriction;
        out << YAML::Key << "FeedAccumulation" << YAML::Value << ps.FeedAccumulation;
        out << YAML::Key << "AccumulationFeedRate" << YAML::Value << ps.AccumulationFeedRate;
        out << YAML::Key << "ScreenStreaksEnabled" << YAML::Value << ps.ScreenStreaksEnabled;
        out << YAML::Key << "ScreenStreakIntensity" << YAML::Value << ps.ScreenStreakIntensity;
        out << YAML::Key << "ScreenStreakLength" << YAML::Value << ps.ScreenStreakLength;
        out << YAML::Key << "LensImpactsEnabled" << YAML::Value << ps.LensImpactsEnabled;
        out << YAML::Key << "LensImpactRate" << YAML::Value << ps.LensImpactRate;
        out << YAML::Key << "LensImpactLifetime" << YAML::Value << ps.LensImpactLifetime;
        out << YAML::Key << "LensImpactSize" << YAML::Value << ps.LensImpactSize;
        out << YAML::Key << "LODNearDistance" << YAML::Value << ps.LODNearDistance;
        out << YAML::Key << "LODFarDistance" << YAML::Value << ps.LODFarDistance;
        out << YAML::Key << "FrameBudgetMs" << YAML::Value << ps.FrameBudgetMs;
        out << YAML::Key << "ParticleColor" << YAML::Value << ps.ParticleColor;
        out << YAML::Key << "ColorVariance" << YAML::Value << ps.ColorVariance;
        out << YAML::Key << "RotationSpeed" << YAML::Value << ps.RotationSpeed;
        out << YAML::EndMap;
    }

    static void DeserializePrecipitationSettings(const YAML::Node& data, PrecipitationSettings& ps)
    {
        OLO_PROFILE_FUNCTION();

        if (auto psNode = data["PrecipitationSettings"]; psNode)
        {
            TrySet(ps.Enabled, psNode["Enabled"]);
            if (auto typeNode = psNode["Type"]; typeNode)
            {
                auto typeVal = typeNode.as<i32>(std::to_underlying(PrecipitationType::Snow));
                if (typeVal >= 0 && typeVal <= std::to_underlying(PrecipitationType::Sleet))
                {
                    ps.Type = static_cast<PrecipitationType>(typeVal);
                }
                else
                {
                    OLO_CORE_WARN("PrecipitationSettings: invalid Type value {}, falling back to Snow", typeVal);
                    ps.Type = PrecipitationType::Snow;
                }
            }
            TrySet(ps.Intensity, psNode["Intensity"]);
            TrySet(ps.TransitionSpeed, psNode["TransitionSpeed"]);
            TrySet(ps.BaseEmissionRate, psNode["BaseEmissionRate"]);
            TrySet(ps.MaxParticlesNearField, psNode["MaxParticlesNearField"]);
            TrySet(ps.MaxParticlesFarField, psNode["MaxParticlesFarField"]);
            TrySet(ps.NearFieldExtent, psNode["NearFieldExtent"]);
            TrySet(ps.NearFieldParticleSize, psNode["NearFieldParticleSize"]);
            TrySet(ps.NearFieldSizeVariance, psNode["NearFieldSizeVariance"]);
            TrySet(ps.NearFieldSpeedMin, psNode["NearFieldSpeedMin"]);
            TrySet(ps.NearFieldSpeedMax, psNode["NearFieldSpeedMax"]);
            TrySet(ps.NearFieldLifetime, psNode["NearFieldLifetime"]);
            TrySet(ps.FarFieldExtent, psNode["FarFieldExtent"]);
            TrySet(ps.FarFieldParticleSize, psNode["FarFieldParticleSize"]);
            TrySet(ps.FarFieldSpeedMin, psNode["FarFieldSpeedMin"]);
            TrySet(ps.FarFieldSpeedMax, psNode["FarFieldSpeedMax"]);
            TrySet(ps.FarFieldLifetime, psNode["FarFieldLifetime"]);
            TrySet(ps.FarFieldAlphaMultiplier, psNode["FarFieldAlphaMultiplier"]);
            TrySet(ps.GravityScale, psNode["GravityScale"]);
            TrySet(ps.WindInfluence, psNode["WindInfluence"]);
            TrySet(ps.DragCoefficient, psNode["DragCoefficient"]);
            TrySet(ps.TurbulenceStrength, psNode["TurbulenceStrength"]);
            TrySet(ps.TurbulenceFrequency, psNode["TurbulenceFrequency"]);
            TrySet(ps.GroundCollisionEnabled, psNode["GroundCollisionEnabled"]);
            TrySet(ps.GroundY, psNode["GroundY"]);
            TrySet(ps.CollisionBounce, psNode["CollisionBounce"]);
            TrySet(ps.CollisionFriction, psNode["CollisionFriction"]);
            TrySet(ps.FeedAccumulation, psNode["FeedAccumulation"]);
            TrySet(ps.AccumulationFeedRate, psNode["AccumulationFeedRate"]);
            TrySet(ps.ScreenStreaksEnabled, psNode["ScreenStreaksEnabled"]);
            TrySet(ps.ScreenStreakIntensity, psNode["ScreenStreakIntensity"]);
            TrySet(ps.ScreenStreakLength, psNode["ScreenStreakLength"]);
            TrySet(ps.LensImpactsEnabled, psNode["LensImpactsEnabled"]);
            TrySet(ps.LensImpactRate, psNode["LensImpactRate"]);
            TrySet(ps.LensImpactLifetime, psNode["LensImpactLifetime"]);
            TrySet(ps.LensImpactSize, psNode["LensImpactSize"]);
            TrySet(ps.LODNearDistance, psNode["LODNearDistance"]);
            TrySet(ps.LODFarDistance, psNode["LODFarDistance"]);
            TrySet(ps.FrameBudgetMs, psNode["FrameBudgetMs"]);
            TrySet(ps.ParticleColor, psNode["ParticleColor"]);
            TrySet(ps.ColorVariance, psNode["ColorVariance"]);
            TrySet(ps.RotationSpeed, psNode["RotationSpeed"]);

            // Validate
            SanitizeFloat(ps.Intensity, 0.0f, 1.0f, 0.5f);
            SanitizeFloat(ps.TransitionSpeed, 0.01f, 10.0f, 0.5f);
            ps.BaseEmissionRate = std::clamp(ps.BaseEmissionRate, 100u, 50000u);
            ps.MaxParticlesNearField = std::clamp(ps.MaxParticlesNearField, 1000u, 500000u);
            ps.MaxParticlesFarField = std::clamp(ps.MaxParticlesFarField, 1000u, 1000000u);
            SanitizeFloat(ps.NearFieldParticleSize, 0.001f, 0.5f, 0.03f);
            SanitizeFloat(ps.FarFieldParticleSize, 0.001f, 0.3f, 0.015f);
            SanitizeFloat(ps.GravityScale, 0.0f, 5.0f, 0.3f);
            SanitizeFloat(ps.WindInfluence, 0.0f, 2.0f, 0.7f);
            SanitizeFloat(ps.DragCoefficient, 0.0f, 10.0f, 1.5f);
            SanitizeFloat(ps.TurbulenceStrength, 0.0f, 5.0f, 0.4f);
            SanitizeFloat(ps.TurbulenceFrequency, 0.0f, 20.0f, 1.2f);
            SanitizeFloat(ps.CollisionBounce, 0.0f, 1.0f, 0.0f);
            SanitizeFloat(ps.CollisionFriction, 0.0f, 1.0f, 1.0f);
            SanitizeFloat(ps.AccumulationFeedRate, 0.0f, 1.0f, 0.001f);
            SanitizeFloat(ps.ScreenStreakIntensity, 0.0f, 1.0f, 0.3f);
            SanitizeFloat(ps.ScreenStreakLength, 0.0f, 2.0f, 0.5f);
            SanitizeFloat(ps.LensImpactRate, 0.0f, 20.0f, 3.0f);
            SanitizeFloat(ps.LensImpactLifetime, 0.1f, 10.0f, 2.0f);
            SanitizeFloat(ps.LensImpactSize, 0.001f, 0.3f, 0.05f);
            SanitizeFloat(ps.LODNearDistance, 1.0f, 100.0f, 30.0f);
            SanitizeFloat(ps.LODFarDistance, 10.0f, 500.0f, 120.0f);
            SanitizeFloat(ps.FrameBudgetMs, 0.1f, 5.0f, 1.0f);
            SanitizeFloat(ps.ColorVariance, 0.0f, 1.0f, 0.05f);
            SanitizeFloat(ps.RotationSpeed, 0.0f, 30.0f, 1.0f);

            // Sanitize vec3 extents: each component must be positive
            SanitizeVec3Clamped(ps.NearFieldExtent, 1.0f, 200.0f, glm::vec3(15.0f, 25.0f, 15.0f));
            SanitizeVec3Clamped(ps.FarFieldExtent, 10.0f, 500.0f, glm::vec3(60.0f, 40.0f, 60.0f));

            // Sanitize speed ranges
            SanitizeFloat(ps.NearFieldSpeedMin, 0.0f, 20.0f, 1.0f);
            SanitizeFloat(ps.NearFieldSpeedMax, 0.0f, 20.0f, 3.0f);
            ps.NearFieldSpeedMax = std::max(ps.NearFieldSpeedMax, ps.NearFieldSpeedMin);
            SanitizeFloat(ps.FarFieldSpeedMin, 0.0f, 15.0f, 0.5f);
            SanitizeFloat(ps.FarFieldSpeedMax, 0.0f, 15.0f, 2.0f);
            ps.FarFieldSpeedMax = std::max(ps.FarFieldSpeedMax, ps.FarFieldSpeedMin);

            // Sanitize lifetimes
            SanitizeFloat(ps.NearFieldLifetime, 0.1f, 30.0f, 4.0f);
            SanitizeFloat(ps.FarFieldLifetime, 0.1f, 60.0f, 8.0f);
            SanitizeFloat(ps.FarFieldAlphaMultiplier, 0.0f, 1.0f, 0.5f);
            SanitizeFloat(ps.NearFieldSizeVariance, 0.0f, 0.1f, 0.01f);

            // Enforce LOD ordering: far >= near
            ps.LODFarDistance = std::max(ps.LODFarDistance, ps.LODNearDistance + 1.0f);

            // Sanitize GroundY
            SanitizeFloat(ps.GroundY, -1000.0f, 1000.0f, 0.0f);

            // Sanitize particle color: clamp components to [0,1]
            for (i32 i = 0; i < 4; ++i)
            {
                if (!std::isfinite(ps.ParticleColor[i]))
                {
                    ps.ParticleColor[i] = (i < 3) ? 0.95f : 0.85f;
                }
                else
                {
                    ps.ParticleColor[i] = std::clamp(ps.ParticleColor[i], 0.0f, 1.0f);
                }
            }
        }
    }

    static void DeserializeSnowDeformerComponent(Entity& entity, const YAML::Node& node)
    {
        OLO_PROFILE_FUNCTION();

        auto& sd = entity.AddComponent<SnowDeformerComponent>();
        TrySet(sd.m_DeformRadius, node["DeformRadius"]);
        TrySet(sd.m_DeformDepth, node["DeformDepth"]);
        TrySet(sd.m_FalloffExponent, node["FalloffExponent"]);
        TrySet(sd.m_CompactionFactor, node["CompactionFactor"]);
        TrySet(sd.m_EmitEjecta, node["EmitEjecta"]);

        // Validate — sanitize NaN/Inf then clamp
        SanitizeFloat(sd.m_DeformRadius, 0.01f, 50.0f, 0.5f);
        SanitizeFloat(sd.m_DeformDepth, 0.0f, 10.0f, 0.1f);
        SanitizeFloat(sd.m_FalloffExponent, 0.1f, 10.0f, 2.0f);
        SanitizeFloat(sd.m_CompactionFactor, 0.0f, 1.0f, 0.5f);
    }

    static void DeserializeFogVolumeComponent(Entity& entity, const YAML::Node& node)
    {
        OLO_PROFILE_FUNCTION();

        auto& fv = entity.AddComponent<FogVolumeComponent>();
        i32 shape = std::to_underlying(fv.m_Shape);
        TrySet(shape, node["Shape"]);
        fv.m_Shape = static_cast<FogVolumeShape>(std::clamp(shape, 0, 2));
        TrySet(fv.m_Extents, node["Extents"]);
        TrySet(fv.m_Color, node["Color"]);
        TrySet(fv.m_Density, node["Density"]);
        TrySet(fv.m_FalloffDistance, node["FalloffDistance"]);
        TrySet(fv.m_Priority, node["Priority"]);
        TrySet(fv.m_BlendWeight, node["BlendWeight"]);
        TrySet(fv.m_Enabled, node["Enabled"]);
        TrySet(fv.m_AffectTransparent, node["AffectTransparent"]);

        // Validate
        SanitizeVec3(fv.m_Extents, glm::vec3(5.0f));
        SanitizeVec3(fv.m_Color, glm::vec3(0.6f, 0.65f, 0.7f));
        SanitizeFloat(fv.m_Density, 0.0f, 100.0f, 0.5f);
        SanitizeFloat(fv.m_FalloffDistance, 0.0f, 100.0f, 1.0f);
        SanitizeFloat(fv.m_BlendWeight, 0.0f, 1.0f, 1.0f);
        fv.m_Priority = std::clamp(fv.m_Priority, -100, 100);
    }

    static void DeserializeDecalComponent(Entity& entity, const YAML::Node& node)
    {
        OLO_PROFILE_FUNCTION();

        auto& dc = entity.AddComponent<DecalComponent>();
        TrySet(dc.m_Color, node["Color"]);
        TrySet(dc.m_Size, node["Size"]);
        TrySet(dc.m_FadeDistance, node["FadeDistance"]);
        TrySet(dc.m_NormalAngleThreshold, node["NormalAngleThreshold"]);

        if (node["AlbedoTexturePath"])
        {
            auto texPath = node["AlbedoTexturePath"].as<std::string>("");
            dc.m_AlbedoTexture = LoadSceneTexture(texPath);
        }
        if (node["NormalTexturePath"])
        {
            auto texPath = node["NormalTexturePath"].as<std::string>("");
            dc.m_NormalTexture = LoadSceneTexture(texPath);
        }
        if (node["RMATexturePath"])
        {
            auto texPath = node["RMATexturePath"].as<std::string>("");
            dc.m_RMATexture = LoadSceneTexture(texPath);
        }
        if (node["EmissiveTexturePath"])
        {
            auto texPath = node["EmissiveTexturePath"].as<std::string>("");
            dc.m_EmissiveTexture = LoadSceneTexture(texPath);
        }
        if (node["Mode"])
        {
            u32 mode = node["Mode"].as<u32>(0);
            if (mode <= 3)
                dc.m_Mode = static_cast<DecalMode>(mode);
        }
        TrySet(dc.m_Transparent, node["Transparent"]);

        // Validate
        SanitizeVec3(dc.m_Size, glm::vec3(1.0f));
        for (int i = 0; i < 4; ++i)
        {
            if (!std::isfinite(dc.m_Color[i]))
            {
                dc.m_Color[i] = 1.0f;
            }
            else
            {
                dc.m_Color[i] = std::clamp(dc.m_Color[i], 0.0f, 1.0f);
            }
        }
        SanitizeFloat(dc.m_FadeDistance, 0.0f, 100.0f, 0.5f);
        SanitizeFloat(dc.m_NormalAngleThreshold, 0.0f, 1.0f, 0.5f);
    }

    static void DeserializeParticleSystemComponent(ParticleSystemComponent& psc, const YAML::Node& particleComponent)
    {
        auto& sys = psc.System;
        auto& emitter = sys.Emitter;

        if (particleComponent["MaxParticles"])
            sys.SetMaxParticles(particleComponent["MaxParticles"].as<u32>());
        TrySet(sys.Playing, particleComponent["Playing"]);
        TrySet(sys.Looping, particleComponent["Looping"]);
        TrySet(sys.Duration, particleComponent["Duration"]);
        TrySet(sys.PlaybackSpeed, particleComponent["PlaybackSpeed"]);
        if (particleComponent["SimulationSpace"])
            sys.SimulationSpace = static_cast<ParticleSpace>(particleComponent["SimulationSpace"].as<int>());

        TrySet(emitter.RateOverTime, particleComponent["RateOverTime"]);
        TrySet(emitter.InitialSpeed, particleComponent["InitialSpeed"]);
        TrySet(emitter.SpeedVariance, particleComponent["SpeedVariance"]);
        TrySet(emitter.LifetimeMin, particleComponent["LifetimeMin"]);
        TrySet(emitter.LifetimeMax, particleComponent["LifetimeMax"]);
        TrySet(emitter.InitialSize, particleComponent["InitialSize"]);
        TrySet(emitter.SizeVariance, particleComponent["SizeVariance"]);
        TrySet(emitter.InitialRotation, particleComponent["InitialRotation"]);
        TrySet(emitter.RotationVariance, particleComponent["RotationVariance"]);
        TrySet(emitter.InitialColor, particleComponent["InitialColor"]);

        // Bursts
        emitter.Bursts.clear();
        if (auto burstsNode = particleComponent["Bursts"]; burstsNode && burstsNode.IsSequence())
        {
            for (const auto& burstNode : burstsNode)
            {
                BurstEntry burst{};
                TrySet(burst.Time, burstNode["Time"]);
                TrySet(burst.Count, burstNode["Count"]);
                TrySet(burst.Probability, burstNode["Probability"]);
                emitter.Bursts.push_back(burst);
            }
        }

        // Emission shape deserialization
        if (auto shapeType = particleComponent["EmissionShapeType"]; shapeType)
        {
            switch (static_cast<EmissionShapeType>(shapeType.as<int>()))
            {
                case EmissionShapeType::Point:
                    emitter.Shape = EmitPoint{};
                    break;
                case EmissionShapeType::Sphere:
                {
                    EmitSphere sphere{};
                    TrySet(sphere.Radius, particleComponent["EmissionSphereRadius"]);
                    emitter.Shape = sphere;
                    break;
                }
                case EmissionShapeType::Box:
                {
                    EmitBox box{};
                    TrySet(box.HalfExtents, particleComponent["EmissionBoxHalfExtents"]);
                    emitter.Shape = box;
                    break;
                }
                case EmissionShapeType::Cone:
                {
                    EmitCone cone{};
                    TrySet(cone.Angle, particleComponent["EmissionConeAngle"]);
                    TrySet(cone.Radius, particleComponent["EmissionConeRadius"]);
                    emitter.Shape = cone;
                    break;
                }
                case EmissionShapeType::Ring:
                {
                    EmitRing ring{};
                    TrySet(ring.InnerRadius, particleComponent["EmissionRingInnerRadius"]);
                    TrySet(ring.OuterRadius, particleComponent["EmissionRingOuterRadius"]);
                    emitter.Shape = ring;
                    break;
                }
                case EmissionShapeType::Edge:
                {
                    EmitEdge edge{};
                    TrySet(edge.Length, particleComponent["EmissionEdgeLength"]);
                    emitter.Shape = edge;
                    break;
                }
                case EmissionShapeType::Mesh:
                {
                    EmitMesh mesh{};
                    i32 primType = 0;
                    TrySet(primType, particleComponent["EmissionMeshPrimitive"]);
                    BuildEmitMeshFromPrimitive(mesh, primType);
                    emitter.Shape = std::move(mesh);
                    break;
                }
                default:
                    break;
            }
        }

        TrySet(sys.GravityModule.Enabled, particleComponent["GravityEnabled"]);
        TrySet(sys.GravityModule.Gravity, particleComponent["Gravity"]);
        TrySet(sys.DragModule.Enabled, particleComponent["DragEnabled"]);
        TrySet(sys.DragModule.DragCoefficient, particleComponent["DragCoefficient"]);
        TrySet(sys.ColorModule.Enabled, particleComponent["ColorOverLifetimeEnabled"]);
        ParticleCurveSerializer::Deserialize4(particleComponent["ColorCurve"], sys.ColorModule.ColorCurve);
        TrySet(sys.SizeModule.Enabled, particleComponent["SizeOverLifetimeEnabled"]);
        ParticleCurveSerializer::Deserialize(particleComponent["SizeCurve"], sys.SizeModule.SizeCurve);
        TrySet(sys.RotationModule.Enabled, particleComponent["RotationOverLifetimeEnabled"]);
        TrySet(sys.RotationModule.AngularVelocity, particleComponent["AngularVelocity"]);
        TrySet(sys.VelocityModule.Enabled, particleComponent["VelocityOverLifetimeEnabled"]);
        TrySet(sys.VelocityModule.LinearAcceleration, particleComponent["LinearAcceleration"]);
        if (!particleComponent["LinearAcceleration"])
            TrySet(sys.VelocityModule.LinearAcceleration, particleComponent["LinearVelocity"]);
        TrySet(sys.VelocityModule.SpeedMultiplier, particleComponent["SpeedMultiplier"]);
        ParticleCurveSerializer::Deserialize(particleComponent["SpeedCurve"], sys.VelocityModule.SpeedCurve);
        TrySet(sys.NoiseModule.Enabled, particleComponent["NoiseEnabled"]);
        TrySet(sys.NoiseModule.Strength, particleComponent["NoiseStrength"]);
        TrySet(sys.NoiseModule.Frequency, particleComponent["NoiseFrequency"]);

        // Phase 2: Collision
        TrySet(sys.CollisionModule.Enabled, particleComponent["CollisionEnabled"]);
        if (auto val = particleComponent["CollisionMode"]; val)
            sys.CollisionModule.Mode = static_cast<CollisionMode>(val.as<int>());
        TrySet(sys.CollisionModule.PlaneNormal, particleComponent["CollisionPlaneNormal"]);
        TrySet(sys.CollisionModule.PlaneOffset, particleComponent["CollisionPlaneOffset"]);
        TrySet(sys.CollisionModule.Bounce, particleComponent["CollisionBounce"]);
        TrySet(sys.CollisionModule.LifetimeLoss, particleComponent["CollisionLifetimeLoss"]);
        TrySet(sys.CollisionModule.KillOnCollide, particleComponent["CollisionKillOnCollide"]);

        // Phase 2: Force Fields (vector, with backward compat for old single-field format)
        if (auto forceFieldsNode = particleComponent["ForceFields"]; forceFieldsNode && forceFieldsNode.IsSequence())
        {
            sys.ForceFields.clear();
            for (auto ffNode : forceFieldsNode)
            {
                ModuleForceField forceField{};
                TrySet(forceField.Enabled, ffNode["Enabled"]);
                if (auto val = ffNode["Type"]; val)
                    forceField.Type = static_cast<ForceFieldType>(val.as<int>());
                TrySet(forceField.Position, ffNode["Position"]);
                TrySet(forceField.Strength, ffNode["Strength"]);
                TrySet(forceField.Radius, ffNode["Radius"]);
                TrySet(forceField.Axis, ffNode["Axis"]);
                sys.ForceFields.push_back(forceField);
            }
        }
        else if (auto oldEnabled = particleComponent["ForceFieldEnabled"]; oldEnabled)
        {
            // Backward compatibility: old single force field format
            ModuleForceField forceField{};
            TrySet(forceField.Enabled, oldEnabled);
            if (auto val = particleComponent["ForceFieldType"]; val)
                forceField.Type = static_cast<ForceFieldType>(val.as<int>());
            TrySet(forceField.Position, particleComponent["ForceFieldPosition"]);
            TrySet(forceField.Strength, particleComponent["ForceFieldStrength"]);
            TrySet(forceField.Radius, particleComponent["ForceFieldRadius"]);
            TrySet(forceField.Axis, particleComponent["ForceFieldAxis"]);
            sys.ForceFields.push_back(forceField);
        }
        else
        {
            // No additional handling required.
        }

        // Phase 2: Trail
        TrySet(sys.TrailModule.Enabled, particleComponent["TrailEnabled"]);
        if (auto val = particleComponent["TrailMaxPoints"]; val)
            sys.TrailModule.MaxTrailPoints = val.as<u32>();
        TrySet(sys.TrailModule.TrailLifetime, particleComponent["TrailLifetime"]);
        TrySet(sys.TrailModule.MinVertexDistance, particleComponent["TrailMinVertexDistance"]);
        TrySet(sys.TrailModule.WidthStart, particleComponent["TrailWidthStart"]);
        TrySet(sys.TrailModule.WidthEnd, particleComponent["TrailWidthEnd"]);
        TrySet(sys.TrailModule.ColorStart, particleComponent["TrailColorStart"]);
        TrySet(sys.TrailModule.ColorEnd, particleComponent["TrailColorEnd"]);

        // Phase 2: Sub-Emitter
        TrySet(sys.SubEmitterModule.Enabled, particleComponent["SubEmitterEnabled"]);

        // Phase 2: LOD
        TrySet(sys.LODDistance1, particleComponent["LODDistance1"]);
        TrySet(sys.LODMaxDistance, particleComponent["LODMaxDistance"]);
        TrySet(sys.WarmUpTime, particleComponent["WarmUpTime"]);

        // Rendering settings
        if (auto val = particleComponent["BlendMode"]; val)
            sys.BlendMode = static_cast<ParticleBlendMode>(val.as<int>());
        if (auto val = particleComponent["RenderMode"]; val)
            sys.RenderMode = static_cast<ParticleRenderMode>(val.as<int>());
        TrySet(sys.DepthSortEnabled, particleComponent["DepthSortEnabled"]);
        TrySet(sys.UseGPU, particleComponent["UseGPU"]);
        TrySet(sys.SoftParticlesEnabled, particleComponent["SoftParticlesEnabled"]);
        TrySet(sys.SoftParticleDistance, particleComponent["SoftParticleDistance"]);
        TrySet(sys.VelocityInheritance, particleComponent["VelocityInheritance"]);

        // GPU Wind / Noise / Ground Collision
        TrySet(sys.WindInfluence, particleComponent["WindInfluence"]);
        TrySet(sys.GPUNoiseStrength, particleComponent["GPUNoiseStrength"]);
        TrySet(sys.GPUNoiseFrequency, particleComponent["GPUNoiseFrequency"]);
        TrySet(sys.GPUGroundCollision, particleComponent["GPUGroundCollision"]);
        TrySet(sys.GPUGroundY, particleComponent["GPUGroundY"]);
        TrySet(sys.GPUCollisionBounce, particleComponent["GPUCollisionBounce"]);
        TrySet(sys.GPUCollisionFriction, particleComponent["GPUCollisionFriction"]);

        // Clamp GPU simulation coefficients to safe ranges (reject non-finite first)
        if (!std::isfinite(sys.WindInfluence))
            sys.WindInfluence = 0.0f;
        if (!std::isfinite(sys.GPUNoiseStrength))
            sys.GPUNoiseStrength = 0.0f;
        if (!std::isfinite(sys.GPUNoiseFrequency))
            sys.GPUNoiseFrequency = 0.0f;
        if (!std::isfinite(sys.GPUGroundY))
            sys.GPUGroundY = 0.0f;
        if (!std::isfinite(sys.GPUCollisionBounce))
            sys.GPUCollisionBounce = 0.0f;
        if (!std::isfinite(sys.GPUCollisionFriction))
            sys.GPUCollisionFriction = 0.0f;
        sys.WindInfluence = std::max(sys.WindInfluence, 0.0f);
        sys.GPUNoiseStrength = std::max(sys.GPUNoiseStrength, 0.0f);
        sys.GPUNoiseFrequency = std::max(sys.GPUNoiseFrequency, 0.0f);
        sys.GPUCollisionBounce = std::clamp(sys.GPUCollisionBounce, 0.0f, 1.0f);
        sys.GPUCollisionFriction = std::clamp(sys.GPUCollisionFriction, 0.0f, 1.0f);

        // Texture sheet animation
        TrySet(sys.TextureSheetModule.Enabled, particleComponent["TextureSheetEnabled"]);
        TrySet(sys.TextureSheetModule.GridX, particleComponent["TextureSheetGridX"]);
        TrySet(sys.TextureSheetModule.GridY, particleComponent["TextureSheetGridY"]);
        TrySet(sys.TextureSheetModule.TotalFrames, particleComponent["TextureSheetTotalFrames"]);
        if (auto val = particleComponent["TextureSheetMode"]; val)
            sys.TextureSheetModule.Mode = static_cast<TextureSheetAnimMode>(val.as<int>());
        TrySet(sys.TextureSheetModule.SpeedRange, particleComponent["TextureSheetSpeedRange"]);
    }

    static void DeserializeTerrainComponent(TerrainComponent& terrain, const YAML::Node& terrainComponent)
    {
        OLO_PROFILE_FUNCTION();

        terrain.m_HeightmapPath = terrainComponent["HeightmapPath"].as<std::string>(terrain.m_HeightmapPath);
        terrain.m_WorldSizeX = terrainComponent["WorldSizeX"].as<f32>(terrain.m_WorldSizeX);
        terrain.m_WorldSizeZ = terrainComponent["WorldSizeZ"].as<f32>(terrain.m_WorldSizeZ);
        terrain.m_HeightScale = terrainComponent["HeightScale"].as<f32>(terrain.m_HeightScale);

        // Procedural generation settings
        terrain.m_ProceduralEnabled = terrainComponent["ProceduralEnabled"].as<bool>(terrain.m_ProceduralEnabled);
        terrain.m_ProceduralSeed = terrainComponent["ProceduralSeed"].as<i32>(terrain.m_ProceduralSeed);
        terrain.m_ProceduralResolution = terrainComponent["ProceduralResolution"].as<u32>(terrain.m_ProceduralResolution);
        terrain.m_ProceduralOctaves = terrainComponent["ProceduralOctaves"].as<u32>(terrain.m_ProceduralOctaves);
        terrain.m_ProceduralFrequency = terrainComponent["ProceduralFrequency"].as<f32>(terrain.m_ProceduralFrequency);
        terrain.m_ProceduralLacunarity = terrainComponent["ProceduralLacunarity"].as<f32>(terrain.m_ProceduralLacunarity);
        terrain.m_ProceduralPersistence = terrainComponent["ProceduralPersistence"].as<f32>(terrain.m_ProceduralPersistence);

        // Advanced height-field shaping
        terrain.m_HeightShaping.RidgeBlend = terrainComponent["ShapingRidgeBlend"].as<f32>(terrain.m_HeightShaping.RidgeBlend);
        terrain.m_HeightShaping.WarpStrength = terrainComponent["ShapingWarpStrength"].as<f32>(terrain.m_HeightShaping.WarpStrength);
        terrain.m_HeightShaping.WarpFrequency = terrainComponent["ShapingWarpFrequency"].as<f32>(terrain.m_HeightShaping.WarpFrequency);
        terrain.m_HeightShaping.TerraceSteps = terrainComponent["ShapingTerraceSteps"].as<u32>(terrain.m_HeightShaping.TerraceSteps);
        terrain.m_HeightShaping.TerraceSharpness = terrainComponent["ShapingTerraceSharpness"].as<f32>(terrain.m_HeightShaping.TerraceSharpness);
        terrain.m_HeightShaping.HeightExponent = terrainComponent["ShapingHeightExponent"].as<f32>(terrain.m_HeightShaping.HeightExponent);

        // Automatic material assignment rules
        terrain.m_AutoMaterial = terrainComponent["AutoMaterial"].as<bool>(terrain.m_AutoMaterial);
        terrain.m_SplatmapGenResolution = terrainComponent["SplatmapGenResolution"].as<u32>(terrain.m_SplatmapGenResolution);
        if (auto rulesNode = terrainComponent["LayerRules"]; rulesNode && rulesNode.IsSequence())
        {
            terrain.m_LayerRules.clear();
            for (const auto& ruleNode : rulesNode)
            {
                TerrainLayerRule rule;
                rule.LayerIndex = ruleNode["LayerIndex"].as<u32>(rule.LayerIndex);
                rule.MinHeight = ruleNode["MinHeight"].as<f32>(rule.MinHeight);
                rule.MaxHeight = ruleNode["MaxHeight"].as<f32>(rule.MaxHeight);
                rule.HeightBlend = ruleNode["HeightBlend"].as<f32>(rule.HeightBlend);
                rule.MinSlopeDeg = ruleNode["MinSlopeDeg"].as<f32>(rule.MinSlopeDeg);
                rule.MaxSlopeDeg = ruleNode["MaxSlopeDeg"].as<f32>(rule.MaxSlopeDeg);
                rule.SlopeBlend = ruleNode["SlopeBlend"].as<f32>(rule.SlopeBlend);
                rule.Strength = ruleNode["Strength"].as<f32>(rule.Strength);
                terrain.m_LayerRules.push_back(rule);
            }
        }

        // Sanitize untrusted YAML values (NaN/inf, out-of-range) so corrupt scene
        // data can't poison terrain generation or trigger huge allocations.
        {
            auto sanitize = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                    v = fallback;
                v = std::clamp(v, lo, hi);
            };
            sanitize(terrain.m_HeightShaping.RidgeBlend, 0.0f, 1.0f, 0.0f);
            sanitize(terrain.m_HeightShaping.WarpStrength, 0.0f, 4.0f, 0.0f);
            sanitize(terrain.m_HeightShaping.WarpFrequency, 0.0f, 64.0f, 2.0f);
            sanitize(terrain.m_HeightShaping.TerraceSharpness, 0.0f, 0.999f, 0.6f);
            sanitize(terrain.m_HeightShaping.HeightExponent, 0.05f, 16.0f, 1.0f);
            terrain.m_HeightShaping.TerraceSteps = std::min(terrain.m_HeightShaping.TerraceSteps, 256u);
            terrain.m_SplatmapGenResolution = std::clamp(terrain.m_SplatmapGenResolution, 16u, 4096u);
            for (TerrainLayerRule& r : terrain.m_LayerRules)
            {
                if (r.LayerIndex >= MAX_TERRAIN_LAYERS)
                    r.LayerIndex = 0;
                sanitize(r.MinHeight, 0.0f, 1.0f, 0.0f);
                sanitize(r.MaxHeight, 0.0f, 1.0f, 1.0f);
                sanitize(r.HeightBlend, 0.0f, 1.0f, 0.0f);
                sanitize(r.MinSlopeDeg, 0.0f, 90.0f, 0.0f);
                sanitize(r.MaxSlopeDeg, 0.0f, 90.0f, 90.0f);
                sanitize(r.SlopeBlend, 0.0f, 90.0f, 0.0f);
                sanitize(r.Strength, 0.0f, 16.0f, 1.0f);
                if (r.MinHeight > r.MaxHeight)
                    std::swap(r.MinHeight, r.MaxHeight);
                if (r.MinSlopeDeg > r.MaxSlopeDeg)
                    std::swap(r.MinSlopeDeg, r.MaxSlopeDeg);
            }
        }

        terrain.m_TessellationEnabled = terrainComponent["TessellationEnabled"].as<bool>(terrain.m_TessellationEnabled);
        terrain.m_TargetTriangleSize = terrainComponent["TargetTriangleSize"].as<f32>(terrain.m_TargetTriangleSize);
        terrain.m_MorphRegion = terrainComponent["MorphRegion"].as<f32>(terrain.m_MorphRegion);

        // Streaming settings
        terrain.m_StreamingEnabled = terrainComponent["StreamingEnabled"].as<bool>(terrain.m_StreamingEnabled);
        terrain.m_TileDirectory = terrainComponent["TileDirectory"].as<std::string>(terrain.m_TileDirectory);
        terrain.m_TileFilePattern = terrainComponent["TileFilePattern"].as<std::string>(terrain.m_TileFilePattern);
        terrain.m_TileWorldSize = terrainComponent["TileWorldSize"].as<f32>(terrain.m_TileWorldSize);
        terrain.m_TileResolution = terrainComponent["TileResolution"].as<u32>(terrain.m_TileResolution);
        terrain.m_StreamingLoadRadius = terrainComponent["StreamingLoadRadius"].as<u32>(terrain.m_StreamingLoadRadius);
        terrain.m_StreamingMaxTiles = terrainComponent["StreamingMaxTiles"].as<u32>(terrain.m_StreamingMaxTiles);

        // Voxel override settings
        terrain.m_VoxelEnabled = terrainComponent["VoxelEnabled"].as<bool>(terrain.m_VoxelEnabled);
        terrain.m_VoxelSize = terrainComponent["VoxelSize"].as<f32>(terrain.m_VoxelSize);

        // Deserialize terrain material layers
        if (auto layersNode = terrainComponent["Layers"]; layersNode && layersNode.IsSequence())
        {
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            if (auto sp0 = terrainComponent["SplatmapPath0"]; sp0)
                terrain.m_Material->SetSplatmapPath(0, sp0.as<std::string>());
            if (auto sp1 = terrainComponent["SplatmapPath1"]; sp1)
                terrain.m_Material->SetSplatmapPath(1, sp1.as<std::string>());

            for (const auto& layerNode : layersNode)
            {
                TerrainLayer layer;
                layer.Name = layerNode["Name"].as<std::string>(layer.Name);
                layer.AlbedoPath = layerNode["AlbedoPath"].as<std::string>(layer.AlbedoPath);
                layer.NormalPath = layerNode["NormalPath"].as<std::string>(layer.NormalPath);
                layer.ARMPath = layerNode["ARMPath"].as<std::string>(layer.ARMPath);
                layer.TilingScale = layerNode["TilingScale"].as<f32>(layer.TilingScale);
                layer.HeightBlendSharpness = layerNode["HeightBlendSharpness"].as<f32>(layer.HeightBlendSharpness);
                layer.TriplanarSharpness = layerNode["TriplanarSharpness"].as<f32>(layer.TriplanarSharpness);
                layer.BaseColor = layerNode["BaseColor"].as<glm::vec3>(layer.BaseColor);
                layer.Roughness = layerNode["Roughness"].as<f32>(layer.Roughness);
                layer.Metallic = layerNode["Metallic"].as<f32>(layer.Metallic);
                terrain.m_Material->AddLayer(layer);
            }
            terrain.m_MaterialNeedsRebuild = true;
        }

        terrain.m_NeedsRebuild = true;
    }

    static void DeserializeFoliageComponent(FoliageComponent& foliage, const YAML::Node& foliageComponent)
    {
        OLO_PROFILE_FUNCTION();

        foliage.m_Enabled = foliageComponent["Enabled"].as<bool>(foliage.m_Enabled);

        if (auto layersNode = foliageComponent["Layers"]; layersNode && layersNode.IsSequence())
        {
            foliage.m_Layers.clear();
            for (auto const& layerNode : layersNode)
            {
                FoliageLayer layer;
                layer.Name = layerNode["Name"].as<std::string>(layer.Name);
                layer.MeshPath = layerNode["MeshPath"].as<std::string>(layer.MeshPath);
                layer.AlbedoPath = layerNode["AlbedoPath"].as<std::string>(layer.AlbedoPath);
                layer.Density = layerNode["Density"].as<f32>(layer.Density);
                layer.SplatmapChannel = layerNode["SplatmapChannel"].as<i32>(layer.SplatmapChannel);
                layer.MinSlopeAngle = layerNode["MinSlopeAngle"].as<f32>(layer.MinSlopeAngle);
                layer.MaxSlopeAngle = layerNode["MaxSlopeAngle"].as<f32>(layer.MaxSlopeAngle);
                layer.MinScale = layerNode["MinScale"].as<f32>(layer.MinScale);
                layer.MaxScale = layerNode["MaxScale"].as<f32>(layer.MaxScale);
                layer.MinHeight = layerNode["MinHeight"].as<f32>(layer.MinHeight);
                layer.MaxHeight = layerNode["MaxHeight"].as<f32>(layer.MaxHeight);
                layer.RandomRotation = layerNode["RandomRotation"].as<bool>(layer.RandomRotation);
                layer.ViewDistance = layerNode["ViewDistance"].as<f32>(layer.ViewDistance);
                layer.FadeStartDistance = layerNode["FadeStartDistance"].as<f32>(layer.FadeStartDistance);
                layer.WindStrength = layerNode["WindStrength"].as<f32>(layer.WindStrength);
                layer.WindSpeed = layerNode["WindSpeed"].as<f32>(layer.WindSpeed);
                layer.BaseColor = layerNode["BaseColor"].as<glm::vec3>(layer.BaseColor);
                layer.Roughness = layerNode["Roughness"].as<f32>(layer.Roughness);
                layer.AlphaCutoff = layerNode["AlphaCutoff"].as<f32>(layer.AlphaCutoff);
                layer.Enabled = layerNode["Enabled"].as<bool>(layer.Enabled);
                foliage.m_Layers.push_back(layer);
            }
        }
        foliage.m_NeedsRebuild = true;
    }

    static void DeserializeWaterComponent(WaterComponent& water, const YAML::Node& waterComponent)
    {
        OLO_PROFILE_FUNCTION();

        water.m_Enabled = waterComponent["Enabled"].as<bool>(water.m_Enabled);
        water.m_WorldSizeX = waterComponent["WorldSizeX"].as<f32>(water.m_WorldSizeX);
        water.m_WorldSizeZ = waterComponent["WorldSizeZ"].as<f32>(water.m_WorldSizeZ);
        water.m_GridResolutionX = waterComponent["GridResolutionX"].as<u32>(water.m_GridResolutionX);
        water.m_GridResolutionZ = waterComponent["GridResolutionZ"].as<u32>(water.m_GridResolutionZ);
        water.m_WaveAmplitude = waterComponent["WaveAmplitude"].as<f32>(water.m_WaveAmplitude);
        water.m_WaveFrequency = waterComponent["WaveFrequency"].as<f32>(water.m_WaveFrequency);
        water.m_WaveSpeed = waterComponent["WaveSpeed"].as<f32>(water.m_WaveSpeed);
        water.m_WaveDir0 = waterComponent["WaveDir0"].as<glm::vec2>(water.m_WaveDir0);
        water.m_WaveDir1 = waterComponent["WaveDir1"].as<glm::vec2>(water.m_WaveDir1);
        water.m_WaveSteepness0 = waterComponent["WaveSteepness0"].as<f32>(water.m_WaveSteepness0);
        water.m_Wavelength0 = waterComponent["Wavelength0"].as<f32>(water.m_Wavelength0);
        water.m_WaveSteepness1 = waterComponent["WaveSteepness1"].as<f32>(water.m_WaveSteepness1);
        water.m_Wavelength1 = waterComponent["Wavelength1"].as<f32>(water.m_Wavelength1);
        water.m_WaterColor = waterComponent["WaterColor"].as<glm::vec3>(water.m_WaterColor);
        water.m_DeepColor = waterComponent["DeepColor"].as<glm::vec3>(water.m_DeepColor);
        water.m_Transparency = waterComponent["Transparency"].as<f32>(water.m_Transparency);
        water.m_Reflectivity = waterComponent["Reflectivity"].as<f32>(water.m_Reflectivity);
        water.m_FresnelPower = waterComponent["FresnelPower"].as<f32>(water.m_FresnelPower);
        water.m_SpecularIntensity = waterComponent["SpecularIntensity"].as<f32>(water.m_SpecularIntensity);
        water.m_NormalMapScrollDir0 = waterComponent["NormalMapScrollDir0"].as<glm::vec2>(water.m_NormalMapScrollDir0);
        water.m_NormalMapScrollDir1 = waterComponent["NormalMapScrollDir1"].as<glm::vec2>(water.m_NormalMapScrollDir1);
        water.m_NormalMapScrollSpeed0 = waterComponent["NormalMapScrollSpeed0"].as<f32>(water.m_NormalMapScrollSpeed0);
        water.m_NormalMapScrollSpeed1 = waterComponent["NormalMapScrollSpeed1"].as<f32>(water.m_NormalMapScrollSpeed1);
        water.m_NormalMapTiling = waterComponent["NormalMapTiling"].as<f32>(water.m_NormalMapTiling);
        water.m_NoiseIntensity = waterComponent["NoiseIntensity"].as<f32>(water.m_NoiseIntensity);
        water.m_NormalMap0 = waterComponent["NormalMap0"].as<u64>(water.m_NormalMap0);
        water.m_NormalMap1 = waterComponent["NormalMap1"].as<u64>(water.m_NormalMap1);
        water.m_NoiseTexture = waterComponent["NoiseTexture"].as<u64>(water.m_NoiseTexture);
        water.m_DepthSofteningDistance = waterComponent["DepthSofteningDistance"].as<f32>(water.m_DepthSofteningDistance);
        water.m_RefractionDistortion = waterComponent["RefractionDistortion"].as<f32>(water.m_RefractionDistortion);
        water.m_RefractionHeightFactor = waterComponent["RefractionHeightFactor"].as<f32>(water.m_RefractionHeightFactor);
        water.m_RefractionColor = waterComponent["RefractionColor"].as<glm::vec3>(water.m_RefractionColor);
        if (auto const refrEnabled = waterComponent["RefractionEnabled"])
        {
            water.m_RefractionEnabled = refrEnabled.as<bool>(water.m_RefractionEnabled);
        }
        water.m_FoamTexture = waterComponent["FoamTexture"].as<u64>(water.m_FoamTexture);
        water.m_FoamHeightStart = waterComponent["FoamHeightStart"].as<f32>(water.m_FoamHeightStart);
        water.m_FoamFadeDistance = waterComponent["FoamFadeDistance"].as<f32>(water.m_FoamFadeDistance);
        water.m_FoamTiling = waterComponent["FoamTiling"].as<f32>(water.m_FoamTiling);
        water.m_FoamBrightness = waterComponent["FoamBrightness"].as<f32>(water.m_FoamBrightness);
        water.m_FoamAngleExponent = waterComponent["FoamAngleExponent"].as<f32>(water.m_FoamAngleExponent);
        water.m_ShorelineFoamPower = waterComponent["ShorelineFoamPower"].as<f32>(water.m_ShorelineFoamPower);
        water.m_SSSColor = waterComponent["SSSColor"].as<glm::vec3>(water.m_SSSColor);
        water.m_SSSIntensity = waterComponent["SSSIntensity"].as<f32>(water.m_SSSIntensity);
        water.m_SSRMaxSteps = waterComponent["SSRMaxSteps"].as<f32>(water.m_SSRMaxSteps);
        water.m_SSRStepSize = waterComponent["SSRStepSize"].as<f32>(water.m_SSRStepSize);
        water.m_SSRMaxDistance = waterComponent["SSRMaxDistance"].as<f32>(water.m_SSRMaxDistance);
        water.m_SSRThickness = waterComponent["SSRThickness"].as<f32>(water.m_SSRThickness);
        if (auto const ssrEnabled = waterComponent["SSREnabled"])
        {
            water.m_SSREnabled = ssrEnabled.as<bool>(water.m_SSREnabled);
        }
        if (auto const tessEnabled = waterComponent["TessellationEnabled"])
        {
            water.m_TessellationEnabled = tessEnabled.as<bool>(water.m_TessellationEnabled);
        }
        water.m_TessellationFactor = waterComponent["TessellationFactor"].as<f32>(water.m_TessellationFactor);
        water.m_TessMinDistance = waterComponent["TessMinDistance"].as<f32>(water.m_TessMinDistance);
        water.m_TessMaxDistance = waterComponent["TessMaxDistance"].as<f32>(water.m_TessMaxDistance);
        water.m_UnderwaterFogColor = waterComponent["UnderwaterFogColor"].as<glm::vec3>(water.m_UnderwaterFogColor);
        water.m_UnderwaterFogDensity = waterComponent["UnderwaterFogDensity"].as<f32>(water.m_UnderwaterFogDensity);
        water.m_UnderwaterRefractionStrength = waterComponent["UnderwaterRefractionStrength"].as<f32>(water.m_UnderwaterRefractionStrength);
        water.m_UnderwaterRefractionScale = waterComponent["UnderwaterRefractionScale"].as<f32>(water.m_UnderwaterRefractionScale);
        water.m_UnderwaterRefractionSpeed = waterComponent["UnderwaterRefractionSpeed"].as<f32>(water.m_UnderwaterRefractionSpeed);
        water.m_UnderwaterChromaticStrength = waterComponent["UnderwaterChromaticStrength"].as<f32>(water.m_UnderwaterChromaticStrength);
        water.m_CausticsIntensity = waterComponent["CausticsIntensity"].as<f32>(water.m_CausticsIntensity);
        water.m_CausticsScale = waterComponent["CausticsScale"].as<f32>(water.m_CausticsScale);
        water.m_CausticsSpeed = waterComponent["CausticsSpeed"].as<f32>(water.m_CausticsSpeed);
        water.m_CausticsMaxDepth = waterComponent["CausticsMaxDepth"].as<f32>(water.m_CausticsMaxDepth);
        water.m_CausticsColor = waterComponent["CausticsColor"].as<glm::vec3>(water.m_CausticsColor);
        water.m_GodRayIntensity = waterComponent["GodRayIntensity"].as<f32>(water.m_GodRayIntensity);
        water.m_GodRayDecay = waterComponent["GodRayDecay"].as<f32>(water.m_GodRayDecay);
        water.m_GodRayDensity = waterComponent["GodRayDensity"].as<f32>(water.m_GodRayDensity);
        water.m_GodRayWeight = waterComponent["GodRayWeight"].as<f32>(water.m_GodRayWeight);
        water.m_GodRayColor = waterComponent["GodRayColor"].as<glm::vec3>(water.m_GodRayColor);
        water.m_GodRaySamples = waterComponent["GodRaySamples"].as<u32>(water.m_GodRaySamples);
        water.m_GodRayDappleFloor = waterComponent["GodRayDappleFloor"].as<f32>(water.m_GodRayDappleFloor);
        water.m_GodRaySunFalloff = waterComponent["GodRaySunFalloff"].as<f32>(water.m_GodRaySunFalloff);
        if (auto const renderFromBelow = waterComponent["RenderFromBelow"])
        {
            water.m_RenderFromBelow = renderFromBelow.as<bool>(water.m_RenderFromBelow);
        }

        // FFT ocean (WATER_FUTURE_IMPROVEMENTS.md §1)
        water.m_UseFFT = waterComponent["UseFFT"].as<bool>(water.m_UseFFT);
        water.m_FFTResolution = waterComponent["FFTResolution"].as<u32>(water.m_FFTResolution);
        water.m_FFTPatchSize = waterComponent["FFTPatchSize"].as<f32>(water.m_FFTPatchSize);
        water.m_FFTWindSpeed = waterComponent["FFTWindSpeed"].as<f32>(water.m_FFTWindSpeed);
        water.m_FFTWindDirection = waterComponent["FFTWindDirection"].as<glm::vec2>(water.m_FFTWindDirection);
        water.m_FFTAmplitude = waterComponent["FFTAmplitude"].as<f32>(water.m_FFTAmplitude);
        water.m_FFTChoppiness = waterComponent["FFTChoppiness"].as<f32>(water.m_FFTChoppiness);
        water.m_FFTHeightScale = waterComponent["FFTHeightScale"].as<f32>(water.m_FFTHeightScale);
        water.m_FFTSeed = waterComponent["FFTSeed"].as<u32>(water.m_FFTSeed);
        water.m_FFTUseGpuCompute = waterComponent["FFTUseGpuCompute"].as<bool>(water.m_FFTUseGpuCompute);

        // Sanitize FFT fields (ranges match the clamps in Scene.cpp / the editor
        // UI) so no NaN/Inf or out-of-range value reaches the spectrum/GPU.
        water.m_FFTResolution = std::clamp(water.m_FFTResolution, 16u, 512u);
        SanitizeFloat(water.m_FFTPatchSize, 1.0f, 5000.0f, 80.0f);
        SanitizeFloat(water.m_FFTWindSpeed, 0.1f, 100.0f, 18.0f);
        SanitizeVec2(water.m_FFTWindDirection, { 1.0f, 0.0f });
        SanitizeFloat(water.m_FFTAmplitude, 0.0f, 100.0f, 2.0f);
        SanitizeFloat(water.m_FFTChoppiness, 0.0f, 5.0f, 1.2f);
        SanitizeFloat(water.m_FFTHeightScale, 0.0f, 20.0f, 1.0f);
        // m_FFTSeed: any u32 is a valid RNG seed; m_UseFFT is a plain bool — no
        // validation needed for either.

        // Clamp grid resolution to safe bounds
        water.m_GridResolutionX = std::clamp(water.m_GridResolutionX, 1u, 1024u);
        water.m_GridResolutionZ = std::clamp(water.m_GridResolutionZ, 1u, 1024u);

        // Sanitize floats
        SanitizeFloat(water.m_WorldSizeX, 0.1f, 10000.0f, 100.0f);
        SanitizeFloat(water.m_WorldSizeZ, 0.1f, 10000.0f, 100.0f);
        SanitizeFloat(water.m_WaveAmplitude, 0.0f, 100.0f, 0.5f);
        SanitizeFloat(water.m_WaveFrequency, 0.0f, 100.0f, 1.0f);
        SanitizeFloat(water.m_WaveSpeed, 0.0f, 100.0f, 1.0f);
        SanitizeFloat(water.m_WaveSteepness0, 0.0f, 1.0f, 0.5f);
        SanitizeFloat(water.m_Wavelength0, 0.1f, 500.0f, 10.0f);
        SanitizeFloat(water.m_WaveSteepness1, 0.0f, 1.0f, 0.3f);
        SanitizeFloat(water.m_Wavelength1, 0.1f, 500.0f, 15.0f);
        SanitizeFloat(water.m_Transparency, 0.0f, 1.0f, 0.6f);
        SanitizeFloat(water.m_Reflectivity, 0.0f, 1.0f, 0.5f);
        SanitizeFloat(water.m_FresnelPower, 0.1f, 20.0f, 5.0f);
        SanitizeFloat(water.m_SpecularIntensity, 0.0f, 10.0f, 1.0f);
        SanitizeFloat(water.m_NormalMapScrollSpeed0, 0.0f, 1.0f, 0.02f);
        SanitizeFloat(water.m_NormalMapScrollSpeed1, 0.0f, 1.0f, 0.015f);
        SanitizeFloat(water.m_NormalMapTiling, 0.0f, 50.0f, 1.0f);
        SanitizeFloat(water.m_NoiseIntensity, 0.0f, 1.0f, 0.3f);

        // Sanitize vec2/vec3 fields
        SanitizeVec2(water.m_WaveDir0, { 1.0f, 0.0f });
        SanitizeVec2(water.m_WaveDir1, { 0.7f, 0.7f });
        SanitizeVec2(water.m_NormalMapScrollDir0, { 1.0f, 0.0f });
        SanitizeVec2(water.m_NormalMapScrollDir1, { 0.0f, 1.0f });
        // Normalize scroll directions to unit length so speed remains independent of vector magnitude
        if (auto const len0 = glm::length(water.m_NormalMapScrollDir0); std::isfinite(len0) && len0 > 1e-4f)
            water.m_NormalMapScrollDir0 /= len0;
        else
            water.m_NormalMapScrollDir0 = { 1.0f, 0.0f };
        if (auto const len1 = glm::length(water.m_NormalMapScrollDir1); std::isfinite(len1) && len1 > 1e-4f)
            water.m_NormalMapScrollDir1 /= len1;
        else
            water.m_NormalMapScrollDir1 = { 0.0f, 1.0f };
        SanitizeVec3(water.m_WaterColor, { 0.1f, 0.4f, 0.5f });
        SanitizeVec3(water.m_DeepColor, { 0.0f, 0.1f, 0.2f });

        // Phase 2+3+5 fields
        SanitizeFloat(water.m_DepthSofteningDistance, 0.0f, 50.0f, 2.0f);
        SanitizeFloat(water.m_RefractionDistortion, 0.0f, 0.5f, 0.05f);
        SanitizeFloat(water.m_RefractionHeightFactor, 0.0f, 2.0f, 0.5f);
        SanitizeVec3(water.m_RefractionColor, { 0.0f, 0.05f, 0.1f });
        SanitizeFloat(water.m_FoamHeightStart, 0.0f, 2.0f, 0.3f);
        SanitizeFloat(water.m_FoamFadeDistance, 0.01f, 5.0f, 0.5f);
        SanitizeFloat(water.m_FoamTiling, 0.0f, 50.0f, 2.0f);
        SanitizeFloat(water.m_FoamBrightness, 0.0f, 5.0f, 1.5f);
        SanitizeFloat(water.m_FoamAngleExponent, 0.1f, 10.0f, 2.0f);
        SanitizeFloat(water.m_ShorelineFoamPower, 0.1f, 10.0f, 3.0f);
        SanitizeVec3(water.m_SSSColor, { 0.0f, 0.5f, 0.4f });
        SanitizeFloat(water.m_SSSIntensity, 0.0f, 5.0f, 0.5f);
        SanitizeFloat(water.m_SSRMaxSteps, 0.0f, 256.0f, 64.0f);
        SanitizeFloat(water.m_SSRStepSize, 0.01f, 1.0f, 0.1f);
        SanitizeFloat(water.m_SSRMaxDistance, 1.0f, 200.0f, 50.0f);
        SanitizeFloat(water.m_SSRThickness, 0.01f, 5.0f, 0.5f);
        SanitizeFloat(water.m_TessellationFactor, 1.0f, 64.0f, 8.0f);
        SanitizeFloat(water.m_TessMinDistance, 1.0f, 500.0f, 10.0f);
        SanitizeFloat(water.m_TessMaxDistance, 10.0f, 1000.0f, 200.0f);
        // Enforce ordering: max must exceed min by at least 1.0
        if (water.m_TessMaxDistance < water.m_TessMinDistance + 1.0f)
            water.m_TessMaxDistance = water.m_TessMinDistance + 1.0f;
        SanitizeVec3(water.m_UnderwaterFogColor, { 0.05f, 0.15f, 0.25f });
        SanitizeFloat(water.m_UnderwaterFogDensity, 0.0f, 10.0f, 0.08f);
        // Underwater refraction distortion (§7.2) + caustics (§7.1)
        SanitizeFloat(water.m_UnderwaterRefractionStrength, 0.0f, 0.1f, 0.006f);
        SanitizeFloat(water.m_UnderwaterRefractionScale, 0.0f, 200.0f, 18.0f);
        SanitizeFloat(water.m_UnderwaterRefractionSpeed, 0.0f, 50.0f, 1.2f);
        SanitizeFloat(water.m_UnderwaterChromaticStrength, 0.0f, 1.0f, 0.4f);
        SanitizeFloat(water.m_CausticsIntensity, 0.0f, 10.0f, 0.5f);
        SanitizeFloat(water.m_CausticsScale, 0.001f, 10.0f, 0.35f);
        SanitizeFloat(water.m_CausticsSpeed, 0.0f, 50.0f, 0.6f);
        SanitizeFloat(water.m_CausticsMaxDepth, 0.1f, 1000.0f, 25.0f);
        SanitizeVec3(water.m_CausticsColor, { 0.7f, 0.85f, 1.0f });
        // Volumetric light shafts / god rays (§3.3)
        SanitizeFloat(water.m_GodRayIntensity, 0.0f, 10.0f, 0.5f);
        SanitizeFloat(water.m_GodRayDecay, 0.0f, 0.999f, 0.97f);
        SanitizeFloat(water.m_GodRayDensity, 0.0f, 2.0f, 0.85f);
        SanitizeFloat(water.m_GodRayWeight, 0.0f, 2.0f, 1.0f);
        SanitizeVec3(water.m_GodRayColor, { 1.0f, 0.95f, 0.8f });
        water.m_GodRaySamples = std::clamp(water.m_GodRaySamples, 1u, 256u);
        SanitizeFloat(water.m_GodRayDappleFloor, 0.0f, 1.0f, 0.35f);
        SanitizeFloat(water.m_GodRaySunFalloff, 1.0f, 64.0f, 16.0f);

        water.m_NeedsRebuild = true;
    }

    static std::string RigidBody2DBodyTypeToString(const Rigidbody2DComponent::BodyType bodyType)
    {
        switch (bodyType)
        {
            using enum OloEngine::Rigidbody2DComponent::BodyType;
            case Static:
                return "Static";
            case Dynamic:
                return "Dynamic";
            case Kinematic:
                return "Kinematic";
        }

        OLO_CORE_ASSERT(false, "Unknown body type");
        return {};
    }

    static Rigidbody2DComponent::BodyType RigidBody2DBodyTypeFromString(const std::string_view bodyTypeString)
    {
        using enum OloEngine::Rigidbody2DComponent::BodyType;
        if (bodyTypeString == "Static")
        {
            return Static;
        }
        if (bodyTypeString == "Dynamic")
        {
            return Dynamic;
        }
        if (bodyTypeString == "Kinematic")
        {
            return Kinematic;
        }

        OLO_CORE_ASSERT(false, "Unknown body type");
        return Static;
    }

    static void DeserializeEntityComponents(Entity& deserializedEntity, const YAML::Node& entity)
    {
        if (auto transformComponent = entity["TransformComponent"]; transformComponent)
        {
            // Entities always have transforms
            auto& tc = deserializedEntity.GetComponent<TransformComponent>();
            tc.Translation = transformComponent["Translation"].as<glm::vec3>();
            tc.SetRotationEuler(transformComponent["Rotation"].as<glm::vec3>());
            tc.Scale = transformComponent["Scale"].as<glm::vec3>();
        }

        if (auto cameraComponent = entity["CameraComponent"]; cameraComponent)
        {
            auto& cc = deserializedEntity.AddComponent<CameraComponent>();

            auto cameraProps = cameraComponent["Camera"];
            cc.Camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>(cameraProps["ProjectionType"].as<int>()));

            cc.Camera.SetPerspectiveVerticalFOV(cameraProps["PerspectiveFOV"].as<f32>());
            cc.Camera.SetPerspectiveNearClip(cameraProps["PerspectiveNear"].as<f32>());
            cc.Camera.SetPerspectiveFarClip(cameraProps["PerspectiveFar"].as<f32>());

            cc.Camera.SetOrthographicSize(cameraProps["OrthographicSize"].as<f32>());
            cc.Camera.SetOrthographicNearClip(cameraProps["OrthographicNear"].as<f32>());
            cc.Camera.SetOrthographicFarClip(cameraProps["OrthographicFar"].as<f32>());

            cc.Primary = cameraComponent["Primary"].as<bool>();
            cc.FixedAspectRatio = cameraComponent["FixedAspectRatio"].as<bool>();

            if (auto rc = cameraComponent["RuntimeControl"]; rc)
            {
                cc.RuntimeControl = rc.as<bool>();
            }
            if (auto fs = cameraComponent["FlySpeed"]; fs)
            {
                f32 const flySpeed = fs.as<f32>();
                if (std::isfinite(flySpeed) && flySpeed > 0.0f)
                {
                    cc.FlySpeed = flySpeed;
                }
            }
        }

        if (auto scriptComponent = entity["ScriptComponent"])
        {
            auto& sc = deserializedEntity.AddComponent<ScriptComponent>();
            sc.ClassName = scriptComponent["ClassName"].as<std::string>();

            if (auto scriptFields = scriptComponent["ScriptFields"]; scriptFields)
            {
                if (Ref<ScriptClass> entityClass = ScriptEngine::GetEntityClass(sc.ClassName))
                {
                    const auto& fields = entityClass->GetFields();
                    auto& entityFields = ScriptEngine::GetScriptFieldMap(deserializedEntity);

                    for (auto scriptField : scriptFields)
                    {
                        std::string fieldName = scriptField["Name"].as<std::string>();
                        auto typeString = scriptField["Type"].as<std::string>();
                        ScriptFieldType type = Utils::ScriptFieldTypeFromString(typeString);

                        ScriptFieldInstance& fieldInstance = entityFields[fieldName];

                        OLO_CORE_ASSERT(fields.contains(fieldName));

                        if (!fields.contains(fieldName))
                        {
                            continue;
                        }

                        fieldInstance.Field = fields.at(fieldName);

                        switch (type)
                        {
                            READ_SCRIPT_FIELD(Float, f32)
                            READ_SCRIPT_FIELD(Double, f64)
                            READ_SCRIPT_FIELD(Bool, bool)
                            READ_SCRIPT_FIELD(Char, char)
                            READ_SCRIPT_FIELD(Byte, i8)
                            READ_SCRIPT_FIELD(Short, i16)
                            READ_SCRIPT_FIELD(Int, i32)
                            READ_SCRIPT_FIELD(Long, i64)
                            READ_SCRIPT_FIELD(UByte, u8)
                            READ_SCRIPT_FIELD(UShort, u16)
                            READ_SCRIPT_FIELD(UInt, u32)
                            READ_SCRIPT_FIELD(ULong, u64)
                            READ_SCRIPT_FIELD(Vector2, glm::vec2)
                            READ_SCRIPT_FIELD(Vector3, glm::vec3)
                            READ_SCRIPT_FIELD(Vector4, glm::vec4)
                            READ_SCRIPT_FIELD(Entity, UUID)
                        }
                    }
                }
            }
        }

        if (auto const& luaScript = entity["LuaScriptComponent"])
        {
            auto& lsc = deserializedEntity.AddComponent<LuaScriptComponent>();
            TrySet(lsc.ScriptFile, luaScript["ScriptFile"]);
        }

        if (const auto& audioSourceComponent = entity["AudioSourceComponent"])
        {
            auto& src = deserializedEntity.AddComponent<AudioSourceComponent>();
            std::string audioFilepath;
            TrySet(audioFilepath, audioSourceComponent["Filepath"]);
            TrySet(src.Config.VolumeMultiplier, audioSourceComponent["VolumeMultiplier"]);
            TrySet(src.Config.PitchMultiplier, audioSourceComponent["PitchMultiplier"]);
            TrySet(src.Config.PlayOnAwake, audioSourceComponent["PlayOnAwake"]);
            TrySet(src.Config.Looping, audioSourceComponent["Looping"]);
            TrySet(src.Config.Spatialization, audioSourceComponent["Spatialization"]);
            TrySetEnum(src.Config.AttenuationModel, audioSourceComponent["AttenuationModel"]);
            TrySet(src.Config.RollOff, audioSourceComponent["RollOff"]);
            TrySet(src.Config.MinGain, audioSourceComponent["MinGain"]);
            TrySet(src.Config.MaxGain, audioSourceComponent["MaxGain"]);
            TrySet(src.Config.MinDistance, audioSourceComponent["MinDistance"]);
            TrySet(src.Config.MaxDistance, audioSourceComponent["MaxDistance"]);
            TrySet(src.Config.ConeInnerAngle, audioSourceComponent["ConeInnerAngle"]);
            TrySet(src.Config.ConeOuterAngle, audioSourceComponent["ConeOuterAngle"]);
            TrySet(src.Config.ConeOuterGain, audioSourceComponent["ConeOuterGain"]);
            TrySet(src.Config.DopplerFactor, audioSourceComponent["DopplerFactor"]);

            // DSP parameters: load + sanitize in one step to prevent drift
            auto TrySetDsp = [&audioSourceComponent](f32& field, const char* key, f32 lo, f32 hi, f32 fallback)
            {
                TrySet(field, audioSourceComponent[key]);
                SanitizeFloat(field, lo, hi, fallback);
            };
            TrySetDsp(src.Config.Spread, "Spread", 0.0f, 1.0f, 1.0f);
            TrySetDsp(src.Config.Focus, "Focus", 0.0f, 1.0f, 1.0f);
            TrySetDsp(src.Config.LowPassCutoff, "LowPassCutoff", 0.0f, 1.0f, 1.0f);
            TrySetDsp(src.Config.HighPassCutoff, "HighPassCutoff", 0.0f, 1.0f, 0.0f);
            TrySetDsp(src.Config.ReverbSend, "ReverbSend", 0.0f, 1.0f, 0.0f);

            TrySet(src.UseEventSystem, audioSourceComponent["UseEventSystem"]);
            TrySet(src.StartEvent, audioSourceComponent["StartEvent"]);
            if (!src.StartEvent.empty())
            {
                // Always derive CommandID from StartEvent — StartEvent is the single source of truth
                src.StartCommandID = Audio::CommandID::FromString(src.StartEvent);
            }
            else if (const auto cmdIDNode = audioSourceComponent["StartCommandID"])
            {
                src.StartCommandID = Audio::CommandID(cmdIDNode.as<u32>(0));
            }
            else
            {
                // No additional handling required.
            }

            if (!audioFilepath.empty())
            {
                std::filesystem::path path = audioFilepath.c_str();
                path = Project::GetAssetFileSystemPath(path);
                src.Source = Ref<AudioSource>::Create(path.string().c_str());
            }
        }

        if (const auto& audioListenerComponent = entity["AudioListenerComponent"])
        {
            auto& src = deserializedEntity.AddComponent<AudioListenerComponent>();
            TrySet(src.Active, audioListenerComponent["Active"]);
            TrySet(src.Config.ConeInnerAngle, audioListenerComponent["ConeInnerAngle"]);
            TrySet(src.Config.ConeOuterAngle, audioListenerComponent["ConeOuterAngle"]);
            TrySet(src.Config.ConeOuterGain, audioListenerComponent["ConeOuterGain"]);
        }

        if (const auto& soundGraphComponent = entity["AudioSoundGraphComponent"])
        {
            auto& sgc = deserializedEntity.AddComponent<AudioSoundGraphComponent>();
            if (auto handleNode = soundGraphComponent["SoundGraphHandle"])
                sgc.SoundGraphHandle = handleNode.as<u64>(0);
            TrySet(sgc.VolumeMultiplier, soundGraphComponent["VolumeMultiplier"]);
            TrySet(sgc.PitchMultiplier, soundGraphComponent["PitchMultiplier"]);
            TrySet(sgc.Looping, soundGraphComponent["Looping"]);
            TrySet(sgc.PlayOnAwake, soundGraphComponent["PlayOnAwake"]);
        }

        if (const auto& videoOverlayComponent = entity["VideoOverlayComponent"])
        {
            auto& voc = deserializedEntity.AddComponent<VideoOverlayComponent>();
            if (auto pathNode = videoOverlayComponent["VideoPath"])
                voc.VideoPath = pathNode.as<std::string>("");
            TrySet(voc.PlayOnStart, videoOverlayComponent["PlayOnStart"]);
            TrySet(voc.SkipOnInput, videoOverlayComponent["SkipOnInput"]);
            TrySet(voc.Looping, videoOverlayComponent["Looping"]);
            TrySet(voc.Volume, videoOverlayComponent["Volume"]);
            SanitizeFloat(voc.Volume, 0.0f, 1.0f, 1.0f);
        }

        if (const auto& videoSurfaceComponent = entity["VideoSurfaceComponent"])
        {
            auto& vsc = deserializedEntity.AddComponent<VideoSurfaceComponent>();
            if (auto pathNode = videoSurfaceComponent["VideoPath"])
                vsc.VideoPath = pathNode.as<std::string>("");
            TrySet(vsc.AutoPlay, videoSurfaceComponent["AutoPlay"]);
            TrySet(vsc.Looping, videoSurfaceComponent["Looping"]);
            TrySet(vsc.Volume, videoSurfaceComponent["Volume"]);
            SanitizeFloat(vsc.Volume, 0.0f, 1.0f, 0.5f);
        }

        if (auto spriteRendererComponent = entity["SpriteRendererComponent"]; spriteRendererComponent)
        {
            auto& src = deserializedEntity.AddComponent<SpriteRendererComponent>();
            src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
            if (spriteRendererComponent["TexturePath"])
            {
                src.Texture = LoadSceneTexture(spriteRendererComponent["TexturePath"].as<std::string>());
            }

            if (spriteRendererComponent["TilingFactor"])
            {
                src.TilingFactor = spriteRendererComponent["TilingFactor"].as<f32>();
            }
        }

        if (auto circleRendererComponent = entity["CircleRendererComponent"]; circleRendererComponent)
        {
            auto& crc = deserializedEntity.AddComponent<CircleRendererComponent>();
            crc.Color = circleRendererComponent["Color"].as<glm::vec4>();
            crc.Thickness = circleRendererComponent["Thickness"].as<f32>();
            crc.Fade = circleRendererComponent["Fade"].as<f32>();
        }

        if (auto rigidbody2DComponent = entity["Rigidbody2DComponent"]; rigidbody2DComponent)
        {
            auto& rb2d = deserializedEntity.AddComponent<Rigidbody2DComponent>();
            rb2d.Type = RigidBody2DBodyTypeFromString(rigidbody2DComponent["BodyType"].as<std::string>());
            rb2d.FixedRotation = rigidbody2DComponent["FixedRotation"].as<bool>();
        }

        if (auto boxCollider2DComponent = entity["BoxCollider2DComponent"]; boxCollider2DComponent)
        {
            auto& bc2d = deserializedEntity.AddComponent<BoxCollider2DComponent>();
            bc2d.Offset = boxCollider2DComponent["Offset"].as<glm::vec2>();
            bc2d.Size = boxCollider2DComponent["Size"].as<glm::vec2>();
            bc2d.Density = boxCollider2DComponent["Density"].as<f32>();
            bc2d.Friction = boxCollider2DComponent["Friction"].as<f32>();
            bc2d.Restitution = boxCollider2DComponent["Restitution"].as<f32>();
            bc2d.RestitutionThreshold = boxCollider2DComponent["RestitutionThreshold"].as<f32>();
        }

        if (auto circleCollider2DComponent = entity["CircleCollider2DComponent"]; circleCollider2DComponent)
        {
            auto& cc2d = deserializedEntity.AddComponent<CircleCollider2DComponent>();
            cc2d.Offset = circleCollider2DComponent["Offset"].as<glm::vec2>();
            cc2d.Radius = circleCollider2DComponent["Radius"].as<f32>();
            cc2d.Density = circleCollider2DComponent["Density"].as<f32>();
            cc2d.Friction = circleCollider2DComponent["Friction"].as<f32>();
            cc2d.Restitution = circleCollider2DComponent["Restitution"].as<f32>();
            cc2d.RestitutionThreshold = circleCollider2DComponent["RestitutionThreshold"].as<f32>();
        }

        if (auto textComponent = entity["TextComponent"]; textComponent)
        {
            auto& tc = deserializedEntity.AddComponent<TextComponent>();
            tc.TextString = textComponent["TextString"].as<std::string>();
            if (textComponent["FontPath"])
            {
                tc.FontAsset = Font::Create(textComponent["FontPath"].as<std::string>());
            }
            tc.Color = textComponent["Color"].as<glm::vec4>();
            tc.Kerning = textComponent["Kerning"].as<float>();
            tc.LineSpacing = textComponent["LineSpacing"].as<float>();
            TrySet(tc.MaxWidth, textComponent["MaxWidth"]);
            if (!std::isfinite(tc.MaxWidth) || tc.MaxWidth < 0.0f)
                tc.MaxWidth = 0.0f;
            TrySet(tc.DropShadow, textComponent["DropShadow"]);
            TrySet(tc.ShadowDistance, textComponent["ShadowDistance"]);
            if (!std::isfinite(tc.ShadowDistance))
                tc.ShadowDistance = 0.02f;
            TrySet(tc.ShadowColor, textComponent["ShadowColor"]);
            for (int ci = 0; ci < 4; ++ci)
            {
                if (!std::isfinite(tc.ShadowColor[ci]))
                    tc.ShadowColor[ci] = (ci == 3) ? 1.0f : 0.0f;
            }
        }

        if (auto localizedTextComponent = entity["LocalizedTextComponent"]; localizedTextComponent)
        {
            auto& ltc = deserializedEntity.AddComponent<LocalizedTextComponent>();
            if (localizedTextComponent["LocalizationKey"])
                ltc.LocalizationKey = localizedTextComponent["LocalizationKey"].as<std::string>();
        }

        if (auto meshComponent = entity["MeshComponent"]; meshComponent)
        {
            auto& mc = deserializedEntity.AddComponent<MeshComponent>();
            if (meshComponent["MeshSourceHandle"])
            {
                u64 handle = meshComponent["MeshSourceHandle"].as<u64>();
                mc.m_MeshSource = AssetManager::GetAsset<MeshSource>(handle);
            }
            if (meshComponent["Primitive"])
            {
                if (const auto primitiveInt = meshComponent["Primitive"].as<i32>(); primitiveInt >= std::to_underlying(MeshPrimitive::None) && primitiveInt <= std::to_underlying(MeshPrimitive::Torus))
                {
                    mc.m_Primitive = static_cast<MeshPrimitive>(primitiveInt);
                }
                else
                {
                    OLO_CORE_WARN("SceneSerializer: Invalid MeshPrimitive value {}, defaulting to None", primitiveInt);
                    mc.m_Primitive = MeshPrimitive::None;
                }
                if (!mc.m_MeshSource && mc.m_Primitive != MeshPrimitive::None)
                {
                    if (auto mesh = CreateMeshFromPrimitive(mc.m_Primitive))
                    {
                        mc.m_MeshSource = mesh->GetMeshSource();
                    }
                }
            }
        }

        if (auto imcNode = entity["InstancedMeshComponent"]; imcNode)
        {
            auto& imc = deserializedEntity.AddComponent<InstancedMeshComponent>();
            if (imcNode["MeshSourceHandle"])
            {
                u64 handle = imcNode["MeshSourceHandle"].as<u64>();
                imc.MeshSource = AssetManager::GetAsset<MeshSource>(handle);
            }
            if (imcNode["OverrideMaterialHandle"])
            {
                u64 handle = imcNode["OverrideMaterialHandle"].as<u64>();
                imc.OverrideMaterial = AssetManager::GetAsset<Material>(handle);
            }
            if (imcNode["FrustumCullPerInstance"])
                imc.FrustumCullPerInstance = imcNode["FrustumCullPerInstance"].as<bool>();
            if (imcNode["CastShadows"])
                imc.CastShadows = imcNode["CastShadows"].as<bool>();
            if (imcNode["CullDistance"])
                imc.CullDistance = imcNode["CullDistance"].as<f32>();
            if (imcNode["PlacementAssetHandle"])
                imc.PlacementAssetHandle = imcNode["PlacementAssetHandle"].as<u64>();
            if (imcNode["Primitive"])
            {
                const auto primitiveInt = imcNode["Primitive"].as<i32>();
                if (primitiveInt >= std::to_underlying(MeshPrimitive::None) && primitiveInt <= std::to_underlying(MeshPrimitive::Torus))
                {
                    imc.Primitive = static_cast<MeshPrimitive>(primitiveInt);
                    // Resolve MeshSource from the primitive if not already
                    // assigned. Mirrors the MeshComponent fallback path.
                    if (!imc.MeshSource && imc.Primitive != MeshPrimitive::None)
                    {
                        if (auto mesh = CreateMeshFromPrimitive(imc.Primitive))
                            imc.MeshSource = mesh->GetMeshSource();
                    }
                }
            }

            // Instances: flat-array form. Transform is 16 floats, Color is 4
            // floats, then EntityID (int) and Custom (float). Anything missing
            // defaults to identity / white tint / -1 / 0.
            if (auto instances = imcNode["Instances"]; instances && instances.IsSequence())
            {
                imc.Instances.reserve(instances.size());
                for (const auto& node : instances)
                {
                    InstanceData inst;
                    if (auto t = node["Transform"]; t && t.IsSequence() && t.size() == 16)
                    {
                        for (sizet i = 0; i < 16; ++i)
                            glm::value_ptr(inst.Transform)[i] = t[i].as<f32>();
                        // A non-finite transform propagates into the instance SSBO and
                        // any later transpose(inverse(...)); reset rather than upload garbage.
                        if (!Math::IsFinite(inst.Transform))
                            inst.Transform = glm::mat4(1.0f);
                    }
                    if (auto c = node["Color"]; c && c.IsSequence() && c.size() == 4)
                    {
                        for (sizet i = 0; i < 4; ++i)
                            glm::value_ptr(inst.Color)[i] = c[i].as<f32>();
                        if (!Math::IsFinite(inst.Color))
                            inst.Color = glm::vec4(1.0f);
                    }
                    if (node["EntityID"])
                        inst.EntityID = node["EntityID"].as<i32>();
                    if (node["Custom"])
                    {
                        inst.Custom = node["Custom"].as<f32>();
                        if (!Math::IsFinite(inst.Custom))
                            inst.Custom = 0.0f;
                    }
                    imc.Instances.push_back(inst);
                }
            }
        }

        if (auto modelComponent = entity["ModelComponent"]; modelComponent)
        {
            auto& mc = deserializedEntity.AddComponent<ModelComponent>();
            if (modelComponent["FilePath"])
            {
                mc.m_FilePath = modelComponent["FilePath"].as<std::string>();
                if (!mc.m_FilePath.empty())
                {
                    mc.Reload(); // Load the model from file
                }
            }
            if (modelComponent["Visible"])
            {
                mc.m_Visible = modelComponent["Visible"].as<bool>();
            }
        }

        if (auto lodGroupComponent = entity["LODGroupComponent"]; lodGroupComponent)
        {
            auto& lodComp = deserializedEntity.AddComponent<LODGroupComponent>();
            if (lodGroupComponent["Enabled"])
            {
                lodComp.m_Enabled = lodGroupComponent["Enabled"].as<bool>();
            }
            if (lodGroupComponent["Bias"])
            {
                f32 bias = lodGroupComponent["Bias"].as<f32>();
                if (bias > 0.0f && std::isfinite(bias))
                {
                    lodComp.m_LODGroup.Bias = bias;
                }
            }
            if (lodGroupComponent["Levels"])
            {
                for (auto levelNode : lodGroupComponent["Levels"])
                {
                    LODLevel level;
                    if (levelNode["MeshHandle"])
                    {
                        level.MeshHandle = levelNode["MeshHandle"].as<u64>();
                    }
                    if (levelNode["MaxDistance"])
                    {
                        f32 maxDist = levelNode["MaxDistance"].as<f32>();
                        if (std::isfinite(maxDist) && maxDist >= 0.0f)
                        {
                            level.MaxDistance = maxDist;
                        }
                        else
                        {
                            continue; // Skip invalid level
                        }
                    }
                    if (levelNode["TriangleCount"])
                    {
                        level.TriangleCount = levelNode["TriangleCount"].as<u32>();
                    }
                    if (level.MeshHandle == 0)
                    {
                        continue; // Skip level with no mesh
                    }
                    lodComp.m_LODGroup.Levels.push_back(level);
                }

                // Ensure levels are sorted by distance
                std::ranges::sort(lodComp.m_LODGroup.Levels,
                                  [](const LODLevel& a, const LODLevel& b)
                                  { return a.MaxDistance < b.MaxDistance; });
            }
        }

        if (auto tileRendererComponent = entity["TileRendererComponent"]; tileRendererComponent)
        {
            auto& tileComp = deserializedEntity.AddComponent<TileRendererComponent>();
            if (tileRendererComponent["MeshSourceHandle"])
            {
                u64 handle = tileRendererComponent["MeshSourceHandle"].as<u64>();
                auto meshSource = AssetManager::GetAsset<MeshSource>(handle);
                if (meshSource)
                {
                    u32 submeshIdx = 0;
                    if (tileRendererComponent["SubmeshIndex"])
                    {
                        submeshIdx = tileRendererComponent["SubmeshIndex"].as<u32>();
                    }
                    if (submeshIdx < static_cast<u32>(meshSource->GetSubmeshes().Num()))
                    {
                        tileComp.TileMesh = Ref<Mesh>::Create(meshSource, submeshIdx);
                    }
                    else
                    {
                        OLO_CORE_WARN("TileRendererComponent: SubmeshIndex {} out of range (mesh has {} submeshes), using 0",
                                      submeshIdx, meshSource->GetSubmeshes().Num());
                        if (meshSource->GetSubmeshes().Num() > 0)
                            tileComp.TileMesh = Ref<Mesh>::Create(meshSource, 0);
                    }
                }
            }
            if (tileRendererComponent["Width"])
            {
                tileComp.Width = std::clamp(tileRendererComponent["Width"].as<u32>(),
                                            1u, TileRendererComponent::MaxGridDimension);
            }
            if (tileRendererComponent["Height"])
            {
                tileComp.Height = std::clamp(tileRendererComponent["Height"].as<u32>(),
                                             1u, TileRendererComponent::MaxGridDimension);
            }
            if (tileRendererComponent["TileSize"])
            {
                f32 tileSize = tileRendererComponent["TileSize"].as<f32>();
                tileComp.TileSize = (std::isfinite(tileSize) && tileSize > 0.0f) ? tileSize : 1.0f;
            }
            if (tileRendererComponent["Materials"])
            {
                tileComp.Materials.clear();
                constexpr sizet maxMaterials = static_cast<sizet>(std::numeric_limits<u8>::max()) + 1;
                for (auto matNode : tileRendererComponent["Materials"])
                {
                    if (tileComp.Materials.size() >= maxMaterials)
                        break;
                    Material mat;
                    if (matNode["AlbedoColor"])
                    {
                        auto albedo = matNode["AlbedoColor"].as<glm::vec3>();
                        mat.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
                    }
                    if (matNode["Metallic"])
                    {
                        mat.SetMetallicFactor(matNode["Metallic"].as<f32>());
                    }
                    if (matNode["Roughness"])
                    {
                        mat.SetRoughnessFactor(matNode["Roughness"].as<f32>());
                    }
                    tileComp.Materials.push_back(std::move(mat));
                }
            }
            if (tileRendererComponent["MaterialIDs"])
            {
                tileComp.MaterialIDs.clear();
                sizet maxIndex = tileComp.Materials.empty()
                                     ? 0
                                     : std::min(tileComp.Materials.size() - 1, static_cast<sizet>(std::numeric_limits<u8>::max()));
                u8 maxMatIdx = static_cast<u8>(maxIndex);
                for (auto idNode : tileRendererComponent["MaterialIDs"])
                {
                    i32 raw = idNode.as<i32>();
                    tileComp.MaterialIDs.push_back(
                        static_cast<u8>(std::clamp(raw, 0, static_cast<i32>(maxMatIdx))));
                }
            }
            // Ensure MaterialIDs matches grid size
            tileComp.MaterialIDs.resize(static_cast<sizet>(tileComp.Width) * tileComp.Height, 0);
        }

        if (auto materialComponent = entity["MaterialComponent"]; materialComponent)
        {
            auto& matc = deserializedEntity.AddComponent<MaterialComponent>();
            if (materialComponent["AlbedoColor"])
            {
                auto albedo = materialComponent["AlbedoColor"].as<glm::vec3>();
                matc.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            }
            if (materialComponent["Metallic"])
            {
                matc.m_Material.SetMetallicFactor(materialComponent["Metallic"].as<f32>());
            }
            if (materialComponent["Roughness"])
            {
                matc.m_Material.SetRoughnessFactor(materialComponent["Roughness"].as<f32>());
            }
            if (materialComponent["ShaderGraphHandle"])
            {
                auto handleVal = materialComponent["ShaderGraphHandle"].as<u64>();
                if (handleVal != 0)
                {
                    OLO_PROFILE_SCOPE("ShaderGraphLoad");
                    // Compile and apply the shader graph if the asset is available
                    if (auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(UUID(handleVal)))
                    {
                        if (auto shader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(handleVal)))
                        {
                            matc.m_ShaderGraphHandle = UUID(handleVal);
                            matc.m_Material.SetShader(shader);
                        }
                        else
                        {
                            OLO_CORE_WARN("SceneSerializer: ShaderGraph {} failed to compile", handleVal);
                        }
                    }
                    else
                    {
                        OLO_CORE_WARN("SceneSerializer: ShaderGraph asset {} not found", handleVal);
                    }
                }
            }
        }

        if (auto dirLightComponent = entity["DirectionalLightComponent"]; dirLightComponent)
        {
            auto& dirLight = deserializedEntity.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = dirLightComponent["Direction"].as<glm::vec3>(dirLight.m_Direction);
            dirLight.m_Color = dirLightComponent["Color"].as<glm::vec3>(dirLight.m_Color);
            dirLight.m_Intensity = dirLightComponent["Intensity"].as<f32>(dirLight.m_Intensity);
            dirLight.m_CastShadows = dirLightComponent["CastShadows"].as<bool>(dirLight.m_CastShadows);
            dirLight.m_ShadowBias = dirLightComponent["ShadowBias"].as<f32>(dirLight.m_ShadowBias);
            dirLight.m_ShadowNormalBias = dirLightComponent["ShadowNormalBias"].as<f32>(dirLight.m_ShadowNormalBias);
            dirLight.m_MaxShadowDistance = dirLightComponent["MaxShadowDistance"].as<f32>(dirLight.m_MaxShadowDistance);
            dirLight.m_CascadeSplitLambda = dirLightComponent["CascadeSplitLambda"].as<f32>(dirLight.m_CascadeSplitLambda);
            dirLight.m_CascadeDebugVisualization = dirLightComponent["CascadeDebugVisualization"].as<bool>(dirLight.m_CascadeDebugVisualization);
        }

        if (auto pointLightComponent = entity["PointLightComponent"]; pointLightComponent)
        {
            auto& pointLight = deserializedEntity.AddComponent<PointLightComponent>();
            pointLight.m_Color = pointLightComponent["Color"].as<glm::vec3>(pointLight.m_Color);
            pointLight.m_Intensity = pointLightComponent["Intensity"].as<f32>(pointLight.m_Intensity);
            pointLight.m_Range = pointLightComponent["Range"].as<f32>(pointLight.m_Range);
            pointLight.m_Attenuation = pointLightComponent["Attenuation"].as<f32>(pointLight.m_Attenuation);
            pointLight.m_CastShadows = pointLightComponent["CastShadows"].as<bool>(pointLight.m_CastShadows);
            pointLight.m_ShadowBias = pointLightComponent["ShadowBias"].as<f32>(pointLight.m_ShadowBias);
            pointLight.m_ShadowNormalBias = pointLightComponent["ShadowNormalBias"].as<f32>(pointLight.m_ShadowNormalBias);
        }

        if (auto spotLightComponent = entity["SpotLightComponent"]; spotLightComponent)
        {
            auto& spotLight = deserializedEntity.AddComponent<SpotLightComponent>();
            spotLight.m_Direction = spotLightComponent["Direction"].as<glm::vec3>(spotLight.m_Direction);
            spotLight.m_Color = spotLightComponent["Color"].as<glm::vec3>(spotLight.m_Color);
            spotLight.m_Intensity = spotLightComponent["Intensity"].as<f32>(spotLight.m_Intensity);
            spotLight.m_Range = spotLightComponent["Range"].as<f32>(spotLight.m_Range);
            spotLight.m_InnerCutoff = spotLightComponent["InnerCutoff"].as<f32>(spotLight.m_InnerCutoff);
            spotLight.m_OuterCutoff = spotLightComponent["OuterCutoff"].as<f32>(spotLight.m_OuterCutoff);
            spotLight.m_Attenuation = spotLightComponent["Attenuation"].as<f32>(spotLight.m_Attenuation);
            spotLight.m_CastShadows = spotLightComponent["CastShadows"].as<bool>(spotLight.m_CastShadows);
            spotLight.m_ShadowBias = spotLightComponent["ShadowBias"].as<f32>(spotLight.m_ShadowBias);
            spotLight.m_ShadowNormalBias = spotLightComponent["ShadowNormalBias"].as<f32>(spotLight.m_ShadowNormalBias);
        }

        if (auto sphereAreaLightComponent = entity["SphereAreaLightComponent"]; sphereAreaLightComponent)
        {
            auto& areaLight = deserializedEntity.AddComponent<SphereAreaLightComponent>();
            if (const auto color = sphereAreaLightComponent["Color"].as<glm::vec3>(areaLight.m_Color); std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z))
                areaLight.m_Color = color;
            if (const f32 intensity = sphereAreaLightComponent["Intensity"].as<f32>(areaLight.m_Intensity); std::isfinite(intensity) && intensity >= 0.0f)
                areaLight.m_Intensity = intensity;
            if (const f32 radius = sphereAreaLightComponent["Radius"].as<f32>(areaLight.m_Radius); std::isfinite(radius) && radius >= 0.0f)
                areaLight.m_Radius = radius;
            if (const f32 range = sphereAreaLightComponent["Range"].as<f32>(areaLight.m_Range); std::isfinite(range) && range >= 0.0f)
                areaLight.m_Range = range;
            areaLight.m_CastShadows = sphereAreaLightComponent["CastShadows"].as<bool>(areaLight.m_CastShadows);
        }

        if (auto procSky = entity["ProceduralSkyComponent"]; procSky)
        {
            auto& sky = deserializedEntity.AddComponent<ProceduralSkyComponent>();
            const auto sunDir = procSky["SunDirection"].as<glm::vec3>(sky.m_SunDirection);
            if (std::isfinite(sunDir.x) && std::isfinite(sunDir.y) && std::isfinite(sunDir.z))
                sky.m_SunDirection = sunDir;
            const f32 turbidity = procSky["Turbidity"].as<f32>(sky.m_Turbidity);
            if (std::isfinite(turbidity) && turbidity > 0.0f)
                sky.m_Turbidity = turbidity;
            const f32 exposure = procSky["Exposure"].as<f32>(sky.m_Exposure);
            if (std::isfinite(exposure) && exposure >= 0.0f)
                sky.m_Exposure = exposure;
            const f32 sunIntensity = procSky["SunIntensity"].as<f32>(sky.m_SunIntensity);
            if (std::isfinite(sunIntensity) && sunIntensity >= 0.0f)
                sky.m_SunIntensity = sunIntensity;
            const f32 sunDiskSize = procSky["SunDiskSize"].as<f32>(sky.m_SunDiskSize);
            if (std::isfinite(sunDiskSize) && sunDiskSize > 0.0f)
                sky.m_SunDiskSize = sunDiskSize;
            sky.m_ShowSunDisk = procSky["ShowSunDisk"].as<bool>(sky.m_ShowSunDisk);
            sky.m_LinkSunToDirectionalLight = procSky["LinkSunToDirectionalLight"].as<bool>(sky.m_LinkSunToDirectionalLight);
            sky.m_EnableSkybox = procSky["EnableSkybox"].as<bool>(sky.m_EnableSkybox);
            sky.m_EnableIBL = procSky["EnableIBL"].as<bool>(sky.m_EnableIBL);
            const f32 iblIntensity = procSky["IBLIntensity"].as<f32>(sky.m_IBLIntensity);
            if (std::isfinite(iblIntensity) && iblIntensity >= 0.0f)
                sky.m_IBLIntensity = iblIntensity;
            const u32 res = procSky["CubemapResolution"].as<u32>(sky.m_CubemapResolution);
            if (res >= 8u && res <= 4096u)
                sky.m_CubemapResolution = res;
        }

        if (auto starSky = entity["StarNestSkyComponent"]; starSky)
        {
            auto& sky = deserializedEntity.AddComponent<StarNestSkyComponent>();
            const auto offset = starSky["Offset"].as<glm::vec3>(sky.m_Offset);
            if (std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(offset.z))
                sky.m_Offset = offset;
            const f32 rotation1 = starSky["Rotation1"].as<f32>(sky.m_Rotation1);
            if (std::isfinite(rotation1))
                sky.m_Rotation1 = rotation1;
            const f32 rotation2 = starSky["Rotation2"].as<f32>(sky.m_Rotation2);
            if (std::isfinite(rotation2))
                sky.m_Rotation2 = rotation2;
            const f32 formuparam = starSky["Formuparam"].as<f32>(sky.m_Formuparam);
            if (std::isfinite(formuparam))
                sky.m_Formuparam = formuparam;
            const f32 stepSize = starSky["StepSize"].as<f32>(sky.m_StepSize);
            if (std::isfinite(stepSize) && stepSize > 0.0f)
                sky.m_StepSize = stepSize;
            const f32 tile = starSky["Tile"].as<f32>(sky.m_Tile);
            if (std::isfinite(tile) && tile > 0.0f)
                sky.m_Tile = tile;
            const f32 brightness = starSky["Brightness"].as<f32>(sky.m_Brightness);
            if (std::isfinite(brightness) && brightness >= 0.0f)
                sky.m_Brightness = brightness;
            const f32 darkMatter = starSky["DarkMatter"].as<f32>(sky.m_DarkMatter);
            if (std::isfinite(darkMatter) && darkMatter >= 0.0f)
                sky.m_DarkMatter = darkMatter;
            const f32 distFading = starSky["DistFading"].as<f32>(sky.m_DistFading);
            if (std::isfinite(distFading) && distFading >= 0.0f)
                sky.m_DistFading = distFading;
            const f32 saturation = starSky["Saturation"].as<f32>(sky.m_Saturation);
            if (std::isfinite(saturation) && saturation >= 0.0f)
                sky.m_Saturation = saturation;
            const f32 intensity = starSky["Intensity"].as<f32>(sky.m_Intensity);
            if (std::isfinite(intensity) && intensity >= 0.0f)
                sky.m_Intensity = intensity;
            const i32 iterations = starSky["Iterations"].as<i32>(sky.m_Iterations);
            if (iterations >= 1 && iterations <= kStarNestMaxIterations)
                sky.m_Iterations = iterations;
            const i32 volSteps = starSky["VolSteps"].as<i32>(sky.m_VolSteps);
            if (volSteps >= 1 && volSteps <= kStarNestMaxVolSteps)
                sky.m_VolSteps = volSteps;
            sky.m_EnableSkybox = starSky["EnableSkybox"].as<bool>(sky.m_EnableSkybox);
            sky.m_EnableIBL = starSky["EnableIBL"].as<bool>(sky.m_EnableIBL);
            const f32 iblIntensity = starSky["IBLIntensity"].as<f32>(sky.m_IBLIntensity);
            if (std::isfinite(iblIntensity) && iblIntensity >= 0.0f)
                sky.m_IBLIntensity = iblIntensity;
            const u32 res = starSky["CubemapResolution"].as<u32>(sky.m_CubemapResolution);
            if (res >= 8u && res <= 4096u)
                sky.m_CubemapResolution = res;
        }

        if (auto envMapComponent = entity["EnvironmentMapComponent"]; envMapComponent)
        {
            auto& envMap = deserializedEntity.AddComponent<EnvironmentMapComponent>();
            envMap.m_FilePath = envMapComponent["FilePath"].as<std::string>(envMap.m_FilePath);
            envMap.m_IsCubemapFolder = envMapComponent["IsCubemapFolder"].as<bool>(envMap.m_IsCubemapFolder);
            envMap.m_EnableSkybox = envMapComponent["EnableSkybox"].as<bool>(envMap.m_EnableSkybox);
            envMap.m_Rotation = envMapComponent["Rotation"].as<f32>(envMap.m_Rotation);
            envMap.m_Exposure = envMapComponent["Exposure"].as<f32>(envMap.m_Exposure);
            envMap.m_BlurAmount = envMapComponent["BlurAmount"].as<f32>(envMap.m_BlurAmount);
            envMap.m_EnableIBL = envMapComponent["EnableIBL"].as<bool>(envMap.m_EnableIBL);
            envMap.m_IBLIntensity = envMapComponent["IBLIntensity"].as<f32>(envMap.m_IBLIntensity);
            envMap.m_UseSphericalHarmonics = envMapComponent["UseSphericalHarmonics"].as<bool>(envMap.m_UseSphericalHarmonics);
            envMap.m_Tint = envMapComponent["Tint"].as<glm::vec3>(envMap.m_Tint);
        }

        if (auto rb3dComponent = entity["Rigidbody3DComponent"]; rb3dComponent)
        {
            auto& rb3d = deserializedEntity.AddComponent<Rigidbody3DComponent>();
            rb3d.m_Type = static_cast<BodyType3D>(rb3dComponent["BodyType"].as<int>(std::to_underlying(rb3d.m_Type)));
            rb3d.m_Mass = rb3dComponent["Mass"].as<f32>(rb3d.m_Mass);
            rb3d.m_LinearDrag = rb3dComponent["LinearDrag"].as<f32>(rb3d.m_LinearDrag);
            rb3d.m_AngularDrag = rb3dComponent["AngularDrag"].as<f32>(rb3d.m_AngularDrag);
            rb3d.m_DisableGravity = rb3dComponent["DisableGravity"].as<bool>(rb3d.m_DisableGravity);
            rb3d.m_IsTrigger = rb3dComponent["IsTrigger"].as<bool>(rb3d.m_IsTrigger);
        }

        if (auto bc3dComponent = entity["BoxCollider3DComponent"]; bc3dComponent)
        {
            auto& bc3d = deserializedEntity.AddComponent<BoxCollider3DComponent>();
            bc3d.m_HalfExtents = bc3dComponent["HalfExtents"].as<glm::vec3>(bc3d.m_HalfExtents);
            bc3d.m_Offset = bc3dComponent["Offset"].as<glm::vec3>(bc3d.m_Offset);
            if (bc3dComponent["StaticFriction"])
                bc3d.m_Material.SetStaticFriction(bc3dComponent["StaticFriction"].as<f32>());
            if (bc3dComponent["DynamicFriction"])
                bc3d.m_Material.SetDynamicFriction(bc3dComponent["DynamicFriction"].as<f32>());
            if (bc3dComponent["Restitution"])
                bc3d.m_Material.SetRestitution(bc3dComponent["Restitution"].as<f32>());
        }

        if (auto sc3dComponent = entity["SphereCollider3DComponent"]; sc3dComponent)
        {
            auto& sc3d = deserializedEntity.AddComponent<SphereCollider3DComponent>();
            sc3d.m_Radius = sc3dComponent["Radius"].as<f32>(sc3d.m_Radius);
            sc3d.m_Offset = sc3dComponent["Offset"].as<glm::vec3>(sc3d.m_Offset);
            if (sc3dComponent["StaticFriction"])
                sc3d.m_Material.SetStaticFriction(sc3dComponent["StaticFriction"].as<f32>());
            if (sc3dComponent["DynamicFriction"])
                sc3d.m_Material.SetDynamicFriction(sc3dComponent["DynamicFriction"].as<f32>());
            if (sc3dComponent["Restitution"])
                sc3d.m_Material.SetRestitution(sc3dComponent["Restitution"].as<f32>());
        }

        if (auto cc3dComponent = entity["CapsuleCollider3DComponent"]; cc3dComponent)
        {
            auto& cc3d = deserializedEntity.AddComponent<CapsuleCollider3DComponent>();
            cc3d.m_Radius = cc3dComponent["Radius"].as<f32>(cc3d.m_Radius);
            cc3d.m_HalfHeight = cc3dComponent["HalfHeight"].as<f32>(cc3d.m_HalfHeight);
            cc3d.m_Offset = cc3dComponent["Offset"].as<glm::vec3>(cc3d.m_Offset);
            if (cc3dComponent["StaticFriction"])
                cc3d.m_Material.SetStaticFriction(cc3dComponent["StaticFriction"].as<f32>());
            if (cc3dComponent["DynamicFriction"])
                cc3d.m_Material.SetDynamicFriction(cc3dComponent["DynamicFriction"].as<f32>());
            if (cc3dComponent["Restitution"])
                cc3d.m_Material.SetRestitution(cc3dComponent["Restitution"].as<f32>());
        }

        if (auto prefabComponent = entity["PrefabComponent"]; prefabComponent)
        {
            auto& pc = deserializedEntity.AddComponent<PrefabComponent>();
            pc.m_PrefabID = prefabComponent["PrefabID"].as<u64>();
            pc.m_PrefabEntityID = prefabComponent["PrefabEntityID"].as<u64>();

            // Deserialize override tracking
            if (auto overridden = prefabComponent["OverriddenComponents"]; overridden && overridden.IsSequence())
            {
                for (const auto& name : overridden)
                {
                    if (name.IsDefined() && name.IsScalar())
                        pc.m_OverriddenComponents.insert(name.as<std::string>());
                }
            }
            if (auto added = prefabComponent["AddedComponents"]; added && added.IsSequence())
            {
                for (const auto& name : added)
                {
                    if (name.IsDefined() && name.IsScalar())
                        pc.m_AddedComponents.insert(name.as<std::string>());
                }
            }
            if (auto removed = prefabComponent["RemovedComponents"]; removed && removed.IsSequence())
            {
                for (const auto& name : removed)
                {
                    if (name.IsDefined() && name.IsScalar())
                        pc.m_RemovedComponents.insert(name.as<std::string>());
                }
            }
        }

        if (auto mc3dComponent = entity["MeshCollider3DComponent"]; mc3dComponent)
        {
            auto& mc3d = deserializedEntity.AddComponent<MeshCollider3DComponent>();
            if (mc3dComponent["ColliderAsset"])
                mc3d.m_ColliderAsset = mc3dComponent["ColliderAsset"].as<u64>();
            mc3d.m_Offset = mc3dComponent["Offset"].as<glm::vec3>(mc3d.m_Offset);
            mc3d.m_Scale = mc3dComponent["Scale"].as<glm::vec3>(mc3d.m_Scale);
            mc3d.m_UseComplexAsSimple = mc3dComponent["UseComplexAsSimple"].as<bool>(mc3d.m_UseComplexAsSimple);
            if (mc3dComponent["StaticFriction"])
                mc3d.m_Material.SetStaticFriction(mc3dComponent["StaticFriction"].as<f32>());
            if (mc3dComponent["DynamicFriction"])
                mc3d.m_Material.SetDynamicFriction(mc3dComponent["DynamicFriction"].as<f32>());
            if (mc3dComponent["Restitution"])
                mc3d.m_Material.SetRestitution(mc3dComponent["Restitution"].as<f32>());
        }

        if (auto cmc3dComponent = entity["ConvexMeshCollider3DComponent"]; cmc3dComponent)
        {
            auto& cmc3d = deserializedEntity.AddComponent<ConvexMeshCollider3DComponent>();
            if (cmc3dComponent["ColliderAsset"])
                cmc3d.m_ColliderAsset = cmc3dComponent["ColliderAsset"].as<u64>();
            cmc3d.m_Offset = cmc3dComponent["Offset"].as<glm::vec3>(cmc3d.m_Offset);
            cmc3d.m_Scale = cmc3dComponent["Scale"].as<glm::vec3>(cmc3d.m_Scale);
            cmc3d.m_ConvexRadius = cmc3dComponent["ConvexRadius"].as<f32>(cmc3d.m_ConvexRadius);
            cmc3d.m_MaxVertices = cmc3dComponent["MaxVertices"].as<u32>(cmc3d.m_MaxVertices);
            if (cmc3dComponent["StaticFriction"])
                cmc3d.m_Material.SetStaticFriction(cmc3dComponent["StaticFriction"].as<f32>());
            if (cmc3dComponent["DynamicFriction"])
                cmc3d.m_Material.SetDynamicFriction(cmc3dComponent["DynamicFriction"].as<f32>());
            if (cmc3dComponent["Restitution"])
                cmc3d.m_Material.SetRestitution(cmc3dComponent["Restitution"].as<f32>());
        }

        if (auto tmc3dComponent = entity["TriangleMeshCollider3DComponent"]; tmc3dComponent)
        {
            auto& tmc3d = deserializedEntity.AddComponent<TriangleMeshCollider3DComponent>();
            if (tmc3dComponent["ColliderAsset"])
                tmc3d.m_ColliderAsset = tmc3dComponent["ColliderAsset"].as<u64>();
            tmc3d.m_Offset = tmc3dComponent["Offset"].as<glm::vec3>(tmc3d.m_Offset);
            tmc3d.m_Scale = tmc3dComponent["Scale"].as<glm::vec3>(tmc3d.m_Scale);
            if (tmc3dComponent["StaticFriction"])
                tmc3d.m_Material.SetStaticFriction(tmc3dComponent["StaticFriction"].as<f32>());
            if (tmc3dComponent["DynamicFriction"])
                tmc3d.m_Material.SetDynamicFriction(tmc3dComponent["DynamicFriction"].as<f32>());
            if (tmc3dComponent["Restitution"])
                tmc3d.m_Material.SetRestitution(tmc3dComponent["Restitution"].as<f32>());
        }

        if (auto cc3dComponent = entity["CharacterController3DComponent"]; cc3dComponent)
        {
            auto& cc3d = deserializedEntity.AddComponent<CharacterController3DComponent>();
            cc3d.m_SlopeLimitDeg = cc3dComponent["SlopeLimitDeg"].as<f32>(cc3d.m_SlopeLimitDeg);
            cc3d.m_StepOffset = cc3dComponent["StepOffset"].as<f32>(cc3d.m_StepOffset);
            cc3d.m_JumpPower = cc3dComponent["JumpPower"].as<f32>(cc3d.m_JumpPower);
            cc3d.m_LayerID = cc3dComponent["LayerID"].as<u32>(cc3d.m_LayerID);
            cc3d.m_DisableGravity = cc3dComponent["DisableGravity"].as<bool>(cc3d.m_DisableGravity);
            cc3d.m_ControlMovementInAir = cc3dComponent["ControlMovementInAir"].as<bool>(cc3d.m_ControlMovementInAir);
            cc3d.m_ControlRotationInAir = cc3dComponent["ControlRotationInAir"].as<bool>(cc3d.m_ControlRotationInAir);
        }

        if (auto jointComponent = entity["PhysicsJoint3DComponent"]; jointComponent)
        {
            auto& joint = deserializedEntity.AddComponent<PhysicsJoint3DComponent>();

            // Guard the joint type against an out-of-range enum value on disk.
            if (i32 jointTypeInt = jointComponent["JointType"].as<i32>(std::to_underlying(joint.m_Type));
                jointTypeInt >= 0 && jointTypeInt <= static_cast<i32>(JointType3D::SixDOF))
            {
                joint.m_Type = static_cast<JointType3D>(jointTypeInt);
            }

            joint.m_ConnectedEntity = jointComponent["ConnectedEntity"].as<u64>(static_cast<u64>(joint.m_ConnectedEntity));

            // glm::vec3 decode already rejects non-finite components (falls back to default).
            joint.m_LocalAnchorA = jointComponent["LocalAnchorA"].as<glm::vec3>(joint.m_LocalAnchorA);
            joint.m_LocalAnchorB = jointComponent["LocalAnchorB"].as<glm::vec3>(joint.m_LocalAnchorB);
            joint.m_Axis = jointComponent["Axis"].as<glm::vec3>(joint.m_Axis);

            joint.m_MinDistance = jointComponent["MinDistance"].as<f32>(joint.m_MinDistance);
            joint.m_MaxDistance = jointComponent["MaxDistance"].as<f32>(joint.m_MaxDistance);
            joint.m_HingeMinAngleDeg = jointComponent["HingeMinAngleDeg"].as<f32>(joint.m_HingeMinAngleDeg);
            joint.m_HingeMaxAngleDeg = jointComponent["HingeMaxAngleDeg"].as<f32>(joint.m_HingeMaxAngleDeg);
            joint.m_SliderMinLimit = jointComponent["SliderMinLimit"].as<f32>(joint.m_SliderMinLimit);
            joint.m_SliderMaxLimit = jointComponent["SliderMaxLimit"].as<f32>(joint.m_SliderMaxLimit);
            joint.m_ConeHalfAngleDeg = jointComponent["ConeHalfAngleDeg"].as<f32>(joint.m_ConeHalfAngleDeg);
            joint.m_BreakForce = jointComponent["BreakForce"].as<f32>(joint.m_BreakForce);
            joint.m_BreakTorque = jointComponent["BreakTorque"].as<f32>(joint.m_BreakTorque);

            // Motor + friction. The motor mode is an enum stored as int; guard it
            // against an out-of-range value on disk, the same way m_Type is guarded.
            if (i32 hingeModeInt = jointComponent["HingeMotorMode"].as<i32>(std::to_underlying(joint.m_HingeMotorMode));
                hingeModeInt >= 0 && hingeModeInt <= static_cast<i32>(JointMotorMode::Position))
            {
                joint.m_HingeMotorMode = static_cast<JointMotorMode>(hingeModeInt);
            }
            joint.m_HingeMotorTargetVelocityDeg = jointComponent["HingeMotorTargetVelocityDeg"].as<f32>(joint.m_HingeMotorTargetVelocityDeg);
            joint.m_HingeMotorTargetAngleDeg = jointComponent["HingeMotorTargetAngleDeg"].as<f32>(joint.m_HingeMotorTargetAngleDeg);
            joint.m_HingeMaxMotorTorque = jointComponent["HingeMaxMotorTorque"].as<f32>(joint.m_HingeMaxMotorTorque);
            joint.m_HingeMaxFrictionTorque = jointComponent["HingeMaxFrictionTorque"].as<f32>(joint.m_HingeMaxFrictionTorque);
            joint.m_HingeLimitSpringFrequency = jointComponent["HingeLimitSpringFrequency"].as<f32>(joint.m_HingeLimitSpringFrequency);
            joint.m_HingeLimitSpringDamping = jointComponent["HingeLimitSpringDamping"].as<f32>(joint.m_HingeLimitSpringDamping);

            if (i32 sliderModeInt = jointComponent["SliderMotorMode"].as<i32>(std::to_underlying(joint.m_SliderMotorMode));
                sliderModeInt >= 0 && sliderModeInt <= static_cast<i32>(JointMotorMode::Position))
            {
                joint.m_SliderMotorMode = static_cast<JointMotorMode>(sliderModeInt);
            }
            joint.m_SliderMotorTargetVelocity = jointComponent["SliderMotorTargetVelocity"].as<f32>(joint.m_SliderMotorTargetVelocity);
            joint.m_SliderMotorTargetPosition = jointComponent["SliderMotorTargetPosition"].as<f32>(joint.m_SliderMotorTargetPosition);
            joint.m_SliderMaxMotorForce = jointComponent["SliderMaxMotorForce"].as<f32>(joint.m_SliderMaxMotorForce);
            joint.m_SliderMaxFrictionForce = jointComponent["SliderMaxFrictionForce"].as<f32>(joint.m_SliderMaxFrictionForce);
            joint.m_SliderLimitSpringFrequency = jointComponent["SliderLimitSpringFrequency"].as<f32>(joint.m_SliderLimitSpringFrequency);
            joint.m_SliderLimitSpringDamping = jointComponent["SliderLimitSpringDamping"].as<f32>(joint.m_SliderLimitSpringDamping);

            // SwingTwist swing cone half-angles + twist range (degrees).
            joint.m_SwingNormalHalfAngleDeg = jointComponent["SwingNormalHalfAngleDeg"].as<f32>(joint.m_SwingNormalHalfAngleDeg);
            joint.m_SwingPlaneHalfAngleDeg = jointComponent["SwingPlaneHalfAngleDeg"].as<f32>(joint.m_SwingPlaneHalfAngleDeg);
            joint.m_TwistMinAngleDeg = jointComponent["TwistMinAngleDeg"].as<f32>(joint.m_TwistMinAngleDeg);
            joint.m_TwistMaxAngleDeg = jointComponent["TwistMaxAngleDeg"].as<f32>(joint.m_TwistMaxAngleDeg);

            // SixDOF per-axis modes. Each is an int enum on disk; guard against an
            // out-of-range value the same way m_Type is guarded.
            const auto readAxisMode = [&](const char* key, JointAxisMode& mode)
            {
                if (i32 v = jointComponent[key].as<i32>(std::to_underlying(mode));
                    v >= 0 && v <= static_cast<i32>(JointAxisMode::Free))
                {
                    mode = static_cast<JointAxisMode>(v);
                }
            };
            readAxisMode("SixDOFTransXMode", joint.m_SixDOFTransXMode);
            readAxisMode("SixDOFTransYMode", joint.m_SixDOFTransYMode);
            readAxisMode("SixDOFTransZMode", joint.m_SixDOFTransZMode);
            readAxisMode("SixDOFRotXMode", joint.m_SixDOFRotXMode);
            readAxisMode("SixDOFRotYMode", joint.m_SixDOFRotYMode);
            readAxisMode("SixDOFRotZMode", joint.m_SixDOFRotZMode);

            // SixDOF limits. glm::vec3 decode already rejects non-finite components.
            joint.m_SixDOFTranslationMin = jointComponent["SixDOFTranslationMin"].as<glm::vec3>(joint.m_SixDOFTranslationMin);
            joint.m_SixDOFTranslationMax = jointComponent["SixDOFTranslationMax"].as<glm::vec3>(joint.m_SixDOFTranslationMax);
            joint.m_SixDOFRotationMinDeg = jointComponent["SixDOFRotationMinDeg"].as<glm::vec3>(joint.m_SixDOFRotationMinDeg);
            joint.m_SixDOFRotationMaxDeg = jointComponent["SixDOFRotationMaxDeg"].as<glm::vec3>(joint.m_SixDOFRotationMaxDeg);

            // Reject non-finite floats read from disk and clamp to physically/Jolt-valid ranges.
            SanitizeFloat(joint.m_MinDistance, -1.0f, 10000.0f, 0.0f);
            SanitizeFloat(joint.m_MaxDistance, -1.0f, 10000.0f, 1.0f);
            SanitizeFloat(joint.m_HingeMinAngleDeg, -180.0f, 0.0f, -180.0f);
            SanitizeFloat(joint.m_HingeMaxAngleDeg, 0.0f, 180.0f, 180.0f);
            SanitizeFloat(joint.m_SliderMinLimit, -10000.0f, 10000.0f, 0.0f);
            SanitizeFloat(joint.m_SliderMaxLimit, -10000.0f, 10000.0f, 1.0f);
            SanitizeFloat(joint.m_ConeHalfAngleDeg, 0.0f, 180.0f, 45.0f);
            // Break thresholds: a non-positive value is the "disabled" sentinel,
            // so clamp negatives to 0 and reject non-finite to 0 (unbreakable).
            SanitizeFloat(joint.m_BreakForce, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_BreakTorque, 0.0f, 1.0e9f, 0.0f);
            // Motor targets are signed (a negative velocity/angle/position is
            // valid); only reject non-finite. Max torque/force and friction are
            // magnitudes — clamp to [0, 1e9]; 0 = no authority / no friction.
            SanitizeFloat(joint.m_HingeMotorTargetVelocityDeg, -1.0e9f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_HingeMotorTargetAngleDeg, -360.0f, 360.0f, 0.0f);
            SanitizeFloat(joint.m_HingeMaxMotorTorque, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_HingeMaxFrictionTorque, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_SliderMotorTargetVelocity, -1.0e9f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_SliderMotorTargetPosition, -10000.0f, 10000.0f, 0.0f);
            SanitizeFloat(joint.m_SliderMaxMotorForce, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_SliderMaxFrictionForce, 0.0f, 1.0e9f, 0.0f);
            // Limit-spring frequency (Hz) and damping ratio are magnitudes —
            // clamp to [0, 1e9]; 0 = hard limits / no damping.
            SanitizeFloat(joint.m_HingeLimitSpringFrequency, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_HingeLimitSpringDamping, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_SliderLimitSpringFrequency, 0.0f, 1.0e9f, 0.0f);
            SanitizeFloat(joint.m_SliderLimitSpringDamping, 0.0f, 1.0e9f, 0.0f);
            // SwingTwist: swing half-angles clamp to [0,180]; twist to [-180,180].
            SanitizeFloat(joint.m_SwingNormalHalfAngleDeg, 0.0f, 180.0f, 45.0f);
            SanitizeFloat(joint.m_SwingPlaneHalfAngleDeg, 0.0f, 180.0f, 45.0f);
            SanitizeFloat(joint.m_TwistMinAngleDeg, -180.0f, 180.0f, -45.0f);
            SanitizeFloat(joint.m_TwistMaxAngleDeg, -180.0f, 180.0f, 45.0f);
            // SixDOF limits — clamp each component to a sane range (translation in
            // meters, rotation in degrees). The vec3 decode above already rejected
            // non-finite components; this guards against absurd-but-finite values.
            const auto sanitizeVec3 = [](glm::vec3& v, f32 lo, f32 hi, f32 fallback)
            {
                SanitizeFloat(v.x, lo, hi, fallback);
                SanitizeFloat(v.y, lo, hi, fallback);
                SanitizeFloat(v.z, lo, hi, fallback);
            };
            sanitizeVec3(joint.m_SixDOFTranslationMin, -10000.0f, 10000.0f, -0.5f);
            sanitizeVec3(joint.m_SixDOFTranslationMax, -10000.0f, 10000.0f, 0.5f);
            sanitizeVec3(joint.m_SixDOFRotationMinDeg, -180.0f, 180.0f, -45.0f);
            sanitizeVec3(joint.m_SixDOFRotationMaxDeg, -180.0f, 180.0f, 45.0f);
        }

        if (auto relComponent = entity["RelationshipComponent"]; relComponent)
        {
            auto& rel = deserializedEntity.AddComponent<RelationshipComponent>();
            if (relComponent["ParentHandle"])
                rel.m_ParentHandle = relComponent["ParentHandle"].as<u64>();
            if (auto children = relComponent["Children"]; children)
            {
                for (auto child : children)
                {
                    rel.m_Children.push_back(child.as<u64>());
                }
            }
        }

        if (auto uiCanvasComponent = entity["UICanvasComponent"]; uiCanvasComponent)
        {
            auto& canvas = deserializedEntity.AddComponent<UICanvasComponent>();
            TrySetEnum(canvas.m_RenderMode, uiCanvasComponent["RenderMode"]);
            TrySetEnum(canvas.m_ScaleMode, uiCanvasComponent["ScaleMode"]);
            TrySet(canvas.m_SortOrder, uiCanvasComponent["SortOrder"]);
            TrySet(canvas.m_ReferenceResolution, uiCanvasComponent["ReferenceResolution"]);
        }

        if (auto uiRectTransformComponent = entity["UIRectTransformComponent"]; uiRectTransformComponent)
        {
            auto& rt = deserializedEntity.AddComponent<UIRectTransformComponent>();
            TrySet(rt.m_AnchorMin, uiRectTransformComponent["AnchorMin"]);
            TrySet(rt.m_AnchorMax, uiRectTransformComponent["AnchorMax"]);
            TrySet(rt.m_AnchoredPosition, uiRectTransformComponent["AnchoredPosition"]);
            TrySet(rt.m_SizeDelta, uiRectTransformComponent["SizeDelta"]);
            TrySet(rt.m_Pivot, uiRectTransformComponent["Pivot"]);
            TrySet(rt.m_Rotation, uiRectTransformComponent["Rotation"]);
            TrySet(rt.m_Scale, uiRectTransformComponent["Scale"]);
        }

        if (auto uiImageComponent = entity["UIImageComponent"]; uiImageComponent)
        {
            auto& image = deserializedEntity.AddComponent<UIImageComponent>();
            if (uiImageComponent["TexturePath"])
            {
                image.m_Texture = LoadSceneTexture(uiImageComponent["TexturePath"].as<std::string>());
            }
            TrySet(image.m_Color, uiImageComponent["Color"]);
            TrySet(image.m_BorderInsets, uiImageComponent["BorderInsets"]);
        }

        if (auto uiPanelComponent = entity["UIPanelComponent"]; uiPanelComponent)
        {
            auto& panel = deserializedEntity.AddComponent<UIPanelComponent>();
            TrySet(panel.m_BackgroundColor, uiPanelComponent["BackgroundColor"]);
            if (uiPanelComponent["BackgroundTexturePath"])
            {
                panel.m_BackgroundTexture = LoadSceneTexture(uiPanelComponent["BackgroundTexturePath"].as<std::string>());
            }
        }

        if (auto uiTextComponent = entity["UITextComponent"]; uiTextComponent)
        {
            auto& text = deserializedEntity.AddComponent<UITextComponent>();
            TrySet(text.m_Text, uiTextComponent["Text"]);
            if (uiTextComponent["FontPath"])
            {
                text.m_FontAsset = Font::Create(uiTextComponent["FontPath"].as<std::string>());
            }
            TrySet(text.m_FontSize, uiTextComponent["FontSize"]);
            TrySet(text.m_Color, uiTextComponent["Color"]);
            TrySetEnum(text.m_Alignment, uiTextComponent["Alignment"]);
            TrySet(text.m_Kerning, uiTextComponent["Kerning"]);
            TrySet(text.m_LineSpacing, uiTextComponent["LineSpacing"]);
        }

        if (auto uiButtonComponent = entity["UIButtonComponent"]; uiButtonComponent)
        {
            auto& button = deserializedEntity.AddComponent<UIButtonComponent>();
            TrySet(button.m_NormalColor, uiButtonComponent["NormalColor"]);
            TrySet(button.m_HoveredColor, uiButtonComponent["HoveredColor"]);
            TrySet(button.m_PressedColor, uiButtonComponent["PressedColor"]);
            TrySet(button.m_DisabledColor, uiButtonComponent["DisabledColor"]);
            TrySet(button.m_Interactable, uiButtonComponent["Interactable"]);
        }

        if (auto uiSliderComponent = entity["UISliderComponent"]; uiSliderComponent)
        {
            auto& slider = deserializedEntity.AddComponent<UISliderComponent>();
            TrySet(slider.m_Value, uiSliderComponent["Value"]);
            TrySet(slider.m_MinValue, uiSliderComponent["MinValue"]);
            TrySet(slider.m_MaxValue, uiSliderComponent["MaxValue"]);
            TrySetEnum(slider.m_Direction, uiSliderComponent["Direction"]);
            TrySet(slider.m_BackgroundColor, uiSliderComponent["BackgroundColor"]);
            TrySet(slider.m_FillColor, uiSliderComponent["FillColor"]);
            TrySet(slider.m_HandleColor, uiSliderComponent["HandleColor"]);
            TrySet(slider.m_Interactable, uiSliderComponent["Interactable"]);
        }

        if (auto uiCheckboxComponent = entity["UICheckboxComponent"]; uiCheckboxComponent)
        {
            auto& checkbox = deserializedEntity.AddComponent<UICheckboxComponent>();
            TrySet(checkbox.m_IsChecked, uiCheckboxComponent["IsChecked"]);
            TrySet(checkbox.m_UncheckedColor, uiCheckboxComponent["UncheckedColor"]);
            TrySet(checkbox.m_CheckedColor, uiCheckboxComponent["CheckedColor"]);
            TrySet(checkbox.m_CheckmarkColor, uiCheckboxComponent["CheckmarkColor"]);
            TrySet(checkbox.m_Interactable, uiCheckboxComponent["Interactable"]);
        }

        if (auto uiProgressBarComponent = entity["UIProgressBarComponent"]; uiProgressBarComponent)
        {
            auto& progress = deserializedEntity.AddComponent<UIProgressBarComponent>();
            TrySet(progress.m_Value, uiProgressBarComponent["Value"]);
            TrySet(progress.m_MinValue, uiProgressBarComponent["MinValue"]);
            TrySet(progress.m_MaxValue, uiProgressBarComponent["MaxValue"]);
            TrySetEnum(progress.m_FillMethod, uiProgressBarComponent["FillMethod"]);
            TrySet(progress.m_BackgroundColor, uiProgressBarComponent["BackgroundColor"]);
            TrySet(progress.m_FillColor, uiProgressBarComponent["FillColor"]);
        }

        if (auto uiWorldAnchorComponent = entity["UIWorldAnchorComponent"]; uiWorldAnchorComponent)
        {
            auto& anchor = deserializedEntity.AddComponent<UIWorldAnchorComponent>();
            TrySet(anchor.m_TargetEntity, uiWorldAnchorComponent["TargetEntity"]);
            TrySet(anchor.m_WorldOffset, uiWorldAnchorComponent["WorldOffset"]);
        }

        if (auto uiInputFieldComponent = entity["UIInputFieldComponent"]; uiInputFieldComponent)
        {
            auto& input = deserializedEntity.AddComponent<UIInputFieldComponent>();
            TrySet(input.m_Text, uiInputFieldComponent["Text"]);
            TrySet(input.m_Placeholder, uiInputFieldComponent["Placeholder"]);
            if (uiInputFieldComponent["FontPath"])
            {
                input.m_FontAsset = Font::Create(uiInputFieldComponent["FontPath"].as<std::string>());
            }
            TrySet(input.m_FontSize, uiInputFieldComponent["FontSize"]);
            TrySet(input.m_TextColor, uiInputFieldComponent["TextColor"]);
            TrySet(input.m_PlaceholderColor, uiInputFieldComponent["PlaceholderColor"]);
            TrySet(input.m_BackgroundColor, uiInputFieldComponent["BackgroundColor"]);
            TrySet(input.m_CharacterLimit, uiInputFieldComponent["CharacterLimit"]);
            TrySet(input.m_Interactable, uiInputFieldComponent["Interactable"]);
        }

        if (auto uiScrollViewComponent = entity["UIScrollViewComponent"]; uiScrollViewComponent)
        {
            auto& scrollView = deserializedEntity.AddComponent<UIScrollViewComponent>();
            TrySet(scrollView.m_ScrollPosition, uiScrollViewComponent["ScrollPosition"]);
            TrySet(scrollView.m_ContentSize, uiScrollViewComponent["ContentSize"]);
            TrySetEnum(scrollView.m_ScrollDirection, uiScrollViewComponent["ScrollDirection"]);
            TrySet(scrollView.m_ScrollSpeed, uiScrollViewComponent["ScrollSpeed"]);
            TrySet(scrollView.m_ShowHorizontalScrollbar, uiScrollViewComponent["ShowHorizontalScrollbar"]);
            TrySet(scrollView.m_ShowVerticalScrollbar, uiScrollViewComponent["ShowVerticalScrollbar"]);
            TrySet(scrollView.m_ScrollbarColor, uiScrollViewComponent["ScrollbarColor"]);
            TrySet(scrollView.m_ScrollbarTrackColor, uiScrollViewComponent["ScrollbarTrackColor"]);
        }

        if (auto uiDropdownComponent = entity["UIDropdownComponent"]; uiDropdownComponent)
        {
            auto& dropdown = deserializedEntity.AddComponent<UIDropdownComponent>();
            if (uiDropdownComponent["Options"])
            {
                for (const auto& optionNode : uiDropdownComponent["Options"])
                {
                    UIDropdownOption option;
                    option.m_Label = optionNode.as<std::string>("");
                    dropdown.m_Options.push_back(option);
                }
            }
            TrySet(dropdown.m_SelectedIndex, uiDropdownComponent["SelectedIndex"]);
            TrySet(dropdown.m_BackgroundColor, uiDropdownComponent["BackgroundColor"]);
            TrySet(dropdown.m_HighlightColor, uiDropdownComponent["HighlightColor"]);
            TrySet(dropdown.m_TextColor, uiDropdownComponent["TextColor"]);
            if (uiDropdownComponent["FontPath"])
            {
                dropdown.m_FontAsset = Font::Create(uiDropdownComponent["FontPath"].as<std::string>());
            }
            TrySet(dropdown.m_FontSize, uiDropdownComponent["FontSize"]);
            TrySet(dropdown.m_ItemHeight, uiDropdownComponent["ItemHeight"]);
            TrySet(dropdown.m_Interactable, uiDropdownComponent["Interactable"]);
        }

        if (auto uiGridLayoutComponent = entity["UIGridLayoutComponent"]; uiGridLayoutComponent)
        {
            auto& grid = deserializedEntity.AddComponent<UIGridLayoutComponent>();
            TrySet(grid.m_CellSize, uiGridLayoutComponent["CellSize"]);
            TrySet(grid.m_Spacing, uiGridLayoutComponent["Spacing"]);
            TrySet(grid.m_Padding, uiGridLayoutComponent["Padding"]);
            TrySetEnum(grid.m_StartCorner, uiGridLayoutComponent["StartCorner"]);
            TrySetEnum(grid.m_StartAxis, uiGridLayoutComponent["StartAxis"]);
            TrySet(grid.m_ConstraintCount, uiGridLayoutComponent["ConstraintCount"]);
        }

        if (auto uiToggleComponent = entity["UIToggleComponent"]; uiToggleComponent)
        {
            auto& toggle = deserializedEntity.AddComponent<UIToggleComponent>();
            TrySet(toggle.m_IsOn, uiToggleComponent["IsOn"]);
            TrySet(toggle.m_OffColor, uiToggleComponent["OffColor"]);
            TrySet(toggle.m_OnColor, uiToggleComponent["OnColor"]);
            TrySet(toggle.m_KnobColor, uiToggleComponent["KnobColor"]);
            TrySet(toggle.m_Interactable, uiToggleComponent["Interactable"]);
        }

        if (auto particleComponent = entity["ParticleSystemComponent"]; particleComponent)
        {
            auto& psc = deserializedEntity.AddComponent<ParticleSystemComponent>();
            DeserializeParticleSystemComponent(psc, particleComponent);
        }

        if (auto terrainComponent = entity["TerrainComponent"]; terrainComponent)
        {
            auto& terrain = deserializedEntity.AddComponent<TerrainComponent>();
            DeserializeTerrainComponent(terrain, terrainComponent);
        }

        if (auto foliageComponent = entity["FoliageComponent"]; foliageComponent)
        {
            auto& foliage = deserializedEntity.AddComponent<FoliageComponent>();
            DeserializeFoliageComponent(foliage, foliageComponent);
        }

        if (auto waterComponent = entity["WaterComponent"]; waterComponent)
        {
            auto& water = deserializedEntity.AddComponent<WaterComponent>();
            DeserializeWaterComponent(water, waterComponent);
        }

        if (auto buoyancyComponent = entity["BuoyancyComponent"]; buoyancyComponent)
        {
            auto& buoyancy = deserializedEntity.AddComponent<BuoyancyComponent>();
            buoyancy.m_Enabled = buoyancyComponent["Enabled"].as<bool>(buoyancy.m_Enabled);
            buoyancy.m_ProbeExtents = buoyancyComponent["ProbeExtents"].as<glm::vec3>(buoyancy.m_ProbeExtents);
            buoyancy.m_FluidDensity = buoyancyComponent["FluidDensity"].as<f32>(buoyancy.m_FluidDensity);
            buoyancy.m_BuoyancyScale = buoyancyComponent["BuoyancyScale"].as<f32>(buoyancy.m_BuoyancyScale);
            buoyancy.m_LinearDrag = buoyancyComponent["LinearDrag"].as<f32>(buoyancy.m_LinearDrag);
            buoyancy.m_AngularDrag = buoyancyComponent["AngularDrag"].as<f32>(buoyancy.m_AngularDrag);
            buoyancy.m_SubmergenceRamp = buoyancyComponent["SubmergenceRamp"].as<f32>(buoyancy.m_SubmergenceRamp);

            // Validate every float read from YAML (project rule).
            SanitizeVec3Clamped(buoyancy.m_ProbeExtents, 0.01f, 1000.0f, { 0.5f, 0.5f, 0.5f });
            SanitizeFloat(buoyancy.m_FluidDensity, 1.0f, 100000.0f, 1000.0f);
            SanitizeFloat(buoyancy.m_BuoyancyScale, 0.0f, 1000.0f, 1.0f);
            SanitizeFloat(buoyancy.m_LinearDrag, 0.0f, 1000.0f, 0.8f);
            SanitizeFloat(buoyancy.m_AngularDrag, 0.0f, 1000.0f, 0.5f);
            SanitizeFloat(buoyancy.m_SubmergenceRamp, 0.001f, 100.0f, 0.25f);
        }

        if (auto snowDeformerComponent = entity["SnowDeformerComponent"]; snowDeformerComponent)
        {
            DeserializeSnowDeformerComponent(deserializedEntity, snowDeformerComponent);
        }

        if (auto fogVolumeComponent = entity["FogVolumeComponent"]; fogVolumeComponent)
        {
            DeserializeFogVolumeComponent(deserializedEntity, fogVolumeComponent);
        }

        if (auto decalComponent = entity["DecalComponent"]; decalComponent)
        {
            DeserializeDecalComponent(deserializedEntity, decalComponent);
        }

        if (auto submeshComponent = entity["SubmeshComponent"]; submeshComponent)
        {
            auto& submesh = deserializedEntity.AddComponent<SubmeshComponent>();
            submesh.m_SubmeshIndex = submeshComponent["SubmeshIndex"].as<u32>(submesh.m_SubmeshIndex);
            submesh.m_Visible = submeshComponent["Visible"].as<bool>(submesh.m_Visible);
            // Note: m_Mesh is reconstructed from parent MeshComponent at runtime
        }

        if (auto animComponent = entity["AnimationStateComponent"]; animComponent)
        {
            auto& anim = deserializedEntity.AddComponent<AnimationStateComponent>();
            anim.m_State = static_cast<AnimationStateComponent::State>(animComponent["State"].as<int>(std::to_underlying(anim.m_State)));
            anim.m_CurrentTime = animComponent["CurrentTime"].as<f32>(anim.m_CurrentTime);
            anim.m_BlendDuration = animComponent["BlendDuration"].as<f32>(anim.m_BlendDuration);
            anim.m_CurrentClipIndex = animComponent["CurrentClipIndex"].as<int>(anim.m_CurrentClipIndex);
            anim.m_IsPlaying = animComponent["IsPlaying"].as<bool>(anim.m_IsPlaying);

            // Load source file path (stored as relative, convert to absolute) and reload animated model if available
            if (animComponent["SourceFilePath"])
            {
                auto relativePathStr = animComponent["SourceFilePath"].as<std::string>();
                if (!relativePathStr.empty())
                {
                    // Convert relative path back to absolute
                    std::filesystem::path relativePath(relativePathStr);
                    auto assetDirectory = Project::GetAssetDirectory();
                    auto absolutePath = assetDirectory / relativePath;
                    anim.m_SourceFilePath = absolutePath.string();

                    auto animatedModel = Ref<AnimatedModel>::Create(anim.m_SourceFilePath);
                    if (animatedModel)
                    {
                        // Reload MeshComponent / SkeletonComponent / AnimationStateComponent /
                        // MaterialComponent from the model. resetPlaybackState=false keeps the
                        // playback scalars (state/time/clip index) we just read from YAML; the
                        // importer only clamps the current-clip index into range.
                        ModelImporter::PopulateAnimatedEntity(deserializedEntity, animatedModel,
                                                              anim.m_SourceFilePath, /*resetPlaybackState=*/false);
                        OLO_CORE_INFO("Deserialized animated model '{}': {} clips, {} meshes, {} bones",
                                      anim.m_SourceFilePath, animatedModel->GetAnimations().size(),
                                      animatedModel->GetMeshes().size(),
                                      animatedModel->HasSkeleton() && animatedModel->GetSkeleton()
                                          ? animatedModel->GetSkeleton()->m_BoneNames.size()
                                          : 0);
                    }
                    else
                    {
                        OLO_CORE_ERROR("Failed to load animated model from '{}'", anim.m_SourceFilePath);
                    }
                }
            }
        }

        if (auto skelComponent = entity["SkeletonComponent"]; skelComponent)
        {
            // Only add if not already added by AnimationStateComponent deserialization
            if (!deserializedEntity.HasComponent<SkeletonComponent>())
            {
                deserializedEntity.AddComponent<SkeletonComponent>();
            }
            // Note: Skeleton data is loaded from AnimationStateComponent's source file
        }

        if (auto graphComponent = entity["AnimationGraphComponent"]; graphComponent)
        {
            auto& graphComp = deserializedEntity.AddComponent<AnimationGraphComponent>();
            graphComp.AnimationGraphAssetHandle = graphComponent["AssetHandle"].as<u64>(0);
        }

        if (auto cinematicComponent = entity["CinematicComponent"]; cinematicComponent)
        {
            auto& cine = deserializedEntity.AddComponent<CinematicComponent>();
            cine.Sequence = cinematicComponent["Sequence"].as<u64>(0);
            cine.PlayOnStart = cinematicComponent["PlayOnStart"].as<bool>(cine.PlayOnStart);
            cine.Loop = cinematicComponent["Loop"].as<bool>(cine.Loop);
            if (const f32 speed = cinematicComponent["PlaybackSpeed"].as<f32>(cine.PlaybackSpeed);
                std::isfinite(speed) && speed >= 0.0f)
            {
                cine.PlaybackSpeed = speed;
            }
        }

        if (auto morphComponent = entity["MorphTargetComponent"]; morphComponent)
        {
            auto& morphComp = deserializedEntity.AddComponent<MorphTargetComponent>();
            if (auto weightsNode = morphComponent["Weights"]; weightsNode && weightsNode.IsMap())
            {
                for (auto it = weightsNode.begin(); it != weightsNode.end(); ++it)
                {
                    std::string targetName = it->first.as<std::string>();
                    f32 weight = it->second.as<f32>(0.0f);
                    SanitizeFloat(weight, 0.0f, 1.0f, 0.0f);
                    morphComp.SetWeight(targetName, weight);
                }
            }
        }

        if (auto lpComponent = entity["LightProbeComponent"]; lpComponent)
        {
            auto& lp = deserializedEntity.AddComponent<LightProbeComponent>();
            lp.m_InfluenceRadius = lpComponent["InfluenceRadius"].as<f32>(lp.m_InfluenceRadius);
            lp.m_Intensity = lpComponent["Intensity"].as<f32>(lp.m_Intensity);
            lp.m_Active = lpComponent["Active"].as<bool>(lp.m_Active);

            // Sanitize deserialized values
            if (!std::isfinite(lp.m_InfluenceRadius) || lp.m_InfluenceRadius <= 0.0f)
            {
                lp.m_InfluenceRadius = 10.0f;
            }
            if (!std::isfinite(lp.m_Intensity) || lp.m_Intensity < 0.0f)
            {
                lp.m_Intensity = 1.0f;
            }

            if (auto shNode = lpComponent["SHCoefficients"]; shNode && shNode.IsSequence())
            {
                for (u32 i = 0; i < SH_COEFFICIENT_COUNT && i < shNode.size(); ++i)
                {
                    glm::vec3 coeff = shNode[i].as<glm::vec3>();
                    if (std::isfinite(coeff.x) && std::isfinite(coeff.y) && std::isfinite(coeff.z))
                    {
                        lp.m_SHCoefficients.Coefficients[i] = coeff;
                    }
                }
            }
        }

        if (auto lpvComponent = entity["LightProbeVolumeComponent"]; lpvComponent)
        {
            auto& lpv = deserializedEntity.AddComponent<LightProbeVolumeComponent>();
            lpv.m_BoundsMin = lpvComponent["BoundsMin"].as<glm::vec3>(lpv.m_BoundsMin);
            lpv.m_BoundsMax = lpvComponent["BoundsMax"].as<glm::vec3>(lpv.m_BoundsMax);
            lpv.m_Resolution = lpvComponent["Resolution"].as<glm::ivec3>(lpv.m_Resolution);
            lpv.m_Spacing = lpvComponent["Spacing"].as<f32>(lpv.m_Spacing);
            lpv.m_Intensity = lpvComponent["Intensity"].as<f32>(lpv.m_Intensity);
            lpv.m_Active = lpvComponent["Active"].as<bool>(lpv.m_Active);
            lpv.m_BakedDataAsset = lpvComponent["BakedDataAsset"].as<u64>(lpv.m_BakedDataAsset);

            // Sanitize deserialized values
            if (!std::isfinite(lpv.m_Spacing) || lpv.m_Spacing <= 0.0f)
            {
                lpv.m_Spacing = 5.0f;
            }
            if (!std::isfinite(lpv.m_Intensity) || lpv.m_Intensity < 0.0f)
            {
                lpv.m_Intensity = 1.0f;
            }
            lpv.m_Resolution = glm::max(lpv.m_Resolution, glm::ivec3(1));
            for (i32 axis = 0; axis < 3; ++axis)
            {
                if (lpv.m_BoundsMin[axis] > lpv.m_BoundsMax[axis])
                {
                    std::swap(lpv.m_BoundsMin[axis], lpv.m_BoundsMax[axis]);
                }
            }
        }

        if (auto rpComponent = entity["ReflectionProbeComponent"]; rpComponent)
        {
            auto& rp = deserializedEntity.AddComponent<ReflectionProbeComponent>();
            rp.m_InfluenceRadius = rpComponent["InfluenceRadius"].as<f32>(rp.m_InfluenceRadius);
            rp.m_BlendDistance = rpComponent["BlendDistance"].as<f32>(rp.m_BlendDistance);
            rp.m_Resolution = rpComponent["Resolution"].as<u32>(rp.m_Resolution);
            rp.m_Intensity = rpComponent["Intensity"].as<f32>(rp.m_Intensity);
            rp.m_Active = rpComponent["Active"].as<bool>(rp.m_Active);

            // Sanitize deserialized values — never trust file input
            if (!std::isfinite(rp.m_InfluenceRadius) || rp.m_InfluenceRadius <= 0.0f)
            {
                rp.m_InfluenceRadius = 10.0f;
            }
            if (!std::isfinite(rp.m_BlendDistance) || rp.m_BlendDistance < 0.0f)
            {
                rp.m_BlendDistance = 1.0f;
            }
            if (!std::isfinite(rp.m_Intensity) || rp.m_Intensity < 0.0f)
            {
                rp.m_Intensity = 1.0f;
            }
            // Clamp Resolution to a power-of-two-ish sane range; 0 / huge values
            // would either no-op or OOM the bake step.
            rp.m_Resolution = std::clamp(rp.m_Resolution, 16u, 2048u);

            // Captured cubemap + IBL chain are not persisted — author needs to
            // rebake after load (UI surfaces this via m_NeedsBake).
            rp.m_NeedsBake = true;
        }

        if (auto svComponent = entity["StreamingVolumeComponent"]; svComponent)
        {
            auto& sv = deserializedEntity.AddComponent<StreamingVolumeComponent>();
            TrySet(sv.RegionAssetHandle, svComponent["RegionAssetHandle"]);
            TrySetEnum(sv.ActivationMode, svComponent["ActivationMode"]);
            TrySet(sv.LoadRadius, svComponent["LoadRadius"]);
            TrySet(sv.UnloadRadius, svComponent["UnloadRadius"]);

            // Sanitize
            if (std::to_underlying(sv.ActivationMode) > std::to_underlying(StreamingActivationMode::Manual))
            {
                sv.ActivationMode = StreamingActivationMode::Proximity;
            }
            if (!std::isfinite(sv.LoadRadius))
                sv.LoadRadius = 200.0f;
            if (!std::isfinite(sv.UnloadRadius))
                sv.UnloadRadius = 250.0f;
            sv.LoadRadius = std::max(sv.LoadRadius, 1.0f);
            sv.UnloadRadius = std::max(sv.UnloadRadius, sv.LoadRadius + 1.0f);
        }

        if (auto networkIdentityComponent = entity["NetworkIdentityComponent"]; networkIdentityComponent)
        {
            auto& nic = deserializedEntity.AddComponent<NetworkIdentityComponent>();
            TrySet(nic.OwnerClientID, networkIdentityComponent["OwnerClientID"]);
            TrySetEnum(nic.Authority, networkIdentityComponent["Authority"]);
            TrySet(nic.IsReplicated, networkIdentityComponent["IsReplicated"]);
        }

        if (auto networkInterestComponent = entity["NetworkInterestComponent"]; networkInterestComponent)
        {
            auto& nic = deserializedEntity.AddComponent<NetworkInterestComponent>();
            TrySet(nic.RelevanceRadius, networkInterestComponent["RelevanceRadius"]);
            TrySet(nic.InterestGroup, networkInterestComponent["InterestGroup"]);
        }

        if (auto phaseComponent = entity["PhaseComponent"]; phaseComponent)
        {
            auto& pc = deserializedEntity.AddComponent<PhaseComponent>();
            TrySet(pc.PhaseID, phaseComponent["PhaseID"]);
        }

        if (auto instancePortalComponent = entity["InstancePortalComponent"]; instancePortalComponent)
        {
            auto& ipc = deserializedEntity.AddComponent<InstancePortalComponent>();
            TrySet(ipc.TargetZoneID, instancePortalComponent["TargetZoneID"]);
            TrySet(ipc.InstanceType, instancePortalComponent["InstanceType"]);
            TrySet(ipc.MaxPlayers, instancePortalComponent["MaxPlayers"]);
        }

        if (auto networkLODComponent = entity["NetworkLODComponent"]; networkLODComponent)
        {
            auto& nlc = deserializedEntity.AddComponent<NetworkLODComponent>();
            TrySetEnum(nlc.Level, networkLODComponent["Level"]);
        }

        if (auto dialogueComponent = entity["DialogueComponent"]; dialogueComponent)
        {
            auto& dc = deserializedEntity.AddComponent<DialogueComponent>();
            TrySet(dc.m_DialogueTree, dialogueComponent["DialogueTree"]);
            TrySet(dc.m_AutoTrigger, dialogueComponent["AutoTrigger"]);
            TrySet(dc.m_TriggerRadius, dialogueComponent["TriggerRadius"]);
            SanitizeFloat(dc.m_TriggerRadius, 0.0f, 1e6f, 3.0f);
            TrySet(dc.m_TriggerOnce, dialogueComponent["TriggerOnce"]);
        }

        if (auto navMeshBoundsComponent = entity["NavMeshBoundsComponent"]; navMeshBoundsComponent)
        {
            auto& nmb = deserializedEntity.AddComponent<NavMeshBoundsComponent>();
            TrySet(nmb.m_Min, navMeshBoundsComponent["Min"]);
            TrySet(nmb.m_Max, navMeshBoundsComponent["Max"]);
            SanitizeVec3(nmb.m_Min, glm::vec3(-100.0f, -10.0f, -100.0f));
            SanitizeVec3(nmb.m_Max, glm::vec3(100.0f, 50.0f, 100.0f));
            // Ensure Min <= Max per axis
            for (int i = 0; i < 3; ++i)
            {
                if (nmb.m_Min[i] > nmb.m_Max[i])
                    std::swap(nmb.m_Min[i], nmb.m_Max[i]);
            }
        }

        if (auto navAgentComponent = entity["NavAgentComponent"]; navAgentComponent)
        {
            auto& nac = deserializedEntity.AddComponent<NavAgentComponent>();
            TrySet(nac.m_Radius, navAgentComponent["Radius"]);
            SanitizeFloat(nac.m_Radius, 0.01f, 100.0f, 0.5f);
            TrySet(nac.m_Height, navAgentComponent["Height"]);
            SanitizeFloat(nac.m_Height, 0.01f, 100.0f, 2.0f);
            TrySet(nac.m_MaxSpeed, navAgentComponent["MaxSpeed"]);
            SanitizeFloat(nac.m_MaxSpeed, 0.0f, 1000.0f, 3.5f);
            TrySet(nac.m_Acceleration, navAgentComponent["Acceleration"]);
            SanitizeFloat(nac.m_Acceleration, 0.0f, 1000.0f, 8.0f);
            TrySet(nac.m_StoppingDistance, navAgentComponent["StoppingDistance"]);
            SanitizeFloat(nac.m_StoppingDistance, 0.0f, 100.0f, 0.1f);
            TrySet(nac.m_AvoidancePriority, navAgentComponent["AvoidancePriority"]);
            TrySet(nac.m_LockYAxis, navAgentComponent["LockYAxis"]);
        }

        if (auto behaviorTreeComponent = entity["BehaviorTreeComponent"]; behaviorTreeComponent)
        {
            auto& btc = deserializedEntity.AddComponent<BehaviorTreeComponent>();
            TrySet(btc.BehaviorTreeAssetHandle, behaviorTreeComponent["BehaviorTreeAsset"]);
        }

        if (auto stateMachineComponent = entity["StateMachineComponent"]; stateMachineComponent)
        {
            auto& smc = deserializedEntity.AddComponent<StateMachineComponent>();
            TrySet(smc.StateMachineAssetHandle, stateMachineComponent["StateMachineAsset"]);
        }

        if (auto goapAgentComponent = entity["GoapAgentComponent"]; goapAgentComponent)
        {
            auto& gac = deserializedEntity.AddComponent<GoapAgentComponent>();
            TrySet(gac.Enabled, goapAgentComponent["Enabled"]);
        }

        if (auto inventoryComponent = entity["InventoryComponent"]; inventoryComponent)
        {
            auto& ic = deserializedEntity.AddComponent<InventoryComponent>();
            i32 capacity = std::max(inventoryComponent["Capacity"].as<i32>(40), 1);
            ic.PlayerInventory.SetCapacity(capacity);
            TrySet(ic.PlayerInventory.MaxWeight, inventoryComponent["MaxWeight"]);
            TrySet(ic.Currency, inventoryComponent["Currency"]);

            if (auto items = inventoryComponent["Items"]; items && items.IsSequence())
            {
                for (auto const& itemNode : items)
                {
                    ItemInstance item;
                    item.InstanceID = itemNode["InstanceID"].as<u64>(0);
                    item.ItemDefinitionID = itemNode["DefinitionID"].as<std::string>("");
                    TrySet(item.StackCount, itemNode["StackCount"]);
                    TrySet(item.Durability, itemNode["Durability"]);
                    TrySet(item.MaxDurability, itemNode["MaxDurability"]);

                    if (auto affixes = itemNode["Affixes"]; affixes && affixes.IsSequence())
                    {
                        for (auto const& affixNode : affixes)
                        {
                            ItemAffix affix;
                            affix.Name = affixNode["Name"].as<std::string>("");
                            affix.Attribute = affixNode["Attribute"].as<std::string>("");
                            affix.Value = affixNode["Value"].as<f32>(0.0f);
                            item.Affixes.push_back(std::move(affix));
                        }
                    }

                    if (auto customData = itemNode["CustomData"]; customData && customData.IsMap())
                    {
                        for (auto const& kv : customData)
                        {
                            item.CustomData[kv.first.as<std::string>()] = kv.second.as<std::string>("");
                        }
                    }

                    if (auto slotNode = itemNode["Slot"]; slotNode && slotNode.IsScalar())
                    {
                        if (!ic.PlayerInventory.AddItemToSlot(slotNode.as<i32>(0), item))
                        {
                            OLO_CORE_WARN("[SceneSerializer] Failed to add item '{}' to slot {}", item.ItemDefinitionID, slotNode.as<i32>(0));
                        }
                    }
                    else
                    {
                        if (!ic.PlayerInventory.AddItem(item))
                        {
                            OLO_CORE_WARN("[SceneSerializer] Failed to add item '{}' to inventory", item.ItemDefinitionID);
                        }
                    }
                }
            }

            if (auto equipment = inventoryComponent["Equipment"]; equipment && equipment.IsSequence())
            {
                for (auto const& eqNode : equipment)
                {
                    ItemInstance item;
                    item.InstanceID = eqNode["InstanceID"].as<u64>(0);
                    item.ItemDefinitionID = eqNode["DefinitionID"].as<std::string>("");
                    TrySet(item.StackCount, eqNode["StackCount"]);
                    TrySet(item.Durability, eqNode["Durability"]);
                    TrySet(item.MaxDurability, eqNode["MaxDurability"]);

                    if (auto affixes = eqNode["Affixes"]; affixes && affixes.IsSequence())
                    {
                        for (auto const& affixNode : affixes)
                        {
                            ItemAffix affix;
                            affix.DefinitionID = affixNode["DefinitionID"].as<std::string>("");
                            affix.Type = AffixTypeFromString(affixNode["Type"].as<std::string>("Prefix"));
                            affix.Tier = affixNode["Tier"].as<i32>(0);
                            affix.Name = affixNode["Name"].as<std::string>("");
                            affix.Attribute = affixNode["Attribute"].as<std::string>("");
                            affix.Value = affixNode["Value"].as<f32>(0.0f);
                            item.Affixes.push_back(std::move(affix));
                        }
                    }

                    if (auto customData = eqNode["CustomData"]; customData && customData.IsMap())
                    {
                        for (auto const& kv : customData)
                        {
                            item.CustomData[kv.first.as<std::string>()] = kv.second.as<std::string>("");
                        }
                    }

                    auto slotNode = eqNode["Slot"];
                    if (!slotNode || !slotNode.IsScalar())
                    {
                        OLO_CORE_WARN("[SceneSerializer] Equipment entry missing 'Slot' field — skipping (item: {})", item.ItemDefinitionID);
                        continue;
                    }
                    auto slotStr = slotNode.as<std::string>("");
                    if (slotStr.empty())
                    {
                        OLO_CORE_WARN("[SceneSerializer] Equipment entry has empty 'Slot' — skipping (item: {})", item.ItemDefinitionID);
                        continue;
                    }
                    auto slot = EquipmentSlots::SlotFromString(slotStr);
                    if (slot == EquipmentSlots::Slot::Count)
                    {
                        OLO_CORE_WARN("[SceneSerializer] Invalid equipment slot '{}' — skipping", slotStr);
                        continue;
                    }
                    // Direct-equip for deserialization (no inventory mutation)
                    ic.Equipment.DirectEquip(slot, item);
                }
            }
        }

        if (auto itemPickupComponent = entity["ItemPickupComponent"]; itemPickupComponent)
        {
            auto& pc = deserializedEntity.AddComponent<ItemPickupComponent>();
            pc.Item.InstanceID = itemPickupComponent["InstanceID"].as<u64>(0);
            pc.Item.ItemDefinitionID = itemPickupComponent["DefinitionID"].as<std::string>("");
            TrySet(pc.Item.StackCount, itemPickupComponent["StackCount"]);
            TrySet(pc.Item.Durability, itemPickupComponent["Durability"]);
            TrySet(pc.Item.MaxDurability, itemPickupComponent["MaxDurability"]);
            TrySet(pc.PickupRadius, itemPickupComponent["PickupRadius"]);
            TrySet(pc.AutoPickup, itemPickupComponent["AutoPickup"]);
            TrySet(pc.DespawnTimer, itemPickupComponent["DespawnTimer"]);

            if (auto affixes = itemPickupComponent["Affixes"]; affixes && affixes.IsSequence())
            {
                for (auto const& affixNode : affixes)
                {
                    ItemAffix affix;
                    affix.DefinitionID = affixNode["DefinitionID"].as<std::string>("");
                    affix.Type = AffixTypeFromString(affixNode["Type"].as<std::string>("Prefix"));
                    affix.Tier = affixNode["Tier"].as<i32>(0);
                    affix.Name = affixNode["Name"].as<std::string>("");
                    affix.Attribute = affixNode["Attribute"].as<std::string>("");
                    affix.Value = affixNode["Value"].as<f32>(0.0f);
                    pc.Item.Affixes.push_back(std::move(affix));
                }
            }

            if (auto customData = itemPickupComponent["CustomData"]; customData && customData.IsMap())
            {
                for (auto const& kv : customData)
                {
                    pc.Item.CustomData[kv.first.as<std::string>()] = kv.second.as<std::string>("");
                }
            }
        }

        if (auto itemContainerComponent = entity["ItemContainerComponent"]; itemContainerComponent)
        {
            auto& cc = deserializedEntity.AddComponent<ItemContainerComponent>();
            i32 capacity = std::max(itemContainerComponent["Capacity"].as<i32>(20), 1);
            cc.Contents.SetCapacity(capacity);
            TrySet(cc.IsShop, itemContainerComponent["IsShop"]);
            TrySet(cc.LootTableID, itemContainerComponent["LootTableID"]);
            TrySet(cc.HasBeenLooted, itemContainerComponent["HasBeenLooted"]);

            if (auto items = itemContainerComponent["Items"]; items && items.IsSequence())
            {
                for (auto const& itemNode : items)
                {
                    ItemInstance item;
                    item.InstanceID = itemNode["InstanceID"].as<u64>(0);
                    item.ItemDefinitionID = itemNode["DefinitionID"].as<std::string>("");
                    TrySet(item.StackCount, itemNode["StackCount"]);
                    TrySet(item.Durability, itemNode["Durability"]);
                    TrySet(item.MaxDurability, itemNode["MaxDurability"]);

                    if (auto affixes = itemNode["Affixes"]; affixes && affixes.IsSequence())
                    {
                        for (auto const& affixNode : affixes)
                        {
                            ItemAffix affix;
                            affix.DefinitionID = affixNode["DefinitionID"].as<std::string>("");
                            affix.Type = AffixTypeFromString(affixNode["Type"].as<std::string>("Prefix"));
                            affix.Tier = affixNode["Tier"].as<i32>(0);
                            affix.Name = affixNode["Name"].as<std::string>("");
                            affix.Attribute = affixNode["Attribute"].as<std::string>("");
                            affix.Value = affixNode["Value"].as<f32>(0.0f);
                            item.Affixes.push_back(std::move(affix));
                        }
                    }

                    if (auto customData = itemNode["CustomData"]; customData && customData.IsMap())
                    {
                        for (auto const& kv : customData)
                        {
                            item.CustomData[kv.first.as<std::string>()] = kv.second.as<std::string>("");
                        }
                    }

                    if (auto slotNode = itemNode["Slot"]; slotNode && slotNode.IsScalar())
                    {
                        if (!cc.Contents.AddItemToSlot(slotNode.as<i32>(0), item))
                        {
                            OLO_CORE_WARN("[SceneSerializer] Failed to add item '{}' to container slot {}", item.ItemDefinitionID, slotNode.as<i32>(0));
                        }
                    }
                    else
                    {
                        if (!cc.Contents.AddItem(item))
                        {
                            OLO_CORE_WARN("[SceneSerializer] Failed to add item '{}' to container", item.ItemDefinitionID);
                        }
                    }
                }
            }
        }

        if (auto questJournalComponent = entity["QuestJournalComponent"]; questJournalComponent)
        {
            auto& qjc = deserializedEntity.AddComponent<QuestJournalComponent>();

            if (auto tags = questJournalComponent["Tags"]; tags && tags.IsSequence())
            {
                for (auto const& tagNode : tags)
                {
                    qjc.Journal.AddTag(tagNode.as<std::string>(""));
                }
            }

            // Player state for requirement evaluation
            if (auto level = questJournalComponent["PlayerLevel"]; level)
            {
                qjc.Journal.SetPlayerLevel(std::max(level.as<i32>(0), 0));
            }
            if (auto playerClass = questJournalComponent["PlayerClass"]; playerClass)
            {
                qjc.Journal.SetPlayerClass(playerClass.as<std::string>(""));
            }
            if (auto playerFaction = questJournalComponent["PlayerFaction"]; playerFaction)
            {
                qjc.Journal.SetPlayerFaction(playerFaction.as<std::string>(""));
            }
            if (auto reputations = questJournalComponent["Reputations"]; reputations && reputations.IsMap())
            {
                for (auto it = reputations.begin(); it != reputations.end(); ++it)
                {
                    qjc.Journal.SetReputation(it->first.as<std::string>(""), it->second.as<i32>(0));
                }
            }
            if (auto items = questJournalComponent["Items"]; items && items.IsMap())
            {
                for (auto it = items.begin(); it != items.end(); ++it)
                {
                    qjc.Journal.SetItemCount(it->first.as<std::string>(""), std::max(it->second.as<i32>(0), 0));
                }
            }
            if (auto stats = questJournalComponent["Stats"]; stats && stats.IsMap())
            {
                for (auto it = stats.begin(); it != stats.end(); ++it)
                {
                    qjc.Journal.SetStat(it->first.as<std::string>(""), it->second.as<i32>(0));
                }
            }

            if (auto completed = questJournalComponent["CompletedQuests"]; completed && completed.IsSequence())
            {
                for (auto const& node : completed)
                {
                    if (node.IsMap())
                    {
                        auto questID = node["QuestID"].as<std::string>("");
                        auto branchID = node["BranchID"].as<std::string>("");
                        qjc.Journal.AddCompletedQuestID(questID, branchID);
                    }
                    else
                    {
                        qjc.Journal.AddCompletedQuestID(node.as<std::string>(""));
                    }
                }
            }

            if (auto failed = questJournalComponent["FailedQuests"]; failed && failed.IsSequence())
            {
                for (auto const& node : failed)
                {
                    qjc.Journal.AddFailedQuestID(node.as<std::string>(""));
                }
            }

            if (auto activeQuests = questJournalComponent["ActiveQuests"]; activeQuests && activeQuests.IsSequence())
            {
                for (auto const& questNode : activeQuests)
                {
                    QuestJournal::ActiveQuestState state;
                    state.QuestID = questNode["QuestID"].as<std::string>("");
                    state.Status = QuestStatusFromString(questNode["Status"].as<std::string>("Active"));
                    state.CurrentStageIndex = std::max(questNode["CurrentStageIndex"].as<i32>(0), 0);
                    state.ElapsedTime = questNode["ElapsedTime"].as<f32>(0.0f);
                    if (!std::isfinite(state.ElapsedTime) || state.ElapsedTime < 0.0f)
                    {
                        state.ElapsedTime = 0.0f;
                    }

                    // Try to load the definition from database
                    if (const auto* def = QuestDatabase::Get(state.QuestID); def)
                    {
                        state.Definition = *def;
                    }
                    else
                    {
                        OLO_CORE_WARN("[SceneSerializer] Active quest '{}': definition not found in QuestDatabase, preserving state with stub", state.QuestID);
                        state.Definition.QuestID = state.QuestID;
                    }

                    // Clamp stage index to valid range
                    if (!state.Definition.Stages.empty())
                    {
                        state.CurrentStageIndex = std::min(state.CurrentStageIndex, static_cast<i32>(state.Definition.Stages.size()) - 1);
                    }

                    if (auto objectives = questNode["Objectives"]; objectives && objectives.IsSequence())
                    {
                        for (auto const& objNode : objectives)
                        {
                            QuestObjective obj;
                            obj.ObjectiveID = objNode["ObjectiveID"].as<std::string>("");
                            obj.Description = objNode["Description"].as<std::string>("");
                            obj.ObjectiveType = ObjectiveTypeFromString(objNode["Type"].as<std::string>("Custom"));
                            obj.TargetID = objNode["TargetID"].as<std::string>("");
                            obj.RequiredCount = std::max(objNode["RequiredCount"].as<i32>(1), 1);
                            obj.CurrentCount = std::clamp(objNode["CurrentCount"].as<i32>(0), 0, obj.RequiredCount);
                            obj.IsOptional = objNode["IsOptional"].as<bool>(false);
                            obj.IsHidden = objNode["IsHidden"].as<bool>(false);
                            obj.IsCompleted = (obj.CurrentCount >= obj.RequiredCount);
                            state.ObjectiveStates.push_back(std::move(obj));
                        }
                    }

                    // Copy QuestID into a local before `std::move(state)` —
                    // function-argument evaluation order is unspecified, and
                    // MSVC evaluates RHS first, leaving the LHS reference
                    // pointing at a moved-from empty string. Then
                    // m_ActiveQuests[""] gets the entry and the quest is
                    // un-findable by its real ID.
                    std::string keyCopy = state.QuestID;
                    qjc.Journal.SetActiveQuestState(keyCopy, std::move(state));
                }
            }

            if (auto cooldowns = questJournalComponent["QuestCooldowns"]; cooldowns && cooldowns.IsMap())
            {
                for (auto it = cooldowns.begin(); it != cooldowns.end(); ++it)
                {
                    qjc.Journal.SetQuestCooldown(it->first.as<std::string>(""), it->second.as<f32>(0.0f));
                }
            }
        }

        if (auto questGiverComponent = entity["QuestGiverComponent"]; questGiverComponent)
        {
            auto& qgc = deserializedEntity.AddComponent<QuestGiverComponent>();
            TrySet(qgc.QuestMarkerIcon, questGiverComponent["QuestMarkerIcon"]);

            if (auto offered = questGiverComponent["OfferedQuestIDs"]; offered && offered.IsSequence())
            {
                for (auto const& node : offered)
                {
                    qgc.OfferedQuestIDs.push_back(node.as<std::string>(""));
                }
            }

            if (auto turnIn = questGiverComponent["TurnInQuestIDs"]; turnIn && turnIn.IsSequence())
            {
                for (auto const& node : turnIn)
                {
                    qgc.TurnInQuestIDs.push_back(node.as<std::string>(""));
                }
            }
        }

        if (auto abilityComponentNode = entity["AbilityComponent"]; abilityComponentNode)
        {
            auto& ac = deserializedEntity.AddComponent<AbilityComponent>();

            // Deserialize attributes
            if (auto attributes = abilityComponentNode["Attributes"]; attributes && attributes.IsSequence())
            {
                for (auto const& attrNode : attributes)
                {
                    std::string name = attrNode["Name"].as<std::string>("");
                    if (name.empty())
                    {
                        continue;
                    }
                    f32 baseValue = attrNode["BaseValue"].as<f32>(0.0f);
                    SanitizeFloat(baseValue, -1e6f, 1e6f, 0.0f);
                    ac.Attributes.DefineAttribute(name, baseValue);
                }
            }

            // Deserialize owned tags
            if (auto tags = abilityComponentNode["OwnedTags"]; tags && tags.IsSequence())
            {
                for (auto const& tagNode : tags)
                {
                    std::string tagStr = tagNode.as<std::string>("");
                    if (!tagStr.empty())
                    {
                        ac.OwnedTags.AddTag(GameplayTag(tagStr));
                    }
                }
            }

            // Deserialize abilities
            if (auto abilities = abilityComponentNode["Abilities"]; abilities && abilities.IsSequence())
            {
                for (auto const& abilityNode : abilities)
                {
                    ActiveAbility aa;
                    aa.Definition.Name = abilityNode["Name"].as<std::string>("");
                    aa.Definition.AbilityTag = GameplayTag(abilityNode["AbilityTag"].as<std::string>(""));
                    aa.Definition.CooldownDuration = abilityNode["CooldownDuration"].as<f32>(0.0f);
                    aa.Definition.ResourceCost = abilityNode["ResourceCost"].as<f32>(0.0f);
                    aa.Definition.CostAttribute = abilityNode["CostAttribute"].as<std::string>("Mana");
                    aa.Definition.IsChanneled = abilityNode["IsChanneled"].as<bool>(false);
                    aa.Definition.IsToggled = abilityNode["IsToggled"].as<bool>(false);
                    aa.Definition.ChannelDuration = abilityNode["ChannelDuration"].as<f32>(0.0f);
                    SanitizeFloat(aa.Definition.CooldownDuration, 0.0f, 600.0f, 0.0f);
                    SanitizeFloat(aa.Definition.ResourceCost, 0.0f, 10000.0f, 0.0f);
                    SanitizeFloat(aa.Definition.ChannelDuration, 0.0f, 60.0f, 0.0f);

                    if (auto requiredTags = abilityNode["RequiredTags"]; requiredTags && requiredTags.IsSequence())
                    {
                        for (auto const& t : requiredTags)
                        {
                            aa.Definition.RequiredTags.AddTag(GameplayTag(t.as<std::string>("")));
                        }
                    }
                    if (auto blockedTags = abilityNode["BlockedTags"]; blockedTags && blockedTags.IsSequence())
                    {
                        for (auto const& t : blockedTags)
                        {
                            aa.Definition.BlockedTags.AddTag(GameplayTag(t.as<std::string>("")));
                        }
                    }

                    // Deserialize activation effects
                    DeserializeEffectList(abilityNode, "ActivationEffects", aa.Definition.ActivationEffects);

                    // Deserialize target activation effects (optional, backwards-compatible)
                    DeserializeEffectList(abilityNode, "TargetActivationEffects", aa.Definition.TargetActivationEffects);

                    ac.Abilities.push_back(std::move(aa));
                }
            }
        }

        if (auto nameplateNode = entity["NameplateComponent"]; nameplateNode)
        {
            auto& nc = deserializedEntity.AddComponent<NameplateComponent>();
            TrySet(nc.m_Enabled, nameplateNode["Enabled"]);
            TrySet(nc.m_ShowHealthBar, nameplateNode["ShowHealthBar"]);
            TrySet(nc.m_ShowManaBar, nameplateNode["ShowManaBar"]);
            TrySet(nc.m_WorldOffset, nameplateNode["WorldOffset"]);
            TrySet(nc.m_BarSize, nameplateNode["BarSize"]);
            TrySet(nc.m_HealthBarColor, nameplateNode["HealthBarColor"]);
            TrySet(nc.m_ManaBarColor, nameplateNode["ManaBarColor"]);
            TrySet(nc.m_BarBackgroundColor, nameplateNode["BarBackgroundColor"]);
            TrySet(nc.m_ManaBarGap, nameplateNode["ManaBarGap"]);
        }

        if (auto ikNode = entity["IKTargetComponent"]; ikNode)
        {
            auto& ik = deserializedEntity.AddComponent<IKTargetComponent>();
            TrySet(ik.AimIKEnabled, ikNode["AimIKEnabled"]);
            TrySet(ik.AimBoneIndex, ikNode["AimBoneIndex"]);
            TrySet(ik.AimTarget, ikNode["AimTarget"]);
            TrySet(ik.AimAxis, ikNode["AimAxis"]);
            TrySet(ik.AimOffset, ikNode["AimOffset"]);
            TrySet(ik.AimPoleVector, ikNode["AimPoleVector"]);
            TrySet(ik.AimChainLength, ikNode["AimChainLength"]);
            TrySet(ik.AimChainFactor, ikNode["AimChainFactor"]);
            TrySet(ik.AimWeight, ikNode["AimWeight"]);
            if (auto aimTargetNode = ikNode["AimTargetEntity"]; aimTargetNode)
            {
                ik.AimTargetEntity = aimTargetNode.as<u64>(0);
            }
            TrySet(ik.LimbIKEnabled, ikNode["LimbIKEnabled"]);
            TrySet(ik.LimbBoneIndex, ikNode["LimbBoneIndex"]);
            TrySet(ik.LimbTarget, ikNode["LimbTarget"]);
            TrySet(ik.LimbChainLength, ikNode["LimbChainLength"]);
            TrySet(ik.LimbWeight, ikNode["LimbWeight"]);
            if (auto limbTargetNode = ikNode["LimbTargetEntity"]; limbTargetNode)
            {
                ik.LimbTargetEntity = limbTargetNode.as<u64>(0);
            }

            // Sanitize float/vector fields — replace NaN/Inf with defaults and clamp to valid ranges
            ik.AimChainLength = std::max(1u, ik.AimChainLength);
            ik.LimbChainLength = std::max(2u, ik.LimbChainLength);
            SanitizeVec3(ik.AimTarget, glm::vec3(0.0f));
            SanitizeVec3(ik.AimAxis, glm::vec3(0.0f, 0.0f, 1.0f));
            SanitizeVec3(ik.AimOffset, glm::vec3(0.0f));
            SanitizeVec3(ik.AimPoleVector, glm::vec3(0.0f, 1.0f, 0.0f));
            SanitizeVec3(ik.LimbTarget, glm::vec3(0.0f));
            SanitizeFloat(ik.AimChainFactor, 0.0f, 1.0f, 0.5f);
            SanitizeFloat(ik.AimWeight, 0.0f, 1.0f, 1.0f);
            SanitizeFloat(ik.LimbWeight, 0.0f, 1.0f, 1.0f);
        }

        if (auto springNode = entity["SpringBoneComponent"]; springNode)
        {
            auto& spring = deserializedEntity.AddComponent<SpringBoneComponent>();
            TrySet(spring.Enabled, springNode["Enabled"]);
            TrySet(spring.EndBoneIndex, springNode["EndBoneIndex"]);
            TrySet(spring.ChainLength, springNode["ChainLength"]);
            TrySet(spring.Stiffness, springNode["Stiffness"]);
            TrySet(spring.Damping, springNode["Damping"]);
            TrySet(spring.Gravity, springNode["Gravity"]);
            TrySet(spring.Weight, springNode["Weight"]);

            // Sanitize float/vector fields — replace NaN/Inf with defaults and clamp to valid ranges
            spring.ChainLength = std::max(2u, spring.ChainLength);
            SanitizeFloat(spring.Stiffness, 0.0f, 1e6f, 80.0f);
            SanitizeFloat(spring.Damping, 0.0f, 1e6f, 12.0f);
            SanitizeVec3(spring.Gravity, glm::vec3(0.0f, -9.81f, 0.0f));
            SanitizeFloat(spring.Weight, 0.0f, 1.0f, 1.0f);
        }
    }

    SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
        : m_Scene(scene)
    {
    }

    void SceneSerializer::SerializeEntity(YAML::Emitter& out, Entity entity)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(entity.HasComponent<IDComponent>());

        out << YAML::BeginMap; // Entity
        out << YAML::Key << "Entity" << YAML::Value << entity.GetUUID();

        if (entity.HasComponent<TagComponent>())
        {
            out << YAML::Key << "TagComponent";
            out << YAML::BeginMap; // TagComponent

            auto const& tag = entity.GetComponent<TagComponent>().Tag;
            out << YAML::Key << "Tag" << YAML::Value << tag;

            out << YAML::EndMap; // TagComponent
        }

        if (entity.HasComponent<TransformComponent>())
        {
            out << YAML::Key << "TransformComponent";
            out << YAML::BeginMap; // TransformComponent

            auto const& tc = entity.GetComponent<TransformComponent>();
            out << YAML::Key << "Translation" << YAML::Value << tc.Translation;
            out << YAML::Key << "Rotation" << YAML::Value << tc.GetRotationEuler();
            out << YAML::Key << "Scale" << YAML::Value << tc.Scale;

            out << YAML::EndMap; // TransformComponent
        }

        if (entity.HasComponent<CameraComponent>())
        {
            out << YAML::Key << "CameraComponent";
            out << YAML::BeginMap; // CameraComponent

            auto const& cameraComponent = entity.GetComponent<CameraComponent>();
            auto const& camera = cameraComponent.Camera;

            out << YAML::Key << "Camera" << YAML::Value;
            out << YAML::BeginMap; // Camera
            out << YAML::Key << "ProjectionType" << YAML::Value << std::to_underlying(camera.GetProjectionType());
            out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveVerticalFOV();
            out << YAML::Key << "PerspectiveNear" << YAML::Value << camera.GetPerspectiveNearClip();
            out << YAML::Key << "PerspectiveFar" << YAML::Value << camera.GetPerspectiveFarClip();
            out << YAML::Key << "OrthographicSize" << YAML::Value << camera.GetOrthographicSize();
            out << YAML::Key << "OrthographicNear" << YAML::Value << camera.GetOrthographicNearClip();
            out << YAML::Key << "OrthographicFar" << YAML::Value << camera.GetOrthographicFarClip();
            out << YAML::EndMap; // Camera

            out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
            out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;
            out << YAML::Key << "RuntimeControl" << YAML::Value << cameraComponent.RuntimeControl;
            out << YAML::Key << "FlySpeed" << YAML::Value << cameraComponent.FlySpeed;

            out << YAML::EndMap; // CameraComponent
        }

        if (entity.HasComponent<ScriptComponent>())
        {
            auto const& scriptComponent = entity.GetComponent<ScriptComponent>();

            out << YAML::Key << "ScriptComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "ClassName" << YAML::Value << scriptComponent.ClassName;

            // Fields
            Ref<ScriptClass> entityClass = ScriptEngine::GetEntityClass(scriptComponent.ClassName);
            if (const auto& fields = entityClass->GetFields(); !fields.empty())
            {
                out << YAML::Key << "ScriptFields" << YAML::Value;
                auto& entityFields = ScriptEngine::GetScriptFieldMap(entity);
                out << YAML::BeginSeq;
                for (const auto& [name, field] : fields)
                {
                    if (!entityFields.contains(name))
                    {
                        continue;
                    }

                    out << YAML::BeginMap;
                    out << YAML::Key << "Name" << YAML::Value << name;
                    out << YAML::Key << "Type" << YAML::Value << Utils::ScriptFieldTypeToString(field.Type);

                    out << YAML::Key << "Data" << YAML::Value;
                    ScriptFieldInstance& scriptField = entityFields.at(name);

                    switch (field.Type)
                    {
                        WRITE_SCRIPT_FIELD(Float, f32);
                        WRITE_SCRIPT_FIELD(Double, f64);
                        WRITE_SCRIPT_FIELD(Bool, bool);
                        WRITE_SCRIPT_FIELD(Char, char);
                        WRITE_SCRIPT_FIELD(Byte, i8);
                        WRITE_SCRIPT_FIELD(Short, i16);
                        WRITE_SCRIPT_FIELD(Int, i32);
                        WRITE_SCRIPT_FIELD(Long, i64);
                        WRITE_SCRIPT_FIELD(UByte, u8);
                        WRITE_SCRIPT_FIELD(UShort, u16);
                        WRITE_SCRIPT_FIELD(UInt, u32);
                        WRITE_SCRIPT_FIELD(ULong, u64);
                        WRITE_SCRIPT_FIELD(Vector2, glm::vec2);
                        WRITE_SCRIPT_FIELD(Vector3, glm::vec3);
                        WRITE_SCRIPT_FIELD(Vector4, glm::vec4);
                        WRITE_SCRIPT_FIELD(Entity, UUID);
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap;
        }

        if (entity.HasComponent<LuaScriptComponent>())
        {
            auto const& luaScript = entity.GetComponent<LuaScriptComponent>();
            out << YAML::Key << "LuaScriptComponent";
            out << YAML::BeginMap;
            out << YAML::Key << "ScriptFile" << YAML::Value << luaScript.ScriptFile;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<AudioSourceComponent>())
        {
            out << YAML::Key << "AudioSourceComponent";
            out << YAML::BeginMap; // AudioSourceComponent

            const auto& audioSourceComponent = entity.GetComponent<AudioSourceComponent>();
            std::string f = (audioSourceComponent.Source ? Project::GetAssetRelativeFileSystemPath(audioSourceComponent.Source->GetPath()).string().c_str() : "");
            out << YAML::Key << "Filepath" << YAML::Value << f.c_str();
            out << YAML::Key << "VolumeMultiplier" << YAML::Value << audioSourceComponent.Config.VolumeMultiplier;
            out << YAML::Key << "PitchMultiplier" << YAML::Value << audioSourceComponent.Config.PitchMultiplier;
            out << YAML::Key << "PlayOnAwake" << YAML::Value << audioSourceComponent.Config.PlayOnAwake;
            out << YAML::Key << "Looping" << YAML::Value << audioSourceComponent.Config.Looping;
            out << YAML::Key << "Spatialization" << YAML::Value << audioSourceComponent.Config.Spatialization;
            out << YAML::Key << "AttenuationModel" << YAML::Value << (int)audioSourceComponent.Config.AttenuationModel;
            out << YAML::Key << "RollOff" << YAML::Value << audioSourceComponent.Config.RollOff;
            out << YAML::Key << "MinGain" << YAML::Value << audioSourceComponent.Config.MinGain;
            out << YAML::Key << "MaxGain" << YAML::Value << audioSourceComponent.Config.MaxGain;
            out << YAML::Key << "MinDistance" << YAML::Value << audioSourceComponent.Config.MinDistance;
            out << YAML::Key << "MaxDistance" << YAML::Value << audioSourceComponent.Config.MaxDistance;
            out << YAML::Key << "ConeInnerAngle" << YAML::Value << audioSourceComponent.Config.ConeInnerAngle;
            out << YAML::Key << "ConeOuterAngle" << YAML::Value << audioSourceComponent.Config.ConeOuterAngle;
            out << YAML::Key << "ConeOuterGain" << YAML::Value << audioSourceComponent.Config.ConeOuterGain;
            out << YAML::Key << "DopplerFactor" << YAML::Value << audioSourceComponent.Config.DopplerFactor;
            out << YAML::Key << "Spread" << YAML::Value << audioSourceComponent.Config.Spread;
            out << YAML::Key << "Focus" << YAML::Value << audioSourceComponent.Config.Focus;
            out << YAML::Key << "LowPassCutoff" << YAML::Value << audioSourceComponent.Config.LowPassCutoff;
            out << YAML::Key << "HighPassCutoff" << YAML::Value << audioSourceComponent.Config.HighPassCutoff;
            out << YAML::Key << "ReverbSend" << YAML::Value << audioSourceComponent.Config.ReverbSend;
            out << YAML::Key << "UseEventSystem" << YAML::Value << audioSourceComponent.UseEventSystem;
            out << YAML::Key << "StartEvent" << YAML::Value << audioSourceComponent.StartEvent;
            // Derive CommandID from StartEvent to keep YAML consistent
            auto derivedCmdID = audioSourceComponent.StartEvent.empty()
                                    ? audioSourceComponent.StartCommandID
                                    : Audio::CommandID::FromString(audioSourceComponent.StartEvent);
            out << YAML::Key << "StartCommandID" << YAML::Value << derivedCmdID.ID;

            out << YAML::EndMap; // AudioSourceComponent
        }

        if (entity.HasComponent<AudioListenerComponent>())
        {
            out << YAML::Key << "AudioListenerComponent";
            out << YAML::BeginMap; // AudioListenerComponent

            const auto& audioListenerComponent = entity.GetComponent<AudioListenerComponent>();
            out << YAML::Key << "Active" << YAML::Value << audioListenerComponent.Active;
            out << YAML::Key << "ConeInnerAngle" << YAML::Value << audioListenerComponent.Config.ConeInnerAngle;
            out << YAML::Key << "ConeOuterAngle" << YAML::Value << audioListenerComponent.Config.ConeOuterAngle;
            out << YAML::Key << "ConeOuterGain" << YAML::Value << audioListenerComponent.Config.ConeOuterGain;

            out << YAML::EndMap; // AudioListenerComponent
        }

        if (entity.HasComponent<AudioSoundGraphComponent>())
        {
            out << YAML::Key << "AudioSoundGraphComponent";
            out << YAML::BeginMap; // AudioSoundGraphComponent

            const auto& sgc = entity.GetComponent<AudioSoundGraphComponent>();
            out << YAML::Key << "SoundGraphHandle" << YAML::Value << static_cast<u64>(sgc.SoundGraphHandle);
            out << YAML::Key << "VolumeMultiplier" << YAML::Value << sgc.VolumeMultiplier;
            out << YAML::Key << "PitchMultiplier" << YAML::Value << sgc.PitchMultiplier;
            out << YAML::Key << "Looping" << YAML::Value << sgc.Looping;
            out << YAML::Key << "PlayOnAwake" << YAML::Value << sgc.PlayOnAwake;

            out << YAML::EndMap; // AudioSoundGraphComponent
        }

        if (entity.HasComponent<VideoOverlayComponent>())
        {
            out << YAML::Key << "VideoOverlayComponent";
            out << YAML::BeginMap; // VideoOverlayComponent

            const auto& voc = entity.GetComponent<VideoOverlayComponent>();
            out << YAML::Key << "VideoPath" << YAML::Value << voc.VideoPath;
            out << YAML::Key << "PlayOnStart" << YAML::Value << voc.PlayOnStart;
            out << YAML::Key << "SkipOnInput" << YAML::Value << voc.SkipOnInput;
            out << YAML::Key << "Looping" << YAML::Value << voc.Looping;
            out << YAML::Key << "Volume" << YAML::Value << voc.Volume;

            out << YAML::EndMap; // VideoOverlayComponent
        }

        if (entity.HasComponent<VideoSurfaceComponent>())
        {
            out << YAML::Key << "VideoSurfaceComponent";
            out << YAML::BeginMap; // VideoSurfaceComponent

            const auto& vsc = entity.GetComponent<VideoSurfaceComponent>();
            out << YAML::Key << "VideoPath" << YAML::Value << vsc.VideoPath;
            out << YAML::Key << "AutoPlay" << YAML::Value << vsc.AutoPlay;
            out << YAML::Key << "Looping" << YAML::Value << vsc.Looping;
            out << YAML::Key << "Volume" << YAML::Value << vsc.Volume;

            out << YAML::EndMap; // VideoSurfaceComponent
        }

        if (entity.HasComponent<SpriteRendererComponent>())
        {
            out << YAML::Key << "SpriteRendererComponent";
            out << YAML::BeginMap; // SpriteRendererComponent

            auto const& spriteRendererComponent = entity.GetComponent<SpriteRendererComponent>();
            out << YAML::Key << "Color" << YAML::Value << spriteRendererComponent.Color;
            if (auto& texture = spriteRendererComponent.Texture)
            {
                out << YAML::Key << "TexturePath" << YAML::Value << texture->GetPath();
                out << YAML::Key << "TilingFactor" << YAML::Value << spriteRendererComponent.TilingFactor;
            }

            out << YAML::EndMap; // SpriteRendererComponent
        }

        if (entity.HasComponent<CircleRendererComponent>())
        {
            out << YAML::Key << "CircleRendererComponent";
            out << YAML::BeginMap; // CircleRendererComponent

            auto const& circleRendererComponent = entity.GetComponent<CircleRendererComponent>();
            out << YAML::Key << "Color" << YAML::Value << circleRendererComponent.Color;
            out << YAML::Key << "Thickness" << YAML::Value << circleRendererComponent.Thickness;
            out << YAML::Key << "Fade" << YAML::Value << circleRendererComponent.Fade;

            out << YAML::EndMap; // CircleRendererComponent
        }

        if (entity.HasComponent<Rigidbody2DComponent>())
        {
            out << YAML::Key << "Rigidbody2DComponent";
            out << YAML::BeginMap; // Rigidbody2DComponent

            auto const& rb2dComponent = entity.GetComponent<Rigidbody2DComponent>();
            out << YAML::Key << "BodyType" << YAML::Value << RigidBody2DBodyTypeToString(rb2dComponent.Type);
            out << YAML::Key << "FixedRotation" << YAML::Value << rb2dComponent.FixedRotation;

            out << YAML::EndMap; // Rigidbody2DComponent
        }

        if (entity.HasComponent<BoxCollider2DComponent>())
        {
            out << YAML::Key << "BoxCollider2DComponent";
            out << YAML::BeginMap; // BoxCollider2DComponent

            auto const& bc2dComponent = entity.GetComponent<BoxCollider2DComponent>();
            out << YAML::Key << "Offset" << YAML::Value << bc2dComponent.Offset;
            out << YAML::Key << "Size" << YAML::Value << bc2dComponent.Size;
            out << YAML::Key << "Density" << YAML::Value << bc2dComponent.Density;
            out << YAML::Key << "Friction" << YAML::Value << bc2dComponent.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << bc2dComponent.Restitution;
            out << YAML::Key << "RestitutionThreshold" << YAML::Value << bc2dComponent.RestitutionThreshold;

            out << YAML::EndMap; // BoxCollider2DComponent
        }

        if (entity.HasComponent<CircleCollider2DComponent>())
        {
            out << YAML::Key << "CircleCollider2DComponent";
            out << YAML::BeginMap; // CircleCollider2DComponent

            auto const& cc2dComponent = entity.GetComponent<CircleCollider2DComponent>();
            out << YAML::Key << "Offset" << YAML::Value << cc2dComponent.Offset;
            out << YAML::Key << "Radius" << YAML::Value << cc2dComponent.Radius;
            out << YAML::Key << "Density" << YAML::Value << cc2dComponent.Density;
            out << YAML::Key << "Friction" << YAML::Value << cc2dComponent.Friction;
            out << YAML::Key << "Restitution" << YAML::Value << cc2dComponent.Restitution;
            out << YAML::Key << "RestitutionThreshold" << YAML::Value << cc2dComponent.RestitutionThreshold;

            out << YAML::EndMap; // CircleCollider2DComponent
        }

        if (entity.HasComponent<TextComponent>())
        {
            out << YAML::Key << "TextComponent";
            out << YAML::BeginMap; // TextComponent

            auto const& textComponent = entity.GetComponent<TextComponent>();
            out << YAML::Key << "TextString" << YAML::Value << textComponent.TextString;
            if (textComponent.FontAsset)
            {
                out << YAML::Key << "FontPath" << YAML::Value << textComponent.FontAsset->GetPath();
            }
            out << YAML::Key << "Color" << YAML::Value << textComponent.Color;
            out << YAML::Key << "Kerning" << YAML::Value << textComponent.Kerning;
            out << YAML::Key << "LineSpacing" << YAML::Value << textComponent.LineSpacing;
            out << YAML::Key << "MaxWidth" << YAML::Value << textComponent.MaxWidth;
            out << YAML::Key << "DropShadow" << YAML::Value << textComponent.DropShadow;
            out << YAML::Key << "ShadowDistance" << YAML::Value << textComponent.ShadowDistance;
            out << YAML::Key << "ShadowColor" << YAML::Value << textComponent.ShadowColor;

            out << YAML::EndMap; // TextComponent
        }

        if (entity.HasComponent<LocalizedTextComponent>())
        {
            out << YAML::Key << "LocalizedTextComponent";
            out << YAML::BeginMap;
            const auto& ltc = entity.GetComponent<LocalizedTextComponent>();
            out << YAML::Key << "LocalizationKey" << YAML::Value << ltc.LocalizationKey;
            out << YAML::EndMap;
        }

        if (entity.HasComponent<MeshComponent>())
        {
            out << YAML::Key << "MeshComponent";
            out << YAML::BeginMap; // MeshComponent

            auto const& meshComponent = entity.GetComponent<MeshComponent>();
            // Only serialize valid asset handles (non-zero); handle 0 indicates uninitialized/runtime data
            if (meshComponent.m_MeshSource && meshComponent.m_MeshSource->GetHandle() != 0)
            {
                out << YAML::Key << "MeshSourceHandle" << YAML::Value << static_cast<u64>(meshComponent.m_MeshSource->GetHandle());
            }

            if (meshComponent.m_Primitive != MeshPrimitive::None)
            {
                out << YAML::Key << "Primitive" << YAML::Value << std::to_underlying(meshComponent.m_Primitive);
            }

            out << YAML::EndMap; // MeshComponent
        }

        if (entity.HasComponent<ModelComponent>())
        {
            out << YAML::Key << "ModelComponent";
            out << YAML::BeginMap; // ModelComponent

            auto const& modelComponent = entity.GetComponent<ModelComponent>();
            out << YAML::Key << "FilePath" << YAML::Value << modelComponent.m_FilePath;
            out << YAML::Key << "Visible" << YAML::Value << modelComponent.m_Visible;

            out << YAML::EndMap; // ModelComponent
        }

        if (entity.HasComponent<InstancedMeshComponent>())
        {
            auto const& imc = entity.GetComponent<InstancedMeshComponent>();
            out << YAML::Key << "InstancedMeshComponent";
            out << YAML::BeginMap; // InstancedMeshComponent

            if (imc.MeshSource && imc.MeshSource->GetHandle() != 0)
                out << YAML::Key << "MeshSourceHandle" << YAML::Value << static_cast<u64>(imc.MeshSource->GetHandle());
            if (imc.OverrideMaterial && imc.OverrideMaterial->GetHandle() != 0)
                out << YAML::Key << "OverrideMaterialHandle" << YAML::Value << static_cast<u64>(imc.OverrideMaterial->GetHandle());

            out << YAML::Key << "FrustumCullPerInstance" << YAML::Value << imc.FrustumCullPerInstance;
            out << YAML::Key << "CastShadows" << YAML::Value << imc.CastShadows;
            out << YAML::Key << "CullDistance" << YAML::Value << imc.CullDistance;
            if (imc.PlacementAssetHandle != 0)
                out << YAML::Key << "PlacementAssetHandle" << YAML::Value << static_cast<u64>(imc.PlacementAssetHandle);
            if (imc.Primitive != MeshPrimitive::None)
                out << YAML::Key << "Primitive" << YAML::Value << std::to_underlying(imc.Primitive);

            // Only Transform, Color, EntityID, and Custom are authored
            // round-trip data. Normal and PrevTransform are runtime-derived.
            out << YAML::Key << "Instances" << YAML::Value << YAML::BeginSeq;
            for (const auto& inst : imc.Instances)
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
                // because the loader assigns the literal 0.0f default (see cpp-coding-quality §2a).
                {
                    constexpr f32 defaultCustom = 0.0f;
                    if (!Math::BitwiseEqual(inst.Custom, defaultCustom))
                        out << YAML::Key << "Custom" << YAML::Value << inst.Custom;
                }
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // InstancedMeshComponent
        }

        if (entity.HasComponent<LODGroupComponent>())
        {
            out << YAML::Key << "LODGroupComponent";
            out << YAML::BeginMap; // LODGroupComponent

            auto const& lodComp = entity.GetComponent<LODGroupComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << lodComp.m_Enabled;
            out << YAML::Key << "Bias" << YAML::Value << lodComp.m_LODGroup.Bias;

            out << YAML::Key << "Levels" << YAML::Value << YAML::BeginSeq;
            for (const auto& level : lodComp.m_LODGroup.Levels)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "MeshHandle" << YAML::Value << static_cast<u64>(level.MeshHandle);
                out << YAML::Key << "MaxDistance" << YAML::Value << level.MaxDistance;
                out << YAML::Key << "TriangleCount" << YAML::Value << level.TriangleCount;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // LODGroupComponent
        }

        if (entity.HasComponent<TileRendererComponent>())
        {
            out << YAML::Key << "TileRendererComponent";
            out << YAML::BeginMap; // TileRendererComponent

            auto const& tileComp = entity.GetComponent<TileRendererComponent>();
            if (tileComp.TileMesh && tileComp.TileMesh->GetMeshSource() && tileComp.TileMesh->GetMeshSource()->GetHandle() != 0)
            {
                out << YAML::Key << "MeshSourceHandle" << YAML::Value << static_cast<u64>(tileComp.TileMesh->GetMeshSource()->GetHandle());
                out << YAML::Key << "SubmeshIndex" << YAML::Value << tileComp.TileMesh->GetSubmeshIndex();
            }
            out << YAML::Key << "Width" << YAML::Value << tileComp.Width;
            out << YAML::Key << "Height" << YAML::Value << tileComp.Height;
            out << YAML::Key << "TileSize" << YAML::Value << tileComp.TileSize;

            out << YAML::Key << "Materials" << YAML::Value << YAML::BeginSeq;
            for (const auto& mat : tileComp.Materials)
            {
                out << YAML::BeginMap;
                auto baseColor = mat.GetBaseColorFactor();
                out << YAML::Key << "AlbedoColor" << YAML::Value << glm::vec3(baseColor.r, baseColor.g, baseColor.b);
                out << YAML::Key << "Metallic" << YAML::Value << mat.GetMetallicFactor();
                out << YAML::Key << "Roughness" << YAML::Value << mat.GetRoughnessFactor();
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "MaterialIDs" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (u8 id : tileComp.MaterialIDs)
            {
                out << static_cast<i32>(id);
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // TileRendererComponent
        }

        if (entity.HasComponent<MaterialComponent>())
        {
            out << YAML::Key << "MaterialComponent";
            out << YAML::BeginMap; // MaterialComponent

            auto const& matComponent = entity.GetComponent<MaterialComponent>();
            auto baseColor = matComponent.m_Material.GetBaseColorFactor();
            out << YAML::Key << "AlbedoColor" << YAML::Value << glm::vec3(baseColor.r, baseColor.g, baseColor.b);
            out << YAML::Key << "Metallic" << YAML::Value << matComponent.m_Material.GetMetallicFactor();
            out << YAML::Key << "Roughness" << YAML::Value << matComponent.m_Material.GetRoughnessFactor();

            if (matComponent.m_ShaderGraphHandle != 0)
                out << YAML::Key << "ShaderGraphHandle" << YAML::Value << static_cast<u64>(matComponent.m_ShaderGraphHandle);

            out << YAML::EndMap; // MaterialComponent
        }

        if (entity.HasComponent<DirectionalLightComponent>())
        {
            out << YAML::Key << "DirectionalLightComponent";
            out << YAML::BeginMap; // DirectionalLightComponent

            auto const& dirLight = entity.GetComponent<DirectionalLightComponent>();
            out << YAML::Key << "Direction" << YAML::Value << dirLight.m_Direction;
            out << YAML::Key << "Color" << YAML::Value << dirLight.m_Color;
            out << YAML::Key << "Intensity" << YAML::Value << dirLight.m_Intensity;
            out << YAML::Key << "CastShadows" << YAML::Value << dirLight.m_CastShadows;
            out << YAML::Key << "ShadowBias" << YAML::Value << dirLight.m_ShadowBias;
            out << YAML::Key << "ShadowNormalBias" << YAML::Value << dirLight.m_ShadowNormalBias;
            out << YAML::Key << "MaxShadowDistance" << YAML::Value << dirLight.m_MaxShadowDistance;
            out << YAML::Key << "CascadeSplitLambda" << YAML::Value << dirLight.m_CascadeSplitLambda;
            out << YAML::Key << "CascadeDebugVisualization" << YAML::Value << dirLight.m_CascadeDebugVisualization;

            out << YAML::EndMap; // DirectionalLightComponent
        }

        if (entity.HasComponent<PointLightComponent>())
        {
            out << YAML::Key << "PointLightComponent";
            out << YAML::BeginMap; // PointLightComponent

            auto const& pointLight = entity.GetComponent<PointLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << pointLight.m_Color;
            out << YAML::Key << "Intensity" << YAML::Value << pointLight.m_Intensity;
            out << YAML::Key << "Range" << YAML::Value << pointLight.m_Range;
            out << YAML::Key << "Attenuation" << YAML::Value << pointLight.m_Attenuation;
            out << YAML::Key << "CastShadows" << YAML::Value << pointLight.m_CastShadows;
            out << YAML::Key << "ShadowBias" << YAML::Value << pointLight.m_ShadowBias;
            out << YAML::Key << "ShadowNormalBias" << YAML::Value << pointLight.m_ShadowNormalBias;

            out << YAML::EndMap; // PointLightComponent
        }

        if (entity.HasComponent<SpotLightComponent>())
        {
            out << YAML::Key << "SpotLightComponent";
            out << YAML::BeginMap; // SpotLightComponent

            auto const& spotLight = entity.GetComponent<SpotLightComponent>();
            out << YAML::Key << "Direction" << YAML::Value << spotLight.m_Direction;
            out << YAML::Key << "Color" << YAML::Value << spotLight.m_Color;
            out << YAML::Key << "Intensity" << YAML::Value << spotLight.m_Intensity;
            out << YAML::Key << "Range" << YAML::Value << spotLight.m_Range;
            out << YAML::Key << "InnerCutoff" << YAML::Value << spotLight.m_InnerCutoff;
            out << YAML::Key << "OuterCutoff" << YAML::Value << spotLight.m_OuterCutoff;
            out << YAML::Key << "Attenuation" << YAML::Value << spotLight.m_Attenuation;
            out << YAML::Key << "CastShadows" << YAML::Value << spotLight.m_CastShadows;
            out << YAML::Key << "ShadowBias" << YAML::Value << spotLight.m_ShadowBias;
            out << YAML::Key << "ShadowNormalBias" << YAML::Value << spotLight.m_ShadowNormalBias;

            out << YAML::EndMap; // SpotLightComponent
        }

        if (entity.HasComponent<SphereAreaLightComponent>())
        {
            out << YAML::Key << "SphereAreaLightComponent";
            out << YAML::BeginMap; // SphereAreaLightComponent

            auto const& areaLight = entity.GetComponent<SphereAreaLightComponent>();
            out << YAML::Key << "Color" << YAML::Value << areaLight.m_Color;
            out << YAML::Key << "Intensity" << YAML::Value << areaLight.m_Intensity;
            out << YAML::Key << "Radius" << YAML::Value << areaLight.m_Radius;
            out << YAML::Key << "Range" << YAML::Value << areaLight.m_Range;
            out << YAML::Key << "CastShadows" << YAML::Value << areaLight.m_CastShadows;

            out << YAML::EndMap; // SphereAreaLightComponent
        }

        if (entity.HasComponent<ProceduralSkyComponent>())
        {
            out << YAML::Key << "ProceduralSkyComponent";
            out << YAML::BeginMap; // ProceduralSkyComponent

            auto const& sky = entity.GetComponent<ProceduralSkyComponent>();
            out << YAML::Key << "SunDirection" << YAML::Value << sky.m_SunDirection;
            out << YAML::Key << "Turbidity" << YAML::Value << sky.m_Turbidity;
            out << YAML::Key << "Exposure" << YAML::Value << sky.m_Exposure;
            out << YAML::Key << "SunIntensity" << YAML::Value << sky.m_SunIntensity;
            out << YAML::Key << "SunDiskSize" << YAML::Value << sky.m_SunDiskSize;
            out << YAML::Key << "ShowSunDisk" << YAML::Value << sky.m_ShowSunDisk;
            out << YAML::Key << "LinkSunToDirectionalLight" << YAML::Value << sky.m_LinkSunToDirectionalLight;
            out << YAML::Key << "EnableSkybox" << YAML::Value << sky.m_EnableSkybox;
            out << YAML::Key << "EnableIBL" << YAML::Value << sky.m_EnableIBL;
            out << YAML::Key << "IBLIntensity" << YAML::Value << sky.m_IBLIntensity;
            out << YAML::Key << "CubemapResolution" << YAML::Value << sky.m_CubemapResolution;

            out << YAML::EndMap; // ProceduralSkyComponent
        }

        if (entity.HasComponent<StarNestSkyComponent>())
        {
            out << YAML::Key << "StarNestSkyComponent";
            out << YAML::BeginMap; // StarNestSkyComponent

            auto const& sky = entity.GetComponent<StarNestSkyComponent>();
            out << YAML::Key << "Offset" << YAML::Value << sky.m_Offset;
            out << YAML::Key << "Rotation1" << YAML::Value << sky.m_Rotation1;
            out << YAML::Key << "Rotation2" << YAML::Value << sky.m_Rotation2;
            out << YAML::Key << "Formuparam" << YAML::Value << sky.m_Formuparam;
            out << YAML::Key << "StepSize" << YAML::Value << sky.m_StepSize;
            out << YAML::Key << "Tile" << YAML::Value << sky.m_Tile;
            out << YAML::Key << "Brightness" << YAML::Value << sky.m_Brightness;
            out << YAML::Key << "DarkMatter" << YAML::Value << sky.m_DarkMatter;
            out << YAML::Key << "DistFading" << YAML::Value << sky.m_DistFading;
            out << YAML::Key << "Saturation" << YAML::Value << sky.m_Saturation;
            out << YAML::Key << "Intensity" << YAML::Value << sky.m_Intensity;
            out << YAML::Key << "Iterations" << YAML::Value << sky.m_Iterations;
            out << YAML::Key << "VolSteps" << YAML::Value << sky.m_VolSteps;
            out << YAML::Key << "EnableSkybox" << YAML::Value << sky.m_EnableSkybox;
            out << YAML::Key << "EnableIBL" << YAML::Value << sky.m_EnableIBL;
            out << YAML::Key << "IBLIntensity" << YAML::Value << sky.m_IBLIntensity;
            out << YAML::Key << "CubemapResolution" << YAML::Value << sky.m_CubemapResolution;

            out << YAML::EndMap; // StarNestSkyComponent
        }

        if (entity.HasComponent<EnvironmentMapComponent>())
        {
            out << YAML::Key << "EnvironmentMapComponent";
            out << YAML::BeginMap; // EnvironmentMapComponent

            auto const& envMap = entity.GetComponent<EnvironmentMapComponent>();
            out << YAML::Key << "FilePath" << YAML::Value << envMap.m_FilePath;
            out << YAML::Key << "IsCubemapFolder" << YAML::Value << envMap.m_IsCubemapFolder;
            out << YAML::Key << "EnableSkybox" << YAML::Value << envMap.m_EnableSkybox;
            out << YAML::Key << "Rotation" << YAML::Value << envMap.m_Rotation;
            out << YAML::Key << "Exposure" << YAML::Value << envMap.m_Exposure;
            out << YAML::Key << "BlurAmount" << YAML::Value << envMap.m_BlurAmount;
            out << YAML::Key << "EnableIBL" << YAML::Value << envMap.m_EnableIBL;
            out << YAML::Key << "IBLIntensity" << YAML::Value << envMap.m_IBLIntensity;
            out << YAML::Key << "UseSphericalHarmonics" << YAML::Value << envMap.m_UseSphericalHarmonics;
            out << YAML::Key << "Tint" << YAML::Value << envMap.m_Tint;

            out << YAML::EndMap; // EnvironmentMapComponent
        }

        if (entity.HasComponent<Rigidbody3DComponent>())
        {
            out << YAML::Key << "Rigidbody3DComponent";
            out << YAML::BeginMap; // Rigidbody3DComponent

            auto const& rb3dComponent = entity.GetComponent<Rigidbody3DComponent>();
            out << YAML::Key << "BodyType" << YAML::Value << std::to_underlying(rb3dComponent.m_Type);
            out << YAML::Key << "Mass" << YAML::Value << rb3dComponent.m_Mass;
            out << YAML::Key << "LinearDrag" << YAML::Value << rb3dComponent.m_LinearDrag;
            out << YAML::Key << "AngularDrag" << YAML::Value << rb3dComponent.m_AngularDrag;
            out << YAML::Key << "DisableGravity" << YAML::Value << rb3dComponent.m_DisableGravity;
            out << YAML::Key << "IsTrigger" << YAML::Value << rb3dComponent.m_IsTrigger;

            out << YAML::EndMap; // Rigidbody3DComponent
        }

        if (entity.HasComponent<BoxCollider3DComponent>())
        {
            out << YAML::Key << "BoxCollider3DComponent";
            out << YAML::BeginMap; // BoxCollider3DComponent

            auto const& bc3dComponent = entity.GetComponent<BoxCollider3DComponent>();
            out << YAML::Key << "HalfExtents" << YAML::Value << bc3dComponent.m_HalfExtents;
            out << YAML::Key << "Offset" << YAML::Value << bc3dComponent.m_Offset;
            out << YAML::Key << "StaticFriction" << YAML::Value << bc3dComponent.m_Material.GetStaticFriction();
            out << YAML::Key << "DynamicFriction" << YAML::Value << bc3dComponent.m_Material.GetDynamicFriction();
            out << YAML::Key << "Restitution" << YAML::Value << bc3dComponent.m_Material.GetRestitution();

            out << YAML::EndMap; // BoxCollider3DComponent
        }

        if (entity.HasComponent<SphereCollider3DComponent>())
        {
            out << YAML::Key << "SphereCollider3DComponent";
            out << YAML::BeginMap; // SphereCollider3DComponent

            auto const& sc3dComponent = entity.GetComponent<SphereCollider3DComponent>();
            out << YAML::Key << "Radius" << YAML::Value << sc3dComponent.m_Radius;
            out << YAML::Key << "Offset" << YAML::Value << sc3dComponent.m_Offset;
            out << YAML::Key << "StaticFriction" << YAML::Value << sc3dComponent.m_Material.GetStaticFriction();
            out << YAML::Key << "DynamicFriction" << YAML::Value << sc3dComponent.m_Material.GetDynamicFriction();
            out << YAML::Key << "Restitution" << YAML::Value << sc3dComponent.m_Material.GetRestitution();

            out << YAML::EndMap; // SphereCollider3DComponent
        }

        if (entity.HasComponent<CapsuleCollider3DComponent>())
        {
            out << YAML::Key << "CapsuleCollider3DComponent";
            out << YAML::BeginMap; // CapsuleCollider3DComponent

            auto const& cc3dComponent = entity.GetComponent<CapsuleCollider3DComponent>();
            out << YAML::Key << "Radius" << YAML::Value << cc3dComponent.m_Radius;
            out << YAML::Key << "HalfHeight" << YAML::Value << cc3dComponent.m_HalfHeight;
            out << YAML::Key << "Offset" << YAML::Value << cc3dComponent.m_Offset;
            out << YAML::Key << "StaticFriction" << YAML::Value << cc3dComponent.m_Material.GetStaticFriction();
            out << YAML::Key << "DynamicFriction" << YAML::Value << cc3dComponent.m_Material.GetDynamicFriction();
            out << YAML::Key << "Restitution" << YAML::Value << cc3dComponent.m_Material.GetRestitution();

            out << YAML::EndMap; // CapsuleCollider3DComponent
        }

        if (entity.HasComponent<PrefabComponent>())
        {
            out << YAML::Key << "PrefabComponent";
            out << YAML::BeginMap; // PrefabComponent

            auto const& prefabComponent = entity.GetComponent<PrefabComponent>();
            out << YAML::Key << "PrefabID" << YAML::Value << prefabComponent.m_PrefabID;
            out << YAML::Key << "PrefabEntityID" << YAML::Value << prefabComponent.m_PrefabEntityID;

            // Serialize override tracking (sorted for deterministic output)
            if (!prefabComponent.m_OverriddenComponents.empty())
            {
                std::vector<std::string> sorted(prefabComponent.m_OverriddenComponents.begin(), prefabComponent.m_OverriddenComponents.end());
                std::ranges::sort(sorted);
                out << YAML::Key << "OverriddenComponents" << YAML::Value << YAML::BeginSeq;
                for (const auto& name : sorted)
                    out << name;
                out << YAML::EndSeq;
            }

            if (!prefabComponent.m_AddedComponents.empty())
            {
                std::vector<std::string> sorted(prefabComponent.m_AddedComponents.begin(), prefabComponent.m_AddedComponents.end());
                std::ranges::sort(sorted);
                out << YAML::Key << "AddedComponents" << YAML::Value << YAML::BeginSeq;
                for (const auto& name : sorted)
                    out << name;
                out << YAML::EndSeq;
            }

            if (!prefabComponent.m_RemovedComponents.empty())
            {
                std::vector<std::string> sorted(prefabComponent.m_RemovedComponents.begin(), prefabComponent.m_RemovedComponents.end());
                std::ranges::sort(sorted);
                out << YAML::Key << "RemovedComponents" << YAML::Value << YAML::BeginSeq;
                for (const auto& name : sorted)
                    out << name;
                out << YAML::EndSeq;
            }

            out << YAML::EndMap; // PrefabComponent
        }

        if (entity.HasComponent<MeshCollider3DComponent>())
        {
            out << YAML::Key << "MeshCollider3DComponent";
            out << YAML::BeginMap; // MeshCollider3DComponent

            auto const& mc3dComponent = entity.GetComponent<MeshCollider3DComponent>();
            out << YAML::Key << "ColliderAsset" << YAML::Value << static_cast<u64>(mc3dComponent.m_ColliderAsset);
            out << YAML::Key << "Offset" << YAML::Value << mc3dComponent.m_Offset;
            out << YAML::Key << "Scale" << YAML::Value << mc3dComponent.m_Scale;
            out << YAML::Key << "UseComplexAsSimple" << YAML::Value << mc3dComponent.m_UseComplexAsSimple;
            out << YAML::Key << "StaticFriction" << YAML::Value << mc3dComponent.m_Material.GetStaticFriction();
            out << YAML::Key << "DynamicFriction" << YAML::Value << mc3dComponent.m_Material.GetDynamicFriction();
            out << YAML::Key << "Restitution" << YAML::Value << mc3dComponent.m_Material.GetRestitution();

            out << YAML::EndMap; // MeshCollider3DComponent
        }

        if (entity.HasComponent<ConvexMeshCollider3DComponent>())
        {
            out << YAML::Key << "ConvexMeshCollider3DComponent";
            out << YAML::BeginMap; // ConvexMeshCollider3DComponent

            auto const& cmc3dComponent = entity.GetComponent<ConvexMeshCollider3DComponent>();
            out << YAML::Key << "ColliderAsset" << YAML::Value << static_cast<u64>(cmc3dComponent.m_ColliderAsset);
            out << YAML::Key << "Offset" << YAML::Value << cmc3dComponent.m_Offset;
            out << YAML::Key << "Scale" << YAML::Value << cmc3dComponent.m_Scale;
            out << YAML::Key << "ConvexRadius" << YAML::Value << cmc3dComponent.m_ConvexRadius;
            out << YAML::Key << "MaxVertices" << YAML::Value << cmc3dComponent.m_MaxVertices;
            out << YAML::Key << "StaticFriction" << YAML::Value << cmc3dComponent.m_Material.GetStaticFriction();
            out << YAML::Key << "DynamicFriction" << YAML::Value << cmc3dComponent.m_Material.GetDynamicFriction();
            out << YAML::Key << "Restitution" << YAML::Value << cmc3dComponent.m_Material.GetRestitution();

            out << YAML::EndMap; // ConvexMeshCollider3DComponent
        }

        if (entity.HasComponent<TriangleMeshCollider3DComponent>())
        {
            out << YAML::Key << "TriangleMeshCollider3DComponent";
            out << YAML::BeginMap; // TriangleMeshCollider3DComponent

            auto const& tmc3dComponent = entity.GetComponent<TriangleMeshCollider3DComponent>();
            out << YAML::Key << "ColliderAsset" << YAML::Value << static_cast<u64>(tmc3dComponent.m_ColliderAsset);
            out << YAML::Key << "Offset" << YAML::Value << tmc3dComponent.m_Offset;
            out << YAML::Key << "Scale" << YAML::Value << tmc3dComponent.m_Scale;
            out << YAML::Key << "StaticFriction" << YAML::Value << tmc3dComponent.m_Material.GetStaticFriction();
            out << YAML::Key << "DynamicFriction" << YAML::Value << tmc3dComponent.m_Material.GetDynamicFriction();
            out << YAML::Key << "Restitution" << YAML::Value << tmc3dComponent.m_Material.GetRestitution();

            out << YAML::EndMap; // TriangleMeshCollider3DComponent
        }

        if (entity.HasComponent<CharacterController3DComponent>())
        {
            out << YAML::Key << "CharacterController3DComponent";
            out << YAML::BeginMap; // CharacterController3DComponent

            auto const& cc3dComponent = entity.GetComponent<CharacterController3DComponent>();
            out << YAML::Key << "SlopeLimitDeg" << YAML::Value << cc3dComponent.m_SlopeLimitDeg;
            out << YAML::Key << "StepOffset" << YAML::Value << cc3dComponent.m_StepOffset;
            out << YAML::Key << "JumpPower" << YAML::Value << cc3dComponent.m_JumpPower;
            out << YAML::Key << "LayerID" << YAML::Value << cc3dComponent.m_LayerID;
            out << YAML::Key << "DisableGravity" << YAML::Value << cc3dComponent.m_DisableGravity;
            out << YAML::Key << "ControlMovementInAir" << YAML::Value << cc3dComponent.m_ControlMovementInAir;
            out << YAML::Key << "ControlRotationInAir" << YAML::Value << cc3dComponent.m_ControlRotationInAir;

            out << YAML::EndMap; // CharacterController3DComponent
        }

        if (entity.HasComponent<PhysicsJoint3DComponent>())
        {
            out << YAML::Key << "PhysicsJoint3DComponent";
            out << YAML::BeginMap; // PhysicsJoint3DComponent

            auto const& joint = entity.GetComponent<PhysicsJoint3DComponent>();
            out << YAML::Key << "JointType" << YAML::Value << std::to_underlying(joint.m_Type);
            out << YAML::Key << "ConnectedEntity" << YAML::Value << joint.m_ConnectedEntity;
            out << YAML::Key << "LocalAnchorA" << YAML::Value << joint.m_LocalAnchorA;
            out << YAML::Key << "LocalAnchorB" << YAML::Value << joint.m_LocalAnchorB;
            out << YAML::Key << "Axis" << YAML::Value << joint.m_Axis;
            out << YAML::Key << "MinDistance" << YAML::Value << joint.m_MinDistance;
            out << YAML::Key << "MaxDistance" << YAML::Value << joint.m_MaxDistance;
            out << YAML::Key << "HingeMinAngleDeg" << YAML::Value << joint.m_HingeMinAngleDeg;
            out << YAML::Key << "HingeMaxAngleDeg" << YAML::Value << joint.m_HingeMaxAngleDeg;
            out << YAML::Key << "SliderMinLimit" << YAML::Value << joint.m_SliderMinLimit;
            out << YAML::Key << "SliderMaxLimit" << YAML::Value << joint.m_SliderMaxLimit;
            out << YAML::Key << "ConeHalfAngleDeg" << YAML::Value << joint.m_ConeHalfAngleDeg;
            out << YAML::Key << "BreakForce" << YAML::Value << joint.m_BreakForce;
            out << YAML::Key << "BreakTorque" << YAML::Value << joint.m_BreakTorque;
            out << YAML::Key << "HingeMotorMode" << YAML::Value << std::to_underlying(joint.m_HingeMotorMode);
            out << YAML::Key << "HingeMotorTargetVelocityDeg" << YAML::Value << joint.m_HingeMotorTargetVelocityDeg;
            out << YAML::Key << "HingeMotorTargetAngleDeg" << YAML::Value << joint.m_HingeMotorTargetAngleDeg;
            out << YAML::Key << "HingeMaxMotorTorque" << YAML::Value << joint.m_HingeMaxMotorTorque;
            out << YAML::Key << "HingeMaxFrictionTorque" << YAML::Value << joint.m_HingeMaxFrictionTorque;
            out << YAML::Key << "HingeLimitSpringFrequency" << YAML::Value << joint.m_HingeLimitSpringFrequency;
            out << YAML::Key << "HingeLimitSpringDamping" << YAML::Value << joint.m_HingeLimitSpringDamping;
            out << YAML::Key << "SliderMotorMode" << YAML::Value << std::to_underlying(joint.m_SliderMotorMode);
            out << YAML::Key << "SliderMotorTargetVelocity" << YAML::Value << joint.m_SliderMotorTargetVelocity;
            out << YAML::Key << "SliderMotorTargetPosition" << YAML::Value << joint.m_SliderMotorTargetPosition;
            out << YAML::Key << "SliderMaxMotorForce" << YAML::Value << joint.m_SliderMaxMotorForce;
            out << YAML::Key << "SliderMaxFrictionForce" << YAML::Value << joint.m_SliderMaxFrictionForce;
            out << YAML::Key << "SliderLimitSpringFrequency" << YAML::Value << joint.m_SliderLimitSpringFrequency;
            out << YAML::Key << "SliderLimitSpringDamping" << YAML::Value << joint.m_SliderLimitSpringDamping;
            out << YAML::Key << "SwingNormalHalfAngleDeg" << YAML::Value << joint.m_SwingNormalHalfAngleDeg;
            out << YAML::Key << "SwingPlaneHalfAngleDeg" << YAML::Value << joint.m_SwingPlaneHalfAngleDeg;
            out << YAML::Key << "TwistMinAngleDeg" << YAML::Value << joint.m_TwistMinAngleDeg;
            out << YAML::Key << "TwistMaxAngleDeg" << YAML::Value << joint.m_TwistMaxAngleDeg;
            out << YAML::Key << "SixDOFTransXMode" << YAML::Value << std::to_underlying(joint.m_SixDOFTransXMode);
            out << YAML::Key << "SixDOFTransYMode" << YAML::Value << std::to_underlying(joint.m_SixDOFTransYMode);
            out << YAML::Key << "SixDOFTransZMode" << YAML::Value << std::to_underlying(joint.m_SixDOFTransZMode);
            out << YAML::Key << "SixDOFRotXMode" << YAML::Value << std::to_underlying(joint.m_SixDOFRotXMode);
            out << YAML::Key << "SixDOFRotYMode" << YAML::Value << std::to_underlying(joint.m_SixDOFRotYMode);
            out << YAML::Key << "SixDOFRotZMode" << YAML::Value << std::to_underlying(joint.m_SixDOFRotZMode);
            out << YAML::Key << "SixDOFTranslationMin" << YAML::Value << joint.m_SixDOFTranslationMin;
            out << YAML::Key << "SixDOFTranslationMax" << YAML::Value << joint.m_SixDOFTranslationMax;
            out << YAML::Key << "SixDOFRotationMinDeg" << YAML::Value << joint.m_SixDOFRotationMinDeg;
            out << YAML::Key << "SixDOFRotationMaxDeg" << YAML::Value << joint.m_SixDOFRotationMaxDeg;

            out << YAML::EndMap; // PhysicsJoint3DComponent
        }

        if (entity.HasComponent<RelationshipComponent>())
        {
            out << YAML::Key << "RelationshipComponent";
            out << YAML::BeginMap; // RelationshipComponent

            auto const& relComponent = entity.GetComponent<RelationshipComponent>();
            out << YAML::Key << "ParentHandle" << YAML::Value << relComponent.m_ParentHandle;
            out << YAML::Key << "Children" << YAML::Value << YAML::BeginSeq;
            for (const auto& childUUID : relComponent.m_Children)
            {
                out << childUUID;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // RelationshipComponent
        }

        if (entity.HasComponent<UICanvasComponent>())
        {
            out << YAML::Key << "UICanvasComponent";
            out << YAML::BeginMap; // UICanvasComponent

            auto const& canvas = entity.GetComponent<UICanvasComponent>();
            out << YAML::Key << "RenderMode" << YAML::Value << static_cast<int>(std::to_underlying(canvas.m_RenderMode));
            out << YAML::Key << "ScaleMode" << YAML::Value << static_cast<int>(std::to_underlying(canvas.m_ScaleMode));
            out << YAML::Key << "SortOrder" << YAML::Value << canvas.m_SortOrder;
            out << YAML::Key << "ReferenceResolution" << YAML::Value << canvas.m_ReferenceResolution;

            out << YAML::EndMap; // UICanvasComponent
        }

        if (entity.HasComponent<UIRectTransformComponent>())
        {
            out << YAML::Key << "UIRectTransformComponent";
            out << YAML::BeginMap; // UIRectTransformComponent

            auto const& rt = entity.GetComponent<UIRectTransformComponent>();
            out << YAML::Key << "AnchorMin" << YAML::Value << rt.m_AnchorMin;
            out << YAML::Key << "AnchorMax" << YAML::Value << rt.m_AnchorMax;
            out << YAML::Key << "AnchoredPosition" << YAML::Value << rt.m_AnchoredPosition;
            out << YAML::Key << "SizeDelta" << YAML::Value << rt.m_SizeDelta;
            out << YAML::Key << "Pivot" << YAML::Value << rt.m_Pivot;
            out << YAML::Key << "Rotation" << YAML::Value << rt.m_Rotation;
            out << YAML::Key << "Scale" << YAML::Value << rt.m_Scale;

            out << YAML::EndMap; // UIRectTransformComponent
        }

        if (entity.HasComponent<UIImageComponent>())
        {
            out << YAML::Key << "UIImageComponent";
            out << YAML::BeginMap; // UIImageComponent

            auto const& image = entity.GetComponent<UIImageComponent>();
            if (image.m_Texture)
            {
                out << YAML::Key << "TexturePath" << YAML::Value << image.m_Texture->GetPath();
            }
            out << YAML::Key << "Color" << YAML::Value << image.m_Color;
            out << YAML::Key << "BorderInsets" << YAML::Value << image.m_BorderInsets;

            out << YAML::EndMap; // UIImageComponent
        }

        if (entity.HasComponent<UIPanelComponent>())
        {
            out << YAML::Key << "UIPanelComponent";
            out << YAML::BeginMap; // UIPanelComponent

            auto const& panel = entity.GetComponent<UIPanelComponent>();
            out << YAML::Key << "BackgroundColor" << YAML::Value << panel.m_BackgroundColor;
            if (panel.m_BackgroundTexture)
            {
                out << YAML::Key << "BackgroundTexturePath" << YAML::Value << panel.m_BackgroundTexture->GetPath();
            }

            out << YAML::EndMap; // UIPanelComponent
        }

        if (entity.HasComponent<UITextComponent>())
        {
            out << YAML::Key << "UITextComponent";
            out << YAML::BeginMap; // UITextComponent

            auto const& text = entity.GetComponent<UITextComponent>();
            out << YAML::Key << "Text" << YAML::Value << text.m_Text;
            if (text.m_FontAsset)
            {
                out << YAML::Key << "FontPath" << YAML::Value << text.m_FontAsset->GetPath();
            }
            out << YAML::Key << "FontSize" << YAML::Value << text.m_FontSize;
            out << YAML::Key << "Color" << YAML::Value << text.m_Color;
            out << YAML::Key << "Alignment" << YAML::Value << static_cast<int>(std::to_underlying(text.m_Alignment));
            out << YAML::Key << "Kerning" << YAML::Value << text.m_Kerning;
            out << YAML::Key << "LineSpacing" << YAML::Value << text.m_LineSpacing;

            out << YAML::EndMap; // UITextComponent
        }

        if (entity.HasComponent<UIButtonComponent>())
        {
            out << YAML::Key << "UIButtonComponent";
            out << YAML::BeginMap; // UIButtonComponent

            auto const& button = entity.GetComponent<UIButtonComponent>();
            out << YAML::Key << "NormalColor" << YAML::Value << button.m_NormalColor;
            out << YAML::Key << "HoveredColor" << YAML::Value << button.m_HoveredColor;
            out << YAML::Key << "PressedColor" << YAML::Value << button.m_PressedColor;
            out << YAML::Key << "DisabledColor" << YAML::Value << button.m_DisabledColor;
            out << YAML::Key << "Interactable" << YAML::Value << button.m_Interactable;

            out << YAML::EndMap; // UIButtonComponent
        }

        if (entity.HasComponent<UISliderComponent>())
        {
            out << YAML::Key << "UISliderComponent";
            out << YAML::BeginMap; // UISliderComponent

            auto const& slider = entity.GetComponent<UISliderComponent>();
            out << YAML::Key << "Value" << YAML::Value << slider.m_Value;
            out << YAML::Key << "MinValue" << YAML::Value << slider.m_MinValue;
            out << YAML::Key << "MaxValue" << YAML::Value << slider.m_MaxValue;
            out << YAML::Key << "Direction" << YAML::Value << static_cast<int>(std::to_underlying(slider.m_Direction));
            out << YAML::Key << "BackgroundColor" << YAML::Value << slider.m_BackgroundColor;
            out << YAML::Key << "FillColor" << YAML::Value << slider.m_FillColor;
            out << YAML::Key << "HandleColor" << YAML::Value << slider.m_HandleColor;
            out << YAML::Key << "Interactable" << YAML::Value << slider.m_Interactable;

            out << YAML::EndMap; // UISliderComponent
        }

        if (entity.HasComponent<UICheckboxComponent>())
        {
            out << YAML::Key << "UICheckboxComponent";
            out << YAML::BeginMap; // UICheckboxComponent

            auto const& checkbox = entity.GetComponent<UICheckboxComponent>();
            out << YAML::Key << "IsChecked" << YAML::Value << checkbox.m_IsChecked;
            out << YAML::Key << "UncheckedColor" << YAML::Value << checkbox.m_UncheckedColor;
            out << YAML::Key << "CheckedColor" << YAML::Value << checkbox.m_CheckedColor;
            out << YAML::Key << "CheckmarkColor" << YAML::Value << checkbox.m_CheckmarkColor;
            out << YAML::Key << "Interactable" << YAML::Value << checkbox.m_Interactable;

            out << YAML::EndMap; // UICheckboxComponent
        }

        if (entity.HasComponent<UIProgressBarComponent>())
        {
            out << YAML::Key << "UIProgressBarComponent";
            out << YAML::BeginMap; // UIProgressBarComponent

            auto const& progress = entity.GetComponent<UIProgressBarComponent>();
            out << YAML::Key << "Value" << YAML::Value << progress.m_Value;
            out << YAML::Key << "MinValue" << YAML::Value << progress.m_MinValue;
            out << YAML::Key << "MaxValue" << YAML::Value << progress.m_MaxValue;
            out << YAML::Key << "FillMethod" << YAML::Value << static_cast<int>(std::to_underlying(progress.m_FillMethod));
            out << YAML::Key << "BackgroundColor" << YAML::Value << progress.m_BackgroundColor;
            out << YAML::Key << "FillColor" << YAML::Value << progress.m_FillColor;

            out << YAML::EndMap; // UIProgressBarComponent
        }

        if (entity.HasComponent<UIWorldAnchorComponent>())
        {
            out << YAML::Key << "UIWorldAnchorComponent";
            out << YAML::BeginMap; // UIWorldAnchorComponent

            auto const& anchor = entity.GetComponent<UIWorldAnchorComponent>();
            out << YAML::Key << "TargetEntity" << YAML::Value << anchor.m_TargetEntity;
            out << YAML::Key << "WorldOffset" << YAML::Value << anchor.m_WorldOffset;

            out << YAML::EndMap; // UIWorldAnchorComponent
        }

        if (entity.HasComponent<UIInputFieldComponent>())
        {
            out << YAML::Key << "UIInputFieldComponent";
            out << YAML::BeginMap; // UIInputFieldComponent

            auto const& input = entity.GetComponent<UIInputFieldComponent>();
            out << YAML::Key << "Text" << YAML::Value << input.m_Text;
            out << YAML::Key << "Placeholder" << YAML::Value << input.m_Placeholder;
            if (input.m_FontAsset)
            {
                out << YAML::Key << "FontPath" << YAML::Value << input.m_FontAsset->GetPath();
            }
            out << YAML::Key << "FontSize" << YAML::Value << input.m_FontSize;
            out << YAML::Key << "TextColor" << YAML::Value << input.m_TextColor;
            out << YAML::Key << "PlaceholderColor" << YAML::Value << input.m_PlaceholderColor;
            out << YAML::Key << "BackgroundColor" << YAML::Value << input.m_BackgroundColor;
            out << YAML::Key << "CharacterLimit" << YAML::Value << input.m_CharacterLimit;
            out << YAML::Key << "Interactable" << YAML::Value << input.m_Interactable;

            out << YAML::EndMap; // UIInputFieldComponent
        }

        if (entity.HasComponent<UIScrollViewComponent>())
        {
            out << YAML::Key << "UIScrollViewComponent";
            out << YAML::BeginMap; // UIScrollViewComponent

            auto const& scrollView = entity.GetComponent<UIScrollViewComponent>();
            out << YAML::Key << "ScrollPosition" << YAML::Value << scrollView.m_ScrollPosition;
            out << YAML::Key << "ContentSize" << YAML::Value << scrollView.m_ContentSize;
            out << YAML::Key << "ScrollDirection" << YAML::Value << static_cast<int>(std::to_underlying(scrollView.m_ScrollDirection));
            out << YAML::Key << "ScrollSpeed" << YAML::Value << scrollView.m_ScrollSpeed;
            out << YAML::Key << "ShowHorizontalScrollbar" << YAML::Value << scrollView.m_ShowHorizontalScrollbar;
            out << YAML::Key << "ShowVerticalScrollbar" << YAML::Value << scrollView.m_ShowVerticalScrollbar;
            out << YAML::Key << "ScrollbarColor" << YAML::Value << scrollView.m_ScrollbarColor;
            out << YAML::Key << "ScrollbarTrackColor" << YAML::Value << scrollView.m_ScrollbarTrackColor;

            out << YAML::EndMap; // UIScrollViewComponent
        }

        if (entity.HasComponent<UIDropdownComponent>())
        {
            out << YAML::Key << "UIDropdownComponent";
            out << YAML::BeginMap; // UIDropdownComponent

            auto const& dropdown = entity.GetComponent<UIDropdownComponent>();
            out << YAML::Key << "Options" << YAML::Value << YAML::BeginSeq;
            for (const auto& option : dropdown.m_Options)
            {
                out << option.m_Label;
            }
            out << YAML::EndSeq;
            out << YAML::Key << "SelectedIndex" << YAML::Value << dropdown.m_SelectedIndex;
            out << YAML::Key << "BackgroundColor" << YAML::Value << dropdown.m_BackgroundColor;
            out << YAML::Key << "HighlightColor" << YAML::Value << dropdown.m_HighlightColor;
            out << YAML::Key << "TextColor" << YAML::Value << dropdown.m_TextColor;
            if (dropdown.m_FontAsset)
            {
                out << YAML::Key << "FontPath" << YAML::Value << dropdown.m_FontAsset->GetPath();
            }
            out << YAML::Key << "FontSize" << YAML::Value << dropdown.m_FontSize;
            out << YAML::Key << "ItemHeight" << YAML::Value << dropdown.m_ItemHeight;
            out << YAML::Key << "Interactable" << YAML::Value << dropdown.m_Interactable;

            out << YAML::EndMap; // UIDropdownComponent
        }

        if (entity.HasComponent<UIGridLayoutComponent>())
        {
            out << YAML::Key << "UIGridLayoutComponent";
            out << YAML::BeginMap; // UIGridLayoutComponent

            auto const& grid = entity.GetComponent<UIGridLayoutComponent>();
            out << YAML::Key << "CellSize" << YAML::Value << grid.m_CellSize;
            out << YAML::Key << "Spacing" << YAML::Value << grid.m_Spacing;
            out << YAML::Key << "Padding" << YAML::Value << grid.m_Padding;
            out << YAML::Key << "StartCorner" << YAML::Value << static_cast<int>(std::to_underlying(grid.m_StartCorner));
            out << YAML::Key << "StartAxis" << YAML::Value << static_cast<int>(std::to_underlying(grid.m_StartAxis));
            out << YAML::Key << "ConstraintCount" << YAML::Value << grid.m_ConstraintCount;

            out << YAML::EndMap; // UIGridLayoutComponent
        }

        if (entity.HasComponent<UIToggleComponent>())
        {
            out << YAML::Key << "UIToggleComponent";
            out << YAML::BeginMap; // UIToggleComponent

            auto const& toggle = entity.GetComponent<UIToggleComponent>();
            out << YAML::Key << "IsOn" << YAML::Value << toggle.m_IsOn;
            out << YAML::Key << "OffColor" << YAML::Value << toggle.m_OffColor;
            out << YAML::Key << "OnColor" << YAML::Value << toggle.m_OnColor;
            out << YAML::Key << "KnobColor" << YAML::Value << toggle.m_KnobColor;
            out << YAML::Key << "Interactable" << YAML::Value << toggle.m_Interactable;

            out << YAML::EndMap; // UIToggleComponent
        }

        if (entity.HasComponent<ParticleSystemComponent>())
        {
            out << YAML::Key << "ParticleSystemComponent";
            out << YAML::BeginMap; // ParticleSystemComponent

            auto const& psc = entity.GetComponent<ParticleSystemComponent>();
            auto const& sys = psc.System;
            auto const& emitter = sys.Emitter;

            out << YAML::Key << "MaxParticles" << YAML::Value << sys.GetMaxParticles();
            out << YAML::Key << "Playing" << YAML::Value << sys.Playing;
            out << YAML::Key << "Looping" << YAML::Value << sys.Looping;
            out << YAML::Key << "Duration" << YAML::Value << sys.Duration;
            out << YAML::Key << "PlaybackSpeed" << YAML::Value << sys.PlaybackSpeed;
            out << YAML::Key << "SimulationSpace" << YAML::Value << static_cast<int>(std::to_underlying(sys.SimulationSpace));

            // Emitter
            out << YAML::Key << "RateOverTime" << YAML::Value << emitter.RateOverTime;
            out << YAML::Key << "InitialSpeed" << YAML::Value << emitter.InitialSpeed;
            out << YAML::Key << "SpeedVariance" << YAML::Value << emitter.SpeedVariance;
            out << YAML::Key << "LifetimeMin" << YAML::Value << emitter.LifetimeMin;
            out << YAML::Key << "LifetimeMax" << YAML::Value << emitter.LifetimeMax;
            out << YAML::Key << "InitialSize" << YAML::Value << emitter.InitialSize;
            out << YAML::Key << "SizeVariance" << YAML::Value << emitter.SizeVariance;
            out << YAML::Key << "InitialRotation" << YAML::Value << emitter.InitialRotation;
            out << YAML::Key << "RotationVariance" << YAML::Value << emitter.RotationVariance;
            out << YAML::Key << "InitialColor" << YAML::Value << emitter.InitialColor;
            out << YAML::Key << "EmissionShapeType" << YAML::Value << std::to_underlying(GetEmissionShapeType(emitter.Shape));

            // Bursts
            out << YAML::Key << "Bursts" << YAML::Value << YAML::BeginSeq;
            for (const auto& burst : emitter.Bursts)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Time" << YAML::Value << burst.Time;
                out << YAML::Key << "Count" << YAML::Value << burst.Count;
                out << YAML::Key << "Probability" << YAML::Value << burst.Probability;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            // Emission shape parameters
            if (auto* sphere = std::get_if<EmitSphere>(&emitter.Shape))
            {
                out << YAML::Key << "EmissionSphereRadius" << YAML::Value << sphere->Radius;
            }
            if (auto* box = std::get_if<EmitBox>(&emitter.Shape))
            {
                out << YAML::Key << "EmissionBoxHalfExtents" << YAML::Value << box->HalfExtents;
            }
            if (auto* cone = std::get_if<EmitCone>(&emitter.Shape))
            {
                out << YAML::Key << "EmissionConeAngle" << YAML::Value << cone->Angle;
                out << YAML::Key << "EmissionConeRadius" << YAML::Value << cone->Radius;
            }
            if (auto* ring = std::get_if<EmitRing>(&emitter.Shape))
            {
                out << YAML::Key << "EmissionRingInnerRadius" << YAML::Value << ring->InnerRadius;
                out << YAML::Key << "EmissionRingOuterRadius" << YAML::Value << ring->OuterRadius;
            }
            if (auto* edge = std::get_if<EmitEdge>(&emitter.Shape))
            {
                out << YAML::Key << "EmissionEdgeLength" << YAML::Value << edge->Length;
            }
            if (auto* mesh = std::get_if<EmitMesh>(&emitter.Shape))
            {
                out << YAML::Key << "EmissionMeshPrimitive" << YAML::Value << mesh->PrimitiveType;
            }

            // Modules
            out << YAML::Key << "GravityEnabled" << YAML::Value << sys.GravityModule.Enabled;
            out << YAML::Key << "Gravity" << YAML::Value << sys.GravityModule.Gravity;
            out << YAML::Key << "DragEnabled" << YAML::Value << sys.DragModule.Enabled;
            out << YAML::Key << "DragCoefficient" << YAML::Value << sys.DragModule.DragCoefficient;
            out << YAML::Key << "ColorOverLifetimeEnabled" << YAML::Value << sys.ColorModule.Enabled;
            ParticleCurveSerializer::Serialize4(out, "ColorCurve", sys.ColorModule.ColorCurve);
            out << YAML::Key << "SizeOverLifetimeEnabled" << YAML::Value << sys.SizeModule.Enabled;
            ParticleCurveSerializer::Serialize(out, "SizeCurve", sys.SizeModule.SizeCurve);
            out << YAML::Key << "RotationOverLifetimeEnabled" << YAML::Value << sys.RotationModule.Enabled;
            out << YAML::Key << "AngularVelocity" << YAML::Value << sys.RotationModule.AngularVelocity;
            out << YAML::Key << "VelocityOverLifetimeEnabled" << YAML::Value << sys.VelocityModule.Enabled;
            out << YAML::Key << "LinearAcceleration" << YAML::Value << sys.VelocityModule.LinearAcceleration;
            out << YAML::Key << "SpeedMultiplier" << YAML::Value << sys.VelocityModule.SpeedMultiplier;
            ParticleCurveSerializer::Serialize(out, "SpeedCurve", sys.VelocityModule.SpeedCurve);
            out << YAML::Key << "NoiseEnabled" << YAML::Value << sys.NoiseModule.Enabled;
            out << YAML::Key << "NoiseStrength" << YAML::Value << sys.NoiseModule.Strength;
            out << YAML::Key << "NoiseFrequency" << YAML::Value << sys.NoiseModule.Frequency;

            // Phase 2: Collision
            out << YAML::Key << "CollisionEnabled" << YAML::Value << sys.CollisionModule.Enabled;
            out << YAML::Key << "CollisionMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.CollisionModule.Mode));
            out << YAML::Key << "CollisionPlaneNormal" << YAML::Value << sys.CollisionModule.PlaneNormal;
            out << YAML::Key << "CollisionPlaneOffset" << YAML::Value << sys.CollisionModule.PlaneOffset;
            out << YAML::Key << "CollisionBounce" << YAML::Value << sys.CollisionModule.Bounce;
            out << YAML::Key << "CollisionLifetimeLoss" << YAML::Value << sys.CollisionModule.LifetimeLoss;
            out << YAML::Key << "CollisionKillOnCollide" << YAML::Value << sys.CollisionModule.KillOnCollide;

            // Phase 2: Force Fields (vector)
            out << YAML::Key << "ForceFields" << YAML::Value << YAML::BeginSeq;
            for (const auto& ff : sys.ForceFields)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Enabled" << YAML::Value << ff.Enabled;
                out << YAML::Key << "Type" << YAML::Value << static_cast<int>(std::to_underlying(ff.Type));
                out << YAML::Key << "Position" << YAML::Value << ff.Position;
                out << YAML::Key << "Strength" << YAML::Value << ff.Strength;
                out << YAML::Key << "Radius" << YAML::Value << ff.Radius;
                out << YAML::Key << "Axis" << YAML::Value << ff.Axis;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            // Phase 2: Trail
            out << YAML::Key << "TrailEnabled" << YAML::Value << sys.TrailModule.Enabled;
            out << YAML::Key << "TrailMaxPoints" << YAML::Value << sys.TrailModule.MaxTrailPoints;
            out << YAML::Key << "TrailLifetime" << YAML::Value << sys.TrailModule.TrailLifetime;
            out << YAML::Key << "TrailMinVertexDistance" << YAML::Value << sys.TrailModule.MinVertexDistance;
            out << YAML::Key << "TrailWidthStart" << YAML::Value << sys.TrailModule.WidthStart;
            out << YAML::Key << "TrailWidthEnd" << YAML::Value << sys.TrailModule.WidthEnd;
            out << YAML::Key << "TrailColorStart" << YAML::Value << sys.TrailModule.ColorStart;
            out << YAML::Key << "TrailColorEnd" << YAML::Value << sys.TrailModule.ColorEnd;

            // Phase 2: Sub-Emitter
            out << YAML::Key << "SubEmitterEnabled" << YAML::Value << sys.SubEmitterModule.Enabled;

            // Phase 2: LOD
            out << YAML::Key << "LODDistance1" << YAML::Value << sys.LODDistance1;
            out << YAML::Key << "LODMaxDistance" << YAML::Value << sys.LODMaxDistance;

            // Warm-up
            out << YAML::Key << "WarmUpTime" << YAML::Value << sys.WarmUpTime;

            // Rendering settings
            out << YAML::Key << "BlendMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.BlendMode));
            out << YAML::Key << "RenderMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.RenderMode));
            out << YAML::Key << "DepthSortEnabled" << YAML::Value << sys.DepthSortEnabled;
            out << YAML::Key << "UseGPU" << YAML::Value << sys.UseGPU;
            out << YAML::Key << "SoftParticlesEnabled" << YAML::Value << sys.SoftParticlesEnabled;
            out << YAML::Key << "SoftParticleDistance" << YAML::Value << sys.SoftParticleDistance;
            out << YAML::Key << "VelocityInheritance" << YAML::Value << sys.VelocityInheritance;

            // GPU Wind / Noise / Ground Collision
            out << YAML::Key << "WindInfluence" << YAML::Value << sys.WindInfluence;
            out << YAML::Key << "GPUNoiseStrength" << YAML::Value << sys.GPUNoiseStrength;
            out << YAML::Key << "GPUNoiseFrequency" << YAML::Value << sys.GPUNoiseFrequency;
            out << YAML::Key << "GPUGroundCollision" << YAML::Value << sys.GPUGroundCollision;
            out << YAML::Key << "GPUGroundY" << YAML::Value << sys.GPUGroundY;
            out << YAML::Key << "GPUCollisionBounce" << YAML::Value << sys.GPUCollisionBounce;
            out << YAML::Key << "GPUCollisionFriction" << YAML::Value << sys.GPUCollisionFriction;

            // Texture sheet animation
            out << YAML::Key << "TextureSheetEnabled" << YAML::Value << sys.TextureSheetModule.Enabled;
            out << YAML::Key << "TextureSheetGridX" << YAML::Value << sys.TextureSheetModule.GridX;
            out << YAML::Key << "TextureSheetGridY" << YAML::Value << sys.TextureSheetModule.GridY;
            out << YAML::Key << "TextureSheetTotalFrames" << YAML::Value << sys.TextureSheetModule.TotalFrames;
            out << YAML::Key << "TextureSheetMode" << YAML::Value << static_cast<int>(std::to_underlying(sys.TextureSheetModule.Mode));
            out << YAML::Key << "TextureSheetSpeedRange" << YAML::Value << sys.TextureSheetModule.SpeedRange;

            out << YAML::EndMap; // ParticleSystemComponent
        }

        if (entity.HasComponent<TerrainComponent>())
        {
            out << YAML::Key << "TerrainComponent";
            out << YAML::BeginMap; // TerrainComponent

            auto const& terrain = entity.GetComponent<TerrainComponent>();
            out << YAML::Key << "HeightmapPath" << YAML::Value << terrain.m_HeightmapPath;
            out << YAML::Key << "WorldSizeX" << YAML::Value << terrain.m_WorldSizeX;
            out << YAML::Key << "WorldSizeZ" << YAML::Value << terrain.m_WorldSizeZ;
            out << YAML::Key << "HeightScale" << YAML::Value << terrain.m_HeightScale;

            // Procedural generation settings
            out << YAML::Key << "ProceduralEnabled" << YAML::Value << terrain.m_ProceduralEnabled;
            out << YAML::Key << "ProceduralSeed" << YAML::Value << terrain.m_ProceduralSeed;
            out << YAML::Key << "ProceduralResolution" << YAML::Value << terrain.m_ProceduralResolution;
            out << YAML::Key << "ProceduralOctaves" << YAML::Value << terrain.m_ProceduralOctaves;
            out << YAML::Key << "ProceduralFrequency" << YAML::Value << terrain.m_ProceduralFrequency;
            out << YAML::Key << "ProceduralLacunarity" << YAML::Value << terrain.m_ProceduralLacunarity;
            out << YAML::Key << "ProceduralPersistence" << YAML::Value << terrain.m_ProceduralPersistence;

            // Advanced height-field shaping
            out << YAML::Key << "ShapingRidgeBlend" << YAML::Value << terrain.m_HeightShaping.RidgeBlend;
            out << YAML::Key << "ShapingWarpStrength" << YAML::Value << terrain.m_HeightShaping.WarpStrength;
            out << YAML::Key << "ShapingWarpFrequency" << YAML::Value << terrain.m_HeightShaping.WarpFrequency;
            out << YAML::Key << "ShapingTerraceSteps" << YAML::Value << terrain.m_HeightShaping.TerraceSteps;
            out << YAML::Key << "ShapingTerraceSharpness" << YAML::Value << terrain.m_HeightShaping.TerraceSharpness;
            out << YAML::Key << "ShapingHeightExponent" << YAML::Value << terrain.m_HeightShaping.HeightExponent;

            // Automatic material assignment rules
            out << YAML::Key << "AutoMaterial" << YAML::Value << terrain.m_AutoMaterial;
            out << YAML::Key << "SplatmapGenResolution" << YAML::Value << terrain.m_SplatmapGenResolution;
            if (!terrain.m_LayerRules.empty())
            {
                out << YAML::Key << "LayerRules";
                out << YAML::Value << YAML::BeginSeq;
                for (const auto& rule : terrain.m_LayerRules)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "LayerIndex" << YAML::Value << rule.LayerIndex;
                    out << YAML::Key << "MinHeight" << YAML::Value << rule.MinHeight;
                    out << YAML::Key << "MaxHeight" << YAML::Value << rule.MaxHeight;
                    out << YAML::Key << "HeightBlend" << YAML::Value << rule.HeightBlend;
                    out << YAML::Key << "MinSlopeDeg" << YAML::Value << rule.MinSlopeDeg;
                    out << YAML::Key << "MaxSlopeDeg" << YAML::Value << rule.MaxSlopeDeg;
                    out << YAML::Key << "SlopeBlend" << YAML::Value << rule.SlopeBlend;
                    out << YAML::Key << "Strength" << YAML::Value << rule.Strength;
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            out << YAML::Key << "TessellationEnabled" << YAML::Value << terrain.m_TessellationEnabled;
            out << YAML::Key << "TargetTriangleSize" << YAML::Value << terrain.m_TargetTriangleSize;
            out << YAML::Key << "MorphRegion" << YAML::Value << terrain.m_MorphRegion;

            // Streaming settings
            out << YAML::Key << "StreamingEnabled" << YAML::Value << terrain.m_StreamingEnabled;
            out << YAML::Key << "TileDirectory" << YAML::Value << terrain.m_TileDirectory;
            out << YAML::Key << "TileFilePattern" << YAML::Value << terrain.m_TileFilePattern;
            out << YAML::Key << "TileWorldSize" << YAML::Value << terrain.m_TileWorldSize;
            out << YAML::Key << "TileResolution" << YAML::Value << terrain.m_TileResolution;
            out << YAML::Key << "StreamingLoadRadius" << YAML::Value << terrain.m_StreamingLoadRadius;
            out << YAML::Key << "StreamingMaxTiles" << YAML::Value << terrain.m_StreamingMaxTiles;

            // Voxel override settings
            out << YAML::Key << "VoxelEnabled" << YAML::Value << terrain.m_VoxelEnabled;
            out << YAML::Key << "VoxelSize" << YAML::Value << terrain.m_VoxelSize;

            // Terrain material layers
            if (terrain.m_Material && terrain.m_Material->GetLayerCount() > 0)
            {
                const auto& mat = terrain.m_Material;
                out << YAML::Key << "SplatmapPath0" << YAML::Value << mat->GetSplatmapPath(0);
                out << YAML::Key << "SplatmapPath1" << YAML::Value << mat->GetSplatmapPath(1);
                out << YAML::Key << "Layers";
                out << YAML::Value << YAML::BeginSeq;
                for (u32 i = 0; i < mat->GetLayerCount(); ++i)
                {
                    const auto& layer = mat->GetLayer(i);
                    out << YAML::BeginMap;
                    out << YAML::Key << "Name" << YAML::Value << layer.Name;
                    out << YAML::Key << "AlbedoPath" << YAML::Value << layer.AlbedoPath;
                    out << YAML::Key << "NormalPath" << YAML::Value << layer.NormalPath;
                    out << YAML::Key << "ARMPath" << YAML::Value << layer.ARMPath;
                    out << YAML::Key << "TilingScale" << YAML::Value << layer.TilingScale;
                    out << YAML::Key << "HeightBlendSharpness" << YAML::Value << layer.HeightBlendSharpness;
                    out << YAML::Key << "TriplanarSharpness" << YAML::Value << layer.TriplanarSharpness;
                    out << YAML::Key << "BaseColor" << YAML::Value << layer.BaseColor;
                    out << YAML::Key << "Roughness" << YAML::Value << layer.Roughness;
                    out << YAML::Key << "Metallic" << YAML::Value << layer.Metallic;
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap; // TerrainComponent
        }

        if (entity.HasComponent<FoliageComponent>())
        {
            out << YAML::Key << "FoliageComponent";
            out << YAML::BeginMap;

            auto const& foliage = entity.GetComponent<FoliageComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << foliage.m_Enabled;

            if (!foliage.m_Layers.empty())
            {
                out << YAML::Key << "Layers";
                out << YAML::Value << YAML::BeginSeq;
                for (const auto& layer : foliage.m_Layers)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Name" << YAML::Value << layer.Name;
                    out << YAML::Key << "MeshPath" << YAML::Value << layer.MeshPath;
                    out << YAML::Key << "AlbedoPath" << YAML::Value << layer.AlbedoPath;
                    out << YAML::Key << "Density" << YAML::Value << layer.Density;
                    out << YAML::Key << "SplatmapChannel" << YAML::Value << layer.SplatmapChannel;
                    out << YAML::Key << "MinSlopeAngle" << YAML::Value << layer.MinSlopeAngle;
                    out << YAML::Key << "MaxSlopeAngle" << YAML::Value << layer.MaxSlopeAngle;
                    out << YAML::Key << "MinScale" << YAML::Value << layer.MinScale;
                    out << YAML::Key << "MaxScale" << YAML::Value << layer.MaxScale;
                    out << YAML::Key << "MinHeight" << YAML::Value << layer.MinHeight;
                    out << YAML::Key << "MaxHeight" << YAML::Value << layer.MaxHeight;
                    out << YAML::Key << "RandomRotation" << YAML::Value << layer.RandomRotation;
                    out << YAML::Key << "ViewDistance" << YAML::Value << layer.ViewDistance;
                    out << YAML::Key << "FadeStartDistance" << YAML::Value << layer.FadeStartDistance;
                    out << YAML::Key << "WindStrength" << YAML::Value << layer.WindStrength;
                    out << YAML::Key << "WindSpeed" << YAML::Value << layer.WindSpeed;
                    out << YAML::Key << "BaseColor" << YAML::Value << layer.BaseColor;
                    out << YAML::Key << "Roughness" << YAML::Value << layer.Roughness;
                    out << YAML::Key << "AlphaCutoff" << YAML::Value << layer.AlphaCutoff;
                    out << YAML::Key << "Enabled" << YAML::Value << layer.Enabled;
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            out << YAML::EndMap; // FoliageComponent
        }

        if (entity.HasComponent<WaterComponent>())
        {
            out << YAML::Key << "WaterComponent";
            out << YAML::BeginMap;

            auto const& water = entity.GetComponent<WaterComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << water.m_Enabled;
            out << YAML::Key << "WorldSizeX" << YAML::Value << water.m_WorldSizeX;
            out << YAML::Key << "WorldSizeZ" << YAML::Value << water.m_WorldSizeZ;
            out << YAML::Key << "GridResolutionX" << YAML::Value << water.m_GridResolutionX;
            out << YAML::Key << "GridResolutionZ" << YAML::Value << water.m_GridResolutionZ;
            out << YAML::Key << "WaveAmplitude" << YAML::Value << water.m_WaveAmplitude;
            out << YAML::Key << "WaveFrequency" << YAML::Value << water.m_WaveFrequency;
            out << YAML::Key << "WaveSpeed" << YAML::Value << water.m_WaveSpeed;
            out << YAML::Key << "WaveDir0" << YAML::Value << water.m_WaveDir0;
            out << YAML::Key << "WaveDir1" << YAML::Value << water.m_WaveDir1;
            out << YAML::Key << "WaveSteepness0" << YAML::Value << water.m_WaveSteepness0;
            out << YAML::Key << "Wavelength0" << YAML::Value << water.m_Wavelength0;
            out << YAML::Key << "WaveSteepness1" << YAML::Value << water.m_WaveSteepness1;
            out << YAML::Key << "Wavelength1" << YAML::Value << water.m_Wavelength1;
            out << YAML::Key << "WaterColor" << YAML::Value << water.m_WaterColor;
            out << YAML::Key << "DeepColor" << YAML::Value << water.m_DeepColor;
            out << YAML::Key << "Transparency" << YAML::Value << water.m_Transparency;
            out << YAML::Key << "Reflectivity" << YAML::Value << water.m_Reflectivity;
            out << YAML::Key << "FresnelPower" << YAML::Value << water.m_FresnelPower;
            out << YAML::Key << "SpecularIntensity" << YAML::Value << water.m_SpecularIntensity;
            out << YAML::Key << "NormalMapScrollDir0" << YAML::Value << water.m_NormalMapScrollDir0;
            out << YAML::Key << "NormalMapScrollDir1" << YAML::Value << water.m_NormalMapScrollDir1;
            out << YAML::Key << "NormalMapScrollSpeed0" << YAML::Value << water.m_NormalMapScrollSpeed0;
            out << YAML::Key << "NormalMapScrollSpeed1" << YAML::Value << water.m_NormalMapScrollSpeed1;
            out << YAML::Key << "NormalMapTiling" << YAML::Value << water.m_NormalMapTiling;
            out << YAML::Key << "NoiseIntensity" << YAML::Value << water.m_NoiseIntensity;
            out << YAML::Key << "NormalMap0" << YAML::Value << water.m_NormalMap0;
            out << YAML::Key << "NormalMap1" << YAML::Value << water.m_NormalMap1;
            out << YAML::Key << "NoiseTexture" << YAML::Value << water.m_NoiseTexture;
            out << YAML::Key << "DepthSofteningDistance" << YAML::Value << water.m_DepthSofteningDistance;
            out << YAML::Key << "RefractionDistortion" << YAML::Value << water.m_RefractionDistortion;
            out << YAML::Key << "RefractionHeightFactor" << YAML::Value << water.m_RefractionHeightFactor;
            out << YAML::Key << "RefractionColor" << YAML::Value << water.m_RefractionColor;
            out << YAML::Key << "RefractionEnabled" << YAML::Value << water.m_RefractionEnabled;
            out << YAML::Key << "FoamTexture" << YAML::Value << water.m_FoamTexture;
            out << YAML::Key << "FoamHeightStart" << YAML::Value << water.m_FoamHeightStart;
            out << YAML::Key << "FoamFadeDistance" << YAML::Value << water.m_FoamFadeDistance;
            out << YAML::Key << "FoamTiling" << YAML::Value << water.m_FoamTiling;
            out << YAML::Key << "FoamBrightness" << YAML::Value << water.m_FoamBrightness;
            out << YAML::Key << "FoamAngleExponent" << YAML::Value << water.m_FoamAngleExponent;
            out << YAML::Key << "ShorelineFoamPower" << YAML::Value << water.m_ShorelineFoamPower;
            out << YAML::Key << "SSSColor" << YAML::Value << water.m_SSSColor;
            out << YAML::Key << "SSSIntensity" << YAML::Value << water.m_SSSIntensity;
            out << YAML::Key << "SSRMaxSteps" << YAML::Value << water.m_SSRMaxSteps;
            out << YAML::Key << "SSRStepSize" << YAML::Value << water.m_SSRStepSize;
            out << YAML::Key << "SSRMaxDistance" << YAML::Value << water.m_SSRMaxDistance;
            out << YAML::Key << "SSRThickness" << YAML::Value << water.m_SSRThickness;
            out << YAML::Key << "SSREnabled" << YAML::Value << water.m_SSREnabled;
            out << YAML::Key << "TessellationEnabled" << YAML::Value << water.m_TessellationEnabled;
            out << YAML::Key << "TessellationFactor" << YAML::Value << water.m_TessellationFactor;
            out << YAML::Key << "TessMinDistance" << YAML::Value << water.m_TessMinDistance;
            out << YAML::Key << "TessMaxDistance" << YAML::Value << water.m_TessMaxDistance;
            out << YAML::Key << "UnderwaterFogColor" << YAML::Value << water.m_UnderwaterFogColor;
            out << YAML::Key << "UnderwaterFogDensity" << YAML::Value << water.m_UnderwaterFogDensity;
            out << YAML::Key << "UnderwaterRefractionStrength" << YAML::Value << water.m_UnderwaterRefractionStrength;
            out << YAML::Key << "UnderwaterRefractionScale" << YAML::Value << water.m_UnderwaterRefractionScale;
            out << YAML::Key << "UnderwaterRefractionSpeed" << YAML::Value << water.m_UnderwaterRefractionSpeed;
            out << YAML::Key << "UnderwaterChromaticStrength" << YAML::Value << water.m_UnderwaterChromaticStrength;
            out << YAML::Key << "CausticsIntensity" << YAML::Value << water.m_CausticsIntensity;
            out << YAML::Key << "CausticsScale" << YAML::Value << water.m_CausticsScale;
            out << YAML::Key << "CausticsSpeed" << YAML::Value << water.m_CausticsSpeed;
            out << YAML::Key << "CausticsMaxDepth" << YAML::Value << water.m_CausticsMaxDepth;
            out << YAML::Key << "CausticsColor" << YAML::Value << water.m_CausticsColor;
            out << YAML::Key << "GodRayIntensity" << YAML::Value << water.m_GodRayIntensity;
            out << YAML::Key << "GodRayDecay" << YAML::Value << water.m_GodRayDecay;
            out << YAML::Key << "GodRayDensity" << YAML::Value << water.m_GodRayDensity;
            out << YAML::Key << "GodRayWeight" << YAML::Value << water.m_GodRayWeight;
            out << YAML::Key << "GodRayColor" << YAML::Value << water.m_GodRayColor;
            out << YAML::Key << "GodRaySamples" << YAML::Value << water.m_GodRaySamples;
            out << YAML::Key << "GodRayDappleFloor" << YAML::Value << water.m_GodRayDappleFloor;
            out << YAML::Key << "GodRaySunFalloff" << YAML::Value << water.m_GodRaySunFalloff;
            out << YAML::Key << "RenderFromBelow" << YAML::Value << water.m_RenderFromBelow;

            // FFT ocean (WATER_FUTURE_IMPROVEMENTS.md §1)
            out << YAML::Key << "UseFFT" << YAML::Value << water.m_UseFFT;
            out << YAML::Key << "FFTResolution" << YAML::Value << water.m_FFTResolution;
            out << YAML::Key << "FFTPatchSize" << YAML::Value << water.m_FFTPatchSize;
            out << YAML::Key << "FFTWindSpeed" << YAML::Value << water.m_FFTWindSpeed;
            out << YAML::Key << "FFTWindDirection" << YAML::Value << water.m_FFTWindDirection;
            out << YAML::Key << "FFTAmplitude" << YAML::Value << water.m_FFTAmplitude;
            out << YAML::Key << "FFTChoppiness" << YAML::Value << water.m_FFTChoppiness;
            out << YAML::Key << "FFTHeightScale" << YAML::Value << water.m_FFTHeightScale;
            out << YAML::Key << "FFTSeed" << YAML::Value << water.m_FFTSeed;
            out << YAML::Key << "FFTUseGpuCompute" << YAML::Value << water.m_FFTUseGpuCompute;

            out << YAML::EndMap; // WaterComponent
        }

        if (entity.HasComponent<BuoyancyComponent>())
        {
            out << YAML::Key << "BuoyancyComponent";
            out << YAML::BeginMap;

            auto const& buoyancy = entity.GetComponent<BuoyancyComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << buoyancy.m_Enabled;
            out << YAML::Key << "ProbeExtents" << YAML::Value << buoyancy.m_ProbeExtents;
            out << YAML::Key << "FluidDensity" << YAML::Value << buoyancy.m_FluidDensity;
            out << YAML::Key << "BuoyancyScale" << YAML::Value << buoyancy.m_BuoyancyScale;
            out << YAML::Key << "LinearDrag" << YAML::Value << buoyancy.m_LinearDrag;
            out << YAML::Key << "AngularDrag" << YAML::Value << buoyancy.m_AngularDrag;
            out << YAML::Key << "SubmergenceRamp" << YAML::Value << buoyancy.m_SubmergenceRamp;

            out << YAML::EndMap; // BuoyancyComponent
        }

        if (entity.HasComponent<SnowDeformerComponent>())
        {
            out << YAML::Key << "SnowDeformerComponent";
            out << YAML::BeginMap;

            auto const& sd = entity.GetComponent<SnowDeformerComponent>();
            out << YAML::Key << "DeformRadius" << YAML::Value << sd.m_DeformRadius;
            out << YAML::Key << "DeformDepth" << YAML::Value << sd.m_DeformDepth;
            out << YAML::Key << "FalloffExponent" << YAML::Value << sd.m_FalloffExponent;
            out << YAML::Key << "CompactionFactor" << YAML::Value << sd.m_CompactionFactor;
            out << YAML::Key << "EmitEjecta" << YAML::Value << sd.m_EmitEjecta;

            out << YAML::EndMap; // SnowDeformerComponent
        }

        if (entity.HasComponent<FogVolumeComponent>())
        {
            out << YAML::Key << "FogVolumeComponent";
            out << YAML::BeginMap;

            auto const& fv = entity.GetComponent<FogVolumeComponent>();
            out << YAML::Key << "Shape" << YAML::Value << std::to_underlying(fv.m_Shape);
            out << YAML::Key << "Extents" << YAML::Value << fv.m_Extents;
            out << YAML::Key << "Color" << YAML::Value << fv.m_Color;
            out << YAML::Key << "Density" << YAML::Value << fv.m_Density;
            out << YAML::Key << "FalloffDistance" << YAML::Value << fv.m_FalloffDistance;
            out << YAML::Key << "Priority" << YAML::Value << fv.m_Priority;
            out << YAML::Key << "BlendWeight" << YAML::Value << fv.m_BlendWeight;
            out << YAML::Key << "Enabled" << YAML::Value << fv.m_Enabled;
            out << YAML::Key << "AffectTransparent" << YAML::Value << fv.m_AffectTransparent;

            out << YAML::EndMap; // FogVolumeComponent
        }

        if (entity.HasComponent<DecalComponent>())
        {
            out << YAML::Key << "DecalComponent";
            out << YAML::BeginMap;

            auto const& dc = entity.GetComponent<DecalComponent>();
            out << YAML::Key << "Color" << YAML::Value << dc.m_Color;
            out << YAML::Key << "Size" << YAML::Value << dc.m_Size;
            out << YAML::Key << "FadeDistance" << YAML::Value << dc.m_FadeDistance;
            out << YAML::Key << "NormalAngleThreshold" << YAML::Value << dc.m_NormalAngleThreshold;

            if (dc.m_AlbedoTexture)
            {
                out << YAML::Key << "AlbedoTexturePath" << YAML::Value << dc.m_AlbedoTexture->GetPath();
            }
            if (dc.m_NormalTexture)
            {
                out << YAML::Key << "NormalTexturePath" << YAML::Value << dc.m_NormalTexture->GetPath();
            }
            if (dc.m_RMATexture)
            {
                out << YAML::Key << "RMATexturePath" << YAML::Value << dc.m_RMATexture->GetPath();
            }
            if (dc.m_EmissiveTexture)
            {
                out << YAML::Key << "EmissiveTexturePath" << YAML::Value << dc.m_EmissiveTexture->GetPath();
            }
            out << YAML::Key << "Mode" << YAML::Value << static_cast<u32>(std::to_underlying(dc.m_Mode));
            out << YAML::Key << "Transparent" << YAML::Value << dc.m_Transparent;

            out << YAML::EndMap; // DecalComponent
        }

        if (entity.HasComponent<SubmeshComponent>())
        {
            out << YAML::Key << "SubmeshComponent";
            out << YAML::BeginMap; // SubmeshComponent

            auto const& submeshComponent = entity.GetComponent<SubmeshComponent>();
            out << YAML::Key << "SubmeshIndex" << YAML::Value << submeshComponent.m_SubmeshIndex;
            out << YAML::Key << "Visible" << YAML::Value << submeshComponent.m_Visible;
            // Note: m_Mesh and m_BoneEntityIds are runtime data, reconstructed from parent MeshComponent

            out << YAML::EndMap; // SubmeshComponent
        }

        if (entity.HasComponent<AnimationStateComponent>())
        {
            out << YAML::Key << "AnimationStateComponent";
            out << YAML::BeginMap; // AnimationStateComponent

            auto const& animComponent = entity.GetComponent<AnimationStateComponent>();
            out << YAML::Key << "State" << YAML::Value << std::to_underlying(animComponent.m_State);
            out << YAML::Key << "CurrentTime" << YAML::Value << animComponent.m_CurrentTime;
            out << YAML::Key << "BlendDuration" << YAML::Value << animComponent.m_BlendDuration;
            out << YAML::Key << "CurrentClipIndex" << YAML::Value << animComponent.m_CurrentClipIndex;
            out << YAML::Key << "IsPlaying" << YAML::Value << animComponent.m_IsPlaying;
            // Store source file path as relative path for portability
            if (!animComponent.m_SourceFilePath.empty())
            {
                std::filesystem::path sourcePath(animComponent.m_SourceFilePath);
                auto assetDirectory = Project::GetAssetDirectory();
                auto relativePath = std::filesystem::relative(sourcePath, assetDirectory);
                out << YAML::Key << "SourceFilePath" << YAML::Value << relativePath.generic_string();
            }
            else
            {
                out << YAML::Key << "SourceFilePath" << YAML::Value << "";
            }
            // Save current clip name for reference (helps with debugging)
            if (animComponent.m_CurrentClip)
            {
                out << YAML::Key << "CurrentClipName" << YAML::Value << animComponent.m_CurrentClip->Name;
            }

            out << YAML::EndMap; // AnimationStateComponent
        }

        if (entity.HasComponent<SkeletonComponent>())
        {
            out << YAML::Key << "SkeletonComponent";
            out << YAML::BeginMap; // SkeletonComponent

            // Note: Skeleton is typically loaded from model file, stored as reference
            // The cache is runtime data, not serialized

            out << YAML::EndMap; // SkeletonComponent
        }

        if (entity.HasComponent<AnimationGraphComponent>())
        {
            out << YAML::Key << "AnimationGraphComponent";
            out << YAML::BeginMap;

            auto const& graphComp = entity.GetComponent<AnimationGraphComponent>();
            out << YAML::Key << "AssetHandle" << YAML::Value << graphComp.AnimationGraphAssetHandle;

            out << YAML::EndMap; // AnimationGraphComponent
        }

        if (entity.HasComponent<CinematicComponent>())
        {
            out << YAML::Key << "CinematicComponent";
            out << YAML::BeginMap;

            auto const& cine = entity.GetComponent<CinematicComponent>();
            out << YAML::Key << "Sequence" << YAML::Value << static_cast<u64>(cine.Sequence);
            out << YAML::Key << "PlayOnStart" << YAML::Value << cine.PlayOnStart;
            out << YAML::Key << "Loop" << YAML::Value << cine.Loop;
            out << YAML::Key << "PlaybackSpeed" << YAML::Value << cine.PlaybackSpeed;

            out << YAML::EndMap; // CinematicComponent
        }

        if (entity.HasComponent<MorphTargetComponent>())
        {
            out << YAML::Key << "MorphTargetComponent";
            out << YAML::BeginMap; // MorphTargetComponent

            auto const& morphComp = entity.GetComponent<MorphTargetComponent>();

            // Serialize weights (sorted by name for deterministic output)
            out << YAML::Key << "Weights";
            out << YAML::BeginMap;
            std::vector<std::string> sortedNames;
            sortedNames.reserve(morphComp.Weights.size());
            for (const auto& [name, weight] : morphComp.Weights)
                sortedNames.push_back(name);
            std::ranges::sort(sortedNames);
            for (const auto& name : sortedNames)
            {
                out << YAML::Key << name << YAML::Value << morphComp.Weights.at(name);
            }
            out << YAML::EndMap; // Weights

            out << YAML::EndMap; // MorphTargetComponent
        }

        if (entity.HasComponent<LightProbeComponent>())
        {
            out << YAML::Key << "LightProbeComponent";
            out << YAML::BeginMap;

            auto const& lp = entity.GetComponent<LightProbeComponent>();
            out << YAML::Key << "InfluenceRadius" << YAML::Value << lp.m_InfluenceRadius;
            out << YAML::Key << "Intensity" << YAML::Value << lp.m_Intensity;
            out << YAML::Key << "Active" << YAML::Value << lp.m_Active;

            out << YAML::Key << "SHCoefficients";
            out << YAML::BeginSeq;
            for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
            {
                out << lp.m_SHCoefficients.Coefficients[i];
            }
            out << YAML::EndSeq;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<LightProbeVolumeComponent>())
        {
            out << YAML::Key << "LightProbeVolumeComponent";
            out << YAML::BeginMap;

            auto const& lpv = entity.GetComponent<LightProbeVolumeComponent>();
            out << YAML::Key << "BoundsMin" << YAML::Value << lpv.m_BoundsMin;
            out << YAML::Key << "BoundsMax" << YAML::Value << lpv.m_BoundsMax;
            out << YAML::Key << "Resolution" << YAML::Value << lpv.m_Resolution;
            out << YAML::Key << "Spacing" << YAML::Value << lpv.m_Spacing;
            out << YAML::Key << "Intensity" << YAML::Value << lpv.m_Intensity;
            out << YAML::Key << "Active" << YAML::Value << lpv.m_Active;
            out << YAML::Key << "BakedDataAsset" << YAML::Value << lpv.m_BakedDataAsset;

            out << YAML::EndMap;
        }

        if (entity.HasComponent<ReflectionProbeComponent>())
        {
            out << YAML::Key << "ReflectionProbeComponent";
            out << YAML::BeginMap;

            auto const& rp = entity.GetComponent<ReflectionProbeComponent>();
            out << YAML::Key << "InfluenceRadius" << YAML::Value << rp.m_InfluenceRadius;
            out << YAML::Key << "BlendDistance" << YAML::Value << rp.m_BlendDistance;
            out << YAML::Key << "Resolution" << YAML::Value << rp.m_Resolution;
            out << YAML::Key << "Intensity" << YAML::Value << rp.m_Intensity;
            out << YAML::Key << "Active" << YAML::Value << rp.m_Active;

            // m_BakedEnvironment + m_NeedsBake are runtime-only; rebake on load.

            out << YAML::EndMap;
        }

        if (entity.HasComponent<StreamingVolumeComponent>())
        {
            out << YAML::Key << "StreamingVolumeComponent";
            out << YAML::BeginMap;

            auto const& sv = entity.GetComponent<StreamingVolumeComponent>();
            out << YAML::Key << "RegionAssetHandle" << YAML::Value << sv.RegionAssetHandle;
            out << YAML::Key << "ActivationMode" << YAML::Value << static_cast<i32>(std::to_underlying(sv.ActivationMode));
            out << YAML::Key << "LoadRadius" << YAML::Value << sv.LoadRadius;
            out << YAML::Key << "UnloadRadius" << YAML::Value << sv.UnloadRadius;

            out << YAML::EndMap; // StreamingVolumeComponent
        }

        if (entity.HasComponent<NetworkIdentityComponent>())
        {
            out << YAML::Key << "NetworkIdentityComponent";
            out << YAML::BeginMap;

            auto const& nic = entity.GetComponent<NetworkIdentityComponent>();
            out << YAML::Key << "OwnerClientID" << YAML::Value << nic.OwnerClientID;
            out << YAML::Key << "Authority" << YAML::Value << static_cast<i32>(std::to_underlying(nic.Authority));
            out << YAML::Key << "IsReplicated" << YAML::Value << nic.IsReplicated;

            out << YAML::EndMap; // NetworkIdentityComponent
        }

        if (entity.HasComponent<NetworkInterestComponent>())
        {
            out << YAML::Key << "NetworkInterestComponent";
            out << YAML::BeginMap;

            auto const& nic = entity.GetComponent<NetworkInterestComponent>();
            out << YAML::Key << "RelevanceRadius" << YAML::Value << nic.RelevanceRadius;
            out << YAML::Key << "InterestGroup" << YAML::Value << nic.InterestGroup;

            out << YAML::EndMap; // NetworkInterestComponent
        }

        if (entity.HasComponent<PhaseComponent>())
        {
            out << YAML::Key << "PhaseComponent";
            out << YAML::BeginMap;

            auto const& pc = entity.GetComponent<PhaseComponent>();
            out << YAML::Key << "PhaseID" << YAML::Value << pc.PhaseID;

            out << YAML::EndMap; // PhaseComponent
        }

        if (entity.HasComponent<InstancePortalComponent>())
        {
            out << YAML::Key << "InstancePortalComponent";
            out << YAML::BeginMap;

            auto const& ipc = entity.GetComponent<InstancePortalComponent>();
            out << YAML::Key << "TargetZoneID" << YAML::Value << ipc.TargetZoneID;
            out << YAML::Key << "InstanceType" << YAML::Value << static_cast<u32>(ipc.InstanceType);
            out << YAML::Key << "MaxPlayers" << YAML::Value << ipc.MaxPlayers;

            out << YAML::EndMap; // InstancePortalComponent
        }

        if (entity.HasComponent<NetworkLODComponent>())
        {
            out << YAML::Key << "NetworkLODComponent";
            out << YAML::BeginMap;

            auto const& nlc = entity.GetComponent<NetworkLODComponent>();
            out << YAML::Key << "Level" << YAML::Value << static_cast<i32>(std::to_underlying(nlc.Level));

            out << YAML::EndMap; // NetworkLODComponent
        }

        if (entity.HasComponent<DialogueComponent>())
        {
            out << YAML::Key << "DialogueComponent";
            out << YAML::BeginMap;

            auto const& dc = entity.GetComponent<DialogueComponent>();
            out << YAML::Key << "DialogueTree" << YAML::Value << dc.m_DialogueTree;
            out << YAML::Key << "AutoTrigger" << YAML::Value << dc.m_AutoTrigger;
            out << YAML::Key << "TriggerRadius" << YAML::Value << dc.m_TriggerRadius;
            out << YAML::Key << "TriggerOnce" << YAML::Value << dc.m_TriggerOnce;

            out << YAML::EndMap; // DialogueComponent
        }

        if (entity.HasComponent<NavMeshBoundsComponent>())
        {
            out << YAML::Key << "NavMeshBoundsComponent";
            out << YAML::BeginMap;

            auto const& nmb = entity.GetComponent<NavMeshBoundsComponent>();
            out << YAML::Key << "Min" << YAML::Value << nmb.m_Min;
            out << YAML::Key << "Max" << YAML::Value << nmb.m_Max;

            out << YAML::EndMap; // NavMeshBoundsComponent
        }

        if (entity.HasComponent<NavAgentComponent>())
        {
            out << YAML::Key << "NavAgentComponent";
            out << YAML::BeginMap;

            auto const& nac = entity.GetComponent<NavAgentComponent>();
            out << YAML::Key << "Radius" << YAML::Value << nac.m_Radius;
            out << YAML::Key << "Height" << YAML::Value << nac.m_Height;
            out << YAML::Key << "MaxSpeed" << YAML::Value << nac.m_MaxSpeed;
            out << YAML::Key << "Acceleration" << YAML::Value << nac.m_Acceleration;
            out << YAML::Key << "StoppingDistance" << YAML::Value << nac.m_StoppingDistance;
            out << YAML::Key << "AvoidancePriority" << YAML::Value << nac.m_AvoidancePriority;
            out << YAML::Key << "LockYAxis" << YAML::Value << nac.m_LockYAxis;

            out << YAML::EndMap; // NavAgentComponent
        }

        if (entity.HasComponent<BehaviorTreeComponent>())
        {
            out << YAML::Key << "BehaviorTreeComponent";
            out << YAML::BeginMap;

            auto const& btc = entity.GetComponent<BehaviorTreeComponent>();
            out << YAML::Key << "BehaviorTreeAsset" << YAML::Value << btc.BehaviorTreeAssetHandle;

            out << YAML::EndMap; // BehaviorTreeComponent
        }

        if (entity.HasComponent<StateMachineComponent>())
        {
            out << YAML::Key << "StateMachineComponent";
            out << YAML::BeginMap;

            auto const& smc = entity.GetComponent<StateMachineComponent>();
            out << YAML::Key << "StateMachineAsset" << YAML::Value << smc.StateMachineAssetHandle;

            out << YAML::EndMap; // StateMachineComponent
        }

        if (entity.HasComponent<GoapAgentComponent>())
        {
            out << YAML::Key << "GoapAgentComponent";
            out << YAML::BeginMap;

            auto const& gac = entity.GetComponent<GoapAgentComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << gac.Enabled;

            out << YAML::EndMap; // GoapAgentComponent
        }

        if (entity.HasComponent<InventoryComponent>())
        {
            out << YAML::Key << "InventoryComponent";
            out << YAML::BeginMap;

            auto const& ic = entity.GetComponent<InventoryComponent>();
            out << YAML::Key << "Capacity" << YAML::Value << ic.PlayerInventory.GetCapacity();
            out << YAML::Key << "MaxWeight" << YAML::Value << ic.PlayerInventory.MaxWeight;
            out << YAML::Key << "Currency" << YAML::Value << ic.Currency;

            // Serialize inventory items
            out << YAML::Key << "Items" << YAML::Value << YAML::BeginSeq;
            for (i32 slot = 0; slot < ic.PlayerInventory.GetCapacity(); ++slot)
            {
                const auto* item = ic.PlayerInventory.GetItemAtSlot(slot);
                if (item)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Slot" << YAML::Value << slot;
                    out << YAML::Key << "InstanceID" << YAML::Value << item->InstanceID;
                    out << YAML::Key << "DefinitionID" << YAML::Value << item->ItemDefinitionID;
                    out << YAML::Key << "StackCount" << YAML::Value << item->StackCount;
                    out << YAML::Key << "Durability" << YAML::Value << item->Durability;
                    out << YAML::Key << "MaxDurability" << YAML::Value << item->MaxDurability;

                    if (!item->Affixes.empty())
                    {
                        out << YAML::Key << "Affixes" << YAML::Value << YAML::BeginSeq;
                        for (auto const& affix : item->Affixes)
                        {
                            out << YAML::BeginMap;
                            out << YAML::Key << "DefinitionID" << YAML::Value << affix.DefinitionID;
                            out << YAML::Key << "Type" << YAML::Value << AffixTypeToString(affix.Type);
                            out << YAML::Key << "Tier" << YAML::Value << affix.Tier;
                            out << YAML::Key << "Name" << YAML::Value << affix.Name;
                            out << YAML::Key << "Attribute" << YAML::Value << affix.Attribute;
                            out << YAML::Key << "Value" << YAML::Value << affix.Value;
                            out << YAML::EndMap;
                        }
                        out << YAML::EndSeq;
                    }
                    if (!item->CustomData.empty())
                    {
                        out << YAML::Key << "CustomData" << YAML::Value << YAML::BeginMap;
                        for (auto const& [key, value] : item->CustomData)
                        {
                            out << YAML::Key << key << YAML::Value << value;
                        }
                        out << YAML::EndMap;
                    }
                    out << YAML::EndMap;
                }
            }
            out << YAML::EndSeq;

            // Serialize equipment
            out << YAML::Key << "Equipment" << YAML::Value << YAML::BeginSeq;
            for (i32 i = 0; i < EquipmentSlots::SlotCount; ++i)
            {
                auto slot = static_cast<EquipmentSlots::Slot>(i);
                const auto* item = ic.Equipment.GetEquipped(slot);
                if (item)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Slot" << YAML::Value << EquipmentSlots::SlotToString(slot);
                    out << YAML::Key << "InstanceID" << YAML::Value << item->InstanceID;
                    out << YAML::Key << "DefinitionID" << YAML::Value << item->ItemDefinitionID;
                    out << YAML::Key << "StackCount" << YAML::Value << item->StackCount;
                    out << YAML::Key << "Durability" << YAML::Value << item->Durability;
                    out << YAML::Key << "MaxDurability" << YAML::Value << item->MaxDurability;

                    if (!item->Affixes.empty())
                    {
                        out << YAML::Key << "Affixes" << YAML::Value << YAML::BeginSeq;
                        for (auto const& affix : item->Affixes)
                        {
                            out << YAML::BeginMap;
                            out << YAML::Key << "DefinitionID" << YAML::Value << affix.DefinitionID;
                            out << YAML::Key << "Type" << YAML::Value << AffixTypeToString(affix.Type);
                            out << YAML::Key << "Tier" << YAML::Value << affix.Tier;
                            out << YAML::Key << "Name" << YAML::Value << affix.Name;
                            out << YAML::Key << "Attribute" << YAML::Value << affix.Attribute;
                            out << YAML::Key << "Value" << YAML::Value << affix.Value;
                            out << YAML::EndMap;
                        }
                        out << YAML::EndSeq;
                    }
                    if (!item->CustomData.empty())
                    {
                        out << YAML::Key << "CustomData" << YAML::Value << YAML::BeginMap;
                        for (auto const& [key, value] : item->CustomData)
                        {
                            out << YAML::Key << key << YAML::Value << value;
                        }
                        out << YAML::EndMap;
                    }
                    out << YAML::EndMap;
                }
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // InventoryComponent
        }

        if (entity.HasComponent<ItemPickupComponent>())
        {
            out << YAML::Key << "ItemPickupComponent";
            out << YAML::BeginMap;

            auto const& pc = entity.GetComponent<ItemPickupComponent>();
            out << YAML::Key << "InstanceID" << YAML::Value << pc.Item.InstanceID;
            out << YAML::Key << "DefinitionID" << YAML::Value << pc.Item.ItemDefinitionID;
            out << YAML::Key << "StackCount" << YAML::Value << pc.Item.StackCount;
            out << YAML::Key << "Durability" << YAML::Value << pc.Item.Durability;
            out << YAML::Key << "MaxDurability" << YAML::Value << pc.Item.MaxDurability;
            out << YAML::Key << "PickupRadius" << YAML::Value << pc.PickupRadius;
            out << YAML::Key << "AutoPickup" << YAML::Value << pc.AutoPickup;
            out << YAML::Key << "DespawnTimer" << YAML::Value << pc.DespawnTimer;

            if (!pc.Item.Affixes.empty())
            {
                out << YAML::Key << "Affixes" << YAML::Value << YAML::BeginSeq;
                for (auto const& affix : pc.Item.Affixes)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "DefinitionID" << YAML::Value << affix.DefinitionID;
                    out << YAML::Key << "Type" << YAML::Value << AffixTypeToString(affix.Type);
                    out << YAML::Key << "Tier" << YAML::Value << affix.Tier;
                    out << YAML::Key << "Name" << YAML::Value << affix.Name;
                    out << YAML::Key << "Attribute" << YAML::Value << affix.Attribute;
                    out << YAML::Key << "Value" << YAML::Value << affix.Value;
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }
            if (!pc.Item.CustomData.empty())
            {
                out << YAML::Key << "CustomData" << YAML::Value << YAML::BeginMap;
                for (auto const& [key, value] : pc.Item.CustomData)
                {
                    out << YAML::Key << key << YAML::Value << value;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap; // ItemPickupComponent
        }

        if (entity.HasComponent<ItemContainerComponent>())
        {
            out << YAML::Key << "ItemContainerComponent";
            out << YAML::BeginMap;

            auto const& cc = entity.GetComponent<ItemContainerComponent>();
            out << YAML::Key << "Capacity" << YAML::Value << cc.Contents.GetCapacity();
            out << YAML::Key << "IsShop" << YAML::Value << cc.IsShop;
            out << YAML::Key << "LootTableID" << YAML::Value << cc.LootTableID;
            out << YAML::Key << "HasBeenLooted" << YAML::Value << cc.HasBeenLooted;

            out << YAML::Key << "Items" << YAML::Value << YAML::BeginSeq;
            for (i32 slot = 0; slot < cc.Contents.GetCapacity(); ++slot)
            {
                const auto* item = cc.Contents.GetItemAtSlot(slot);
                if (item)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "Slot" << YAML::Value << slot;
                    out << YAML::Key << "InstanceID" << YAML::Value << item->InstanceID;
                    out << YAML::Key << "DefinitionID" << YAML::Value << item->ItemDefinitionID;
                    out << YAML::Key << "StackCount" << YAML::Value << item->StackCount;
                    out << YAML::Key << "Durability" << YAML::Value << item->Durability;
                    out << YAML::Key << "MaxDurability" << YAML::Value << item->MaxDurability;

                    if (!item->Affixes.empty())
                    {
                        out << YAML::Key << "Affixes" << YAML::Value << YAML::BeginSeq;
                        for (auto const& affix : item->Affixes)
                        {
                            out << YAML::BeginMap;
                            out << YAML::Key << "DefinitionID" << YAML::Value << affix.DefinitionID;
                            out << YAML::Key << "Type" << YAML::Value << AffixTypeToString(affix.Type);
                            out << YAML::Key << "Tier" << YAML::Value << affix.Tier;
                            out << YAML::Key << "Name" << YAML::Value << affix.Name;
                            out << YAML::Key << "Attribute" << YAML::Value << affix.Attribute;
                            out << YAML::Key << "Value" << YAML::Value << affix.Value;
                            out << YAML::EndMap;
                        }
                        out << YAML::EndSeq;
                    }
                    if (!item->CustomData.empty())
                    {
                        out << YAML::Key << "CustomData" << YAML::Value << YAML::BeginMap;
                        for (auto const& [key, value] : item->CustomData)
                        {
                            out << YAML::Key << key << YAML::Value << value;
                        }
                        out << YAML::EndMap;
                    }
                    out << YAML::EndMap;
                }
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // ItemContainerComponent
        }

        if (entity.HasComponent<QuestJournalComponent>())
        {
            out << YAML::Key << "QuestJournalComponent";
            out << YAML::BeginMap;

            auto const& qjc = entity.GetComponent<QuestJournalComponent>();

            // Serialize tags (sorted for deterministic output)
            {
                out << YAML::Key << "Tags" << YAML::Value << YAML::BeginSeq;
                std::vector<std::string> sortedTags(qjc.Journal.GetTags().begin(), qjc.Journal.GetTags().end());
                std::ranges::sort(sortedTags);
                for (auto const& tag : sortedTags)
                {
                    out << tag;
                }
                out << YAML::EndSeq;
            }

            // Serialize player state for requirement evaluation
            out << YAML::Key << "PlayerLevel" << YAML::Value << qjc.Journal.GetPlayerLevel();
            if (!qjc.Journal.GetPlayerClass().empty())
            {
                out << YAML::Key << "PlayerClass" << YAML::Value << qjc.Journal.GetPlayerClass();
            }
            if (!qjc.Journal.GetPlayerFaction().empty())
            {
                out << YAML::Key << "PlayerFaction" << YAML::Value << qjc.Journal.GetPlayerFaction();
            }
            if (!qjc.Journal.GetReputations().empty())
            {
                out << YAML::Key << "Reputations" << YAML::Value << YAML::BeginMap;
                std::vector<std::pair<std::string, i32>> sortedReps(qjc.Journal.GetReputations().begin(), qjc.Journal.GetReputations().end());
                std::ranges::sort(sortedReps);
                for (auto const& [factionId, value] : sortedReps)
                {
                    out << YAML::Key << factionId << YAML::Value << value;
                }
                out << YAML::EndMap;
            }
            if (!qjc.Journal.GetItems().empty())
            {
                out << YAML::Key << "Items" << YAML::Value << YAML::BeginMap;
                std::vector<std::pair<std::string, i32>> sortedItems(qjc.Journal.GetItems().begin(), qjc.Journal.GetItems().end());
                std::ranges::sort(sortedItems);
                for (auto const& [itemId, count] : sortedItems)
                {
                    out << YAML::Key << itemId << YAML::Value << count;
                }
                out << YAML::EndMap;
            }
            if (!qjc.Journal.GetStats().empty())
            {
                out << YAML::Key << "Stats" << YAML::Value << YAML::BeginMap;
                std::vector<std::pair<std::string, i32>> sortedStats(qjc.Journal.GetStats().begin(), qjc.Journal.GetStats().end());
                std::ranges::sort(sortedStats);
                for (auto const& [statName, value] : sortedStats)
                {
                    out << YAML::Key << statName << YAML::Value << value;
                }
                out << YAML::EndMap;
            }

            // Serialize completed quests (sorted for deterministic output)
            {
                out << YAML::Key << "CompletedQuests" << YAML::Value << YAML::BeginSeq;
                std::vector<std::string> sortedCompleted(qjc.Journal.GetCompletedQuestIDs().begin(), qjc.Journal.GetCompletedQuestIDs().end());
                std::ranges::sort(sortedCompleted);
                for (auto const& id : sortedCompleted)
                {
                    out << YAML::BeginMap;
                    out << YAML::Key << "QuestID" << YAML::Value << id;
                    out << YAML::Key << "BranchID" << YAML::Value << qjc.Journal.GetCompletedQuestBranch(id);
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            // Serialize failed quests (sorted for deterministic output)
            {
                out << YAML::Key << "FailedQuests" << YAML::Value << YAML::BeginSeq;
                std::vector<std::string> sortedFailed(qjc.Journal.GetFailedQuestIDs().begin(), qjc.Journal.GetFailedQuestIDs().end());
                std::ranges::sort(sortedFailed);
                for (auto const& id : sortedFailed)
                {
                    out << id;
                }
                out << YAML::EndSeq;
            }

            // Serialize active quests (sorted for deterministic output)
            {
                out << YAML::Key << "ActiveQuests" << YAML::Value << YAML::BeginSeq;
                std::vector<std::string> sortedActiveIds;
                for (auto const& [questId, state] : qjc.Journal.GetActiveQuestStates())
                {
                    sortedActiveIds.push_back(questId);
                }
                std::ranges::sort(sortedActiveIds);
                for (auto const& questId : sortedActiveIds)
                {
                    auto const& state = qjc.Journal.GetActiveQuestStates().at(questId);
                    out << YAML::BeginMap;
                    out << YAML::Key << "QuestID" << YAML::Value << state.QuestID;
                    out << YAML::Key << "Status" << YAML::Value << QuestStatusToString(state.Status);
                    out << YAML::Key << "CurrentStageIndex" << YAML::Value << state.CurrentStageIndex;
                    out << YAML::Key << "ElapsedTime" << YAML::Value << state.ElapsedTime;

                    out << YAML::Key << "Objectives" << YAML::Value << YAML::BeginSeq;
                    for (auto const& obj : state.ObjectiveStates)
                    {
                        out << YAML::BeginMap;
                        out << YAML::Key << "ObjectiveID" << YAML::Value << obj.ObjectiveID;
                        out << YAML::Key << "Description" << YAML::Value << obj.Description;
                        out << YAML::Key << "Type" << YAML::Value << ObjectiveTypeToString(obj.ObjectiveType);
                        out << YAML::Key << "TargetID" << YAML::Value << obj.TargetID;
                        out << YAML::Key << "RequiredCount" << YAML::Value << obj.RequiredCount;
                        out << YAML::Key << "CurrentCount" << YAML::Value << obj.CurrentCount;
                        out << YAML::Key << "IsOptional" << YAML::Value << obj.IsOptional;
                        out << YAML::Key << "IsHidden" << YAML::Value << obj.IsHidden;
                        out << YAML::Key << "IsCompleted" << YAML::Value << obj.IsCompleted;
                        out << YAML::EndMap;
                    }
                    out << YAML::EndSeq;

                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }

            // Serialize cooldowns for repeatable quests
            if (!qjc.Journal.GetQuestCooldowns().empty())
            {
                out << YAML::Key << "QuestCooldowns" << YAML::Value << YAML::BeginMap;
                std::vector<std::pair<std::string, f32>> sortedCooldowns(qjc.Journal.GetQuestCooldowns().begin(), qjc.Journal.GetQuestCooldowns().end());
                std::ranges::sort(sortedCooldowns);
                for (auto const& [questId, remaining] : sortedCooldowns)
                {
                    out << YAML::Key << questId << YAML::Value << remaining;
                }
                out << YAML::EndMap;
            }

            out << YAML::EndMap; // QuestJournalComponent
        }

        if (entity.HasComponent<QuestGiverComponent>())
        {
            out << YAML::Key << "QuestGiverComponent";
            out << YAML::BeginMap;

            auto const& qgc = entity.GetComponent<QuestGiverComponent>();
            out << YAML::Key << "QuestMarkerIcon" << YAML::Value << qgc.QuestMarkerIcon;

            out << YAML::Key << "OfferedQuestIDs" << YAML::Value << YAML::BeginSeq;
            for (auto const& id : qgc.OfferedQuestIDs)
            {
                out << id;
            }
            out << YAML::EndSeq;

            out << YAML::Key << "TurnInQuestIDs" << YAML::Value << YAML::BeginSeq;
            for (auto const& id : qgc.TurnInQuestIDs)
            {
                out << id;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // QuestGiverComponent
        }

        if (entity.HasComponent<AbilityComponent>())
        {
            out << YAML::Key << "AbilityComponent";
            out << YAML::BeginMap;

            auto const& ac = entity.GetComponent<AbilityComponent>();

            // Serialize attributes
            auto attrNames = ac.Attributes.GetAttributeNames();
            std::ranges::sort(attrNames);
            out << YAML::Key << "Attributes" << YAML::Value << YAML::BeginSeq;
            for (auto const& name : attrNames)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << name;
                out << YAML::Key << "BaseValue" << YAML::Value << ac.Attributes.GetBaseValue(name);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            // Serialize owned tags
            out << YAML::Key << "OwnedTags" << YAML::Value << YAML::BeginSeq;
            for (auto const& tag : ac.OwnedTags.GetTags())
            {
                out << tag.GetTagString();
            }
            out << YAML::EndSeq;

            // Serialize abilities
            out << YAML::Key << "Abilities" << YAML::Value << YAML::BeginSeq;
            for (auto const& ability : ac.Abilities)
            {
                auto const& def = ability.Definition;
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << def.Name;
                out << YAML::Key << "AbilityTag" << YAML::Value << def.AbilityTag.GetTagString();
                out << YAML::Key << "CooldownDuration" << YAML::Value << def.CooldownDuration;
                out << YAML::Key << "ResourceCost" << YAML::Value << def.ResourceCost;
                out << YAML::Key << "CostAttribute" << YAML::Value << def.CostAttribute;
                out << YAML::Key << "IsChanneled" << YAML::Value << def.IsChanneled;
                out << YAML::Key << "IsToggled" << YAML::Value << def.IsToggled;
                out << YAML::Key << "ChannelDuration" << YAML::Value << def.ChannelDuration;

                out << YAML::Key << "RequiredTags" << YAML::Value << YAML::BeginSeq;
                for (auto const& t : def.RequiredTags.GetTags())
                {
                    out << t.GetTagString();
                }
                out << YAML::EndSeq;

                out << YAML::Key << "BlockedTags" << YAML::Value << YAML::BeginSeq;
                for (auto const& t : def.BlockedTags.GetTags())
                {
                    out << t.GetTagString();
                }
                out << YAML::EndSeq;

                // Serialize activation effects
                SerializeEffectList(out, "ActivationEffects", def.ActivationEffects);

                // Serialize target activation effects (only written when non-empty)
                if (!def.TargetActivationEffects.empty())
                {
                    SerializeEffectList(out, "TargetActivationEffects", def.TargetActivationEffects);
                }

                out << YAML::EndMap; // Ability
            }
            out << YAML::EndSeq;

            out << YAML::EndMap; // AbilityComponent
        }

        if (entity.HasComponent<NameplateComponent>())
        {
            out << YAML::Key << "NameplateComponent";
            out << YAML::BeginMap;

            auto const& nc = entity.GetComponent<NameplateComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << nc.m_Enabled;
            out << YAML::Key << "ShowHealthBar" << YAML::Value << nc.m_ShowHealthBar;
            out << YAML::Key << "ShowManaBar" << YAML::Value << nc.m_ShowManaBar;
            out << YAML::Key << "WorldOffset" << YAML::Value << nc.m_WorldOffset;
            out << YAML::Key << "BarSize" << YAML::Value << nc.m_BarSize;
            out << YAML::Key << "HealthBarColor" << YAML::Value << nc.m_HealthBarColor;
            out << YAML::Key << "ManaBarColor" << YAML::Value << nc.m_ManaBarColor;
            out << YAML::Key << "BarBackgroundColor" << YAML::Value << nc.m_BarBackgroundColor;
            out << YAML::Key << "ManaBarGap" << YAML::Value << nc.m_ManaBarGap;

            out << YAML::EndMap; // NameplateComponent
        }

        if (entity.HasComponent<IKTargetComponent>())
        {
            out << YAML::Key << "IKTargetComponent";
            out << YAML::BeginMap;

            auto const& ik = entity.GetComponent<IKTargetComponent>();
            out << YAML::Key << "AimIKEnabled" << YAML::Value << ik.AimIKEnabled;
            out << YAML::Key << "AimBoneIndex" << YAML::Value << ik.AimBoneIndex;
            out << YAML::Key << "AimTarget" << YAML::Value << ik.AimTarget;
            out << YAML::Key << "AimAxis" << YAML::Value << ik.AimAxis;
            out << YAML::Key << "AimOffset" << YAML::Value << ik.AimOffset;
            out << YAML::Key << "AimPoleVector" << YAML::Value << ik.AimPoleVector;
            out << YAML::Key << "AimChainLength" << YAML::Value << ik.AimChainLength;
            out << YAML::Key << "AimChainFactor" << YAML::Value << ik.AimChainFactor;
            out << YAML::Key << "AimWeight" << YAML::Value << ik.AimWeight;
            if (static_cast<u64>(ik.AimTargetEntity) != 0)
            {
                out << YAML::Key << "AimTargetEntity" << YAML::Value << static_cast<u64>(ik.AimTargetEntity);
            }
            out << YAML::Key << "LimbIKEnabled" << YAML::Value << ik.LimbIKEnabled;
            out << YAML::Key << "LimbBoneIndex" << YAML::Value << ik.LimbBoneIndex;
            out << YAML::Key << "LimbTarget" << YAML::Value << ik.LimbTarget;
            out << YAML::Key << "LimbChainLength" << YAML::Value << ik.LimbChainLength;
            out << YAML::Key << "LimbWeight" << YAML::Value << ik.LimbWeight;
            if (static_cast<u64>(ik.LimbTargetEntity) != 0)
            {
                out << YAML::Key << "LimbTargetEntity" << YAML::Value << static_cast<u64>(ik.LimbTargetEntity);
            }

            out << YAML::EndMap; // IKTargetComponent
        }

        if (entity.HasComponent<SpringBoneComponent>())
        {
            out << YAML::Key << "SpringBoneComponent";
            out << YAML::BeginMap;

            auto const& spring = entity.GetComponent<SpringBoneComponent>();
            out << YAML::Key << "Enabled" << YAML::Value << spring.Enabled;
            out << YAML::Key << "EndBoneIndex" << YAML::Value << spring.EndBoneIndex;
            out << YAML::Key << "ChainLength" << YAML::Value << spring.ChainLength;
            out << YAML::Key << "Stiffness" << YAML::Value << spring.Stiffness;
            out << YAML::Key << "Damping" << YAML::Value << spring.Damping;
            out << YAML::Key << "Gravity" << YAML::Value << spring.Gravity;
            out << YAML::Key << "Weight" << YAML::Value << spring.Weight;

            out << YAML::EndMap; // SpringBoneComponent
        }

        out << YAML::EndMap; // Entity
    }

    void SceneSerializer::Serialize(const std::filesystem::path& filepath) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Scene" << YAML::Value << m_Scene->GetName();

        out << YAML::Key << "PostProcessSettings";
        out << YAML::BeginMap;
        {
            auto const& pp = m_Scene->GetPostProcessSettings();
            out << YAML::Key << "TonemapOperator" << YAML::Value << std::to_underlying(pp.Tonemap);
            out << YAML::Key << "Exposure" << YAML::Value << pp.Exposure;
            out << YAML::Key << "Gamma" << YAML::Value << pp.Gamma;
            out << YAML::Key << "BloomEnabled" << YAML::Value << pp.BloomEnabled;
            out << YAML::Key << "BloomThreshold" << YAML::Value << pp.BloomThreshold;
            out << YAML::Key << "BloomIntensity" << YAML::Value << pp.BloomIntensity;
            out << YAML::Key << "BloomIterations" << YAML::Value << pp.BloomIterations;
            out << YAML::Key << "VignetteEnabled" << YAML::Value << pp.VignetteEnabled;
            out << YAML::Key << "VignetteIntensity" << YAML::Value << pp.VignetteIntensity;
            out << YAML::Key << "VignetteSmoothness" << YAML::Value << pp.VignetteSmoothness;
            out << YAML::Key << "ChromaticAberrationEnabled" << YAML::Value << pp.ChromaticAberrationEnabled;
            out << YAML::Key << "ChromaticAberrationIntensity" << YAML::Value << pp.ChromaticAberrationIntensity;
            out << YAML::Key << "FXAAEnabled" << YAML::Value << pp.FXAAEnabled;
            out << YAML::Key << "DOFEnabled" << YAML::Value << pp.DOFEnabled;
            out << YAML::Key << "DOFFocusDistance" << YAML::Value << pp.DOFFocusDistance;
            out << YAML::Key << "DOFFocusRange" << YAML::Value << pp.DOFFocusRange;
            out << YAML::Key << "DOFBokehRadius" << YAML::Value << pp.DOFBokehRadius;
            out << YAML::Key << "MotionBlurEnabled" << YAML::Value << pp.MotionBlurEnabled;
            out << YAML::Key << "MotionBlurStrength" << YAML::Value << pp.MotionBlurStrength;
            out << YAML::Key << "MotionBlurSamples" << YAML::Value << pp.MotionBlurSamples;
            out << YAML::Key << "ColorGradingEnabled" << YAML::Value << pp.ColorGradingEnabled;
            out << YAML::Key << "SSAOEnabled" << YAML::Value << pp.SSAOEnabled;
            out << YAML::Key << "SSAORadius" << YAML::Value << pp.SSAORadius;
            out << YAML::Key << "SSAOBias" << YAML::Value << pp.SSAOBias;
            out << YAML::Key << "SSAOIntensity" << YAML::Value << pp.SSAOIntensity;
            out << YAML::Key << "SSAOSamples" << YAML::Value << pp.SSAOSamples;
            out << YAML::Key << "SSAODebugView" << YAML::Value << pp.SSAODebugView;
            out << YAML::Key << "SSREnabled" << YAML::Value << pp.SSREnabled;
            out << YAML::Key << "SSRMaxDistance" << YAML::Value << pp.SSRMaxDistance;
            out << YAML::Key << "SSRThickness" << YAML::Value << pp.SSRThickness;
            out << YAML::Key << "SSRStride" << YAML::Value << pp.SSRStride;
            out << YAML::Key << "SSRMaxSteps" << YAML::Value << pp.SSRMaxSteps;
            out << YAML::Key << "SSRBinarySearchSteps" << YAML::Value << pp.SSRBinarySearchSteps;
            out << YAML::Key << "SSRIntensity" << YAML::Value << pp.SSRIntensity;
            out << YAML::Key << "SSRMaxRoughness" << YAML::Value << pp.SSRMaxRoughness;
            out << YAML::Key << "SSREdgeFade" << YAML::Value << pp.SSREdgeFade;
            out << YAML::Key << "SSRDebugView" << YAML::Value << pp.SSRDebugView;
            out << YAML::Key << "SSGIEnabled" << YAML::Value << pp.SSGIEnabled;
            out << YAML::Key << "SSGIIntensity" << YAML::Value << pp.SSGIIntensity;
            out << YAML::Key << "SSGIMaxDistance" << YAML::Value << pp.SSGIMaxDistance;
            out << YAML::Key << "SSGIThickness" << YAML::Value << pp.SSGIThickness;
            out << YAML::Key << "SSGIStride" << YAML::Value << pp.SSGIStride;
            out << YAML::Key << "SSGIMaxSteps" << YAML::Value << pp.SSGIMaxSteps;
            out << YAML::Key << "SSGIRayCount" << YAML::Value << pp.SSGIRayCount;
            out << YAML::Key << "SSGIEdgeFade" << YAML::Value << pp.SSGIEdgeFade;
            out << YAML::Key << "SSGIDebugView" << YAML::Value << pp.SSGIDebugView;
            out << YAML::Key << "AutoExposureEnabled" << YAML::Value << pp.AutoExposureEnabled;
            out << YAML::Key << "AutoExposureMinLogLuminance" << YAML::Value << pp.AutoExposureMinLogLuminance;
            out << YAML::Key << "AutoExposureMaxLogLuminance" << YAML::Value << pp.AutoExposureMaxLogLuminance;
            out << YAML::Key << "AutoExposureSpeedUp" << YAML::Value << pp.AutoExposureSpeedUp;
            out << YAML::Key << "AutoExposureSpeedDown" << YAML::Value << pp.AutoExposureSpeedDown;
            out << YAML::Key << "AutoExposureCompensation" << YAML::Value << pp.AutoExposureCompensation;
            out << YAML::Key << "AutoExposureMinExposure" << YAML::Value << pp.AutoExposureMinExposure;
            out << YAML::Key << "AutoExposureMaxExposure" << YAML::Value << pp.AutoExposureMaxExposure;
        }
        out << YAML::EndMap;

        SerializeSnowSettings(out, m_Scene->GetSnowSettings());
        SerializeFogSettings(out, m_Scene->GetFogSettings());
        SerializeWindSettings(out, m_Scene->GetWindSettings());
        SerializeSnowAccumulationSettings(out, m_Scene->GetSnowAccumulationSettings());
        SerializeSnowEjectaSettings(out, m_Scene->GetSnowEjectaSettings());
        SerializePrecipitationSettings(out, m_Scene->GetPrecipitationSettings());

        // Streaming settings
        {
            auto const& ss = m_Scene->GetStreamingSettings();
            out << YAML::Key << "StreamingSettings";
            out << YAML::BeginMap;
            out << YAML::Key << "Enabled" << YAML::Value << ss.Enabled;
            out << YAML::Key << "DefaultLoadRadius" << YAML::Value << ss.DefaultLoadRadius;
            out << YAML::Key << "DefaultUnloadRadius" << YAML::Value << ss.DefaultUnloadRadius;
            out << YAML::Key << "MaxLoadedRegions" << YAML::Value << ss.MaxLoadedRegions;
            out << YAML::Key << "RegionDirectory" << YAML::Value << ss.RegionDirectory;
            out << YAML::EndMap;
        }

        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        ForEachEntitySorted([&out](Entity entity)
                            { SerializeEntity(out, entity); });
        out << YAML::EndSeq;
        out << YAML::EndMap;

        std::ofstream fout(filepath);
        fout << out.c_str();
    }

    [[maybe_unused]] void SceneSerializer::SerializeRuntime([[maybe_unused]] const std::filesystem::path& filepath) const
    {
        // Not implemented
        OLO_CORE_ASSERT(false);
    }

    void SceneSerializer::ForEachEntitySorted(const std::function<void(Entity)>& fn) const
    {
        std::vector<entt::entity> sortedEntities;
        m_Scene->m_Registry.view<entt::entity>().each([&sortedEntities](auto entityID)
                                                      { sortedEntities.push_back(entityID); });
        std::ranges::sort(sortedEntities, [this](entt::entity a, entt::entity b)
                          {
                              const u64 uuidA = m_Scene->m_Registry.get<IDComponent>(a).ID;
                              const u64 uuidB = m_Scene->m_Registry.get<IDComponent>(b).ID;
                              return uuidA < uuidB; });
        for (auto entityID : sortedEntities)
        {
            // SAFETY: m_Scene is const Ref<Scene>, but Entity requires non-const Scene*
            // This is safe because serialization only reads entity data
            Entity const entity = { entityID, const_cast<Scene*>(m_Scene.get()) };
            if (!entity)
            {
                continue;
            }

            fn(entity);
        }
    }

    Entity SceneSerializer::DeserializeEntity(u64 uuid, const std::string& name, const YAML::Node& entityNode)
    {
        Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);
        try
        {
            DeserializeEntityComponents(deserializedEntity, entityNode);
        }
        catch (...)
        {
            // Remove the half-initialized entity so the scene stays consistent
            m_Scene->DestroyEntity(deserializedEntity);
            throw;
        }
        return deserializedEntity;
    }

    bool SceneSerializer::Deserialize(const std::filesystem::path& filepath)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(filepath.string());
        }
        catch (const YAML::Exception& e)
        {
            // Catch the base class — yaml-cpp may throw RepresentationException
            // or other non-Parser subclasses on malformed top-level docs.
            OLO_CORE_ERROR("Failed to load .olo file '{0}'\n     {1}", filepath, e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to load .olo file '{0}'\n     {1}", filepath, e.what());
            return false;
        }

        if (!data || !data.IsMap())
        {
            return false;
        }

        auto sceneNameNode = data["Scene"];
        if (!sceneNameNode || !sceneNameNode.IsScalar())
        {
            return false;
        }

        std::string sceneName;
        try
        {
            sceneName = sceneNameNode.as<std::string>();
        }
        catch (const YAML::Exception&)
        {
            return false;
        }

        OLO_CORE_TRACE("Deserializing scene '{0}'", sceneName);
        m_Scene->SetName(sceneName);

        try
        {
            if (auto ppNode = data["PostProcessSettings"]; ppNode && ppNode.IsMap())
            {
                auto& pp = m_Scene->GetPostProcessSettings();
                TrySetEnum(pp.Tonemap, ppNode["TonemapOperator"]);
                TrySet(pp.Exposure, ppNode["Exposure"]);
                TrySet(pp.Gamma, ppNode["Gamma"]);
                TrySet(pp.BloomEnabled, ppNode["BloomEnabled"]);
                TrySet(pp.BloomThreshold, ppNode["BloomThreshold"]);
                TrySet(pp.BloomIntensity, ppNode["BloomIntensity"]);
                TrySet(pp.BloomIterations, ppNode["BloomIterations"]);
                TrySet(pp.VignetteEnabled, ppNode["VignetteEnabled"]);
                TrySet(pp.VignetteIntensity, ppNode["VignetteIntensity"]);
                TrySet(pp.VignetteSmoothness, ppNode["VignetteSmoothness"]);
                TrySet(pp.ChromaticAberrationEnabled, ppNode["ChromaticAberrationEnabled"]);
                TrySet(pp.ChromaticAberrationIntensity, ppNode["ChromaticAberrationIntensity"]);
                TrySet(pp.FXAAEnabled, ppNode["FXAAEnabled"]);
                TrySet(pp.DOFEnabled, ppNode["DOFEnabled"]);
                TrySet(pp.DOFFocusDistance, ppNode["DOFFocusDistance"]);
                TrySet(pp.DOFFocusRange, ppNode["DOFFocusRange"]);
                TrySet(pp.DOFBokehRadius, ppNode["DOFBokehRadius"]);
                TrySet(pp.MotionBlurEnabled, ppNode["MotionBlurEnabled"]);
                TrySet(pp.MotionBlurStrength, ppNode["MotionBlurStrength"]);
                TrySet(pp.MotionBlurSamples, ppNode["MotionBlurSamples"]);
                TrySet(pp.ColorGradingEnabled, ppNode["ColorGradingEnabled"]);
                TrySet(pp.SSAOEnabled, ppNode["SSAOEnabled"]);
                TrySet(pp.SSAORadius, ppNode["SSAORadius"]);
                TrySet(pp.SSAOBias, ppNode["SSAOBias"]);
                TrySet(pp.SSAOIntensity, ppNode["SSAOIntensity"]);
                TrySet(pp.SSAOSamples, ppNode["SSAOSamples"]);
                TrySet(pp.SSAODebugView, ppNode["SSAODebugView"]);
                TrySet(pp.SSREnabled, ppNode["SSREnabled"]);
                TrySet(pp.SSRMaxDistance, ppNode["SSRMaxDistance"]);
                TrySet(pp.SSRThickness, ppNode["SSRThickness"]);
                TrySet(pp.SSRStride, ppNode["SSRStride"]);
                TrySet(pp.SSRMaxSteps, ppNode["SSRMaxSteps"]);
                TrySet(pp.SSRBinarySearchSteps, ppNode["SSRBinarySearchSteps"]);
                TrySet(pp.SSRIntensity, ppNode["SSRIntensity"]);
                TrySet(pp.SSRMaxRoughness, ppNode["SSRMaxRoughness"]);
                TrySet(pp.SSREdgeFade, ppNode["SSREdgeFade"]);
                TrySet(pp.SSRDebugView, ppNode["SSRDebugView"]);
                TrySet(pp.SSGIEnabled, ppNode["SSGIEnabled"]);
                TrySet(pp.SSGIIntensity, ppNode["SSGIIntensity"]);
                TrySet(pp.SSGIMaxDistance, ppNode["SSGIMaxDistance"]);
                TrySet(pp.SSGIThickness, ppNode["SSGIThickness"]);
                TrySet(pp.SSGIStride, ppNode["SSGIStride"]);
                TrySet(pp.SSGIMaxSteps, ppNode["SSGIMaxSteps"]);
                TrySet(pp.SSGIRayCount, ppNode["SSGIRayCount"]);
                TrySet(pp.SSGIEdgeFade, ppNode["SSGIEdgeFade"]);
                TrySet(pp.SSGIDebugView, ppNode["SSGIDebugView"]);
                TrySet(pp.AutoExposureEnabled, ppNode["AutoExposureEnabled"]);
                TrySet(pp.AutoExposureMinLogLuminance, ppNode["AutoExposureMinLogLuminance"]);
                TrySet(pp.AutoExposureMaxLogLuminance, ppNode["AutoExposureMaxLogLuminance"]);
                TrySet(pp.AutoExposureSpeedUp, ppNode["AutoExposureSpeedUp"]);
                TrySet(pp.AutoExposureSpeedDown, ppNode["AutoExposureSpeedDown"]);
                TrySet(pp.AutoExposureCompensation, ppNode["AutoExposureCompensation"]);
                TrySet(pp.AutoExposureMinExposure, ppNode["AutoExposureMinExposure"]);
                TrySet(pp.AutoExposureMaxExposure, ppNode["AutoExposureMaxExposure"]);

                // Floats read from YAML must be finite and ordered (min<=max).
                SanitizeAutoExposure(pp);
                SanitizeSSR(pp);
                SanitizeSSGI(pp);
            }

            DeserializeSnowSettings(data, m_Scene->GetSnowSettings());
            DeserializeFogSettings(data, m_Scene->GetFogSettings());
            DeserializeWindSettings(data, m_Scene->GetWindSettings());
            DeserializeSnowAccumulationSettings(data, m_Scene->GetSnowAccumulationSettings());
            DeserializeSnowEjectaSettings(data, m_Scene->GetSnowEjectaSettings());
            DeserializePrecipitationSettings(data, m_Scene->GetPrecipitationSettings());

            if (auto ssNode = data["StreamingSettings"]; ssNode && ssNode.IsMap())
            {
                auto& ss = m_Scene->GetStreamingSettings();
                TrySet(ss.Enabled, ssNode["Enabled"]);
                TrySet(ss.DefaultLoadRadius, ssNode["DefaultLoadRadius"]);
                TrySet(ss.DefaultUnloadRadius, ssNode["DefaultUnloadRadius"]);
                TrySet(ss.MaxLoadedRegions, ssNode["MaxLoadedRegions"]);
                TrySet(ss.RegionDirectory, ssNode["RegionDirectory"]);

                SanitizeStreamingSettings(ss);
            }

            if (const auto entities = data["Entities"]; entities && entities.IsSequence())
            {
                u32 entityCount = 0;
                u32 failedCount = 0;

                for (auto entity : entities)
                {
                    try
                    {
                        if (!entity.IsMap())
                        {
                            OLO_CORE_WARN("SceneSerializer: Skipping non-map entity entry");
                            ++failedCount;
                            continue;
                        }

                        auto entityIdNode = entity["Entity"];
                        if (!entityIdNode || !entityIdNode.IsScalar())
                        {
                            OLO_CORE_WARN("SceneSerializer: Skipping entity with missing or non-scalar 'Entity' id");
                            ++failedCount;
                            continue;
                        }
                        auto uuid = entityIdNode.as<u64>();

                        std::string name;
                        if (auto tagComponent = entity["TagComponent"]; tagComponent && tagComponent.IsMap())
                        {
                            if (auto tagNode = tagComponent["Tag"]; tagNode && tagNode.IsScalar())
                            {
                                name = tagNode.as<std::string>();
                            }
                        }

                        OLO_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

                        DeserializeEntity(uuid, name, entity);
                        ++entityCount;
                    }
                    catch (const std::exception& e)
                    {
                        OLO_CORE_ERROR("SceneSerializer: Failed to deserialize entity — {}", e.what());
                        ++failedCount;
                    }
                }

                OLO_CORE_INFO("SceneSerializer: Deserialized {} entities ({} failed)", entityCount, failedCount);

                if (failedCount > 0)
                {
                    OLO_CORE_ERROR("SceneSerializer: {} entities failed to deserialize — aborting", failedCount);
                    return false;
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("SceneSerializer::Deserialize: YAML exception during deserialise: {}", e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SceneSerializer::Deserialize: Exception during deserialise: {}", e.what());
            return false;
        }

        m_Scene->SetName(std::filesystem::path(filepath).filename().string());

        return true;
    }

    [[nodiscard("Store this!")]] [[maybe_unused]] bool SceneSerializer::DeserializeRuntime([[maybe_unused]] const std::filesystem::path& filepath)
    {
        // Not implemented
        OLO_CORE_ASSERT(false);
        return false;
    }

    std::string SceneSerializer::SerializeToYAML() const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Scene" << YAML::Value << m_Scene->GetName();

        out << YAML::Key << "PostProcessSettings";
        out << YAML::BeginMap;
        {
            auto const& pp = m_Scene->GetPostProcessSettings();
            out << YAML::Key << "TonemapOperator" << YAML::Value << std::to_underlying(pp.Tonemap);
            out << YAML::Key << "Exposure" << YAML::Value << pp.Exposure;
            out << YAML::Key << "Gamma" << YAML::Value << pp.Gamma;
            out << YAML::Key << "BloomEnabled" << YAML::Value << pp.BloomEnabled;
            out << YAML::Key << "BloomThreshold" << YAML::Value << pp.BloomThreshold;
            out << YAML::Key << "BloomIntensity" << YAML::Value << pp.BloomIntensity;
            out << YAML::Key << "BloomIterations" << YAML::Value << pp.BloomIterations;
            out << YAML::Key << "VignetteEnabled" << YAML::Value << pp.VignetteEnabled;
            out << YAML::Key << "VignetteIntensity" << YAML::Value << pp.VignetteIntensity;
            out << YAML::Key << "VignetteSmoothness" << YAML::Value << pp.VignetteSmoothness;
            out << YAML::Key << "ChromaticAberrationEnabled" << YAML::Value << pp.ChromaticAberrationEnabled;
            out << YAML::Key << "ChromaticAberrationIntensity" << YAML::Value << pp.ChromaticAberrationIntensity;
            out << YAML::Key << "FXAAEnabled" << YAML::Value << pp.FXAAEnabled;
            out << YAML::Key << "DOFEnabled" << YAML::Value << pp.DOFEnabled;
            out << YAML::Key << "DOFFocusDistance" << YAML::Value << pp.DOFFocusDistance;
            out << YAML::Key << "DOFFocusRange" << YAML::Value << pp.DOFFocusRange;
            out << YAML::Key << "DOFBokehRadius" << YAML::Value << pp.DOFBokehRadius;
            out << YAML::Key << "MotionBlurEnabled" << YAML::Value << pp.MotionBlurEnabled;
            out << YAML::Key << "MotionBlurStrength" << YAML::Value << pp.MotionBlurStrength;
            out << YAML::Key << "MotionBlurSamples" << YAML::Value << pp.MotionBlurSamples;
            out << YAML::Key << "ColorGradingEnabled" << YAML::Value << pp.ColorGradingEnabled;
            out << YAML::Key << "SSAOEnabled" << YAML::Value << pp.SSAOEnabled;
            out << YAML::Key << "SSAORadius" << YAML::Value << pp.SSAORadius;
            out << YAML::Key << "SSAOBias" << YAML::Value << pp.SSAOBias;
            out << YAML::Key << "SSAOIntensity" << YAML::Value << pp.SSAOIntensity;
            out << YAML::Key << "SSAOSamples" << YAML::Value << pp.SSAOSamples;
            out << YAML::Key << "SSAODebugView" << YAML::Value << pp.SSAODebugView;
            out << YAML::Key << "SSREnabled" << YAML::Value << pp.SSREnabled;
            out << YAML::Key << "SSRMaxDistance" << YAML::Value << pp.SSRMaxDistance;
            out << YAML::Key << "SSRThickness" << YAML::Value << pp.SSRThickness;
            out << YAML::Key << "SSRStride" << YAML::Value << pp.SSRStride;
            out << YAML::Key << "SSRMaxSteps" << YAML::Value << pp.SSRMaxSteps;
            out << YAML::Key << "SSRBinarySearchSteps" << YAML::Value << pp.SSRBinarySearchSteps;
            out << YAML::Key << "SSRIntensity" << YAML::Value << pp.SSRIntensity;
            out << YAML::Key << "SSRMaxRoughness" << YAML::Value << pp.SSRMaxRoughness;
            out << YAML::Key << "SSREdgeFade" << YAML::Value << pp.SSREdgeFade;
            out << YAML::Key << "SSRDebugView" << YAML::Value << pp.SSRDebugView;
            out << YAML::Key << "SSGIEnabled" << YAML::Value << pp.SSGIEnabled;
            out << YAML::Key << "SSGIIntensity" << YAML::Value << pp.SSGIIntensity;
            out << YAML::Key << "SSGIMaxDistance" << YAML::Value << pp.SSGIMaxDistance;
            out << YAML::Key << "SSGIThickness" << YAML::Value << pp.SSGIThickness;
            out << YAML::Key << "SSGIStride" << YAML::Value << pp.SSGIStride;
            out << YAML::Key << "SSGIMaxSteps" << YAML::Value << pp.SSGIMaxSteps;
            out << YAML::Key << "SSGIRayCount" << YAML::Value << pp.SSGIRayCount;
            out << YAML::Key << "SSGIEdgeFade" << YAML::Value << pp.SSGIEdgeFade;
            out << YAML::Key << "SSGIDebugView" << YAML::Value << pp.SSGIDebugView;
            out << YAML::Key << "AutoExposureEnabled" << YAML::Value << pp.AutoExposureEnabled;
            out << YAML::Key << "AutoExposureMinLogLuminance" << YAML::Value << pp.AutoExposureMinLogLuminance;
            out << YAML::Key << "AutoExposureMaxLogLuminance" << YAML::Value << pp.AutoExposureMaxLogLuminance;
            out << YAML::Key << "AutoExposureSpeedUp" << YAML::Value << pp.AutoExposureSpeedUp;
            out << YAML::Key << "AutoExposureSpeedDown" << YAML::Value << pp.AutoExposureSpeedDown;
            out << YAML::Key << "AutoExposureCompensation" << YAML::Value << pp.AutoExposureCompensation;
            out << YAML::Key << "AutoExposureMinExposure" << YAML::Value << pp.AutoExposureMinExposure;
            out << YAML::Key << "AutoExposureMaxExposure" << YAML::Value << pp.AutoExposureMaxExposure;
        }
        out << YAML::EndMap;

        SerializeSnowSettings(out, m_Scene->GetSnowSettings());
        SerializeFogSettings(out, m_Scene->GetFogSettings());
        SerializeWindSettings(out, m_Scene->GetWindSettings());
        SerializeSnowAccumulationSettings(out, m_Scene->GetSnowAccumulationSettings());
        SerializeSnowEjectaSettings(out, m_Scene->GetSnowEjectaSettings());
        SerializePrecipitationSettings(out, m_Scene->GetPrecipitationSettings());

        // Streaming settings
        {
            auto const& ss = m_Scene->GetStreamingSettings();
            out << YAML::Key << "StreamingSettings";
            out << YAML::BeginMap;
            out << YAML::Key << "Enabled" << YAML::Value << ss.Enabled;
            out << YAML::Key << "DefaultLoadRadius" << YAML::Value << ss.DefaultLoadRadius;
            out << YAML::Key << "DefaultUnloadRadius" << YAML::Value << ss.DefaultUnloadRadius;
            out << YAML::Key << "MaxLoadedRegions" << YAML::Value << ss.MaxLoadedRegions;
            out << YAML::Key << "RegionDirectory" << YAML::Value << ss.RegionDirectory;
            out << YAML::EndMap;
        }

        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        ForEachEntitySorted([&out](Entity entity)
                            { SerializeEntity(out, entity); });
        out << YAML::EndSeq;
        out << YAML::EndMap;

        return std::string(out.c_str());
    }

    bool SceneSerializer::DeserializeFromYAML(const std::string& yamlString)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (const YAML::Exception& e)
        {
            // Fuzz-derived inputs hit non-ParserException paths too
            // (RepresentationException, BadConversion at top-level, …).
            // Catch the base class so a malformed document tears the
            // call down instead of propagating out of this entry point.
            OLO_CORE_ERROR("Failed to load scene...\n     {0}", e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to load scene...\n     {0}", e.what());
            return false;
        }

        if (!data || !data.IsMap())
        {
            return false;
        }

        auto sceneNameNode = data["Scene"];
        if (!sceneNameNode || !sceneNameNode.IsScalar())
        {
            return false;
        }

        std::string sceneName;
        try
        {
            sceneName = sceneNameNode.as<std::string>();
        }
        catch (const YAML::Exception&)
        {
            return false;
        }
        OLO_CORE_TRACE("Deserializing scene '{0}'", sceneName);

        // Top-level guard for the schema-walk: fuzz inputs that pass the
        // "is map + Scene scalar" check above can still hit `.as<T>()`
        // throws or null derefs deep in settings/entity helpers.
        try
        {
            if (auto ppNode = data["PostProcessSettings"]; ppNode && ppNode.IsMap())
            {
                auto& pp = m_Scene->GetPostProcessSettings();
                TrySetEnum(pp.Tonemap, ppNode["TonemapOperator"]);
                TrySet(pp.Exposure, ppNode["Exposure"]);
                TrySet(pp.Gamma, ppNode["Gamma"]);
                TrySet(pp.BloomEnabled, ppNode["BloomEnabled"]);
                TrySet(pp.BloomThreshold, ppNode["BloomThreshold"]);
                TrySet(pp.BloomIntensity, ppNode["BloomIntensity"]);
                TrySet(pp.BloomIterations, ppNode["BloomIterations"]);
                TrySet(pp.VignetteEnabled, ppNode["VignetteEnabled"]);
                TrySet(pp.VignetteIntensity, ppNode["VignetteIntensity"]);
                TrySet(pp.VignetteSmoothness, ppNode["VignetteSmoothness"]);
                TrySet(pp.ChromaticAberrationEnabled, ppNode["ChromaticAberrationEnabled"]);
                TrySet(pp.ChromaticAberrationIntensity, ppNode["ChromaticAberrationIntensity"]);
                TrySet(pp.FXAAEnabled, ppNode["FXAAEnabled"]);
                TrySet(pp.DOFEnabled, ppNode["DOFEnabled"]);
                TrySet(pp.DOFFocusDistance, ppNode["DOFFocusDistance"]);
                TrySet(pp.DOFFocusRange, ppNode["DOFFocusRange"]);
                TrySet(pp.DOFBokehRadius, ppNode["DOFBokehRadius"]);
                TrySet(pp.MotionBlurEnabled, ppNode["MotionBlurEnabled"]);
                TrySet(pp.MotionBlurStrength, ppNode["MotionBlurStrength"]);
                TrySet(pp.MotionBlurSamples, ppNode["MotionBlurSamples"]);
                TrySet(pp.ColorGradingEnabled, ppNode["ColorGradingEnabled"]);
                TrySet(pp.SSAOEnabled, ppNode["SSAOEnabled"]);
                TrySet(pp.SSAORadius, ppNode["SSAORadius"]);
                TrySet(pp.SSAOBias, ppNode["SSAOBias"]);
                TrySet(pp.SSAOIntensity, ppNode["SSAOIntensity"]);
                TrySet(pp.SSAOSamples, ppNode["SSAOSamples"]);
                TrySet(pp.SSAODebugView, ppNode["SSAODebugView"]);
                TrySet(pp.SSREnabled, ppNode["SSREnabled"]);
                TrySet(pp.SSRMaxDistance, ppNode["SSRMaxDistance"]);
                TrySet(pp.SSRThickness, ppNode["SSRThickness"]);
                TrySet(pp.SSRStride, ppNode["SSRStride"]);
                TrySet(pp.SSRMaxSteps, ppNode["SSRMaxSteps"]);
                TrySet(pp.SSRBinarySearchSteps, ppNode["SSRBinarySearchSteps"]);
                TrySet(pp.SSRIntensity, ppNode["SSRIntensity"]);
                TrySet(pp.SSRMaxRoughness, ppNode["SSRMaxRoughness"]);
                TrySet(pp.SSREdgeFade, ppNode["SSREdgeFade"]);
                TrySet(pp.SSRDebugView, ppNode["SSRDebugView"]);
                TrySet(pp.SSGIEnabled, ppNode["SSGIEnabled"]);
                TrySet(pp.SSGIIntensity, ppNode["SSGIIntensity"]);
                TrySet(pp.SSGIMaxDistance, ppNode["SSGIMaxDistance"]);
                TrySet(pp.SSGIThickness, ppNode["SSGIThickness"]);
                TrySet(pp.SSGIStride, ppNode["SSGIStride"]);
                TrySet(pp.SSGIMaxSteps, ppNode["SSGIMaxSteps"]);
                TrySet(pp.SSGIRayCount, ppNode["SSGIRayCount"]);
                TrySet(pp.SSGIEdgeFade, ppNode["SSGIEdgeFade"]);
                TrySet(pp.SSGIDebugView, ppNode["SSGIDebugView"]);
                TrySet(pp.AutoExposureEnabled, ppNode["AutoExposureEnabled"]);
                TrySet(pp.AutoExposureMinLogLuminance, ppNode["AutoExposureMinLogLuminance"]);
                TrySet(pp.AutoExposureMaxLogLuminance, ppNode["AutoExposureMaxLogLuminance"]);
                TrySet(pp.AutoExposureSpeedUp, ppNode["AutoExposureSpeedUp"]);
                TrySet(pp.AutoExposureSpeedDown, ppNode["AutoExposureSpeedDown"]);
                TrySet(pp.AutoExposureCompensation, ppNode["AutoExposureCompensation"]);
                TrySet(pp.AutoExposureMinExposure, ppNode["AutoExposureMinExposure"]);
                TrySet(pp.AutoExposureMaxExposure, ppNode["AutoExposureMaxExposure"]);

                // Floats read from YAML must be finite and ordered (min<=max).
                SanitizeAutoExposure(pp);
                SanitizeSSR(pp);
                SanitizeSSGI(pp);
            }

            DeserializeSnowSettings(data, m_Scene->GetSnowSettings());
            DeserializeFogSettings(data, m_Scene->GetFogSettings());
            DeserializeWindSettings(data, m_Scene->GetWindSettings());
            DeserializeSnowAccumulationSettings(data, m_Scene->GetSnowAccumulationSettings());
            DeserializeSnowEjectaSettings(data, m_Scene->GetSnowEjectaSettings());
            DeserializePrecipitationSettings(data, m_Scene->GetPrecipitationSettings());

            if (auto ssNode = data["StreamingSettings"]; ssNode && ssNode.IsMap())
            {
                auto& ss = m_Scene->GetStreamingSettings();
                TrySet(ss.Enabled, ssNode["Enabled"]);
                TrySet(ss.DefaultLoadRadius, ssNode["DefaultLoadRadius"]);
                TrySet(ss.DefaultUnloadRadius, ssNode["DefaultUnloadRadius"]);
                TrySet(ss.MaxLoadedRegions, ssNode["MaxLoadedRegions"]);
                TrySet(ss.RegionDirectory, ssNode["RegionDirectory"]);

                SanitizeStreamingSettings(ss);
            }

            auto entities = data["Entities"];
            if (entities && entities.IsSequence())
            {
                for (auto entity : entities)
                {
                    try
                    {
                        if (!entity.IsMap())
                        {
                            OLO_CORE_WARN("SceneSerializer::DeserializeFromYAML: Skipping non-map entity entry");
                            continue;
                        }

                        auto entityIdNode = entity["Entity"];
                        if (!entityIdNode || !entityIdNode.IsScalar())
                        {
                            OLO_CORE_WARN("SceneSerializer::DeserializeFromYAML: Skipping entity with missing or non-scalar 'Entity' id");
                            continue;
                        }
                        u64 uuid = entityIdNode.as<u64>();

                        std::string name;
                        if (auto tagComponent = entity["TagComponent"]; tagComponent && tagComponent.IsMap())
                        {
                            if (auto tagNode = tagComponent["Tag"]; tagNode && tagNode.IsScalar())
                            {
                                name = tagNode.as<std::string>();
                            }
                        }

                        OLO_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

                        DeserializeEntity(uuid, name, entity);
                    }
                    catch (const std::exception& e)
                    {
                        OLO_CORE_ERROR("SceneSerializer::DeserializeFromYAML: Failed to deserialize entity — {}", e.what());
                        return false;
                    }
                    catch (...)
                    {
                        OLO_CORE_ERROR("SceneSerializer::DeserializeFromYAML: Failed to deserialize entity (unknown exception)");
                        return false;
                    }
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("SceneSerializer::DeserializeFromYAML: YAML exception during deserialise: {}", e.what());
            return false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SceneSerializer::DeserializeFromYAML: Exception during deserialise: {}", e.what());
            return false;
        }

        return true;
    }

    std::vector<UUID> SceneSerializer::DeserializeAdditive(const YAML::Node& entitiesNode)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<UUID> createdUUIDs;

        if (!entitiesNode || !entitiesNode.IsSequence())
        {
            return createdUUIDs;
        }

        createdUUIDs.reserve(entitiesNode.size());

        for (auto entity : entitiesNode)
        {
            try
            {
                if (!entity["Entity"])
                {
                    OLO_CORE_WARN("DeserializeAdditive: skipping entity node without 'Entity' key");
                    continue;
                }
                auto uuid = entity["Entity"].as<u64>();

                // Skip if entity already exists in the scene
                if (m_Scene->m_EntityMap.Contains(uuid))
                {
                    continue;
                }

                std::string name;
                if (auto tagComponent = entity["TagComponent"]; tagComponent)
                {
                    name = tagComponent["Tag"].as<std::string>();
                }

                OLO_CORE_TRACE("Additive deserialized entity with ID = {0}, name = {1}", uuid, name);

                DeserializeEntity(uuid, name, entity);
                createdUUIDs.emplace_back(uuid);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("DeserializeAdditive: Failed to deserialize entity — {}", e.what());
            }
            catch (...)
            {
                OLO_CORE_ERROR("DeserializeAdditive: Failed to deserialize entity (unknown exception)");
            }
        }

        return createdUUIDs;
    }
} // namespace OloEngine
