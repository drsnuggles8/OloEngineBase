#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Core/YAMLConverters.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Particle/EmissionShapeUtils.h"
#include "OloEngine/Particle/ParticleCurveSerializer.h"

#include <fstream>

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

            // Clamp to safe bounds
            wind.GridWorldSize = std::clamp(wind.GridWorldSize, 0.1f, 10000.0f);
            wind.GridResolution = std::clamp(wind.GridResolution, 1u, 2048u);

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

    SceneSerializer::SceneSerializer(const Ref<Scene>& scene)
        : m_Scene(scene)
    {
    }

    static void SerializeEntity(YAML::Emitter& out, Entity entity)
    {
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
            out << YAML::Key << "Rotation" << YAML::Value << tc.Rotation;
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
            out << YAML::Key << "ProjectionType" << YAML::Value << static_cast<int>(camera.GetProjectionType());
            out << YAML::Key << "PerspectiveFOV" << YAML::Value << camera.GetPerspectiveVerticalFOV();
            out << YAML::Key << "PerspectiveNear" << YAML::Value << camera.GetPerspectiveNearClip();
            out << YAML::Key << "PerspectiveFar" << YAML::Value << camera.GetPerspectiveFarClip();
            out << YAML::Key << "OrthographicSize" << YAML::Value << camera.GetOrthographicSize();
            out << YAML::Key << "OrthographicNear" << YAML::Value << camera.GetOrthographicNearClip();
            out << YAML::Key << "OrthographicFar" << YAML::Value << camera.GetOrthographicFarClip();
            out << YAML::EndMap; // Camera

            out << YAML::Key << "Primary" << YAML::Value << cameraComponent.Primary;
            out << YAML::Key << "FixedAspectRatio" << YAML::Value << cameraComponent.FixedAspectRatio;

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

            out << YAML::EndMap; // TextComponent
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
                out << YAML::Key << "Primitive" << YAML::Value << static_cast<i32>(meshComponent.m_Primitive);
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

        if (entity.HasComponent<MaterialComponent>())
        {
            out << YAML::Key << "MaterialComponent";
            out << YAML::BeginMap; // MaterialComponent

            auto const& matComponent = entity.GetComponent<MaterialComponent>();
            auto baseColor = matComponent.m_Material.GetBaseColorFactor();
            out << YAML::Key << "AlbedoColor" << YAML::Value << glm::vec3(baseColor.r, baseColor.g, baseColor.b);
            out << YAML::Key << "Metallic" << YAML::Value << matComponent.m_Material.GetMetallicFactor();
            out << YAML::Key << "Roughness" << YAML::Value << matComponent.m_Material.GetRoughnessFactor();

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
            out << YAML::Key << "Tint" << YAML::Value << envMap.m_Tint;

            out << YAML::EndMap; // EnvironmentMapComponent
        }

        if (entity.HasComponent<Rigidbody3DComponent>())
        {
            out << YAML::Key << "Rigidbody3DComponent";
            out << YAML::BeginMap; // Rigidbody3DComponent

            auto const& rb3dComponent = entity.GetComponent<Rigidbody3DComponent>();
            out << YAML::Key << "BodyType" << YAML::Value << static_cast<int>(rb3dComponent.m_Type);
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
            out << YAML::Key << "RenderMode" << YAML::Value << static_cast<int>(canvas.m_RenderMode);
            out << YAML::Key << "ScaleMode" << YAML::Value << static_cast<int>(canvas.m_ScaleMode);
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
            out << YAML::Key << "Alignment" << YAML::Value << static_cast<int>(text.m_Alignment);
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
            out << YAML::Key << "Direction" << YAML::Value << static_cast<int>(slider.m_Direction);
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
            out << YAML::Key << "FillMethod" << YAML::Value << static_cast<int>(progress.m_FillMethod);
            out << YAML::Key << "BackgroundColor" << YAML::Value << progress.m_BackgroundColor;
            out << YAML::Key << "FillColor" << YAML::Value << progress.m_FillColor;

            out << YAML::EndMap; // UIProgressBarComponent
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
            out << YAML::Key << "ScrollDirection" << YAML::Value << static_cast<int>(scrollView.m_ScrollDirection);
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
            out << YAML::Key << "StartCorner" << YAML::Value << static_cast<int>(grid.m_StartCorner);
            out << YAML::Key << "StartAxis" << YAML::Value << static_cast<int>(grid.m_StartAxis);
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
            out << YAML::Key << "SimulationSpace" << YAML::Value << static_cast<int>(sys.SimulationSpace);

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
            out << YAML::Key << "EmissionShapeType" << YAML::Value << static_cast<int>(GetEmissionShapeType(emitter.Shape));

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
            out << YAML::Key << "CollisionMode" << YAML::Value << static_cast<int>(sys.CollisionModule.Mode);
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
                out << YAML::Key << "Type" << YAML::Value << static_cast<int>(ff.Type);
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
            out << YAML::Key << "BlendMode" << YAML::Value << static_cast<int>(sys.BlendMode);
            out << YAML::Key << "RenderMode" << YAML::Value << static_cast<int>(sys.RenderMode);
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
            out << YAML::Key << "TextureSheetMode" << YAML::Value << static_cast<int>(sys.TextureSheetModule.Mode);
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
            out << YAML::Key << "State" << YAML::Value << static_cast<int>(animComponent.m_State);
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
            out << YAML::Key << "TonemapOperator" << YAML::Value << static_cast<int>(pp.Tonemap);
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
        }
        out << YAML::EndMap;

        SerializeSnowSettings(out, m_Scene->GetSnowSettings());
        SerializeWindSettings(out, m_Scene->GetWindSettings());

        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        m_Scene->m_Registry.view<entt::entity>().each([&](auto entityID)
                                                      {
				// SAFETY: m_Scene is const Ref<Scene>, but Entity requires non-const Scene*
				// This is safe because serialization only reads entity data
				Entity const entity = { entityID, const_cast<Scene*>(m_Scene.get()) };
				if (!entity)
				{
					return;
				}

				SerializeEntity(out, entity); });
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

    bool SceneSerializer::Deserialize(const std::filesystem::path& filepath)
    {
        OLO_PROFILE_FUNCTION();

        YAML::Node data;
        try
        {
            data = YAML::LoadFile(filepath.string());
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("Failed to load .olo file '{0}'\n     {1}", filepath, e.what());
            return false;
        }

        if (!data["Scene"])
        {
            return false;
        }

        auto sceneName = data["Scene"].as<std::string>();
        OLO_CORE_TRACE("Deserializing scene '{0}'", sceneName);
        m_Scene->SetName(sceneName);

        if (auto ppNode = data["PostProcessSettings"]; ppNode)
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
        }

        DeserializeSnowSettings(data, m_Scene->GetSnowSettings());
        DeserializeWindSettings(data, m_Scene->GetWindSettings());

        if (const auto entities = data["Entities"]; entities)
        {
            for (auto entity : entities)
            {
                auto uuid = entity["Entity"].as<u64>();

                std::string name;
                if (auto tagComponent = entity["TagComponent"]; tagComponent)
                {
                    name = tagComponent["Tag"].as<std::string>();
                }

                OLO_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

                Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);

                if (auto transformComponent = entity["TransformComponent"]; transformComponent)
                {
                    // Entities always have transforms
                    auto& tc = deserializedEntity.GetComponent<TransformComponent>();
                    tc.Translation = transformComponent["Translation"].as<glm::vec3>();
                    tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
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
                                name = scriptField["Name"].as<std::string>();
                                auto typeString = scriptField["Type"].as<std::string>();
                                ScriptFieldType type = Utils::ScriptFieldTypeFromString(typeString);

                                ScriptFieldInstance& fieldInstance = entityFields[name];

                                OLO_CORE_ASSERT(fields.contains(name));

                                if (!fields.contains(name))
                                {
                                    continue;
                                }

                                fieldInstance.Field = fields.at(name);

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

                if (auto spriteRendererComponent = entity["SpriteRendererComponent"]; spriteRendererComponent)
                {
                    auto& src = deserializedEntity.AddComponent<SpriteRendererComponent>();
                    src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
                    if (spriteRendererComponent["TexturePath"])
                    {
                        src.Texture = Texture2D::Create(spriteRendererComponent["TexturePath"].as<std::string>());
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
                        const auto primitiveInt = meshComponent["Primitive"].as<i32>();
                        if (primitiveInt >= static_cast<i32>(MeshPrimitive::None) && primitiveInt <= static_cast<i32>(MeshPrimitive::Torus))
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
                    envMap.m_Tint = envMapComponent["Tint"].as<glm::vec3>(envMap.m_Tint);
                }

                if (auto rb3dComponent = entity["Rigidbody3DComponent"]; rb3dComponent)
                {
                    auto& rb3d = deserializedEntity.AddComponent<Rigidbody3DComponent>();
                    rb3d.m_Type = static_cast<BodyType3D>(rb3dComponent["BodyType"].as<int>(static_cast<int>(rb3d.m_Type)));
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
                        image.m_Texture = Texture2D::Create(uiImageComponent["TexturePath"].as<std::string>());
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
                        panel.m_BackgroundTexture = Texture2D::Create(uiPanelComponent["BackgroundTexturePath"].as<std::string>());
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
                    anim.m_State = static_cast<AnimationStateComponent::State>(animComponent["State"].as<int>(static_cast<int>(anim.m_State)));
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
                                // Load animations
                                if (animatedModel->HasAnimations())
                                {
                                    anim.m_AvailableClips = animatedModel->GetAnimations();
                                    if (anim.m_CurrentClipIndex >= 0 && anim.m_CurrentClipIndex < static_cast<int>(anim.m_AvailableClips.size()))
                                    {
                                        anim.m_CurrentClip = anim.m_AvailableClips[anim.m_CurrentClipIndex];
                                    }
                                    else if (!anim.m_AvailableClips.empty())
                                    {
                                        anim.m_CurrentClip = anim.m_AvailableClips[0];
                                        anim.m_CurrentClipIndex = 0;
                                    }
                                    OLO_CORE_INFO("Deserialized AnimationStateComponent: loaded {} clips from '{}'",
                                                  anim.m_AvailableClips.size(), anim.m_SourceFilePath);
                                }

                                // Update or add MeshComponent with loaded mesh data
                                if (!animatedModel->GetMeshes().empty())
                                {
                                    if (!deserializedEntity.HasComponent<MeshComponent>())
                                    {
                                        deserializedEntity.AddComponent<MeshComponent>();
                                    }
                                    auto& meshComp = deserializedEntity.GetComponent<MeshComponent>();
                                    meshComp.m_MeshSource = animatedModel->GetMeshes()[0];
                                    OLO_CORE_INFO("Deserialized MeshComponent: loaded mesh from animated model");
                                }

                                // Update or add SkeletonComponent with loaded skeleton data
                                if (animatedModel->HasSkeleton())
                                {
                                    if (!deserializedEntity.HasComponent<SkeletonComponent>())
                                    {
                                        deserializedEntity.AddComponent<SkeletonComponent>();
                                    }
                                    auto& skelComp = deserializedEntity.GetComponent<SkeletonComponent>();
                                    skelComp.m_Skeleton = animatedModel->GetSkeleton();
                                    OLO_CORE_INFO("Deserialized SkeletonComponent: loaded {} bones from animated model",
                                                  skelComp.m_Skeleton ? skelComp.m_Skeleton->m_BoneNames.size() : 0);
                                }

                                // Update MaterialComponent if model has materials
                                if (!animatedModel->GetMaterials().empty())
                                {
                                    if (!deserializedEntity.HasComponent<MaterialComponent>())
                                    {
                                        deserializedEntity.AddComponent<MaterialComponent>();
                                    }
                                    auto& matComp = deserializedEntity.GetComponent<MaterialComponent>();
                                    matComp.m_Material = animatedModel->GetMaterials()[0];
                                    OLO_CORE_INFO("Deserialized MaterialComponent: loaded material from animated model");
                                }
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
            }
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
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Scene" << YAML::Value << m_Scene->GetName();

        out << YAML::Key << "PostProcessSettings";
        out << YAML::BeginMap;
        {
            auto const& pp = m_Scene->GetPostProcessSettings();
            out << YAML::Key << "TonemapOperator" << YAML::Value << static_cast<int>(pp.Tonemap);
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
        }
        out << YAML::EndMap;

        SerializeSnowSettings(out, m_Scene->GetSnowSettings());
        SerializeWindSettings(out, m_Scene->GetWindSettings());

        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
        m_Scene->m_Registry.view<entt::entity>().each([&](auto entityID)
                                                      {
				// SAFETY: m_Scene is const Ref<Scene>, but Entity requires non-const Scene*
				// This is safe because serialization only reads entity data
				Entity const entity = { entityID, const_cast<Scene*>(m_Scene.get()) };
				if (!entity)
				{
					return;
				}

				SerializeEntity(out, entity); });
        out << YAML::EndSeq;
        out << YAML::EndMap;

        return std::string(out.c_str());
    }

    bool SceneSerializer::DeserializeFromYAML(const std::string& yamlString)
    {
        YAML::Node data;
        try
        {
            data = YAML::Load(yamlString);
        }
        catch (YAML::ParserException& e)
        {
            OLO_CORE_ERROR("Failed to load scene...\n     {0}", e.what());
            return false;
        }
        catch (YAML::BadFile&)
        {
            OLO_CORE_ERROR("Failed to load scene from string");
            return false;
        }

        if (!data["Scene"])
        {
            return false;
        }

        std::string sceneName = data["Scene"].as<std::string>();
        OLO_CORE_TRACE("Deserializing scene '{0}'", sceneName);

        if (auto ppNode = data["PostProcessSettings"]; ppNode)
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
        }

        DeserializeSnowSettings(data, m_Scene->GetSnowSettings());
        DeserializeWindSettings(data, m_Scene->GetWindSettings());

        auto entities = data["Entities"];
        if (entities)
        {
            for (auto entity : entities)
            {
                u64 uuid = entity["Entity"].as<u64>();

                std::string name;
                auto tagComponent = entity["TagComponent"];
                if (tagComponent)
                {
                    name = tagComponent["Tag"].as<std::string>();
                }

                OLO_CORE_TRACE("Deserialized entity with ID = {0}, name = {1}", uuid, name);

                Entity deserializedEntity = m_Scene->CreateEntityWithUUID(uuid, name);

                auto transformComponent = entity["TransformComponent"];
                if (transformComponent)
                {
                    // Entities always have transforms
                    auto& tc = deserializedEntity.GetComponent<TransformComponent>();
                    tc.Translation = transformComponent["Translation"].as<glm::vec3>();
                    tc.Rotation = transformComponent["Rotation"].as<glm::vec3>();
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

                if (auto spriteRendererComponent = entity["SpriteRendererComponent"]; spriteRendererComponent)
                {
                    auto& src = deserializedEntity.AddComponent<SpriteRendererComponent>();
                    src.Color = spriteRendererComponent["Color"].as<glm::vec4>();
                    if (spriteRendererComponent["TexturePath"])
                    {
                        src.Texture = Texture2D::Create(spriteRendererComponent["TexturePath"].as<std::string>());
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
                }

                if (auto prefabComponent = entity["PrefabComponent"]; prefabComponent)
                {
                    auto& pc = deserializedEntity.AddComponent<PrefabComponent>();
                    pc.m_PrefabID = prefabComponent["PrefabID"].as<u64>();
                    pc.m_PrefabEntityID = prefabComponent["PrefabEntityID"].as<u64>();
                }

                // 3D Components (matching Deserialize method)
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
                        const auto primitiveInt = meshComponent["Primitive"].as<i32>();
                        if (primitiveInt >= static_cast<i32>(MeshPrimitive::None) && primitiveInt <= static_cast<i32>(MeshPrimitive::Torus))
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

                if (auto modelComponent = entity["ModelComponent"]; modelComponent)
                {
                    auto& mc = deserializedEntity.AddComponent<ModelComponent>();
                    if (modelComponent["FilePath"])
                    {
                        mc.m_FilePath = modelComponent["FilePath"].as<std::string>();
                        if (!mc.m_FilePath.empty())
                        {
                            mc.Reload();
                        }
                    }
                    if (modelComponent["Visible"])
                    {
                        mc.m_Visible = modelComponent["Visible"].as<bool>();
                    }
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
                    envMap.m_Tint = envMapComponent["Tint"].as<glm::vec3>(envMap.m_Tint);
                }

                if (auto rb3dComponent = entity["Rigidbody3DComponent"]; rb3dComponent)
                {
                    auto& rb3d = deserializedEntity.AddComponent<Rigidbody3DComponent>();
                    rb3d.m_Type = static_cast<BodyType3D>(rb3dComponent["BodyType"].as<int>(static_cast<int>(rb3d.m_Type)));
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
                        image.m_Texture = Texture2D::Create(uiImageComponent["TexturePath"].as<std::string>());
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
                        panel.m_BackgroundTexture = Texture2D::Create(uiPanelComponent["BackgroundTexturePath"].as<std::string>());
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

                if (auto submeshComponent = entity["SubmeshComponent"]; submeshComponent)
                {
                    auto& submesh = deserializedEntity.AddComponent<SubmeshComponent>();
                    submesh.m_SubmeshIndex = submeshComponent["SubmeshIndex"].as<u32>(submesh.m_SubmeshIndex);
                    submesh.m_Visible = submeshComponent["Visible"].as<bool>(submesh.m_Visible);
                }

                if (auto animComponent = entity["AnimationStateComponent"]; animComponent)
                {
                    auto& anim = deserializedEntity.AddComponent<AnimationStateComponent>();
                    anim.m_State = static_cast<AnimationStateComponent::State>(animComponent["State"].as<int>(static_cast<int>(anim.m_State)));
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
                                // Load animations
                                if (animatedModel->HasAnimations())
                                {
                                    anim.m_AvailableClips = animatedModel->GetAnimations();
                                    if (anim.m_CurrentClipIndex >= 0 && anim.m_CurrentClipIndex < static_cast<int>(anim.m_AvailableClips.size()))
                                    {
                                        anim.m_CurrentClip = anim.m_AvailableClips[anim.m_CurrentClipIndex];
                                    }
                                    else if (!anim.m_AvailableClips.empty())
                                    {
                                        anim.m_CurrentClip = anim.m_AvailableClips[0];
                                        anim.m_CurrentClipIndex = 0;
                                    }
                                    OLO_CORE_INFO("Deserialized AnimationStateComponent: loaded {} clips from '{}'",
                                                  anim.m_AvailableClips.size(), anim.m_SourceFilePath);
                                }

                                // Update or add MeshComponent with loaded mesh data
                                if (!animatedModel->GetMeshes().empty())
                                {
                                    if (!deserializedEntity.HasComponent<MeshComponent>())
                                    {
                                        deserializedEntity.AddComponent<MeshComponent>();
                                    }
                                    auto& meshComp = deserializedEntity.GetComponent<MeshComponent>();
                                    meshComp.m_MeshSource = animatedModel->GetMeshes()[0];
                                }

                                // Update or add SkeletonComponent with loaded skeleton data
                                if (animatedModel->HasSkeleton())
                                {
                                    if (!deserializedEntity.HasComponent<SkeletonComponent>())
                                    {
                                        deserializedEntity.AddComponent<SkeletonComponent>();
                                    }
                                    auto& skelComp = deserializedEntity.GetComponent<SkeletonComponent>();
                                    skelComp.m_Skeleton = animatedModel->GetSkeleton();
                                }

                                // Update MaterialComponent if model has materials
                                if (!animatedModel->GetMaterials().empty())
                                {
                                    if (!deserializedEntity.HasComponent<MaterialComponent>())
                                    {
                                        deserializedEntity.AddComponent<MaterialComponent>();
                                    }
                                    auto& matComp = deserializedEntity.GetComponent<MaterialComponent>();
                                    matComp.m_Material = animatedModel->GetMaterials()[0];
                                }
                            }
                        }
                    }
                }

                if (auto skelComponent = entity["SkeletonComponent"]; skelComponent)
                {
                    if (!deserializedEntity.HasComponent<SkeletonComponent>())
                    {
                        deserializedEntity.AddComponent<SkeletonComponent>();
                    }
                    // Note: Skeleton data is loaded from AnimationStateComponent's source file
                }
            }
        }

        return true;
    }
} // namespace OloEngine
