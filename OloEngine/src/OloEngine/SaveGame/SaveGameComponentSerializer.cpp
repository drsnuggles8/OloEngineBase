#include "OloEnginePCH.h"
#include "SaveGameComponentSerializer.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Audio/AudioListener.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Particle/EmissionShape.h"
#include "OloEngine/Particle/ParticleCollision.h"
#include "OloEngine/Particle/ParticleCurve.h"
#include "OloEngine/Particle/ParticleEmitter.h"
#include "OloEngine/Particle/ParticleModules.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Particle/ParticleTrail.h"
#include "OloEngine/Particle/SubEmitter.h"
#include "OloEngine/Physics3D/ColliderMaterial.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

namespace OloEngine
{
    std::unordered_map<u32, SaveGameSerializeFn> SaveGameComponentSerializer::s_Registry;

    // ========================================================================
    // Helper: ColliderMaterial
    // ========================================================================

    static void SerializeColliderMaterial(FArchive& ar, ColliderMaterial& m)
    {
        if (ar.IsSaving())
        {
            f32 sf = m.GetStaticFriction();
            f32 df = m.GetDynamicFriction();
            f32 restitution = m.GetRestitution();
            f32 density = m.GetDensity();
            ar << sf << df << restitution << density;
        }
        else
        {
            f32 sf{}, df{}, restitution{}, density{};
            ar << sf << df << restitution << density;
            m.SetStaticFriction(sf);
            m.SetDynamicFriction(df);
            m.SetRestitution(restitution);
            m.SetDensity(density);
        }
    }

    // ========================================================================
    // Helper: SceneCamera (uses getters/setters)
    // ========================================================================

    static void SerializeSceneCamera(FArchive& ar, SceneCamera& cam)
    {
        if (ar.IsSaving())
        {
            auto projType = cam.GetProjectionType();
            ar << projType;
            f32 perspFOV = cam.GetPerspectiveVerticalFOV();
            f32 perspNear = cam.GetPerspectiveNearClip();
            f32 perspFar = cam.GetPerspectiveFarClip();
            f32 orthoSize = cam.GetOrthographicSize();
            f32 orthoNear = cam.GetOrthographicNearClip();
            f32 orthoFar = cam.GetOrthographicFarClip();
            ar << perspFOV << perspNear << perspFar;
            ar << orthoSize << orthoNear << orthoFar;
        }
        else
        {
            SceneCamera::ProjectionType projType{};
            ar << projType;
            cam.SetProjectionType(projType);
            f32 perspFOV{}, perspNear{}, perspFar{};
            f32 orthoSize{}, orthoNear{}, orthoFar{};
            ar << perspFOV << perspNear << perspFar;
            ar << orthoSize << orthoNear << orthoFar;
            cam.SetPerspectiveVerticalFOV(perspFOV);
            cam.SetPerspectiveNearClip(perspNear);
            cam.SetPerspectiveFarClip(perspFar);
            cam.SetOrthographicSize(orthoSize);
            cam.SetOrthographicNearClip(orthoNear);
            cam.SetOrthographicFarClip(orthoFar);
        }
    }

    // ========================================================================
    // Helper: AudioSourceConfig / AudioListenerConfig
    // ========================================================================

    static void SerializeAudioSourceConfig(FArchive& ar, AudioSourceConfig& c)
    {
        ar << c.VolumeMultiplier << c.PitchMultiplier;
        ar << c.PlayOnAwake << c.Looping << c.Spatialization;
        ar << c.AttenuationModel;
        ar << c.RollOff << c.MinGain << c.MaxGain;
        ar << c.MinDistance << c.MaxDistance;
        ar << c.ConeInnerAngle << c.ConeOuterAngle << c.ConeOuterGain;
        ar << c.DopplerFactor;
    }

    static void SerializeAudioListenerConfig(FArchive& ar, AudioListenerConfig& c)
    {
        ar << c.ConeInnerAngle << c.ConeOuterAngle << c.ConeOuterGain;
    }

    // ========================================================================
    // Helper: ParticleCurve / ParticleCurve4
    // ========================================================================

    static void SerializeParticleCurve(FArchive& ar, ParticleCurve& curve)
    {
        u32 count = std::min(curve.KeyCount, static_cast<u32>(curve.Keys.size()));
        ar << count;
        if (ar.IsLoading())
        {
            u32 fileCount = count;
            u32 cap = static_cast<u32>(curve.Keys.size());
            curve.KeyCount = std::min(fileCount, cap);
            for (u32 i = 0; i < fileCount; ++i)
            {
                if (i < cap)
                {
                    ar << curve.Keys[i].Time << curve.Keys[i].Value;
                }
                else
                {
                    f32 discardTime{}, discardValue{};
                    ar << discardTime << discardValue;
                }
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < count; ++i)
            {
                ar << curve.Keys[i].Time << curve.Keys[i].Value;
            }
        }
    }

    static void SerializeParticleCurve4(FArchive& ar, ParticleCurve4& curve)
    {
        SerializeParticleCurve(ar, curve.R);
        SerializeParticleCurve(ar, curve.G);
        SerializeParticleCurve(ar, curve.B);
        SerializeParticleCurve(ar, curve.A);
    }

    // ========================================================================
    // Helper: EmissionShape (variant)
    // ========================================================================

    static void SerializeEmissionShape(FArchive& ar, EmissionShape& shape)
    {
        if (ar.IsSaving())
        {
            EmissionShapeType type = GetEmissionShapeType(shape);
            ar << type;
            std::visit([&ar](auto&& s)
                       {
                           using T = std::decay_t<decltype(s)>;
                           if constexpr (std::is_same_v<T, EmitSphere>)
                           {
                               ar << s.Radius;
                           }
                           else if constexpr (std::is_same_v<T, EmitBox>)
                           {
                               ar << s.HalfExtents;
                           }
                           else if constexpr (std::is_same_v<T, EmitCone>)
                           {
                               ar << s.Angle << s.Radius;
                           }
                           else if constexpr (std::is_same_v<T, EmitRing>)
                           {
                               ar << s.InnerRadius << s.OuterRadius;
                           }
                           else if constexpr (std::is_same_v<T, EmitEdge>)
                           {
                               ar << s.Length;
                           }
                           else if constexpr (std::is_same_v<T, EmitMesh>)
                           {
                               ar << s.PrimitiveType;
                           }
                           // EmitPoint has no data
                       },
                       shape);
        }
        else
        {
            EmissionShapeType type{};
            ar << type;
            switch (type)
            {
                case EmissionShapeType::Point:
                    shape = EmitPoint{};
                    break;
                case EmissionShapeType::Sphere:
                {
                    EmitSphere s;
                    ar << s.Radius;
                    shape = s;
                    break;
                }
                case EmissionShapeType::Box:
                {
                    EmitBox s;
                    ar << s.HalfExtents;
                    shape = s;
                    break;
                }
                case EmissionShapeType::Cone:
                {
                    EmitCone s;
                    ar << s.Angle << s.Radius;
                    shape = s;
                    break;
                }
                case EmissionShapeType::Ring:
                {
                    EmitRing s;
                    ar << s.InnerRadius << s.OuterRadius;
                    shape = s;
                    break;
                }
                case EmissionShapeType::Edge:
                {
                    EmitEdge s;
                    ar << s.Length;
                    shape = s;
                    break;
                }
                case EmissionShapeType::Mesh:
                {
                    EmitMesh s;
                    ar << s.PrimitiveType;
                    shape = s;
                    break;
                }
                default:
                {
                    OLO_CORE_ERROR("[SaveGameComponentSerializer] Unknown EmissionShapeType: {}", static_cast<u32>(type));
                    shape = EmitPoint{};
                    break;
                }
            }
        }
    }

    // ========================================================================
    // Helper: BurstEntry
    // ========================================================================

    static void SerializeBurstEntry(FArchive& ar, BurstEntry& b)
    {
        ar << b.Time << b.Count << b.Probability;
    }

    // ========================================================================
    // Helper: ParticleEmitter
    // ========================================================================

    static void SerializeParticleEmitter(FArchive& ar, ParticleEmitter& e)
    {
        ar << e.RateOverTime << e.InitialSpeed << e.SpeedVariance;
        ar << e.LifetimeMin << e.LifetimeMax;
        ar << e.InitialSize << e.SizeVariance;
        ar << e.InitialRotation << e.RotationVariance;
        ar << e.InitialColor;
        SerializeEmissionShape(ar, e.Shape);

        u32 burstCount = static_cast<u32>(e.Bursts.size());
        ar << burstCount;
        if (ar.IsLoading())
        {
            u32 fileCount = burstCount;
            u32 clampedCount = std::min(fileCount, 1024u);
            e.Bursts.resize(clampedCount);
            for (u32 i = 0; i < fileCount; ++i)
            {
                if (i < clampedCount)
                {
                    SerializeBurstEntry(ar, e.Bursts[i]);
                }
                else
                {
                    BurstEntry discard{};
                    SerializeBurstEntry(ar, discard);
                }
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < burstCount; ++i)
            {
                SerializeBurstEntry(ar, e.Bursts[i]);
            }
        }
    }

    // ========================================================================
    // Helper: Particle modules
    // ========================================================================

    static void SerializeModuleColorOverLifetime(FArchive& ar, ModuleColorOverLifetime& m)
    {
        ar << m.Enabled;
        SerializeParticleCurve4(ar, m.ColorCurve);
    }

    static void SerializeModuleSizeOverLifetime(FArchive& ar, ModuleSizeOverLifetime& m)
    {
        ar << m.Enabled;
        SerializeParticleCurve(ar, m.SizeCurve);
    }

    static void SerializeModuleVelocityOverLifetime(FArchive& ar, ModuleVelocityOverLifetime& m)
    {
        ar << m.Enabled;
        ar << m.LinearAcceleration;
        ar << m.SpeedMultiplier;
        SerializeParticleCurve(ar, m.SpeedCurve);
    }

    static void SerializeModuleRotationOverLifetime(FArchive& ar, ModuleRotationOverLifetime& m)
    {
        ar << m.Enabled << m.AngularVelocity;
    }

    static void SerializeModuleGravity(FArchive& ar, ModuleGravity& m)
    {
        ar << m.Enabled << m.Gravity;
    }

    static void SerializeModuleDrag(FArchive& ar, ModuleDrag& m)
    {
        ar << m.Enabled << m.DragCoefficient;
    }

    static void SerializeModuleNoise(FArchive& ar, ModuleNoise& m)
    {
        ar << m.Enabled << m.Strength << m.Frequency;
    }

    static void SerializeModuleCollision(FArchive& ar, ModuleCollision& m)
    {
        ar << m.Enabled << m.Mode;
        ar << m.PlaneNormal << m.PlaneOffset;
        ar << m.Bounce << m.LifetimeLoss << m.KillOnCollide;
    }

    static void SerializeModuleForceField(FArchive& ar, ModuleForceField& m)
    {
        ar << m.Enabled << m.Type;
        ar << m.Position << m.Strength << m.Radius << m.Axis;
    }

    static void SerializeModuleTrail(FArchive& ar, ModuleTrail& m)
    {
        ar << m.Enabled;
        ar << m.MaxTrailPoints << m.TrailLifetime << m.MinVertexDistance;
        ar << m.WidthStart << m.WidthEnd;
        ar << m.ColorStart << m.ColorEnd;
    }

    static void SerializeSubEmitterEntry(FArchive& ar, SubEmitterEntry& e)
    {
        ar << e.Trigger << e.EmitCount;
        ar << e.InheritVelocity << e.InheritVelocityScale;
        ar << e.ChildSystemIndex;
    }

    static void SerializeModuleSubEmitter(FArchive& ar, ModuleSubEmitter& m)
    {
        ar << m.Enabled;
        u32 count = static_cast<u32>(m.Entries.size());
        ar << count;
        if (ar.IsLoading())
        {
            u32 clampedCount = std::min(count, 1024u);
            m.Entries.resize(clampedCount);
            for (u32 i = 0; i < clampedCount; ++i)
            {
                SerializeSubEmitterEntry(ar, m.Entries[i]);
                if (ar.IsError())
                {
                    return;
                }
            }
            // Drain excess entries to keep stream aligned
            for (u32 i = clampedCount; i < count; ++i)
            {
                SubEmitterEntry discard{};
                SerializeSubEmitterEntry(ar, discard);
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < count; ++i)
            {
                SerializeSubEmitterEntry(ar, m.Entries[i]);
            }
        }
    }

    static void SerializeModuleTextureSheet(FArchive& ar, ModuleTextureSheetAnimation& m)
    {
        ar << m.Enabled;
        ar << m.GridX << m.GridY << m.TotalFrames;
        ar << m.Mode << m.SpeedRange;
    }

    // ========================================================================
    // Helper: SHCoefficients
    // ========================================================================

    static void SerializeSHCoefficients(FArchive& ar, SHCoefficients& sh)
    {
        for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
        {
            ar << sh.Coefficients[i];
        }
    }

    // ========================================================================
    // Helper: FoliageLayer
    // ========================================================================

    static void SerializeFoliageLayer(FArchive& ar, FoliageLayer& l)
    {
        ar << l.Name << l.MeshPath << l.AlbedoPath;
        ar << l.Density;
        ar << l.SplatmapChannel;
        ar << l.MinSlopeAngle << l.MaxSlopeAngle;
        ar << l.MinScale << l.MaxScale;
        ar << l.MinHeight << l.MaxHeight;
        ar << l.RandomRotation;
        ar << l.ViewDistance << l.FadeStartDistance;
        ar << l.WindStrength << l.WindSpeed;
        ar << l.BaseColor;
        ar << l.Roughness << l.AlphaCutoff;
        ar << l.Enabled;
        // AlbedoTexture (Ref<Texture2D>) is runtime — not serialized
    }

    // ========================================================================
    // Helper: UIDropdownOption
    // ========================================================================

    static void SerializeUIDropdownOption(FArchive& ar, UIDropdownOption& opt)
    {
        ar << opt.m_Label;
    }

    // ========================================================================
    // Helper: LODLevel
    // ========================================================================

    static void SerializeLODLevel(FArchive& ar, LODLevel& l)
    {
        ar << l.MeshHandle << l.MaxDistance << l.TriangleCount;
    }

    // ========================================================================
    // Component Serializers
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, IDComponent& c)
    {
        ar << c.ID;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TagComponent& c)
    {
        ar << c.Tag;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, PrefabComponent& c)
    {
        ar << c.m_PrefabID << c.m_PrefabEntityID;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TransformComponent& c)
    {
        ar << c.Translation << c.Rotation << c.Scale;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, RelationshipComponent& c)
    {
        ar << c.m_Children;
        ar << c.m_ParentHandle;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SpriteRendererComponent& c)
    {
        ar << c.Color << c.TilingFactor;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CircleRendererComponent& c)
    {
        ar << c.Color << c.Thickness << c.Fade;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CameraComponent& c)
    {
        SerializeSceneCamera(ar, c.Camera);
        ar << c.Primary << c.FixedAspectRatio;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, Rigidbody2DComponent& c)
    {
        ar << c.Type << c.FixedRotation;
        ar << c.LinearVelocity << c.AngularVelocity;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, BoxCollider2DComponent& c)
    {
        ar << c.Offset << c.Size;
        ar << c.Density << c.Friction << c.Restitution << c.RestitutionThreshold;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CircleCollider2DComponent& c)
    {
        ar << c.Offset << c.Radius;
        ar << c.Density << c.Friction << c.Restitution << c.RestitutionThreshold;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, Rigidbody3DComponent& c)
    {
        ar << c.m_Type << c.m_LayerID;
        ar << c.m_Mass << c.m_LinearDrag << c.m_AngularDrag;
        ar << c.m_DisableGravity << c.m_IsTrigger;
        ar << c.m_LockedAxes;
        ar << c.m_InitialLinearVelocity << c.m_InitialAngularVelocity;
        ar << c.m_MaxLinearVelocity << c.m_MaxAngularVelocity;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, BoxCollider3DComponent& c)
    {
        ar << c.m_HalfExtents << c.m_Offset;
        SerializeColliderMaterial(ar, c.m_Material);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SphereCollider3DComponent& c)
    {
        ar << c.m_Radius << c.m_Offset;
        SerializeColliderMaterial(ar, c.m_Material);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CapsuleCollider3DComponent& c)
    {
        ar << c.m_Radius << c.m_HalfHeight << c.m_Offset;
        SerializeColliderMaterial(ar, c.m_Material);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, MeshCollider3DComponent& c)
    {
        ar << c.m_ColliderAsset << c.m_Offset << c.m_Scale;
        SerializeColliderMaterial(ar, c.m_Material);
        ar << c.m_UseComplexAsSimple;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ConvexMeshCollider3DComponent& c)
    {
        ar << c.m_ColliderAsset << c.m_Offset << c.m_Scale;
        SerializeColliderMaterial(ar, c.m_Material);
        ar << c.m_ConvexRadius << c.m_MaxVertices;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TriangleMeshCollider3DComponent& c)
    {
        ar << c.m_ColliderAsset << c.m_Offset << c.m_Scale;
        SerializeColliderMaterial(ar, c.m_Material);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CharacterController3DComponent& c)
    {
        ar << c.m_SlopeLimitDeg << c.m_StepOffset << c.m_JumpPower;
        ar << c.m_LayerID;
        ar << c.m_DisableGravity << c.m_ControlMovementInAir << c.m_ControlRotationInAir;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TextComponent& c)
    {
        ar << c.TextString << c.Color;
        ar << c.Kerning << c.LineSpacing;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ScriptComponent& c)
    {
        ar << c.ClassName;
        // Script field values are persisted through ISaveable (CollectSaveableData/RestoreSaveableData)
        // which is orchestrated by SaveGameSerializer when processing ScriptComponents.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, AudioSourceComponent& c)
    {
        SerializeAudioSourceConfig(ar, c.Config);
        // Ref<AudioSource> is runtime — not serialized
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, AudioListenerComponent& c)
    {
        ar << c.Active;
        SerializeAudioListenerConfig(ar, c.Config);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, MaterialComponent& c)
    {
        // Serialize core material properties (matching SceneSerializer coverage)
        auto& mat = c.m_Material;
        if (ar.IsSaving())
        {
            auto type = mat.GetType();
            ar << type;
            auto name = mat.GetName();
            ar << name;
            auto flags = mat.GetFlags();
            ar << flags;

            // Legacy
            auto ambient = mat.GetAmbient();
            auto diffuse = mat.GetDiffuse();
            auto specular = mat.GetSpecular();
            auto shininess = mat.GetShininess();
            auto useTexMaps = mat.IsUsingTextureMaps();
            ar << ambient << diffuse << specular << shininess << useTexMaps;

            // PBR
            auto baseColor = mat.GetBaseColorFactor();
            auto emissive = mat.GetEmissiveFactor();
            auto metallic = mat.GetMetallicFactor();
            auto roughness = mat.GetRoughnessFactor();
            auto normalScale = mat.GetNormalScale();
            auto occStrength = mat.GetOcclusionStrength();
            auto enableIBL = mat.IsIBLEnabled();
            ar << baseColor << emissive << metallic << roughness;
            ar << normalScale << occStrength << enableIBL;
        }
        else
        {
            MaterialType type{};
            ar << type;
            mat.SetType(type);
            std::string name;
            ar << name;
            mat.SetName(name);
            u32 flags{};
            ar << flags;
            mat.SetFlags(flags);

            // Legacy
            glm::vec3 ambient{}, diffuse{}, specular{};
            f32 shininess{};
            bool useTexMaps{};
            ar << ambient << diffuse << specular << shininess << useTexMaps;
            mat.SetAmbient(ambient);
            mat.SetDiffuse(diffuse);
            mat.SetSpecular(specular);
            mat.SetShininess(shininess);
            mat.SetUseTextureMaps(useTexMaps);

            // PBR
            glm::vec4 baseColor{}, emissive{};
            f32 metallic{}, roughness{}, normalScale{}, occStrength{};
            bool enableIBL{};
            ar << baseColor << emissive << metallic << roughness;
            ar << normalScale << occStrength << enableIBL;
            mat.SetBaseColorFactor(baseColor);
            mat.SetEmissiveFactor(emissive);
            mat.SetMetallicFactor(metallic);
            mat.SetRoughnessFactor(roughness);
            mat.SetNormalScale(normalScale);
            mat.SetOcclusionStrength(occStrength);
            mat.SetEnableIBL(enableIBL);
        }
        // Texture maps (Ref<>) are runtime-only asset references — not serialized in save game
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, DirectionalLightComponent& c)
    {
        ar << c.m_Direction << c.m_Color << c.m_Intensity;
        ar << c.m_CastShadows << c.m_ShadowBias << c.m_ShadowNormalBias;
        ar << c.m_MaxShadowDistance << c.m_CascadeSplitLambda << c.m_CascadeDebugVisualization;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, PointLightComponent& c)
    {
        ar << c.m_Color << c.m_Intensity << c.m_Range << c.m_Attenuation;
        ar << c.m_CastShadows << c.m_ShadowBias << c.m_ShadowNormalBias;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SpotLightComponent& c)
    {
        ar << c.m_Direction << c.m_Color << c.m_Intensity;
        ar << c.m_Range << c.m_InnerCutoff << c.m_OuterCutoff << c.m_Attenuation;
        ar << c.m_CastShadows << c.m_ShadowBias << c.m_ShadowNormalBias;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, EnvironmentMapComponent& c)
    {
        ar << c.m_EnvironmentMapAsset << c.m_FilePath;
        ar << c.m_IsCubemapFolder << c.m_EnableSkybox;
        ar << c.m_Rotation << c.m_Exposure << c.m_BlurAmount;
        ar << c.m_EnableIBL << c.m_IBLIntensity << c.m_Tint;
        // Ref<EnvironmentMap> is runtime — not serialized
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, LightProbeComponent& c)
    {
        ar << c.m_InfluenceRadius << c.m_Intensity << c.m_Active;
        SerializeSHCoefficients(ar, c.m_SHCoefficients);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, LightProbeVolumeComponent& c)
    {
        ar << c.m_BoundsMin << c.m_BoundsMax << c.m_Resolution;
        ar << c.m_Spacing << c.m_Intensity;
        ar << c.m_Active << c.m_Dirty << c.m_ShowDebugProbes;
        ar << c.m_BakedDataAsset;
    }

    // ========================================================================
    // UI Components
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UICanvasComponent& c)
    {
        ar << c.m_RenderMode << c.m_ScaleMode;
        ar << c.m_SortOrder << c.m_ReferenceResolution;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIRectTransformComponent& c)
    {
        ar << c.m_AnchorMin << c.m_AnchorMax;
        ar << c.m_AnchoredPosition << c.m_SizeDelta << c.m_Pivot;
        ar << c.m_Rotation << c.m_Scale;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIImageComponent& c)
    {
        ar << c.m_Color << c.m_BorderInsets;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIPanelComponent& c)
    {
        ar << c.m_BackgroundColor;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UITextComponent& c)
    {
        ar << c.m_Text << c.m_FontSize << c.m_Color;
        ar << c.m_Alignment;
        ar << c.m_Kerning << c.m_LineSpacing;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIButtonComponent& c)
    {
        ar << c.m_NormalColor << c.m_HoveredColor << c.m_PressedColor << c.m_DisabledColor;
        ar << c.m_Interactable;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UISliderComponent& c)
    {
        ar << c.m_Value << c.m_MinValue << c.m_MaxValue;
        ar << c.m_Direction;
        ar << c.m_BackgroundColor << c.m_FillColor << c.m_HandleColor;
        ar << c.m_Interactable;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UICheckboxComponent& c)
    {
        ar << c.m_IsChecked;
        ar << c.m_UncheckedColor << c.m_CheckedColor << c.m_CheckmarkColor;
        ar << c.m_Interactable;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIProgressBarComponent& c)
    {
        ar << c.m_Value << c.m_MinValue << c.m_MaxValue;
        ar << c.m_FillMethod;
        ar << c.m_BackgroundColor << c.m_FillColor;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIInputFieldComponent& c)
    {
        ar << c.m_Text << c.m_Placeholder << c.m_FontSize;
        ar << c.m_TextColor << c.m_PlaceholderColor << c.m_BackgroundColor;
        ar << c.m_CharacterLimit << c.m_Interactable;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIScrollViewComponent& c)
    {
        ar << c.m_ScrollPosition << c.m_ContentSize;
        ar << c.m_ScrollDirection << c.m_ScrollSpeed;
        ar << c.m_ShowHorizontalScrollbar << c.m_ShowVerticalScrollbar;
        ar << c.m_ScrollbarColor << c.m_ScrollbarTrackColor;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIDropdownComponent& c)
    {
        u32 optCount = static_cast<u32>(c.m_Options.size());
        ar << optCount;
        if (ar.IsLoading())
        {
            u32 clampedOpt = std::min(optCount, 1024u);
            c.m_Options.resize(clampedOpt);
            for (u32 i = 0; i < clampedOpt; ++i)
            {
                SerializeUIDropdownOption(ar, c.m_Options[i]);
                if (ar.IsError())
                {
                    return;
                }
            }
            // Drain excess entries to keep stream aligned
            for (u32 i = clampedOpt; i < optCount; ++i)
            {
                UIDropdownOption discard{};
                SerializeUIDropdownOption(ar, discard);
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < optCount; ++i)
            {
                SerializeUIDropdownOption(ar, c.m_Options[i]);
            }
        }
        ar << c.m_SelectedIndex;
        ar << c.m_BackgroundColor << c.m_HighlightColor << c.m_TextColor;
        ar << c.m_FontSize << c.m_ItemHeight;
        ar << c.m_Interactable;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIGridLayoutComponent& c)
    {
        ar << c.m_CellSize << c.m_Spacing << c.m_Padding;
        ar << c.m_StartCorner << c.m_StartAxis;
        ar << c.m_ConstraintCount;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIToggleComponent& c)
    {
        ar << c.m_IsOn;
        ar << c.m_OffColor << c.m_OnColor << c.m_KnobColor;
        ar << c.m_Interactable;
    }

    // ========================================================================
    // ParticleSystemComponent
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ParticleSystemComponent& c)
    {
        auto& ps = c.System;

        ar << ps.Playing << ps.Looping << ps.Duration << ps.PlaybackSpeed << ps.WarmUpTime;
        ar << ps.SimulationSpace << ps.BlendMode << ps.RenderMode;
        ar << ps.DepthSortEnabled << ps.UseGPU;
        ar << ps.WindInfluence;
        ar << ps.GPUNoiseStrength << ps.GPUNoiseFrequency;
        ar << ps.GPUGroundCollision << ps.GPUGroundY;
        ar << ps.GPUCollisionBounce << ps.GPUCollisionFriction;
        ar << ps.SoftParticlesEnabled << ps.SoftParticleDistance;
        ar << ps.VelocityInheritance;
        ar << ps.LODDistance1 << ps.LODMaxDistance;

        SerializeParticleEmitter(ar, ps.Emitter);
        SerializeModuleColorOverLifetime(ar, ps.ColorModule);
        SerializeModuleSizeOverLifetime(ar, ps.SizeModule);
        SerializeModuleVelocityOverLifetime(ar, ps.VelocityModule);
        SerializeModuleRotationOverLifetime(ar, ps.RotationModule);
        SerializeModuleGravity(ar, ps.GravityModule);
        SerializeModuleDrag(ar, ps.DragModule);
        SerializeModuleNoise(ar, ps.NoiseModule);
        SerializeModuleCollision(ar, ps.CollisionModule);

        // ForceFields (vector of modules)
        u32 ffCount = static_cast<u32>(ps.ForceFields.size());
        ar << ffCount;
        if (ar.IsLoading())
        {
            u32 clampedFF = std::min(ffCount, 1024u);
            ps.ForceFields.resize(clampedFF);
            for (u32 i = 0; i < clampedFF; ++i)
            {
                SerializeModuleForceField(ar, ps.ForceFields[i]);
                if (ar.IsError())
                {
                    return;
                }
            }
            // Drain excess entries to keep stream aligned
            for (u32 i = clampedFF; i < ffCount; ++i)
            {
                ModuleForceField discard{};
                SerializeModuleForceField(ar, discard);
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < ffCount; ++i)
            {
                SerializeModuleForceField(ar, ps.ForceFields[i]);
            }
        }

        SerializeModuleTrail(ar, ps.TrailModule);
        SerializeModuleSubEmitter(ar, ps.SubEmitterModule);
        SerializeModuleTextureSheet(ar, ps.TextureSheetModule);
    }

    // ========================================================================
    // Terrain, Foliage, Water, Snow, Fog, Decal
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TerrainComponent& c)
    {
        ar << c.m_HeightmapPath;
        ar << c.m_WorldSizeX << c.m_WorldSizeZ << c.m_HeightScale;
        ar << c.m_ProceduralEnabled << c.m_ProceduralSeed;
        ar << c.m_ProceduralResolution << c.m_ProceduralOctaves;
        ar << c.m_ProceduralFrequency << c.m_ProceduralLacunarity << c.m_ProceduralPersistence;
        ar << c.m_TessellationEnabled << c.m_TargetTriangleSize << c.m_MorphRegion;
        ar << c.m_StreamingEnabled << c.m_TileDirectory << c.m_TileFilePattern;
        ar << c.m_TileWorldSize << c.m_TileResolution;
        ar << c.m_StreamingLoadRadius << c.m_StreamingMaxTiles;
        ar << c.m_VoxelEnabled << c.m_VoxelSize;
        // Runtime pointers (TerrainData, ChunkManager, etc.) are not serialized
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, FoliageComponent& c)
    {
        u32 layerCount = static_cast<u32>(c.m_Layers.size());
        ar << layerCount;
        if (ar.IsLoading())
        {
            u32 clampedLayers = std::min(layerCount, 1024u);
            c.m_Layers.resize(clampedLayers);
            for (u32 i = 0; i < clampedLayers; ++i)
            {
                SerializeFoliageLayer(ar, c.m_Layers[i]);
                if (ar.IsError())
                {
                    return;
                }
            }
            // Drain excess entries to keep stream aligned
            for (u32 i = clampedLayers; i < layerCount; ++i)
            {
                FoliageLayer discard{};
                SerializeFoliageLayer(ar, discard);
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < layerCount; ++i)
            {
                SerializeFoliageLayer(ar, c.m_Layers[i]);
            }
        }
        ar << c.m_Enabled;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, WaterComponent& c)
    {
        ar << c.m_WorldSizeX << c.m_WorldSizeZ;
        ar << c.m_WaveAmplitude << c.m_WaveFrequency << c.m_WaveSpeed;
        ar << c.m_WaveDir0 << c.m_WaveSteepness0 << c.m_Wavelength0;
        ar << c.m_WaveDir1 << c.m_WaveSteepness1 << c.m_Wavelength1;
        ar << c.m_WaterColor << c.m_DeepColor;
        ar << c.m_Transparency << c.m_Reflectivity << c.m_FresnelPower << c.m_SpecularIntensity;
        ar << c.m_GridResolutionX << c.m_GridResolutionZ;
        ar << c.m_Enabled;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SnowDeformerComponent& c)
    {
        ar << c.m_DeformRadius << c.m_DeformDepth;
        ar << c.m_FalloffExponent << c.m_CompactionFactor;
        ar << c.m_EmitEjecta;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, FogVolumeComponent& c)
    {
        ar << c.m_Shape << c.m_Extents << c.m_Color;
        ar << c.m_Density << c.m_FalloffDistance;
        ar << c.m_Priority << c.m_BlendWeight;
        ar << c.m_Enabled << c.m_AffectTransparent;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, DecalComponent& c)
    {
        ar << c.m_Color << c.m_Size;
        ar << c.m_FadeDistance << c.m_NormalAngleThreshold;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, LODGroupComponent& c)
    {
        u32 levelCount = static_cast<u32>(c.m_LODGroup.Levels.size());
        ar << levelCount;
        if (ar.IsLoading())
        {
            u32 clampedLevels = std::min(levelCount, 1024u);
            c.m_LODGroup.Levels.resize(clampedLevels);
            for (u32 i = 0; i < clampedLevels; ++i)
            {
                SerializeLODLevel(ar, c.m_LODGroup.Levels[i]);
                if (ar.IsError())
                {
                    return;
                }
            }
            // Drain excess entries to keep stream aligned
            for (u32 i = clampedLevels; i < levelCount; ++i)
            {
                LODLevel discard{};
                SerializeLODLevel(ar, discard);
                if (ar.IsError())
                {
                    return;
                }
            }
        }
        else
        {
            for (u32 i = 0; i < levelCount; ++i)
            {
                SerializeLODLevel(ar, c.m_LODGroup.Levels[i]);
            }
        }
        ar << c.m_LODGroup.Bias;
        ar << c.m_Enabled;
    }

    // ========================================================================
    // Networking Components
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NetworkIdentityComponent& c)
    {
        ar << c.OwnerClientID << c.Authority << c.IsReplicated;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NetworkInterestComponent& c)
    {
        ar << c.RelevanceRadius << c.InterestGroup;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, PhaseComponent& c)
    {
        ar << c.PhaseID;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, InstancePortalComponent& c)
    {
        ar << c.TargetZoneID << c.InstanceType << c.MaxPlayers;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NetworkLODComponent& c)
    {
        ar << c.Level;
    }

    // ========================================================================
    // Animation / Mesh Components
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SubmeshComponent& c)
    {
        ar << c.m_BoneEntityIds << c.m_SubmeshIndex << c.m_Visible;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, MeshComponent& c)
    {
        ar << c.m_Primitive;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ModelComponent& c)
    {
        ar << c.m_FilePath << c.m_Visible;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, AnimationStateComponent& c)
    {
        ar << c.m_State;
        ar << c.m_CurrentClipIndex;
        ar << c.m_CurrentTime << c.m_NextTime;
        ar << c.m_BlendFactor << c.m_Blending;
        ar << c.m_BlendDuration << c.m_BlendTime;
        ar << c.m_IsPlaying;
        ar << c.m_SourceFilePath;
        ar << c.m_BoneEntityIds;
        ar << c.m_RootBoneTransform;
        // Ref<AnimationClip> members are runtime — not serialized
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, StreamingVolumeComponent& c)
    {
        ar << c.RegionAssetHandle << c.ActivationMode;
        ar << c.LoadRadius << c.UnloadRadius;
    }

    // ========================================================================
    // Registry
    // ========================================================================

    template<typename ComponentType>
    void SaveGameComponentSerializer::RegisterSaveComponent(const char* name)
    {
        Register(Hash::GenerateFNVHash(name),
                 [](FArchive& ar, void* comp)
                 { Serialize(ar, *static_cast<ComponentType*>(comp)); });
    }

#define REGISTER_SAVE_COMPONENT(ComponentType) RegisterSaveComponent<ComponentType>(#ComponentType)

    void SaveGameComponentSerializer::RegisterAll()
    {
        OLO_PROFILE_FUNCTION();

        REGISTER_SAVE_COMPONENT(IDComponent);
        REGISTER_SAVE_COMPONENT(TagComponent);
        REGISTER_SAVE_COMPONENT(PrefabComponent);
        REGISTER_SAVE_COMPONENT(TransformComponent);
        REGISTER_SAVE_COMPONENT(RelationshipComponent);
        REGISTER_SAVE_COMPONENT(SpriteRendererComponent);
        REGISTER_SAVE_COMPONENT(CircleRendererComponent);
        REGISTER_SAVE_COMPONENT(CameraComponent);
        REGISTER_SAVE_COMPONENT(Rigidbody2DComponent);
        REGISTER_SAVE_COMPONENT(BoxCollider2DComponent);
        REGISTER_SAVE_COMPONENT(CircleCollider2DComponent);
        REGISTER_SAVE_COMPONENT(Rigidbody3DComponent);
        REGISTER_SAVE_COMPONENT(BoxCollider3DComponent);
        REGISTER_SAVE_COMPONENT(SphereCollider3DComponent);
        REGISTER_SAVE_COMPONENT(CapsuleCollider3DComponent);
        REGISTER_SAVE_COMPONENT(MeshCollider3DComponent);
        REGISTER_SAVE_COMPONENT(ConvexMeshCollider3DComponent);
        REGISTER_SAVE_COMPONENT(TriangleMeshCollider3DComponent);
        REGISTER_SAVE_COMPONENT(CharacterController3DComponent);
        REGISTER_SAVE_COMPONENT(TextComponent);
        REGISTER_SAVE_COMPONENT(ScriptComponent);
        REGISTER_SAVE_COMPONENT(AudioSourceComponent);
        REGISTER_SAVE_COMPONENT(AudioListenerComponent);
        REGISTER_SAVE_COMPONENT(MaterialComponent);
        REGISTER_SAVE_COMPONENT(DirectionalLightComponent);
        REGISTER_SAVE_COMPONENT(PointLightComponent);
        REGISTER_SAVE_COMPONENT(SpotLightComponent);
        REGISTER_SAVE_COMPONENT(EnvironmentMapComponent);
        REGISTER_SAVE_COMPONENT(LightProbeComponent);
        REGISTER_SAVE_COMPONENT(LightProbeVolumeComponent);
        REGISTER_SAVE_COMPONENT(UICanvasComponent);
        REGISTER_SAVE_COMPONENT(UIRectTransformComponent);
        REGISTER_SAVE_COMPONENT(UIImageComponent);
        REGISTER_SAVE_COMPONENT(UIPanelComponent);
        REGISTER_SAVE_COMPONENT(UITextComponent);
        REGISTER_SAVE_COMPONENT(UIButtonComponent);
        REGISTER_SAVE_COMPONENT(UISliderComponent);
        REGISTER_SAVE_COMPONENT(UICheckboxComponent);
        REGISTER_SAVE_COMPONENT(UIProgressBarComponent);
        REGISTER_SAVE_COMPONENT(UIInputFieldComponent);
        REGISTER_SAVE_COMPONENT(UIScrollViewComponent);
        REGISTER_SAVE_COMPONENT(UIDropdownComponent);
        REGISTER_SAVE_COMPONENT(UIGridLayoutComponent);
        REGISTER_SAVE_COMPONENT(UIToggleComponent);
        REGISTER_SAVE_COMPONENT(ParticleSystemComponent);
        REGISTER_SAVE_COMPONENT(TerrainComponent);
        REGISTER_SAVE_COMPONENT(FoliageComponent);
        REGISTER_SAVE_COMPONENT(WaterComponent);
        REGISTER_SAVE_COMPONENT(SnowDeformerComponent);
        REGISTER_SAVE_COMPONENT(FogVolumeComponent);
        REGISTER_SAVE_COMPONENT(DecalComponent);
        REGISTER_SAVE_COMPONENT(LODGroupComponent);
        REGISTER_SAVE_COMPONENT(NetworkIdentityComponent);
        REGISTER_SAVE_COMPONENT(NetworkInterestComponent);
        REGISTER_SAVE_COMPONENT(PhaseComponent);
        REGISTER_SAVE_COMPONENT(InstancePortalComponent);
        REGISTER_SAVE_COMPONENT(NetworkLODComponent);
        REGISTER_SAVE_COMPONENT(SubmeshComponent);
        REGISTER_SAVE_COMPONENT(MeshComponent);
        REGISTER_SAVE_COMPONENT(ModelComponent);
        REGISTER_SAVE_COMPONENT(AnimationStateComponent);
        REGISTER_SAVE_COMPONENT(StreamingVolumeComponent);

        OLO_CORE_TRACE("[SaveGameComponentSerializer] Registered {} component serializers", s_Registry.size());
    }

#undef REGISTER_SAVE_COMPONENT

    void SaveGameComponentSerializer::Register(u32 typeHash, SaveGameSerializeFn serializer)
    {
        OLO_PROFILE_FUNCTION();
        s_Registry[typeHash] = serializer;
    }

    const SaveGameSerializeFn* SaveGameComponentSerializer::GetSerializer(u32 typeHash)
    {
        OLO_PROFILE_FUNCTION();
        auto it = s_Registry.find(typeHash);
        return it != s_Registry.end() ? &it->second : nullptr;
    }

    const std::unordered_map<u32, SaveGameSerializeFn>& SaveGameComponentSerializer::GetRegistry()
    {
        OLO_PROFILE_FUNCTION();
        return s_Registry;
    }

    void SaveGameComponentSerializer::ClearRegistry()
    {
        OLO_PROFILE_FUNCTION();
        s_Registry.clear();
    }

} // namespace OloEngine
