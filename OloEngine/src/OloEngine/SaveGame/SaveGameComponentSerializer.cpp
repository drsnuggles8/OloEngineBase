#include "OloEnginePCH.h"
#include "SaveGameComponentSerializer.h"

#include <algorithm>
#include <cmath>

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/SpringBoneComponent.h"
#include "OloEngine/Animation/Retargeting/RetargetingComponent.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/LocomotionComponent.h"
#include "OloEngine/Animation/NoiseAnimationComponent.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
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
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"

namespace OloEngine
{
    std::unordered_map<u32, SaveGameSerializeFn> SaveGameComponentSerializer::s_Registry;

    // ========================================================================
    // Helper: per-field format-version gating (issue #454)
    // ========================================================================
    //
    // Save-game archives are fixed-order binary: every field is read/written
    // unconditionally in sequence, so a field appended after the component's
    // format was first shipped must be gated behind the version it was
    // introduced in, or an older save desyncs every read that follows it.
    // Saving always writes the current (full) layout -- IsSaving() short-
    // circuits the check -- only loading gates on the archive's recorded
    // FormatVersion (SaveGameSerializer::RestoreSceneState plumbs the save's
    // header FormatVersion into FArchive::ArArchiveVersion before any
    // component is deserialized).
    static bool HasFieldsSince(const FArchive& ar, u32 introducedInVersion)
    {
        return ar.IsSaving() || ar.GetArchiveVersion() >= introducedInVersion;
    }

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
        ar << c.Spread << c.Focus;
        ar << c.LowPassCutoff << c.HighPassCutoff << c.ReverbSend;

        if (ar.IsLoading())
        {
            auto sanitize = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                    return;
                }
                v = std::clamp(v, lo, hi);
            };
            sanitize(c.Spread, 0.0f, 1.0f, 1.0f);
            sanitize(c.Focus, 0.0f, 1.0f, 1.0f);
            sanitize(c.LowPassCutoff, 0.0f, 1.0f, 1.0f);
            sanitize(c.HighPassCutoff, 0.0f, 1.0f, 0.0f);
            sanitize(c.ReverbSend, 0.0f, 1.0f, 0.0f);

            // Non-DSP fields
            sanitize(c.VolumeMultiplier, 0.0f, 10.0f, 1.0f);
            sanitize(c.PitchMultiplier, 0.0f, 10.0f, 1.0f);
            sanitize(c.RollOff, 0.0f, 100.0f, 1.0f);
            sanitize(c.MinGain, 0.0f, 1.0f, 0.0f);
            sanitize(c.MaxGain, 0.0f, 1.0f, 1.0f);
            sanitize(c.MinDistance, 0.0f, 1e6f, 0.3f);
            sanitize(c.MaxDistance, 0.0f, 1e6f, 1000.0f);
            if (c.MinDistance > c.MaxDistance)
            {
                c.MinDistance = c.MaxDistance;
            }
            sanitize(c.ConeInnerAngle, 0.0f, glm::radians(360.0f), glm::radians(360.0f));
            sanitize(c.ConeOuterAngle, 0.0f, glm::radians(360.0f), glm::radians(360.0f));
            sanitize(c.ConeOuterGain, 0.0f, 1.0f, 0.0f);
            sanitize(c.DopplerFactor, 0.0f, 10.0f, 1.0f);
        }
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
                           else
                           {
                               // No additional handling required.
                               // (EmitPoint has no data)
                           } },
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
                    ar.SetError();
                    return;
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
        // Octahedral impostor fields appended in save-format v11 (issue #433).
        // Gated so v10-and-older saves keep the exact pre-v11 field order (Enabled
        // was the last field) and read back with the constructor defaults.
        if (HasFieldsSince(ar, 11))
        {
            ar << l.UseImpostor;
            ar << l.ImpostorStartDistance << l.ImpostorTransitionBand;
            ar << l.ImpostorFramesPerAxis << l.ImpostorAtlasResolution;
            ar << l.ImpostorHemiOctahedral;

            if (ar.IsLoading())
            {
                // Guard a corrupt/hostile save from feeding NaN / a negative band
                // into Impostor::ImpostorFade (mirrors the scene deserializer).
                if (!std::isfinite(l.ImpostorStartDistance))
                    l.ImpostorStartDistance = 40.0f;
                l.ImpostorStartDistance = std::max(l.ImpostorStartDistance, 0.0f);
                if (!std::isfinite(l.ImpostorTransitionBand))
                    l.ImpostorTransitionBand = 15.0f;
                l.ImpostorTransitionBand = std::max(l.ImpostorTransitionBand, 0.0f);
            }
        }
        // AlbedoTexture (Ref<Texture2D>) is runtime — not serialized
    }

    // ========================================================================
    // Helper: TerrainLayerRule (auto-material height/slope assignment)
    // ========================================================================
    static void SerializeTerrainLayerRule(FArchive& ar, TerrainLayerRule& r)
    {
        ar << r.LayerIndex;
        ar << r.MinHeight << r.MaxHeight << r.HeightBlend;
        ar << r.MinSlopeDeg << r.MaxSlopeDeg << r.SlopeBlend;
        ar << r.Strength;
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
        glm::vec3 euler = c.GetRotationEuler();
        ar << c.Translation << euler << c.Scale;
        if (ar.IsLoading())
        {
            c.SetRotationEuler(euler);
        }
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

    void SaveGameComponentSerializer::Serialize(FArchive& ar, PhysicsJoint3DComponent& c)
    {
        ar << c.m_Type << c.m_ConnectedEntity;
        ar << c.m_LocalAnchorA << c.m_LocalAnchorB << c.m_Axis;
        ar << c.m_MinDistance << c.m_MaxDistance;
        ar << c.m_HingeMinAngleDeg << c.m_HingeMaxAngleDeg;
        ar << c.m_SliderMinLimit << c.m_SliderMaxLimit;
        ar << c.m_ConeHalfAngleDeg;

        // Three tails were appended over time, each after a once-final field, so
        // probe AtEnd() before reading each on load and default to "disabled":
        //   1. Break thresholds  (after m_ConeHalfAngleDeg — legacy archives end here)
        //   2. Motor + friction  (after the break thresholds — pre-motor archives end here)
        //   3. Limit springs     (after the motor block — pre-spring archives end here)
        // Mirrors EnvironmentMapComponent above.
        if (ar.IsLoading())
        {
            if (ar.AtEnd())
            {
                c.m_BreakForce = 0.0f;
                c.m_BreakTorque = 0.0f;
            }
            else
            {
                ar << c.m_BreakForce << c.m_BreakTorque;
            }

            // Sanitize untrusted on-disk values; 0 means unbreakable.
            if (!std::isfinite(c.m_BreakForce))
                c.m_BreakForce = 0.0f;
            c.m_BreakForce = std::clamp(c.m_BreakForce, 0.0f, 1.0e9f);
            if (!std::isfinite(c.m_BreakTorque))
                c.m_BreakTorque = 0.0f;
            c.m_BreakTorque = std::clamp(c.m_BreakTorque, 0.0f, 1.0e9f);

            // Motor + friction tail. Archives written before motors existed end
            // after the break thresholds, so default to "motor off, no friction".
            if (ar.AtEnd())
            {
                c.m_HingeMotorMode = JointMotorMode::Off;
                c.m_HingeMotorTargetVelocityDeg = 0.0f;
                c.m_HingeMotorTargetAngleDeg = 0.0f;
                c.m_HingeMaxMotorTorque = 0.0f;
                c.m_HingeMaxFrictionTorque = 0.0f;
                c.m_SliderMotorMode = JointMotorMode::Off;
                c.m_SliderMotorTargetVelocity = 0.0f;
                c.m_SliderMotorTargetPosition = 0.0f;
                c.m_SliderMaxMotorForce = 0.0f;
                c.m_SliderMaxFrictionForce = 0.0f;
            }
            else
            {
                ar << c.m_HingeMotorMode << c.m_HingeMotorTargetVelocityDeg << c.m_HingeMotorTargetAngleDeg
                   << c.m_HingeMaxMotorTorque << c.m_HingeMaxFrictionTorque;
                ar << c.m_SliderMotorMode << c.m_SliderMotorTargetVelocity << c.m_SliderMotorTargetPosition
                   << c.m_SliderMaxMotorForce << c.m_SliderMaxFrictionForce;
            }

            // Sanitize untrusted motor data. Mode is an int enum on disk; clamp an
            // out-of-range value to Off. Max torque/force/friction are magnitudes
            // (>= 0, 0 = no authority/friction); targets are signed (finite only).
            const auto clampMode = [](JointMotorMode& m)
            {
                if (const auto v = static_cast<int>(m); v < 0 || v > static_cast<int>(JointMotorMode::Position))
                    m = JointMotorMode::Off;
            };
            const auto clampMagnitude = [](f32& v)
            {
                if (!std::isfinite(v) || v < 0.0f)
                    v = 0.0f;
                v = std::min(v, 1.0e9f);
            };
            const auto clampTarget = [](f32& v, f32 lo, f32 hi)
            {
                if (!std::isfinite(v))
                    v = 0.0f;
                v = std::clamp(v, lo, hi);
            };
            clampMode(c.m_HingeMotorMode);
            clampTarget(c.m_HingeMotorTargetVelocityDeg, -1.0e9f, 1.0e9f);
            clampTarget(c.m_HingeMotorTargetAngleDeg, -360.0f, 360.0f);
            clampMagnitude(c.m_HingeMaxMotorTorque);
            clampMagnitude(c.m_HingeMaxFrictionTorque);
            clampMode(c.m_SliderMotorMode);
            clampTarget(c.m_SliderMotorTargetVelocity, -1.0e9f, 1.0e9f);
            clampTarget(c.m_SliderMotorTargetPosition, -10000.0f, 10000.0f);
            clampMagnitude(c.m_SliderMaxMotorForce);
            clampMagnitude(c.m_SliderMaxFrictionForce);

            // Limit-spring tail. Archives written before soft limits existed end
            // after the motor block, so default to "hard limits" (frequency 0).
            if (ar.AtEnd())
            {
                c.m_HingeLimitSpringFrequency = 0.0f;
                c.m_HingeLimitSpringDamping = 0.0f;
                c.m_SliderLimitSpringFrequency = 0.0f;
                c.m_SliderLimitSpringDamping = 0.0f;
            }
            else
            {
                ar << c.m_HingeLimitSpringFrequency << c.m_HingeLimitSpringDamping;
                ar << c.m_SliderLimitSpringFrequency << c.m_SliderLimitSpringDamping;
            }

            // Frequency (Hz) and damping ratio are magnitudes; 0 = hard limit /
            // no damping.
            clampMagnitude(c.m_HingeLimitSpringFrequency);
            clampMagnitude(c.m_HingeLimitSpringDamping);
            clampMagnitude(c.m_SliderLimitSpringFrequency);
            clampMagnitude(c.m_SliderLimitSpringDamping);

            // SwingTwist + SixDOF tail. Archives written before these joint types
            // existed end after the limit-spring block, so default to the field
            // defaults (a 45° ragdoll cone / twist and an all-Locked rigid SixDOF).
            if (ar.AtEnd())
            {
                c.m_SwingNormalHalfAngleDeg = 45.0f;
                c.m_SwingPlaneHalfAngleDeg = 45.0f;
                c.m_TwistMinAngleDeg = -45.0f;
                c.m_TwistMaxAngleDeg = 45.0f;
                c.m_SixDOFTransXMode = JointAxisMode::Locked;
                c.m_SixDOFTransYMode = JointAxisMode::Locked;
                c.m_SixDOFTransZMode = JointAxisMode::Locked;
                c.m_SixDOFRotXMode = JointAxisMode::Locked;
                c.m_SixDOFRotYMode = JointAxisMode::Locked;
                c.m_SixDOFRotZMode = JointAxisMode::Locked;
                c.m_SixDOFTranslationMin = glm::vec3(-0.5f);
                c.m_SixDOFTranslationMax = glm::vec3(0.5f);
                c.m_SixDOFRotationMinDeg = glm::vec3(-45.0f);
                c.m_SixDOFRotationMaxDeg = glm::vec3(45.0f);
            }
            else
            {
                ar << c.m_SwingNormalHalfAngleDeg << c.m_SwingPlaneHalfAngleDeg
                   << c.m_TwistMinAngleDeg << c.m_TwistMaxAngleDeg;
                ar << c.m_SixDOFTransXMode << c.m_SixDOFTransYMode << c.m_SixDOFTransZMode
                   << c.m_SixDOFRotXMode << c.m_SixDOFRotYMode << c.m_SixDOFRotZMode;
                ar << c.m_SixDOFTranslationMin << c.m_SixDOFTranslationMax
                   << c.m_SixDOFRotationMinDeg << c.m_SixDOFRotationMaxDeg;
            }

            // Sanitize untrusted SwingTwist/SixDOF data. Modes are int enums on
            // disk (clamp out-of-range to Locked); swing/twist angles and the
            // SixDOF limits are clamped to the same ranges the scene serializer
            // enforces so values round-trip identically through both paths.
            const auto clampAxisMode = [](JointAxisMode& m)
            {
                if (const auto v = static_cast<int>(m); v < 0 || v > static_cast<int>(JointAxisMode::Free))
                    m = JointAxisMode::Locked;
            };
            const auto clampAngle = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                    v = fallback;
                v = std::clamp(v, lo, hi);
            };
            const auto clampVec3 = [](glm::vec3& v, f32 lo, f32 hi, f32 fallback)
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (!std::isfinite(v[i]))
                        v[i] = fallback;
                    v[i] = std::clamp(v[i], lo, hi);
                }
            };
            clampAngle(c.m_SwingNormalHalfAngleDeg, 0.0f, 180.0f, 45.0f);
            clampAngle(c.m_SwingPlaneHalfAngleDeg, 0.0f, 180.0f, 45.0f);
            clampAngle(c.m_TwistMinAngleDeg, -180.0f, 180.0f, -45.0f);
            clampAngle(c.m_TwistMaxAngleDeg, -180.0f, 180.0f, 45.0f);
            clampAxisMode(c.m_SixDOFTransXMode);
            clampAxisMode(c.m_SixDOFTransYMode);
            clampAxisMode(c.m_SixDOFTransZMode);
            clampAxisMode(c.m_SixDOFRotXMode);
            clampAxisMode(c.m_SixDOFRotYMode);
            clampAxisMode(c.m_SixDOFRotZMode);
            clampVec3(c.m_SixDOFTranslationMin, -10000.0f, 10000.0f, -0.5f);
            clampVec3(c.m_SixDOFTranslationMax, -10000.0f, 10000.0f, 0.5f);
            clampVec3(c.m_SixDOFRotationMinDeg, -180.0f, 180.0f, -45.0f);
            clampVec3(c.m_SixDOFRotationMaxDeg, -180.0f, 180.0f, 45.0f);

            // CollideConnected tail (issue #308 item 1). Archives written before
            // this flag existed end after the SwingTwist/SixDOF block, so default
            // to true — the long-standing behavior where jointed bodies collide.
            // A bool has no non-finite states, so no sanitization is needed.
            if (ar.AtEnd())
                c.m_CollideConnected = true;
            else
                ar << c.m_CollideConnected;

            // Pulley + Gear/RackAndPinion tail (issue #308 item 4). Archives
            // written before these constraint types existed end after the
            // CollideConnected flag, so default to the field defaults (a 1:1
            // rope that can contract but not extend, and a +Y connected axis).
            if (ar.AtEnd())
            {
                c.m_PulleyFixedPointA = glm::vec3(0.0f);
                c.m_PulleyFixedPointB = glm::vec3(0.0f);
                c.m_PulleyRatio = 1.0f;
                c.m_PulleyMinLength = 0.0f;
                c.m_PulleyMaxLength = -1.0f;
                c.m_ConnectedAxis = glm::vec3(0.0f, 1.0f, 0.0f);
                c.m_GearRatio = 1.0f;
            }
            else
            {
                ar << c.m_PulleyFixedPointA << c.m_PulleyFixedPointB
                   << c.m_PulleyRatio << c.m_PulleyMinLength << c.m_PulleyMaxLength;
                ar << c.m_ConnectedAxis << c.m_GearRatio;
            }

            // Sanitize untrusted on-disk values. Fixed points / connected axis are
            // world-space vec3 (reject non-finite, clamp absurd magnitudes). The
            // pulley ratio is a length multiplier → non-negative; the gear ratio is
            // signed (reversed coupling). Pulley lengths keep -1 as the "auto
            // length" sentinel. (clampAngle here is just finite+clamp+fallback.)
            clampVec3(c.m_PulleyFixedPointA, -1.0e6f, 1.0e6f, 0.0f);
            clampVec3(c.m_PulleyFixedPointB, -1.0e6f, 1.0e6f, 0.0f);
            clampVec3(c.m_ConnectedAxis, -1.0e6f, 1.0e6f, 0.0f);
            clampAngle(c.m_PulleyRatio, 0.0f, 1.0e9f, 1.0f);
            clampAngle(c.m_PulleyMinLength, -1.0f, 1.0e9f, 0.0f);
            clampAngle(c.m_PulleyMaxLength, -1.0f, 1.0e9f, -1.0f);
            clampAngle(c.m_GearRatio, -1.0e9f, 1.0e9f, 1.0f);

            // Path joint tail (issue #308). Archives written before the Path
            // constraint existed end after the Gear ratio, so default to "no
            // path" (empty points, motor off, hard Free rotation).
            if (ar.AtEnd())
            {
                c.m_PathPoints.clear();
                c.m_PathIsLooping = false;
                c.m_PathRotationMode = JointPathRotationMode::Free;
                c.m_PathMotorMode = JointMotorMode::Off;
                c.m_PathMotorTargetVelocity = 0.0f;
                c.m_PathMotorTargetFraction = 0.0f;
                c.m_PathMaxMotorForce = 0.0f;
                c.m_PathMaxFrictionForce = 0.0f;
            }
            else
            {
                ar << c.m_PathPoints;
                ar << c.m_PathIsLooping;
                ar << c.m_PathRotationMode;
                ar << c.m_PathMotorMode;
                ar << c.m_PathMotorTargetVelocity << c.m_PathMotorTargetFraction
                   << c.m_PathMaxMotorForce << c.m_PathMaxFrictionForce;
            }

            // Sanitize untrusted path data: drop non-finite control points; clamp
            // the rotation/motor modes to valid enum ranges; target velocity is
            // signed, target fraction non-negative, max force/friction magnitudes.
            std::erase_if(c.m_PathPoints, [](const glm::vec3& p)
                          { return !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z); });
            if (const auto v = static_cast<int>(c.m_PathRotationMode); v < 0 || v > static_cast<int>(JointPathRotationMode::FullyConstrained))
                c.m_PathRotationMode = JointPathRotationMode::Free;
            clampMode(c.m_PathMotorMode);
            clampTarget(c.m_PathMotorTargetVelocity, -1.0e9f, 1.0e9f);
            clampTarget(c.m_PathMotorTargetFraction, 0.0f, 1.0e9f);
            clampMagnitude(c.m_PathMaxMotorForce);
            clampMagnitude(c.m_PathMaxFrictionForce);
        }
        else
        {
            ar << c.m_BreakForce << c.m_BreakTorque;
            ar << c.m_HingeMotorMode << c.m_HingeMotorTargetVelocityDeg << c.m_HingeMotorTargetAngleDeg
               << c.m_HingeMaxMotorTorque << c.m_HingeMaxFrictionTorque;
            ar << c.m_SliderMotorMode << c.m_SliderMotorTargetVelocity << c.m_SliderMotorTargetPosition
               << c.m_SliderMaxMotorForce << c.m_SliderMaxFrictionForce;
            ar << c.m_HingeLimitSpringFrequency << c.m_HingeLimitSpringDamping;
            ar << c.m_SliderLimitSpringFrequency << c.m_SliderLimitSpringDamping;
            ar << c.m_SwingNormalHalfAngleDeg << c.m_SwingPlaneHalfAngleDeg
               << c.m_TwistMinAngleDeg << c.m_TwistMaxAngleDeg;
            ar << c.m_SixDOFTransXMode << c.m_SixDOFTransYMode << c.m_SixDOFTransZMode
               << c.m_SixDOFRotXMode << c.m_SixDOFRotYMode << c.m_SixDOFRotZMode;
            ar << c.m_SixDOFTranslationMin << c.m_SixDOFTranslationMax
               << c.m_SixDOFRotationMinDeg << c.m_SixDOFRotationMaxDeg;
            ar << c.m_CollideConnected;
            ar << c.m_PulleyFixedPointA << c.m_PulleyFixedPointB
               << c.m_PulleyRatio << c.m_PulleyMinLength << c.m_PulleyMaxLength;
            ar << c.m_ConnectedAxis << c.m_GearRatio;
            ar << c.m_PathPoints;
            ar << c.m_PathIsLooping;
            ar << c.m_PathRotationMode;
            ar << c.m_PathMotorMode;
            ar << c.m_PathMotorTargetVelocity << c.m_PathMotorTargetFraction
               << c.m_PathMaxMotorForce << c.m_PathMaxFrictionForce;
        }
        // m_RuntimeConstraintToken is a runtime Jolt handle — not serialized.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, VehicleComponent& c)
    {
        ar << c.m_HalfTrackWidth << c.m_FrontAxleOffset << c.m_RearAxleOffset << c.m_WheelAttachmentHeight;
        ar << c.m_WheelRadius << c.m_WheelWidth;
        ar << c.m_SuspensionMinLength << c.m_SuspensionMaxLength << c.m_SuspensionFrequency << c.m_SuspensionDamping;
        ar << c.m_MaxEngineTorque << c.m_MaxSteerAngleDeg << c.m_MaxBrakeTorque;
        ar << c.m_ThrottleInput << c.m_SteerInput << c.m_BrakeInput;

        // Sanitize untrusted on-disk values (mirrors SceneSerializer): dimensions
        // are strictly positive, the attachment height is signed, damping is a
        // [0,1] ratio, and the live inputs clamp to their driver-input ranges.
        // JoltScene::CreateVehicle re-sanitizes before handing values to Jolt.
        if (ar.IsLoading())
        {
            const auto positive = [](f32& v, f32 fallback)
            {
                if (!std::isfinite(v) || v <= 0.0f)
                    v = fallback;
            };
            positive(c.m_HalfTrackWidth, 0.9f);
            positive(c.m_FrontAxleOffset, 1.25f);
            positive(c.m_RearAxleOffset, 1.25f);
            if (!std::isfinite(c.m_WheelAttachmentHeight))
                c.m_WheelAttachmentHeight = -0.4f;
            positive(c.m_WheelRadius, 0.35f);
            positive(c.m_WheelWidth, 0.25f);
            if (!std::isfinite(c.m_SuspensionMinLength) || c.m_SuspensionMinLength < 0.0f)
                c.m_SuspensionMinLength = 0.3f;
            if (!std::isfinite(c.m_SuspensionMaxLength) || c.m_SuspensionMaxLength < 0.0f)
                c.m_SuspensionMaxLength = 0.5f;
            positive(c.m_SuspensionFrequency, 1.5f);
            c.m_SuspensionDamping = std::isfinite(c.m_SuspensionDamping) ? std::clamp(c.m_SuspensionDamping, 0.0f, 1.0f) : 0.5f;
            if (!std::isfinite(c.m_MaxEngineTorque) || c.m_MaxEngineTorque < 0.0f)
                c.m_MaxEngineTorque = 500.0f;
            c.m_MaxSteerAngleDeg = std::isfinite(c.m_MaxSteerAngleDeg) ? std::clamp(c.m_MaxSteerAngleDeg, 0.0f, 180.0f) : 30.0f;
            if (!std::isfinite(c.m_MaxBrakeTorque) || c.m_MaxBrakeTorque < 0.0f)
                c.m_MaxBrakeTorque = 1500.0f;
            c.m_ThrottleInput = std::isfinite(c.m_ThrottleInput) ? std::clamp(c.m_ThrottleInput, -1.0f, 1.0f) : 0.0f;
            c.m_SteerInput = std::isfinite(c.m_SteerInput) ? std::clamp(c.m_SteerInput, -1.0f, 1.0f) : 0.0f;
            c.m_BrakeInput = std::isfinite(c.m_BrakeInput) ? std::clamp(c.m_BrakeInput, 0.0f, 1.0f) : 0.0f;
        }
        // m_RuntimeVehicleToken is a runtime Jolt handle — not serialized.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, RagdollComponent& c)
    {
        // m_Skeleton is a runtime Ref (resolved from m_SkeletonEntity's
        // SkeletonComponent at physics start) and m_RuntimeRagdollToken is a
        // runtime handle — neither is serialized.
        ar << c.m_SkeletonEntity << c.m_Enabled;
        ar << c.m_BoneMass << c.m_BoneRadius << c.m_SwingLimitDeg << c.m_TwistLimitDeg;

        // Sanitize untrusted on-disk values (mirrors SceneSerializer); mass/radius
        // are strictly positive, swing/twist half-angles clamp to [0, 180] degrees.
        // JoltScene::CreateRagdoll re-sanitizes before handing values to Jolt.
        if (ar.IsLoading())
        {
            if (!std::isfinite(c.m_BoneMass) || c.m_BoneMass <= 0.0f)
                c.m_BoneMass = 1.0f;
            c.m_BoneMass = std::min(c.m_BoneMass, 1.0e6f);
            if (!std::isfinite(c.m_BoneRadius) || c.m_BoneRadius <= 0.0f)
                c.m_BoneRadius = 0.05f;
            c.m_BoneRadius = std::min(c.m_BoneRadius, 1.0e3f);
            c.m_SwingLimitDeg = std::isfinite(c.m_SwingLimitDeg) ? std::clamp(c.m_SwingLimitDeg, 0.0f, 180.0f) : 45.0f;
            c.m_TwistLimitDeg = std::isfinite(c.m_TwistLimitDeg) ? std::clamp(c.m_TwistLimitDeg, 0.0f, 180.0f) : 45.0f;
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ClothComponent& c)
    {
        // All fields are authored data — the Jolt soft body and render mesh are runtime
        // state kept in JoltScene / Scene (keyed by entity UUID), never serialized here.
        ar << c.m_Columns << c.m_Rows;
        ar << c.m_Width << c.m_Height << c.m_Mass;
        ar << c.m_Compliance << c.m_BendCompliance << c.m_LinearDamping << c.m_Pressure;
        ar << c.m_Iterations;
        ar << c.m_Attachment;
        ar << c.m_Enabled;

        // ── Format v7: wind-coupling influence scalar (issue #460) ──
        // Appended at the end when kSaveGameFormatVersion was bumped 6→7. A save written
        // before v7 has none of these bytes, so loading one leaves m_WindInfluence at its
        // constructor default (1.0 — full wind).
        if (HasFieldsSince(ar, 7))
        {
            ar << c.m_WindInfluence;
        }

        // ── Format v8: skeleton attachment (issue #460 cape slice) ──
        // Appended when kSaveGameFormatVersion was bumped 7→8. A save written before v8
        // has none of these bytes, so loading one leaves m_AttachmentEntity 0 (no
        // skeleton attachment) and m_AttachmentBone empty.
        if (HasFieldsSince(ar, 8))
        {
            ar << c.m_AttachmentEntity << c.m_AttachmentBone;
        }

        // Sanitize untrusted on-disk values (mirrors SceneSerializer / the clamps in
        // JoltShapes::CreateClothSharedSettings) so a corrupt archive can't blow up the
        // particle count or feed NaNs into Jolt.
        if (ar.IsLoading())
        {
            c.m_Columns = std::clamp(c.m_Columns, 2u, 128u);
            c.m_Rows = std::clamp(c.m_Rows, 2u, 128u);
            c.m_Iterations = std::clamp(c.m_Iterations, 1u, 32u);
            if (!std::isfinite(c.m_Width) || c.m_Width <= 0.0f)
                c.m_Width = 2.0f;
            if (!std::isfinite(c.m_Height) || c.m_Height <= 0.0f)
                c.m_Height = 2.0f;
            if (!std::isfinite(c.m_Mass) || c.m_Mass <= 0.0f)
                c.m_Mass = 1.0f;
            c.m_Compliance = std::isfinite(c.m_Compliance) ? std::max(0.0f, c.m_Compliance) : 0.0f;
            c.m_BendCompliance = std::isfinite(c.m_BendCompliance) ? std::max(0.0f, c.m_BendCompliance) : 0.001f;
            c.m_LinearDamping = std::isfinite(c.m_LinearDamping) ? std::max(0.0f, c.m_LinearDamping) : 0.1f;
            c.m_Pressure = std::isfinite(c.m_Pressure) ? std::max(0.0f, c.m_Pressure) : 0.0f;
            if (c.m_Attachment < ClothAttachment::None || c.m_Attachment > ClothAttachment::LeftEdge)
                c.m_Attachment = ClothAttachment::TopEdge;
            c.m_WindInfluence = std::isfinite(c.m_WindInfluence) ? std::clamp(c.m_WindInfluence, 0.0f, 1.0f) : 1.0f;
        }
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
        SerializeAudioSourceConfig(ar, c.GetConfig());

        // SoundConfig (.olosoundc) preset link — appended after the config block, so
        // probe AtEnd() on load and default to "no preset" for legacy archives written
        // before this field existed (the per-component buffer ends after the config).
        AssetHandle soundConfigHandle = c.GetSoundConfigHandle();
        if (ar.IsLoading() && ar.AtEnd())
        {
            soundConfigHandle = 0;
        }
        else
        {
            ar << soundConfigHandle;
        }
        c.SetSoundConfigHandle(soundConfigHandle);
        // Ref<AudioSource> is runtime — not serialized
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, AudioListenerComponent& c)
    {
        ar << c.Active;
        SerializeAudioListenerConfig(ar, c.Config);
    }

    // Authored fields only — the runtime VideoPlayer (decode thread + GPU texture) is
    // transient and rebuilt by VideoSystem on the next runtime tick.
    void SaveGameComponentSerializer::Serialize(FArchive& ar, VideoOverlayComponent& c)
    {
        ar << c.VideoPath << c.PlayOnStart << c.SkipOnInput << c.Looping << c.Volume;
        // Sanitise on load only (never mutate the live component during a save) so a corrupt
        // save can't feed NaN/out-of-range to SetVolume each frame.
        if (ar.IsLoading())
            c.Volume = std::isfinite(c.Volume) ? std::clamp(c.Volume, 0.0f, 1.0f) : 1.0f;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, VideoSurfaceComponent& c)
    {
        ar << c.VideoPath << c.AutoPlay << c.Looping << c.Volume;
        if (ar.IsLoading())
            c.Volume = std::isfinite(c.Volume) ? std::clamp(c.Volume, 0.0f, 1.0f) : 0.5f;
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

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SphereAreaLightComponent& c)
    {
        ar << c.m_Color << c.m_Intensity << c.m_Radius << c.m_Range << c.m_CastShadows;

        if (ar.IsLoading())
        {
            // Sanitize untrusted on-disk floats so the renderer never sees
            // NaN / inf / negative magnitudes (which would poison the UBO and
            // the BRDF). Mirrors the AudioSourceConfig pattern further up in
            // this file.
            for (int i = 0; i < 3; ++i)
            {
                if (!std::isfinite(c.m_Color[i]) || c.m_Color[i] < 0.0f)
                    c.m_Color[i] = 1.0f;
            }
            if (!std::isfinite(c.m_Intensity) || c.m_Intensity < 0.0f)
                c.m_Intensity = 1.0f;
            if (!std::isfinite(c.m_Radius) || c.m_Radius < 0.0f)
                c.m_Radius = 0.5f;
            if (!std::isfinite(c.m_Range) || c.m_Range < 0.0f)
                c.m_Range = 10.0f;
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ProceduralSkyComponent& c)
    {
        ar << c.m_SunDirection << c.m_Turbidity << c.m_Exposure;
        ar << c.m_SunIntensity << c.m_SunDiskSize << c.m_ShowSunDisk;
        // v10 retired m_LinkSunToDirectionalLight (issue #633 — the
        // TimeOfDayComponent is now the sun driver; v9 was taken by #632's
        // DDGI fields in the same release window). Saves v9 and older carry
        // the bool at this stream position; read and discard it.
        if (ar.IsLoading() && ar.GetArchiveVersion() < 10)
        {
            bool legacyLinkSun = false;
            ar << legacyLinkSun;
        }
        ar << c.m_EnableSkybox << c.m_EnableIBL << c.m_IBLIntensity;
        ar << c.m_CubemapResolution;

        if (ar.IsLoading())
        {
            // Sanitise untrusted on-disk data. Save files are a system
            // boundary: the renderer UBO must never see NaN/inf (poisons
            // tonemapping and every PBR draw via IBL), and an extremely large
            // *finite* value would blow the sky / ambient out to white. So
            // clamp each field into its sane authoring range, not just check
            // finiteness. NaN/inf fall back to the component default.
            for (int i = 0; i < 3; ++i)
            {
                if (!std::isfinite(c.m_SunDirection[i]))
                    c.m_SunDirection = glm::vec3(0.3f, 0.7f, 0.4f);
            }
            c.m_Turbidity = std::isfinite(c.m_Turbidity) ? std::clamp(c.m_Turbidity, 1.7f, 10.0f) : 2.5f;
            c.m_Exposure = std::isfinite(c.m_Exposure) ? std::clamp(c.m_Exposure, 0.0f, 10.0f) : 0.1f;
            c.m_SunIntensity = std::isfinite(c.m_SunIntensity) ? std::clamp(c.m_SunIntensity, 0.0f, 100.0f) : 1.0f;
            c.m_SunDiskSize = std::isfinite(c.m_SunDiskSize) ? std::clamp(c.m_SunDiskSize, 0.01f, 10.0f) : 1.0f;
            c.m_IBLIntensity = std::isfinite(c.m_IBLIntensity) ? std::clamp(c.m_IBLIntensity, 0.0f, 100.0f) : 1.0f;
            if (c.m_CubemapResolution < 8u || c.m_CubemapResolution > 4096u)
                c.m_CubemapResolution = 256u;
        }
        // Ref<EnvironmentMap> and bake hash are runtime — not serialised
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, StarNestSkyComponent& c)
    {
        ar << c.m_Offset << c.m_Rotation1 << c.m_Rotation2;
        ar << c.m_Formuparam << c.m_StepSize << c.m_Tile;
        ar << c.m_Brightness << c.m_DarkMatter << c.m_DistFading << c.m_Saturation;
        ar << c.m_Intensity << c.m_Iterations << c.m_VolSteps;
        ar << c.m_EnableSkybox << c.m_EnableIBL << c.m_IBLIntensity;
        ar << c.m_CubemapResolution;

        if (ar.IsLoading())
        {
            // Sanitise untrusted on-disk data — same boundary policy as the
            // Preetham sky above: a NaN/inf would poison the baked cubemap and
            // every IBL-lit draw, and a wild finite value would blow the nebula
            // (or the loop counts) out of range. Clamp into the authoring ranges
            // ComputeUBO expects; NaN/inf fall back to the component default.
            for (int i = 0; i < 3; ++i)
            {
                if (!std::isfinite(c.m_Offset[i]))
                    c.m_Offset = glm::vec3(1.0f, 0.5f, 0.5f);
            }
            c.m_Rotation1 = std::isfinite(c.m_Rotation1) ? c.m_Rotation1 : 0.5f;
            c.m_Rotation2 = std::isfinite(c.m_Rotation2) ? c.m_Rotation2 : 0.8f;
            c.m_Formuparam = std::isfinite(c.m_Formuparam) ? std::clamp(c.m_Formuparam, 0.0f, 2.0f) : 0.53f;
            c.m_StepSize = std::isfinite(c.m_StepSize) ? std::clamp(c.m_StepSize, 1e-3f, 1.0f) : 0.1f;
            c.m_Tile = std::isfinite(c.m_Tile) ? std::clamp(c.m_Tile, 1e-2f, 4.0f) : 0.85f;
            c.m_Brightness = std::isfinite(c.m_Brightness) ? std::clamp(c.m_Brightness, 0.0f, 1.0f) : 0.0015f;
            c.m_DarkMatter = std::isfinite(c.m_DarkMatter) ? std::clamp(c.m_DarkMatter, 0.0f, 5.0f) : 0.3f;
            c.m_DistFading = std::isfinite(c.m_DistFading) ? std::clamp(c.m_DistFading, 0.0f, 1.0f) : 0.73f;
            c.m_Saturation = std::isfinite(c.m_Saturation) ? std::clamp(c.m_Saturation, 0.0f, 1.0f) : 0.85f;
            c.m_Intensity = std::isfinite(c.m_Intensity) ? std::clamp(c.m_Intensity, 0.0f, 100.0f) : 1.0f;
            c.m_IBLIntensity = std::isfinite(c.m_IBLIntensity) ? std::clamp(c.m_IBLIntensity, 0.0f, 100.0f) : 1.0f;
            c.m_Iterations = std::clamp(c.m_Iterations, 1, kStarNestMaxIterations);
            c.m_VolSteps = std::clamp(c.m_VolSteps, 1, kStarNestMaxVolSteps);
            if (c.m_CubemapResolution < 8u || c.m_CubemapResolution > 4096u)
                c.m_CubemapResolution = 256u;
        }
        // Ref<EnvironmentMap> and bake hash are runtime — not serialised
    }

    namespace
    {
        // Streams one WeatherPreset (nested struct of WeatherStateComponent).
        // Field order is the save-format contract — append-only.
        void SerializeWeatherPreset(FArchive& ar, WeatherPreset& p)
        {
            ar << p.CloudCoverage << p.CloudDensity << p.CloudTypeBlend << p.CloudWetness;
            ar << p.FogEnabled << p.FogDensity << p.FogColor << p.FogHeightFalloff << p.FogMaxOpacity;
            ar << p.WindSpeed << p.WindGustStrength << p.WindTurbulence;
            ar << p.PrecipitationEnabled << p.PrecipitationKind << p.PrecipitationIntensity;
            ar << p.SnowAccumulationEnabled << p.SnowAccumulationRate;
            ar << p.SunDimming << p.WetnessTarget;

            if (ar.IsLoading())
            {
                // Saves are a trust boundary: clamp into authoring ranges,
                // NaN/inf fall back to struct defaults (see the sky
                // serializers above for the policy rationale).
                const WeatherPreset defaults;
                auto sanitize01 = [](f32& v, f32 fallback)
                { v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback; };
                sanitize01(p.CloudCoverage, defaults.CloudCoverage);
                sanitize01(p.CloudDensity, defaults.CloudDensity);
                sanitize01(p.CloudTypeBlend, defaults.CloudTypeBlend);
                sanitize01(p.CloudWetness, defaults.CloudWetness);
                p.FogDensity = std::isfinite(p.FogDensity) ? std::clamp(p.FogDensity, 0.0f, 10.0f) : defaults.FogDensity;
                for (int i = 0; i < 3; ++i)
                {
                    if (!std::isfinite(p.FogColor[i]))
                        p.FogColor = defaults.FogColor;
                }
                p.FogHeightFalloff = std::isfinite(p.FogHeightFalloff) ? std::clamp(p.FogHeightFalloff, 0.0f, 10.0f) : defaults.FogHeightFalloff;
                sanitize01(p.FogMaxOpacity, defaults.FogMaxOpacity);
                p.WindSpeed = std::isfinite(p.WindSpeed) ? std::clamp(p.WindSpeed, 0.0f, 200.0f) : defaults.WindSpeed;
                sanitize01(p.WindGustStrength, defaults.WindGustStrength);
                p.WindTurbulence = std::isfinite(p.WindTurbulence) ? std::clamp(p.WindTurbulence, 0.0f, 10.0f) : defaults.WindTurbulence;
                if (static_cast<i32>(p.PrecipitationKind) < 0 || static_cast<i32>(p.PrecipitationKind) > 3)
                    p.PrecipitationKind = defaults.PrecipitationKind;
                sanitize01(p.PrecipitationIntensity, defaults.PrecipitationIntensity);
                p.SnowAccumulationRate = std::isfinite(p.SnowAccumulationRate) ? std::clamp(p.SnowAccumulationRate, 0.0f, 10.0f) : defaults.SnowAccumulationRate;
                sanitize01(p.SunDimming, defaults.SunDimming);
                sanitize01(p.WetnessTarget, defaults.WetnessTarget);
            }
        }
    } // namespace

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TimeOfDayComponent& c)
    {
        ar << c.m_Enabled << c.m_TimeOfDayHours << c.m_DayOfYear << c.m_LatitudeDegrees;
        ar << c.m_DayLengthMinutes << c.m_TimeScale << c.m_Paused << c.m_AdvanceInEditMode;
        ar << c.m_NorthOffsetDegrees;
        ar << c.m_SunIntensityMax << c.m_MoonIntensityMax << c.m_MoonPhase;
        ar << c.m_SkyExposureDay << c.m_SkyExposureNight << c.m_StarIntensity << c.m_MoonDiskSize;
        ar << c.m_RebakeQuantumGameMinutes;

        if (ar.IsLoading())
        {
            // Sanitise untrusted on-disk data into the authoring ranges the
            // ephemeris/lighting drive expects; NaN/inf fall back to defaults.
            c.m_TimeOfDayHours = std::isfinite(c.m_TimeOfDayHours) ? std::clamp(c.m_TimeOfDayHours, 0.0f, 24.0f) : 10.0f;
            c.m_DayOfYear = std::clamp(c.m_DayOfYear, 1, 365);
            c.m_LatitudeDegrees = std::isfinite(c.m_LatitudeDegrees) ? std::clamp(c.m_LatitudeDegrees, -90.0f, 90.0f) : 48.0f;
            c.m_DayLengthMinutes = std::isfinite(c.m_DayLengthMinutes) ? std::clamp(c.m_DayLengthMinutes, 0.1f, 10080.0f) : 24.0f;
            c.m_TimeScale = std::isfinite(c.m_TimeScale) ? std::clamp(c.m_TimeScale, 0.0f, 1000.0f) : 1.0f;
            c.m_NorthOffsetDegrees = std::isfinite(c.m_NorthOffsetDegrees) ? std::clamp(c.m_NorthOffsetDegrees, -360.0f, 360.0f) : 0.0f;
            c.m_SunIntensityMax = std::isfinite(c.m_SunIntensityMax) ? std::clamp(c.m_SunIntensityMax, 0.0f, 100.0f) : 3.0f;
            c.m_MoonIntensityMax = std::isfinite(c.m_MoonIntensityMax) ? std::clamp(c.m_MoonIntensityMax, 0.0f, 10.0f) : 0.12f;
            c.m_MoonPhase = std::isfinite(c.m_MoonPhase) ? std::clamp(c.m_MoonPhase, 0.0f, 1.0f) : 0.5f;
            c.m_SkyExposureDay = std::isfinite(c.m_SkyExposureDay) ? std::clamp(c.m_SkyExposureDay, 0.0f, 10.0f) : 0.1f;
            c.m_SkyExposureNight = std::isfinite(c.m_SkyExposureNight) ? std::clamp(c.m_SkyExposureNight, 0.0f, 10.0f) : 0.35f;
            c.m_StarIntensity = std::isfinite(c.m_StarIntensity) ? std::clamp(c.m_StarIntensity, 0.0f, 8.0f) : 1.0f;
            c.m_MoonDiskSize = std::isfinite(c.m_MoonDiskSize) ? std::clamp(c.m_MoonDiskSize, 0.1f, 10.0f) : 1.0f;
            c.m_RebakeQuantumGameMinutes = std::isfinite(c.m_RebakeQuantumGameMinutes) ? std::clamp(c.m_RebakeQuantumGameMinutes, 0.25f, 240.0f) : 5.0f;
        }
        // Derived outputs (m_SunDirection/m_MoonDirection/m_SunElevationDegrees/
        // m_IsNight) are per-tick runtime — recomputed by TimeOfDaySystem::Apply.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, WeatherStateComponent& c)
    {
        ar << c.m_Enabled << c.m_CurrentState << c.m_TargetState << c.m_TransitionDuration;
        ar << c.m_WetnessRiseRate << c.m_WetnessDryRate;
        SerializeWeatherPreset(ar, c.m_PresetClear);
        SerializeWeatherPreset(ar, c.m_PresetOvercast);
        SerializeWeatherPreset(ar, c.m_PresetRain);
        SerializeWeatherPreset(ar, c.m_PresetStorm);
        SerializeWeatherPreset(ar, c.m_PresetSnow);
        SerializeWeatherPreset(ar, c.m_PresetFogBank);
        // Persist the live transition + wetness so a mid-storm save resumes
        // mid-storm (unlike scene YAML, where these Skip fields reset).
        ar << c.m_TransitionProgress << c.m_Wetness;

        if (ar.IsLoading())
        {
            auto sanitizeState = [](WeatherStateId& s)
            {
                if (static_cast<i32>(s) < 0 || static_cast<i32>(s) > 5)
                    s = WeatherStateId::Clear;
            };
            sanitizeState(c.m_CurrentState);
            sanitizeState(c.m_TargetState);
            c.m_TransitionDuration = std::isfinite(c.m_TransitionDuration) ? std::clamp(c.m_TransitionDuration, 0.0f, 600.0f) : 10.0f;
            c.m_WetnessRiseRate = std::isfinite(c.m_WetnessRiseRate) ? std::clamp(c.m_WetnessRiseRate, 0.0f, 10.0f) : 0.15f;
            c.m_WetnessDryRate = std::isfinite(c.m_WetnessDryRate) ? std::clamp(c.m_WetnessDryRate, 0.0f, 10.0f) : 0.02f;
            c.m_TransitionProgress = std::isfinite(c.m_TransitionProgress) ? std::clamp(c.m_TransitionProgress, 0.0f, 1.0f) : 1.0f;
            c.m_Wetness = std::isfinite(c.m_Wetness) ? std::clamp(c.m_Wetness, 0.0f, 1.0f) : 0.0f;
        }
        // m_Blended is per-tick runtime — recomputed by WeatherSystem.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CloudscapeComponent& c)
    {
        ar << c.m_Enabled << c.m_LayerBottom << c.m_LayerTop;
        ar << c.m_Coverage << c.m_Density << c.m_TypeBlend << c.m_ErosionStrength;
        ar << c.m_WindAnimationScale << c.m_WeatherMapHandle << c.m_WeatherMapScaleKm;
        ar << c.m_MaxSteps << c.m_LightSteps;
        ar << c.m_SunLightScale << c.m_AmbientScale << c.m_MultiScatterStrength;
        ar << c.m_PhaseG << c.m_PowderStrength;
        ar << c.m_CastCloudShadows << c.m_ShadowStrength << c.m_ShadowMapWorldSize;
        ar << c.m_TemporalBlend << c.m_AffectIBL;

        if (ar.IsLoading())
        {
            c.m_LayerBottom = std::isfinite(c.m_LayerBottom) ? std::clamp(c.m_LayerBottom, 0.0f, 20000.0f) : 1500.0f;
            c.m_LayerTop = std::isfinite(c.m_LayerTop) ? std::clamp(c.m_LayerTop, 100.0f, 30000.0f) : 4000.0f;
            c.m_Coverage = std::isfinite(c.m_Coverage) ? std::clamp(c.m_Coverage, 0.0f, 1.0f) : 0.35f;
            c.m_Density = std::isfinite(c.m_Density) ? std::clamp(c.m_Density, 0.0f, 4.0f) : 1.0f;
            c.m_TypeBlend = std::isfinite(c.m_TypeBlend) ? std::clamp(c.m_TypeBlend, 0.0f, 1.0f) : 0.5f;
            c.m_ErosionStrength = std::isfinite(c.m_ErosionStrength) ? std::clamp(c.m_ErosionStrength, 0.0f, 1.0f) : 0.5f;
            c.m_WindAnimationScale = std::isfinite(c.m_WindAnimationScale) ? std::clamp(c.m_WindAnimationScale, 0.0f, 8.0f) : 1.0f;
            c.m_WeatherMapScaleKm = std::isfinite(c.m_WeatherMapScaleKm) ? std::clamp(c.m_WeatherMapScaleKm, 1.0f, 200.0f) : 12.0f;
            c.m_MaxSteps = std::clamp(c.m_MaxSteps, 16, 128);
            c.m_LightSteps = std::clamp(c.m_LightSteps, 2, 12);
            c.m_SunLightScale = std::isfinite(c.m_SunLightScale) ? std::clamp(c.m_SunLightScale, 0.0f, 10.0f) : 1.0f;
            c.m_AmbientScale = std::isfinite(c.m_AmbientScale) ? std::clamp(c.m_AmbientScale, 0.0f, 10.0f) : 1.0f;
            c.m_MultiScatterStrength = std::isfinite(c.m_MultiScatterStrength) ? std::clamp(c.m_MultiScatterStrength, 0.0f, 1.0f) : 0.5f;
            c.m_PhaseG = std::isfinite(c.m_PhaseG) ? std::clamp(c.m_PhaseG, 0.0f, 0.95f) : 0.6f;
            c.m_PowderStrength = std::isfinite(c.m_PowderStrength) ? std::clamp(c.m_PowderStrength, 0.0f, 2.0f) : 1.0f;
            c.m_ShadowStrength = std::isfinite(c.m_ShadowStrength) ? std::clamp(c.m_ShadowStrength, 0.0f, 1.0f) : 0.8f;
            c.m_ShadowMapWorldSize = std::isfinite(c.m_ShadowMapWorldSize) ? std::clamp(c.m_ShadowMapWorldSize, 500.0f, 50000.0f) : 8000.0f;
            c.m_TemporalBlend = std::isfinite(c.m_TemporalBlend) ? std::clamp(c.m_TemporalBlend, 0.0f, 0.98f) : 0.9f;
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, EnvironmentMapComponent& c)
    {
        ar << c.m_EnvironmentMapAsset << c.m_FilePath;
        ar << c.m_IsCubemapFolder << c.m_EnableSkybox;
        ar << c.m_Rotation << c.m_Exposure << c.m_BlurAmount;
        ar << c.m_EnableIBL << c.m_IBLIntensity << c.m_Tint;
        // m_UseSphericalHarmonics was added in the SH-IBL irradiance feature.
        // It is appended *after* m_Tint (which legacy archives ended with) so
        // older save files round-trip cleanly — we probe AtEnd() on load and
        // default to false when the field is absent. Mirrors the same pattern
        // used for DecalComponent::m_Transparent above.
        if (ar.IsLoading())
        {
            if (ar.AtEnd())
                c.m_UseSphericalHarmonics = false;
            else
                ar << c.m_UseSphericalHarmonics;
        }
        else
        {
            ar << c.m_UseSphericalHarmonics;
        }
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

        // ── Format v9: realtime DDGI fields (issue #632) ──
        // Appended at the end when kSaveGameFormatVersion was bumped 8→9. A save
        // written before v9 has none of these bytes, so loading one leaves every
        // field at its constructor default (Mode::Baked — the pre-#632 behavior).
        if (HasFieldsSince(ar, 9))
        {
            ar << c.m_Mode; // enum class : u8 — FArchive serializes the underlying type
            ar << c.m_RaysPerProbe << c.m_Hysteresis;
            ar << c.m_ProbeCaptureBudget << c.m_RelightBudget;
            ar << c.m_SelfShadowBias;
        }

        // Sanitize untrusted on-disk values (mirrors the SceneSerializer clamps).
        if (ar.IsLoading())
        {
            // Mode is unsigned (u8 underlying), so only the upper bound can be
            // out of range; an unknown mode falls back to Baked.
            if (c.m_Mode > LightProbeVolumeComponent::Mode::Hybrid)
                c.m_Mode = LightProbeVolumeComponent::Mode::Baked;
            c.m_RaysPerProbe = std::clamp(c.m_RaysPerProbe, 1, 4096);
            c.m_Hysteresis = std::isfinite(c.m_Hysteresis) ? std::clamp(c.m_Hysteresis, 0.0f, 0.98f) : 0.9f;
            c.m_ProbeCaptureBudget = std::clamp(c.m_ProbeCaptureBudget, 1, 64);
            c.m_RelightBudget = std::clamp(c.m_RelightBudget, 0, 1048576);
            c.m_SelfShadowBias = std::isfinite(c.m_SelfShadowBias) ? std::clamp(c.m_SelfShadowBias, 0.0f, 4.0f) : 0.3f;
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ReflectionProbeComponent& c)
    {
        ar << c.m_InfluenceRadius << c.m_BlendDistance;
        ar << c.m_Resolution << c.m_Intensity << c.m_Active;
        // Ref<EnvironmentMap> + m_NeedsBake are runtime — author rebakes on load
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
        if (ar.IsLoading())
        {
            if (c.m_Options.empty())
            {
                c.m_SelectedIndex = -1;
            }
            else if (c.m_SelectedIndex >= static_cast<i32>(c.m_Options.size()))
            {
                c.m_SelectedIndex = static_cast<i32>(c.m_Options.size()) - 1;
            }
            else
            {
                // No additional handling required.
            }
        }
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

        // ── Format v3: procedural height-field shaping + auto-material rules ──
        // Appended at the end when kSaveGameFormatVersion was bumped 2→3. A save
        // written before v3 has none of these bytes, so loading one leaves the
        // fields at their constructor defaults (empty m_LayerRules included).
        if (HasFieldsSince(ar, 3))
        {
            ar << c.m_HeightShaping.RidgeBlend << c.m_HeightShaping.WarpStrength << c.m_HeightShaping.WarpFrequency;
            ar << c.m_HeightShaping.TerraceSteps << c.m_HeightShaping.TerraceSharpness << c.m_HeightShaping.HeightExponent;
            ar << c.m_AutoMaterial << c.m_SplatmapGenResolution;

            u32 ruleCount = static_cast<u32>(c.m_LayerRules.size());
            ar << ruleCount;
            if (ar.IsLoading())
            {
                u32 clampedRules = std::min(ruleCount, 256u);
                c.m_LayerRules.resize(clampedRules);
                for (u32 i = 0; i < clampedRules; ++i)
                {
                    SerializeTerrainLayerRule(ar, c.m_LayerRules[i]);
                    if (ar.IsError())
                    {
                        return;
                    }
                }
                // Drain any excess entries to keep the stream aligned.
                for (u32 i = clampedRules; i < ruleCount; ++i)
                {
                    TerrainLayerRule discard{};
                    SerializeTerrainLayerRule(ar, discard);
                    if (ar.IsError())
                    {
                        return;
                    }
                }
            }
            else
            {
                for (u32 i = 0; i < ruleCount; ++i)
                {
                    SerializeTerrainLayerRule(ar, c.m_LayerRules[i]);
                }
            }
        }

        // ── Format v5: hydraulic-erosion generation post-pass iteration count ──
        // Appended at the end when kSaveGameFormatVersion was bumped 4→5.
        if (HasFieldsSince(ar, 5))
        {
            ar << c.m_ProceduralErosionIterations;
        }

        // ── Format v6: static height-field collision toggle (issue #428) ──
        // Appended at the end when kSaveGameFormatVersion was bumped 5→6.
        if (HasFieldsSince(ar, 6))
        {
            ar << c.m_CollisionEnabled;
        }

        if (ar.IsLoading())
        {
            // Sanitize untrusted on-disk values so corrupt save data can't poison
            // terrain generation (NaN/inf in the noise math, or a huge allocation).
            auto sanitize = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                    v = fallback;
                v = std::clamp(v, lo, hi);
            };
            sanitize(c.m_HeightShaping.RidgeBlend, 0.0f, 1.0f, 0.0f);
            sanitize(c.m_HeightShaping.WarpStrength, 0.0f, 4.0f, 0.0f);
            sanitize(c.m_HeightShaping.WarpFrequency, 0.0f, 64.0f, 2.0f);
            sanitize(c.m_HeightShaping.TerraceSharpness, 0.0f, 0.999f, 0.6f);
            sanitize(c.m_HeightShaping.HeightExponent, 0.05f, 16.0f, 1.0f);
            c.m_HeightShaping.TerraceSteps = std::min(c.m_HeightShaping.TerraceSteps, 256u);
            c.m_SplatmapGenResolution = std::clamp(c.m_SplatmapGenResolution, 16u, 4096u);
            c.m_ProceduralErosionIterations = std::clamp(c.m_ProceduralErosionIterations, 0, 64);
            for (TerrainLayerRule& r : c.m_LayerRules)
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
        ar << c.m_NormalMapScrollDir0 << c.m_NormalMapScrollDir1;
        ar << c.m_NormalMapScrollSpeed0 << c.m_NormalMapScrollSpeed1;
        ar << c.m_NormalMapTiling << c.m_NoiseIntensity;
        ar << c.m_NormalMap0 << c.m_NormalMap1 << c.m_NoiseTexture;
        ar << c.m_DepthSofteningDistance << c.m_RefractionDistortion << c.m_RefractionHeightFactor;
        ar << c.m_RefractionColor;
        ar << c.m_RefractionEnabled;
        ar << c.m_FoamTexture;
        ar << c.m_FoamHeightStart << c.m_FoamFadeDistance << c.m_FoamTiling << c.m_FoamBrightness;
        ar << c.m_FoamAngleExponent << c.m_ShorelineFoamPower;
        ar << c.m_SSSColor << c.m_SSSIntensity;
        ar << c.m_SSRMaxSteps << c.m_SSRStepSize << c.m_SSRMaxDistance << c.m_SSRThickness;
        ar << c.m_SSREnabled;
        ar << c.m_TessellationEnabled << c.m_TessellationFactor << c.m_TessMinDistance << c.m_TessMaxDistance;
        // m_UnderwaterFogColor / m_UnderwaterFogDensity / m_RenderFromBelow were
        // added with the §7.2 underwater work. Per-component payloads aren't
        // size-prefixed at this trailing point, so older archives end here —
        // probe AtEnd() and fall back to the component defaults rather than
        // reading past the end (same pattern as m_Transparent below).
        if (ar.IsLoading())
        {
            if (ar.AtEnd())
            {
                c.m_UnderwaterFogColor = glm::vec3(0.05f, 0.15f, 0.25f);
                c.m_UnderwaterFogDensity = 0.08f;
                c.m_RenderFromBelow = true;
            }
            else
            {
                ar << c.m_UnderwaterFogColor << c.m_UnderwaterFogDensity << c.m_RenderFromBelow;
            }
        }
        else
        {
            ar << c.m_UnderwaterFogColor << c.m_UnderwaterFogDensity << c.m_RenderFromBelow;
        }

        // Refraction distortion (§7.2) + caustics (§7.1) were appended after the
        // fog block. Same trailing-AtEnd() probe: an archive written before this
        // addition ends after m_RenderFromBelow, so fall back to the component
        // defaults rather than reading past the end of the component blob.
        auto loadUnderwaterFxDefaults = [](WaterComponent& w)
        {
            w.m_UnderwaterRefractionStrength = 0.006f;
            w.m_UnderwaterRefractionScale = 18.0f;
            w.m_UnderwaterRefractionSpeed = 1.2f;
            w.m_UnderwaterChromaticStrength = 0.4f;
            w.m_CausticsIntensity = 0.5f;
            w.m_CausticsScale = 0.35f;
            w.m_CausticsSpeed = 0.6f;
            w.m_CausticsMaxDepth = 25.0f;
            w.m_CausticsColor = glm::vec3(0.7f, 0.85f, 1.0f);
        };
        if (ar.IsLoading() && ar.AtEnd())
        {
            loadUnderwaterFxDefaults(c);
        }
        else
        {
            ar << c.m_UnderwaterRefractionStrength << c.m_UnderwaterRefractionScale
               << c.m_UnderwaterRefractionSpeed << c.m_UnderwaterChromaticStrength;
            ar << c.m_CausticsIntensity << c.m_CausticsScale << c.m_CausticsSpeed << c.m_CausticsMaxDepth;
            ar << c.m_CausticsColor;
        }

        // God rays (§3.3) were appended after the caustics block — a separate
        // trailing-AtEnd() probe so a save written before god rays existed (but
        // after caustics) still falls back to defaults instead of reading past the
        // end of the component blob.
        auto loadGodRayDefaults = [](WaterComponent& w)
        {
            w.m_GodRayIntensity = 0.5f;
            w.m_GodRayDecay = 0.97f;
            w.m_GodRayDensity = 0.85f;
            w.m_GodRayWeight = 1.0f;
            w.m_GodRayColor = glm::vec3(1.0f, 0.95f, 0.8f);
            w.m_GodRaySamples = 48u;
            w.m_GodRayDappleFloor = 0.35f;
            w.m_GodRaySunFalloff = 16.0f;
        };
        if (ar.IsLoading() && ar.AtEnd())
        {
            loadGodRayDefaults(c);
        }
        else
        {
            ar << c.m_GodRayIntensity << c.m_GodRayDecay << c.m_GodRayDensity << c.m_GodRayWeight;
            ar << c.m_GodRayColor;
            ar << c.m_GodRaySamples;
            ar << c.m_GodRayDappleFloor << c.m_GodRaySunFalloff;
        }

        // FFT ocean (WATER_FUTURE_IMPROVEMENTS.md §1) was appended last (after the
        // god-ray block). Same trailing-AtEnd() probe: archives written before this
        // addition end after god rays (or caustics), so fall back to defaults.
        auto loadFFTDefaults = [](WaterComponent& w)
        {
            w.m_UseFFT = false;
            w.m_FFTResolution = 128u;
            w.m_FFTPatchSize = 80.0f;
            w.m_FFTWindSpeed = 18.0f;
            w.m_FFTWindDirection = glm::vec2(1.0f, 0.0f);
            w.m_FFTAmplitude = 2.0f;
            w.m_FFTChoppiness = 1.2f;
            w.m_FFTHeightScale = 1.0f;
            w.m_FFTSeed = 1337u;
        };
        if (ar.IsLoading() && ar.AtEnd())
        {
            loadFFTDefaults(c);
        }
        else
        {
            ar << c.m_UseFFT << c.m_FFTResolution << c.m_FFTPatchSize << c.m_FFTWindSpeed;
            ar << c.m_FFTWindDirection << c.m_FFTAmplitude << c.m_FFTChoppiness << c.m_FFTHeightScale;
            ar << c.m_FFTSeed;
        }

        // GPU-compute FFT toggle (§1.2) appended after the FFT block — same
        // trailing-AtEnd() probe so archives written before it load fine.
        if (ar.IsLoading() && ar.AtEnd())
        {
            c.m_FFTUseGpuCompute = true;
        }
        else
        {
            ar << c.m_FFTUseGpuCompute;
        }

        // Spectrum selection (§1.4) appended after the GPU-compute toggle — same
        // trailing-AtEnd() probe so archives written before it fall back to the
        // Phillips default. The enum rides the archive as its underlying u32.
        if (ar.IsLoading() && ar.AtEnd())
        {
            c.m_FFTSpectrumType = Ocean::SpectrumType::Phillips;
            c.m_FFTJonswapGamma = 3.3f;
            c.m_FFTJonswapFetch = 100000.0f;
        }
        else
        {
            u32 spectrumType = static_cast<u32>(c.m_FFTSpectrumType);
            ar << spectrumType;
            if (ar.IsLoading())
                c.m_FFTSpectrumType = static_cast<Ocean::SpectrumType>(spectrumType);
            ar << c.m_FFTJonswapGamma << c.m_FFTJonswapFetch;
        }

        // Planar (mirror) reflections (Phase 7) appended after the spectrum block
        // — same trailing-AtEnd() probe so archives written before it fall back to
        // the component defaults (planar reflections off).
        if (ar.IsLoading() && ar.AtEnd())
        {
            c.m_PlanarReflectionsEnabled = false;
            c.m_PlanarReflectionIntensity = 1.0f;
            c.m_PlanarReflectionDistortion = 0.02f;
        }
        else
        {
            ar << c.m_PlanarReflectionsEnabled;
            ar << c.m_PlanarReflectionIntensity << c.m_PlanarReflectionDistortion;
        }

        if (ar.IsLoading())
        {
            auto sanitize = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                    return;
                }
                v = std::clamp(v, lo, hi);
            };

            sanitize(c.m_WorldSizeX, 0.1f, 10000.0f, 100.0f);
            sanitize(c.m_WorldSizeZ, 0.1f, 10000.0f, 100.0f);
            c.m_GridResolutionX = std::clamp(c.m_GridResolutionX, 1u, 1024u);
            c.m_GridResolutionZ = std::clamp(c.m_GridResolutionZ, 1u, 1024u);
            sanitize(c.m_WaveAmplitude, 0.0f, 100.0f, 0.5f);
            sanitize(c.m_WaveFrequency, 0.0f, 100.0f, 1.0f);
            sanitize(c.m_WaveSpeed, 0.0f, 100.0f, 1.0f);
            sanitize(c.m_WaveSteepness0, 0.0f, 1.0f, 0.5f);
            sanitize(c.m_Wavelength0, 0.1f, 500.0f, 10.0f);
            sanitize(c.m_WaveSteepness1, 0.0f, 1.0f, 0.3f);
            sanitize(c.m_Wavelength1, 0.1f, 500.0f, 15.0f);
            sanitize(c.m_Transparency, 0.0f, 1.0f, 0.6f);
            sanitize(c.m_Reflectivity, 0.0f, 1.0f, 0.5f);
            sanitize(c.m_FresnelPower, 0.1f, 20.0f, 5.0f);
            sanitize(c.m_SpecularIntensity, 0.0f, 10.0f, 1.0f);
            sanitize(c.m_NormalMapScrollSpeed0, 0.0f, 1.0f, 0.02f);
            sanitize(c.m_NormalMapScrollSpeed1, 0.0f, 1.0f, 0.015f);
            sanitize(c.m_NormalMapTiling, 0.0f, 50.0f, 1.0f);
            sanitize(c.m_NoiseIntensity, 0.0f, 1.0f, 0.3f);
            sanitize(c.m_DepthSofteningDistance, 0.0f, 50.0f, 2.0f);
            sanitize(c.m_RefractionDistortion, 0.0f, 0.5f, 0.05f);
            sanitize(c.m_RefractionHeightFactor, 0.0f, 2.0f, 0.5f);
            sanitize(c.m_FoamHeightStart, 0.0f, 2.0f, 0.3f);
            sanitize(c.m_FoamFadeDistance, 0.01f, 5.0f, 0.5f);
            sanitize(c.m_FoamTiling, 0.0f, 50.0f, 2.0f);
            sanitize(c.m_FoamBrightness, 0.0f, 5.0f, 1.5f);
            sanitize(c.m_FoamAngleExponent, 0.1f, 10.0f, 2.0f);
            sanitize(c.m_ShorelineFoamPower, 0.1f, 10.0f, 3.0f);
            sanitize(c.m_SSSIntensity, 0.0f, 5.0f, 0.5f);
            sanitize(c.m_SSRMaxSteps, 0.0f, 256.0f, 64.0f);
            sanitize(c.m_SSRStepSize, 0.01f, 1.0f, 0.1f);
            sanitize(c.m_SSRMaxDistance, 1.0f, 200.0f, 50.0f);
            sanitize(c.m_SSRThickness, 0.01f, 5.0f, 0.5f);
            // Planar reflections — match the scene-YAML clamp ranges/defaults
            // (Scene.cpp water submission). The AtEnd() fallback above only
            // covers missing data; sanitize here also rejects NaN/Inf and
            // out-of-range values from a corrupt or hand-edited archive.
            sanitize(c.m_PlanarReflectionIntensity, 0.0f, 1.0f, 1.0f);
            sanitize(c.m_PlanarReflectionDistortion, 0.0f, 0.25f, 0.02f);
            sanitize(c.m_TessellationFactor, 1.0f, 64.0f, 8.0f);
            sanitize(c.m_TessMinDistance, 1.0f, 500.0f, 10.0f);
            sanitize(c.m_TessMaxDistance, 10.0f, 1000.0f, 200.0f);
            c.m_TessMaxDistance = std::max(c.m_TessMaxDistance, c.m_TessMinDistance + 1.0f);

            // Sanitize direction vectors — finiteness check (+ normalize for scroll dirs only)
            auto sanitizeDir2 = [](glm::vec2& v, glm::vec2 const& fallback)
            {
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || glm::dot(v, v) < 1e-6f)
                    v = fallback;
            };
            auto sanitizeScrollDir = [](glm::vec2& v, glm::vec2 const& fallback)
            {
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || glm::dot(v, v) < 1e-6f)
                {
                    v = fallback;
                    return;
                }
                v = glm::normalize(v);
            };
            auto sanitizeColor = [](glm::vec3& v, glm::vec3 const& fallback)
            {
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                {
                    v = fallback;
                    return;
                }
                v = glm::clamp(v, glm::vec3(0.0f), glm::vec3(1.0f));
            };
            sanitizeDir2(c.m_WaveDir0, glm::vec2(1.0f, 0.0f));
            sanitizeDir2(c.m_WaveDir1, glm::vec2(0.7f, 0.7f));
            sanitizeScrollDir(c.m_NormalMapScrollDir0, glm::vec2(1.0f, 0.0f));
            sanitizeScrollDir(c.m_NormalMapScrollDir1, glm::vec2(0.0f, 1.0f));
            sanitizeColor(c.m_WaterColor, glm::vec3(0.1f, 0.4f, 0.5f));
            sanitizeColor(c.m_DeepColor, glm::vec3(0.0f, 0.1f, 0.2f));
            sanitizeColor(c.m_RefractionColor, glm::vec3(0.0f, 0.05f, 0.1f));
            sanitizeColor(c.m_SSSColor, glm::vec3(0.0f, 0.5f, 0.4f));
            sanitizeColor(c.m_UnderwaterFogColor, glm::vec3(0.05f, 0.15f, 0.25f));
            sanitize(c.m_UnderwaterFogDensity, 0.0f, 10.0f, 0.08f);
            sanitize(c.m_UnderwaterRefractionStrength, 0.0f, 0.1f, 0.006f);
            sanitize(c.m_UnderwaterRefractionScale, 0.0f, 200.0f, 18.0f);
            sanitize(c.m_UnderwaterRefractionSpeed, 0.0f, 50.0f, 1.2f);
            sanitize(c.m_UnderwaterChromaticStrength, 0.0f, 1.0f, 0.4f);
            sanitize(c.m_CausticsIntensity, 0.0f, 10.0f, 0.5f);
            sanitize(c.m_CausticsScale, 0.001f, 10.0f, 0.35f);
            sanitize(c.m_CausticsSpeed, 0.0f, 50.0f, 0.6f);
            sanitize(c.m_CausticsMaxDepth, 0.1f, 1000.0f, 25.0f);
            sanitizeColor(c.m_CausticsColor, glm::vec3(0.7f, 0.85f, 1.0f));
            sanitize(c.m_GodRayIntensity, 0.0f, 10.0f, 0.5f);
            sanitize(c.m_GodRayDecay, 0.0f, 0.999f, 0.97f);
            sanitize(c.m_GodRayDensity, 0.0f, 2.0f, 0.85f);
            sanitize(c.m_GodRayWeight, 0.0f, 2.0f, 1.0f);
            sanitizeColor(c.m_GodRayColor, glm::vec3(1.0f, 0.95f, 0.8f));
            c.m_GodRaySamples = std::clamp(c.m_GodRaySamples, 1u, 256u);
            sanitize(c.m_GodRayDappleFloor, 0.0f, 1.0f, 0.35f);
            sanitize(c.m_GodRaySunFalloff, 1.0f, 64.0f, 16.0f);

            // Spectrum (§1.4): unknown enum values fall back to Phillips.
            if (static_cast<u32>(c.m_FFTSpectrumType) > static_cast<u32>(Ocean::SpectrumType::JONSWAP))
                c.m_FFTSpectrumType = Ocean::SpectrumType::Phillips;
            sanitize(c.m_FFTJonswapGamma, 1.0f, 10.0f, 3.3f);
            sanitize(c.m_FFTJonswapFetch, 1.0f, 1.0e6f, 100000.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, BuoyancyComponent& c)
    {
        ar << c.m_Enabled;
        ar << c.m_ProbeExtents;
        ar << c.m_FluidDensity << c.m_BuoyancyScale;
        ar << c.m_LinearDrag << c.m_AngularDrag << c.m_SubmergenceRamp;

        if (ar.IsLoading())
        {
            auto sanitize = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                    v = fallback;
                else
                    v = std::clamp(v, lo, hi);
            };
            if (!std::isfinite(c.m_ProbeExtents.x) || !std::isfinite(c.m_ProbeExtents.y) || !std::isfinite(c.m_ProbeExtents.z))
                c.m_ProbeExtents = glm::vec3(0.5f);
            else
                c.m_ProbeExtents = glm::clamp(c.m_ProbeExtents, glm::vec3(0.01f), glm::vec3(1000.0f));
            sanitize(c.m_FluidDensity, 1.0f, 100000.0f, 1000.0f);
            sanitize(c.m_BuoyancyScale, 0.0f, 1000.0f, 1.0f);
            sanitize(c.m_LinearDrag, 0.0f, 1000.0f, 0.8f);
            sanitize(c.m_AngularDrag, 0.0f, 1000.0f, 0.5f);
            sanitize(c.m_SubmergenceRamp, 0.001f, 100.0f, 0.25f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SnowDeformerComponent& c)
    {
        ar << c.m_DeformRadius << c.m_DeformDepth;
        ar << c.m_FalloffExponent << c.m_CompactionFactor;
        ar << c.m_EmitEjecta;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, VirtualMeshComponent& c)
    {
        ar << c.m_Enabled << c.m_MeshSource;
        ar << c.m_ErrorThresholdPixels << c.m_CastShadows;

        if (ar.IsLoading())
        {
            if (!std::isfinite(c.m_ErrorThresholdPixels))
            {
                c.m_ErrorThresholdPixels = 1.0f;
            }
            c.m_ErrorThresholdPixels = std::clamp(c.m_ErrorThresholdPixels, 0.05f, 64.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, FluidComponent& c)
    {
        ar << c.m_Enabled << c.m_Settings;
        ar << c.m_DomainHalfExtents << c.m_MaxParticles;
        ar << c.m_SolverMode << c.m_PrefillFraction;

        if (ar.IsLoading())
        {
            if (!std::isfinite(c.m_DomainHalfExtents.x) || !std::isfinite(c.m_DomainHalfExtents.y) ||
                !std::isfinite(c.m_DomainHalfExtents.z))
                c.m_DomainHalfExtents = glm::vec3(4.0f);
            else
                c.m_DomainHalfExtents = glm::clamp(c.m_DomainHalfExtents, glm::vec3(0.25f), glm::vec3(256.0f));
            c.m_MaxParticles = std::clamp(c.m_MaxParticles, 64u, 1000000u);
            if (static_cast<i32>(c.m_SolverMode) < 0 || static_cast<i32>(c.m_SolverMode) > 2)
                c.m_SolverMode = FluidSolverMode::Auto;
            if (!std::isfinite(c.m_PrefillFraction))
                c.m_PrefillFraction = 0.0f;
            else
                c.m_PrefillFraction = std::clamp(c.m_PrefillFraction, 0.0f, 1.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, FluidEmitterComponent& c)
    {
        ar << c.m_Enabled << c.m_Rate << c.m_Speed << c.m_SpreadRadius;

        if (ar.IsLoading())
        {
            auto sanitize = [](f32& v, f32 lo, f32 hi, f32 fallback)
            {
                if (!std::isfinite(v))
                    v = fallback;
                else
                    v = std::clamp(v, lo, hi);
            };
            sanitize(c.m_Rate, 0.0f, 200000.0f, 500.0f);
            sanitize(c.m_Speed, 0.0f, 100.0f, 4.0f);
            sanitize(c.m_SpreadRadius, 0.0f, 10.0f, 0.15f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, FluidKillVolumeComponent& c)
    {
        ar << c.m_Enabled << c.m_HalfExtents;

        if (ar.IsLoading())
        {
            if (!std::isfinite(c.m_HalfExtents.x) || !std::isfinite(c.m_HalfExtents.y) ||
                !std::isfinite(c.m_HalfExtents.z))
                c.m_HalfExtents = glm::vec3(1.0f);
            else
                c.m_HalfExtents = glm::clamp(c.m_HalfExtents, glm::vec3(0.01f), glm::vec3(256.0f));
        }
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
        // m_Transparent was added in v2. Per-component payloads are
        // size-prefixed, so v1 archives have no trailing byte at this point;
        // probe AtEnd() to stay compatible instead of bumping the entire
        // archive format for a one-field addition.
        if (ar.IsLoading())
        {
            if (ar.AtEnd())
                c.m_Transparent = false;
            else
                ar << c.m_Transparent;
        }
        else
        {
            ar << c.m_Transparent;
        }
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

    void SaveGameComponentSerializer::Serialize(FArchive& /*ar*/, SkeletonComponent& /*c*/)
    {
        // Skeleton is a runtime Ref reconstructed from the model file on load
        // (Ref<Skeleton>, tag cache, mutex are all rebuild-on-load state). The
        // presence of the component is what we persist — actual bone data
        // comes back when the associated Model / AnimationState reloads.
        // Marker-only serializer; mirrors SceneSerializer.cpp:4589 which also
        // emits an empty map for the component.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, LuaScriptComponent& c)
    {
        ar << c.ScriptFile;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, TileRendererComponent& c)
    {
        // TileMesh is a runtime Ref — rebuilt from MaterialIDs/Materials on load.
        ar << c.Width << c.Height << c.TileSize;
        ar << c.MaterialIDs;

        // Materials: serialize base PBR factors only (full Material has runtime
        // texture / shader state that the renderer rebuilds from MaterialIDs).
        u64 materialCount = c.Materials.size();
        ar << materialCount;
        if (ar.IsLoading())
            c.Materials.resize(materialCount);
        for (u64 i = 0; i < materialCount; ++i)
        {
            if (ar.IsSaving())
            {
                auto base = c.Materials[i].GetBaseColorFactor();
                auto metallic = c.Materials[i].GetMetallicFactor();
                auto roughness = c.Materials[i].GetRoughnessFactor();
                auto emissive = c.Materials[i].GetEmissiveFactor();
                ar << base.x << base.y << base.z << base.w;
                ar << metallic << roughness;
                ar << emissive.x << emissive.y << emissive.z << emissive.w;
            }
            else
            {
                glm::vec4 base{};
                glm::vec4 emissive{};
                f32 metallic{};
                f32 roughness{};
                ar << base.x << base.y << base.z << base.w;
                ar << metallic << roughness;
                ar << emissive.x << emissive.y << emissive.z << emissive.w;
                c.Materials[i].SetBaseColorFactor(base);
                c.Materials[i].SetMetallicFactor(metallic);
                c.Materials[i].SetRoughnessFactor(roughness);
                c.Materials[i].SetEmissiveFactor(emissive);
            }
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, DialogueComponent& c)
    {
        ar << c.m_DialogueTree;
        ar << c.m_AutoTrigger;
        ar << c.m_TriggerRadius;
        ar << c.m_TriggerOnce;
        // m_HasTriggered is runtime-only — not persisted (resets on load).
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NavMeshBoundsComponent& c)
    {
        ar << c.m_Min.x << c.m_Min.y << c.m_Min.z;
        ar << c.m_Max.x << c.m_Max.y << c.m_Max.z;

        // Off-mesh links: length-prefixed list, same save/load-symmetric idiom as the
        // terrain layer rules above. ar.operator<< reads on load, writes on save.
        auto serializeLink = [&ar](OffMeshLink& link)
        {
            ar << link.m_Start.x << link.m_Start.y << link.m_Start.z;
            ar << link.m_End.x << link.m_End.y << link.m_End.z;
            ar << link.m_Radius;
            ar << link.m_Bidirectional;
        };

        u32 linkCount = static_cast<u32>(c.m_Links.size());
        ar << linkCount;
        if (ar.IsLoading())
        {
            constexpr u32 kMaxLinks = 4096u;
            u32 clampedLinks = std::min(linkCount, kMaxLinks);
            c.m_Links.assign(clampedLinks, OffMeshLink{});
            for (u32 i = 0; i < clampedLinks; ++i)
            {
                serializeLink(c.m_Links[i]);
                if (ar.IsError())
                    return;
            }
            // Drain any excess entries to keep the stream aligned.
            for (u32 i = clampedLinks; i < linkCount; ++i)
            {
                OffMeshLink discard{};
                serializeLink(discard);
                if (ar.IsError())
                    return;
            }
            // Sanitize untrusted on-disk values so corrupt save data can't poison the bake.
            for (OffMeshLink& link : c.m_Links)
            {
                if (!Math::IsFinite(link.m_Start))
                    link.m_Start = glm::vec3(0.0f);
                if (!Math::IsFinite(link.m_End))
                    link.m_End = glm::vec3(0.0f);
                if (!std::isfinite(link.m_Radius) || link.m_Radius <= 0.0f)
                    link.m_Radius = 0.6f;
            }
        }
        else
        {
            for (OffMeshLink& link : c.m_Links)
                serializeLink(link);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NavAgentComponent& c)
    {
        ar << c.m_Radius << c.m_Height;
        ar << c.m_MaxSpeed << c.m_Acceleration << c.m_StoppingDistance;
        ar << c.m_AvoidancePriority;
        ar << c.m_LockYAxis;
        // Runtime path state intentionally excluded — recomputed by the nav system.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NameplateComponent& c)
    {
        ar << c.m_Enabled << c.m_ShowHealthBar << c.m_ShowManaBar;
        ar << c.m_WorldOffset.x << c.m_WorldOffset.y << c.m_WorldOffset.z;
        ar << c.m_BarSize.x << c.m_BarSize.y;
        ar << c.m_HealthBarColor.x << c.m_HealthBarColor.y << c.m_HealthBarColor.z << c.m_HealthBarColor.w;
        ar << c.m_ManaBarColor.x << c.m_ManaBarColor.y << c.m_ManaBarColor.z << c.m_ManaBarColor.w;
        ar << c.m_BarBackgroundColor.x << c.m_BarBackgroundColor.y << c.m_BarBackgroundColor.z << c.m_BarBackgroundColor.w;
        ar << c.m_ManaBarGap;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, IKTargetComponent& c)
    {
        // --- Aim IK ---
        ar << c.AimIKEnabled << c.AimBoneIndex;
        ar << c.AimTarget.x << c.AimTarget.y << c.AimTarget.z;
        ar << c.AimAxis.x << c.AimAxis.y << c.AimAxis.z;
        ar << c.AimOffset.x << c.AimOffset.y << c.AimOffset.z;
        ar << c.AimPoleVector.x << c.AimPoleVector.y << c.AimPoleVector.z;
        ar << c.AimChainLength << c.AimChainFactor << c.AimWeight;
        ar << c.AimTargetEntity;

        // --- Limb IK ---
        ar << c.LimbIKEnabled << c.LimbBoneIndex;
        ar << c.LimbTarget.x << c.LimbTarget.y << c.LimbTarget.z;
        ar << c.LimbChainLength << c.LimbWeight;
        ar << c.LimbTargetEntity;

        // ── Format v4: Chain IK (FABRIK full-chain) section ──
        // Appended at the end when kSaveGameFormatVersion was bumped 3→4. A
        // save written before v4 has none of these bytes; loading one leaves
        // the Chain* fields at their constructor defaults, sanitized below.
        if (HasFieldsSince(ar, 4))
        {
            ar << c.ChainIKEnabled << c.ChainBoneIndex;
            ar << c.ChainTarget.x << c.ChainTarget.y << c.ChainTarget.z;
            ar << c.ChainPoleVector.x << c.ChainPoleVector.y << c.ChainPoleVector.z;
            ar << c.ChainLength << c.ChainIterations;
            ar << c.ChainTolerance << c.ChainWeight;
            ar << c.ChainTargetEntity;
        }

        if (!ar.IsSaving())
        {
            // Validate floats loaded from disk — replace NaN/Inf with defaults
            auto sanitize = [](f32& v, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                }
            };
            sanitize(c.ChainTarget.x, 0.0f);
            sanitize(c.ChainTarget.y, 0.0f);
            sanitize(c.ChainTarget.z, 0.0f);
            sanitize(c.ChainPoleVector.x, 0.0f);
            sanitize(c.ChainPoleVector.y, 0.0f);
            sanitize(c.ChainPoleVector.z, 0.0f);
            sanitize(c.ChainTolerance, 0.001f);
            sanitize(c.ChainWeight, 1.0f);

            // Clamp to the same valid ranges the scene-YAML deserializer
            // enforces so finite-but-invalid values can't reach the solver.
            c.ChainLength = std::max(2u, c.ChainLength);
            c.ChainIterations = std::clamp(c.ChainIterations, 1u, 128u);
            c.ChainTolerance = std::clamp(c.ChainTolerance, 0.0f, 10.0f);
            c.ChainWeight = std::clamp(c.ChainWeight, 0.0f, 1.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, SpringBoneComponent& c)
    {
        ar << c.Enabled << c.EndBoneIndex << c.ChainLength;
        ar << c.Stiffness << c.Damping;
        ar << c.Gravity.x << c.Gravity.y << c.Gravity.z;
        ar << c.Weight;

        if (!ar.IsSaving())
        {
            // Validate floats loaded from disk — replace NaN/Inf with defaults
            auto sanitize = [](f32& v, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                }
            };
            sanitize(c.Stiffness, 80.0f);
            sanitize(c.Damping, 12.0f);
            sanitize(c.Gravity.x, 0.0f);
            sanitize(c.Gravity.y, -9.81f);
            sanitize(c.Gravity.z, 0.0f);
            sanitize(c.Weight, 1.0f);

            // Clamp to the same valid ranges the scene-YAML deserializer
            // enforces so finite-but-invalid values can't reach the solver.
            c.ChainLength = std::max(2u, c.ChainLength);
            c.Stiffness = std::clamp(c.Stiffness, 0.0f, 1e6f);
            c.Damping = std::clamp(c.Damping, 0.0f, 1e6f);
            c.Weight = std::clamp(c.Weight, 0.0f, 1.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, LocomotionComponent& c)
    {
        ar << c.Enabled;
        ar << c.SpeedParameter << c.DirectionXParameter << c.DirectionYParameter;
        ar << c.GaitParameter << c.TurnParameter;
        ar << c.UseDesiredVelocity;
        ar << c.WalkEnterSpeed << c.WalkExitSpeed << c.RunEnterSpeed << c.RunExitSpeed;
        ar << c.SpeedSmoothing << c.DirectionReferenceSpeed;
        ar << c.StrideWarp << c.WalkClipSpeed << c.RunClipSpeed << c.MaxStrideScale;

        if (!ar.IsSaving())
        {
            // Validate + clamp to the OLO_SERIALIZE(Clamp) ranges.
            auto sanitize = [](f32& v, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                }
            };
            sanitize(c.WalkEnterSpeed, 0.15f);
            sanitize(c.WalkExitSpeed, 0.08f);
            sanitize(c.RunEnterSpeed, 3.0f);
            sanitize(c.RunExitSpeed, 2.5f);
            sanitize(c.SpeedSmoothing, 12.0f);
            sanitize(c.DirectionReferenceSpeed, 4.0f);
            sanitize(c.WalkClipSpeed, 1.4f);
            sanitize(c.RunClipSpeed, 4.0f);
            sanitize(c.MaxStrideScale, 1.5f);
            c.WalkEnterSpeed = std::clamp(c.WalkEnterSpeed, 0.0f, 100.0f);
            c.WalkExitSpeed = std::clamp(c.WalkExitSpeed, 0.0f, 100.0f);
            c.RunEnterSpeed = std::clamp(c.RunEnterSpeed, 0.0f, 100.0f);
            c.RunExitSpeed = std::clamp(c.RunExitSpeed, 0.0f, 100.0f);
            c.SpeedSmoothing = std::clamp(c.SpeedSmoothing, 0.0f, 100.0f);
            c.DirectionReferenceSpeed = std::clamp(c.DirectionReferenceSpeed, 0.01f, 100.0f);
            c.WalkClipSpeed = std::clamp(c.WalkClipSpeed, 0.0f, 100.0f);
            c.RunClipSpeed = std::clamp(c.RunClipSpeed, 0.0f, 100.0f);
            c.MaxStrideScale = std::clamp(c.MaxStrideScale, 1.0f, 4.0f);
            c.DesiredVelocity = glm::vec3(0.0f); // runtime input, never restored
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, FootIKComponent& c)
    {
        ar << c.Enabled << c.LeftFootBone << c.RightFootBone << c.ChainLength;
        ar << c.EnableToeRoll << c.LeftToeBone << c.RightToeBone;
        ar << c.RaycastUp << c.RaycastDown << c.FootHeight;
        ar << c.AdjustPelvis << c.PelvisBone << c.MaxPelvisDrop << c.PelvisLerpSpeed;
        ar << c.FootLock << c.PlantVelocityThreshold << c.PlantLiftThreshold << c.UnlockBlendTime;
        ar << c.AlignFootToSlope << c.MaxSlopeAngle << c.Weight;
        ar << c.LeftHandEnabled << c.LeftHandBone << c.LeftHandTarget << c.LeftHandTargetEntity;
        ar << c.RightHandEnabled << c.RightHandBone << c.RightHandTarget << c.RightHandTargetEntity;
        ar << c.HandChainLength << c.HandWeight;

        if (!ar.IsSaving())
        {
            // Validate + clamp to the same ranges the scene-YAML deserializer
            // enforces (the OLO_SERIALIZE(Clamp) annotations).
            auto sanitize = [](f32& v, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                }
            };
            sanitize(c.RaycastUp, 0.5f);
            sanitize(c.RaycastDown, 1.0f);
            sanitize(c.FootHeight, 0.1f);
            sanitize(c.MaxPelvisDrop, 0.4f);
            sanitize(c.PelvisLerpSpeed, 10.0f);
            sanitize(c.PlantVelocityThreshold, 0.15f);
            sanitize(c.PlantLiftThreshold, 0.06f);
            sanitize(c.UnlockBlendTime, 0.12f);
            sanitize(c.MaxSlopeAngle, 50.0f);
            sanitize(c.Weight, 1.0f);
            sanitize(c.HandWeight, 1.0f);
            c.ChainLength = std::max(2u, c.ChainLength);
            c.HandChainLength = std::max(2u, c.HandChainLength);
            c.RaycastUp = std::clamp(c.RaycastUp, 0.0f, 10.0f);
            c.RaycastDown = std::clamp(c.RaycastDown, 0.0f, 10.0f);
            c.FootHeight = std::clamp(c.FootHeight, 0.0f, 1.0f);
            c.MaxPelvisDrop = std::clamp(c.MaxPelvisDrop, 0.0f, 2.0f);
            c.PelvisLerpSpeed = std::clamp(c.PelvisLerpSpeed, 0.0f, 100.0f);
            c.PlantVelocityThreshold = std::clamp(c.PlantVelocityThreshold, 0.0f, 10.0f);
            c.PlantLiftThreshold = std::clamp(c.PlantLiftThreshold, 0.0f, 1.0f);
            c.UnlockBlendTime = std::clamp(c.UnlockBlendTime, 0.01f, 2.0f);
            c.MaxSlopeAngle = std::clamp(c.MaxSlopeAngle, 0.0f, 90.0f);
            c.Weight = std::clamp(c.Weight, 0.0f, 1.0f);
            c.HandWeight = std::clamp(c.HandWeight, 0.0f, 1.0f);
            if (!std::isfinite(c.LeftHandTarget.x) || !std::isfinite(c.LeftHandTarget.y) || !std::isfinite(c.LeftHandTarget.z))
            {
                c.LeftHandTarget = glm::vec3(0.0f);
            }
            if (!std::isfinite(c.RightHandTarget.x) || !std::isfinite(c.RightHandTarget.y) || !std::isfinite(c.RightHandTarget.z))
            {
                c.RightHandTarget = glm::vec3(0.0f);
            }
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, RetargetingComponent& c)
    {
        ar << c.Enabled << c.m_SourcePath << c.m_SourceEntity;
        ar << c.UseHumanoidRoles << c.PerBoneTranslation << c.TransferRootTranslation;
        ar << c.RootTranslationScale;
        // Bounded, duplicate-checked round-trip via the generic archive operator
        // (ArchiveExtensions.h) — same path as the quest journal's maps above.
        ar << c.m_SourceRoleOverrides;
        ar << c.m_TargetRoleOverrides;

        if (!ar.IsSaving())
        {
            // Same range the scene-YAML deserializer enforces (Clamp annotation).
            if (!std::isfinite(c.RootTranslationScale))
            {
                c.RootTranslationScale = 0.0f;
            }
            c.RootTranslationScale = std::clamp(c.RootTranslationScale, 0.0f, 1000.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, NoiseAnimationComponent& c)
    {
        ar << c.Enabled << c.EndBoneIndex << c.ChainLength;
        ar << c.Frequency;
        ar << c.RotationAmplitude.x << c.RotationAmplitude.y << c.RotationAmplitude.z;
        ar << c.TranslationAmplitude.x << c.TranslationAmplitude.y << c.TranslationAmplitude.z;
        ar << c.Octaves << c.Lacunarity << c.Gain << c.Seed << c.Weight;

        if (!ar.IsSaving())
        {
            // Validate floats loaded from disk — replace NaN/Inf with defaults
            auto sanitize = [](f32& v, f32 fallback)
            {
                if (!std::isfinite(v))
                {
                    v = fallback;
                }
            };
            sanitize(c.Frequency, 1.0f);
            sanitize(c.RotationAmplitude.x, 0.08f);
            sanitize(c.RotationAmplitude.y, 0.08f);
            sanitize(c.RotationAmplitude.z, 0.08f);
            sanitize(c.TranslationAmplitude.x, 0.0f);
            sanitize(c.TranslationAmplitude.y, 0.0f);
            sanitize(c.TranslationAmplitude.z, 0.0f);
            sanitize(c.Lacunarity, 2.0f);
            sanitize(c.Gain, 0.5f);
            sanitize(c.Weight, 1.0f);

            // Clamp to the same valid ranges the scene-YAML deserializer enforces
            // (SanitizeVec3Clamped / SanitizeFloat) so finite-but-invalid values
            // can't reach the solver.
            c.ChainLength = std::max(1u, c.ChainLength);
            c.Octaves = std::clamp(c.Octaves, 1u, 8u);
            c.Frequency = std::clamp(c.Frequency, 0.0f, 1e4f);
            c.RotationAmplitude.x = std::clamp(c.RotationAmplitude.x, -6.2832f, 6.2832f);
            c.RotationAmplitude.y = std::clamp(c.RotationAmplitude.y, -6.2832f, 6.2832f);
            c.RotationAmplitude.z = std::clamp(c.RotationAmplitude.z, -6.2832f, 6.2832f);
            c.TranslationAmplitude.x = std::clamp(c.TranslationAmplitude.x, -1e4f, 1e4f);
            c.TranslationAmplitude.y = std::clamp(c.TranslationAmplitude.y, -1e4f, 1e4f);
            c.TranslationAmplitude.z = std::clamp(c.TranslationAmplitude.z, -1e4f, 1e4f);
            c.Lacunarity = std::clamp(c.Lacunarity, 1.0f, 8.0f);
            c.Gain = std::clamp(c.Gain, 0.0f, 1.0f);
            c.Weight = std::clamp(c.Weight, 0.0f, 1.0f);
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, UIWorldAnchorComponent& c)
    {
        ar << c.m_TargetEntity;
        ar << c.m_WorldOffset.x << c.m_WorldOffset.y << c.m_WorldOffset.z;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, MorphTargetComponent& c)
    {
        // Ref<MorphTargetSet> + cached base data are runtime — not persisted.
        // Weights map IS persisted so character poses survive save/load.
        u64 weightCount = c.Weights.size();
        ar << weightCount;
        if (ar.IsSaving())
        {
            for (const auto& [name, weight] : c.Weights)
            {
                std::string nameCopy = name; // ar << requires non-const reference
                f32 weightCopy = weight;
                ar << nameCopy << weightCopy;
            }
        }
        else
        {
            c.Weights.clear();
            for (u64 i = 0; i < weightCount; ++i)
            {
                std::string name;
                f32 weight{};
                ar << name << weight;
                c.Weights[name] = weight;
            }
        }
    }

    // ========================================================================
    // Helpers: Gameplay-component nested types
    //
    // These helpers serialize the persisted state only — runtime caches, asset
    // refs, and modifier-fan-out are rebuilt on load by re-applying effects
    // and re-resolving assets from the asset registry.
    // ========================================================================

    static void SerializeBlackboard(FArchive& ar, BTBlackboard& bb)
    {
        if (ar.IsSaving())
        {
            const auto& all = bb.GetAll();
            u64 count = all.size();
            ar << count;
            for (auto& [key, value] : all)
            {
                std::string keyCopy = key;
                ar << keyCopy;
                u8 tag = static_cast<u8>(value.index());
                ar << tag;
                std::visit([&ar](auto& v)
                           {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, UUID>)
                    {
                        u64 raw = static_cast<u64>(v);
                        ar << raw;
                    }
                    else
                    {
                        T copy = v;
                        ar << copy;
                    } }, value);
            }
        }
        else
        {
            bb.Clear();
            u64 count{};
            ar << count;
            for (u64 i = 0; i < count; ++i)
            {
                std::string key;
                ar << key;
                u8 tag{};
                ar << tag;
                switch (tag)
                {
                    case 0:
                    {
                        bool b{};
                        ar << b;
                        bb.Set(key, b);
                        break;
                    }
                    case 1:
                    {
                        i32 v{};
                        ar << v;
                        bb.Set(key, v);
                        break;
                    }
                    case 2:
                    {
                        f32 v{};
                        ar << v;
                        bb.Set(key, v);
                        break;
                    }
                    case 3:
                    {
                        std::string s;
                        ar << s;
                        bb.Set(key, s);
                        break;
                    }
                    case 4:
                    {
                        glm::vec3 v{};
                        ar << v;
                        bb.Set(key, v);
                        break;
                    }
                    case 5:
                    {
                        u64 raw{};
                        ar << raw;
                        bb.Set(key, UUID{ raw });
                        break;
                    }
                    default:
                        // Unknown tag means the stream layout has changed or the
                        // save-file is corrupt. We MUST NOT just skip — the
                        // variant payload is still in the stream, so dropping
                        // it would misalign the cursor and corrupt every entry
                        // after this one. Mark the archive in error and bail;
                        // the caller (FArchive::IsError) aborts the wider load.
                        OLO_CORE_ERROR("[SaveGame] Unknown blackboard variant tag {} — stream is misaligned, aborting load", tag);
                        ar.SetError();
                        return;
                }
            }
        }
    }

    static void SerializeItemAffix(FArchive& ar, ItemAffix& a)
    {
        ar << a.DefinitionID;
        ar << a.Type;
        ar << a.Tier;
        ar << a.Name;
        ar << a.Attribute;
        ar << a.Value;
    }

    static void SerializeItemInstance(FArchive& ar, ItemInstance& it)
    {
        ar << it.InstanceID;
        ar << it.ItemDefinitionID;
        ar << it.StackCount;
        ar << it.Durability;
        ar << it.MaxDurability;

        u64 affixCount = it.Affixes.size();
        ar << affixCount;
        if (ar.IsLoading())
        {
            it.Affixes.resize(affixCount);
        }
        for (u64 i = 0; i < affixCount; ++i)
        {
            SerializeItemAffix(ar, it.Affixes[i]);
        }

        ar << it.CustomData;
    }

    static void SerializeOptionalItem(FArchive& ar, std::optional<ItemInstance>& slot)
    {
        bool hasItem = slot.has_value();
        ar << hasItem;
        if (hasItem)
        {
            if (ar.IsLoading() && !slot.has_value())
            {
                slot.emplace();
            }
            SerializeItemInstance(ar, *slot);
        }
        else if (ar.IsLoading())
        {
            slot.reset();
        }
        else
        {
            // No additional handling required.
        }
    }

    static void SerializeInventory(FArchive& ar, Inventory& inv)
    {
        if (ar.IsSaving())
        {
            i32 capacity = inv.GetCapacity();
            ar << capacity;
            ar << inv.MaxWeight;
            const auto& slots = inv.GetSlots();
            u64 slotCount = slots.size();
            ar << slotCount;
            for (auto const& s : slots)
            {
                std::optional<ItemInstance> copy = s;
                SerializeOptionalItem(ar, copy);
            }
        }
        else
        {
            i32 capacity{};
            ar << capacity;
            // Defensive clamp — a corrupt save-file could deliver a negative
            // capacity, and a naive `static_cast<u64>(capacity)` below would
            // wrap that to ~1.8e19 and admit out-of-bounds AddItemToSlot calls.
            if (capacity < 0)
            {
                OLO_CORE_WARN("[SaveGame] Negative inventory capacity {} in stream — clamping to 0", capacity);
                capacity = 0;
            }
            inv.SetCapacity(capacity);
            ar << inv.MaxWeight;
            u64 slotCount{};
            ar << slotCount;
            const u64 slotBound = static_cast<u64>(capacity);
            for (u64 i = 0; i < slotCount; ++i)
            {
                std::optional<ItemInstance> loaded;
                SerializeOptionalItem(ar, loaded);
                if (loaded && i < slotBound)
                {
                    inv.AddItemToSlot(static_cast<i32>(i), *loaded);
                }
            }
        }
    }

    static void SerializeEquipmentSlots(FArchive& ar, EquipmentSlots& eq)
    {
        constexpr u32 kSlotCount = static_cast<u32>(std::to_underlying(EquipmentSlots::Slot::Count));
        for (u32 i = 0; i < kSlotCount; ++i)
        {
            const auto slot = static_cast<EquipmentSlots::Slot>(i);
            if (ar.IsSaving())
            {
                const ItemInstance* eqItem = eq.GetEquipped(slot);
                bool present = (eqItem != nullptr);
                ar << present;
                if (present)
                {
                    ItemInstance copy = *eqItem;
                    SerializeItemInstance(ar, copy);
                }
            }
            else
            {
                bool present{};
                ar << present;
                if (present)
                {
                    ItemInstance loaded;
                    SerializeItemInstance(ar, loaded);
                    eq.DirectEquip(slot, loaded);
                }
            }
        }
    }

    static void SerializeAnimationParameter(FArchive& ar, AnimationParameter& p)
    {
        ar << p.Name;
        ar << p.ParamType;
        ar << p.FloatValue;
        ar << p.IntValue;
        ar << p.BoolValue;
        ar << p.TriggerConsumed;
    }

    static void SerializeAnimationParameterSet(FArchive& ar, AnimationParameterSet& set)
    {
        if (ar.IsSaving())
        {
            const auto& all = set.GetAll();
            u64 count = all.size();
            ar << count;
            for (auto const& [name, param] : all)
            {
                AnimationParameter copy = param;
                SerializeAnimationParameter(ar, copy);
            }
        }
        else
        {
            u64 count{};
            ar << count;
            for (u64 i = 0; i < count; ++i)
            {
                AnimationParameter p;
                SerializeAnimationParameter(ar, p);
                switch (p.ParamType)
                {
                    case AnimationParameterType::Float:
                        set.DefineFloat(p.Name, p.FloatValue);
                        break;
                    case AnimationParameterType::Int:
                        set.DefineInt(p.Name, p.IntValue);
                        break;
                    case AnimationParameterType::Bool:
                        set.DefineBool(p.Name, p.BoolValue);
                        break;
                    case AnimationParameterType::Trigger:
                        set.DefineTrigger(p.Name);
                        if (!p.TriggerConsumed)
                            set.SetTrigger(p.Name);
                        break;
                }
            }
        }
    }

    static void SerializeGameplayTag(FArchive& ar, GameplayTag& tag)
    {
        // Tag is uniquely identified by its string path; hash is derived on construction.
        if (ar.IsSaving())
        {
            std::string path = tag.GetTagString();
            ar << path;
        }
        else
        {
            std::string path;
            ar << path;
            tag = GameplayTag{ path };
        }
    }

    static void SerializeGameplayTagContainer(FArchive& ar, GameplayTagContainer& container)
    {
        if (ar.IsSaving())
        {
            const auto& tags = container.GetTags();
            u64 count = tags.size();
            ar << count;
            for (auto& t : tags)
            {
                GameplayTag copy = t;
                SerializeGameplayTag(ar, copy);
            }
        }
        else
        {
            container.Clear();
            u64 count{};
            ar << count;
            for (u64 i = 0; i < count; ++i)
            {
                GameplayTag t{ "" };
                SerializeGameplayTag(ar, t);
                if (t.IsValid())
                {
                    container.AddTag(t);
                }
            }
        }
    }

    static void SerializeAttributeModifier(FArchive& ar, AttributeModifier& m)
    {
        ar << m.Op;
        ar << m.Magnitude;
        SerializeGameplayTag(ar, m.Source);
    }

    static void SerializeAttributeSet(FArchive& ar, AttributeSet& set)
    {
        if (ar.IsSaving())
        {
            const auto names = set.GetAttributeNames();
            u64 count = names.size();
            ar << count;
            for (auto& name : names)
            {
                std::string nameCopy = name;
                ar << nameCopy;
                f32 base = set.GetBaseValue(name);
                ar << base;
                const auto& mods = set.GetModifiers(name);
                u64 modCount = mods.size();
                ar << modCount;
                for (auto& m : mods)
                {
                    AttributeModifier copy = m;
                    SerializeAttributeModifier(ar, copy);
                }
            }
        }
        else
        {
            u64 count{};
            ar << count;
            for (u64 i = 0; i < count; ++i)
            {
                std::string name;
                ar << name;
                f32 base{};
                ar << base;
                u64 modCount{};
                ar << modCount;
                std::vector<AttributeModifier> mods(modCount);
                for (u64 m = 0; m < modCount; ++m)
                {
                    SerializeAttributeModifier(ar, mods[m]);
                }
                set.RestoreFromSnapshot(name, base, mods);
            }
        }
    }

    static void SerializeGameplayEffect(FArchive& ar, GameplayEffect& fx)
    {
        ar << fx.Name;
        ar << fx.Policy.DurationType;
        ar << fx.Policy.DurationSeconds;
        ar << fx.Policy.IsPeriodic;
        ar << fx.Policy.PeriodSeconds;

        u64 modCount = fx.Modifiers.size();
        ar << modCount;
        if (ar.IsLoading())
            fx.Modifiers.resize(modCount);
        for (u64 i = 0; i < modCount; ++i)
        {
            ar << fx.Modifiers[i].AttributeName;
            ar << fx.Modifiers[i].Op;
            ar << fx.Modifiers[i].Magnitude;
        }

        SerializeGameplayTagContainer(ar, fx.GrantedTags);
        SerializeGameplayTagContainer(ar, fx.RequiredTags);
        SerializeGameplayTagContainer(ar, fx.BlockedTags);
        ar << fx.MaxStacks;
        ar << fx.RefreshDurationOnStack;
    }

    static void SerializeActiveEffectsContainer(FArchive& ar, ActiveEffectsContainer& c)
    {
        if (ar.IsSaving())
        {
            const auto& effects = c.GetActiveEffects();
            u64 effectCount = effects.size();
            ar << effectCount;
            for (auto const& e : effects)
            {
                GameplayEffect def = e.Definition;
                SerializeGameplayEffect(ar, def);
                f32 remaining = e.RemainingDuration;
                f32 periodTimer = e.PeriodTimer;
                i32 stacks = e.CurrentStacks;
                GameplayTag source = e.SourceTag;
                bool modsApplied = e.ModifiersApplied;
                bool tagsApplied = e.TagsApplied;
                ar << remaining << periodTimer << stacks;
                SerializeGameplayTag(ar, source);
                ar << modsApplied << tagsApplied;
            }
            const auto& grants = c.GetTagGrantCounts();
            u64 grantCount = grants.size();
            ar << grantCount;
            for (auto& [tag, ref] : grants)
            {
                GameplayTag tagCopy = tag;
                i32 refCopy = ref;
                SerializeGameplayTag(ar, tagCopy);
                ar << refCopy;
            }
        }
        else
        {
            u64 effectCount{};
            ar << effectCount;
            std::vector<ActiveEffect> effects(effectCount);
            for (u64 i = 0; i < effectCount; ++i)
            {
                SerializeGameplayEffect(ar, effects[i].Definition);
                ar << effects[i].RemainingDuration;
                ar << effects[i].PeriodTimer;
                ar << effects[i].CurrentStacks;
                SerializeGameplayTag(ar, effects[i].SourceTag);
                ar << effects[i].ModifiersApplied;
                ar << effects[i].TagsApplied;
            }
            u64 grantCount{};
            ar << grantCount;
            std::unordered_map<GameplayTag, i32> grants;
            for (u64 i = 0; i < grantCount; ++i)
            {
                GameplayTag t{ "" };
                SerializeGameplayTag(ar, t);
                i32 ref{};
                ar << ref;
                grants[t] = ref;
            }
            c.RestoreFromSnapshot(std::move(effects), std::move(grants));
        }
    }

    static void SerializeCooldownManager(FArchive& ar, CooldownManager& cm)
    {
        if (ar.IsSaving())
        {
            u64 count = cm.Size();
            ar << count;
            cm.ForEachCooldown([&ar](const GameplayTag& tag, f32 dur, f32 rem)
                               {
                GameplayTag tagCopy = tag;
                f32 durCopy = dur;
                f32 remCopy = rem;
                SerializeGameplayTag(ar, tagCopy);
                ar << durCopy << remCopy; });
        }
        else
        {
            cm.ResetAll();
            u64 count{};
            ar << count;
            for (u64 i = 0; i < count; ++i)
            {
                GameplayTag t{ "" };
                SerializeGameplayTag(ar, t);
                f32 dur{}, rem{};
                ar << dur << rem;
                cm.RestoreFromSnapshot(t, dur, rem);
            }
        }
    }

    static void SerializeGameplayAbilityDef(FArchive& ar, GameplayAbilityDef& def)
    {
        ar << def.Name;
        SerializeGameplayTag(ar, def.AbilityTag);
        SerializeGameplayTagContainer(ar, def.RequiredTags);
        SerializeGameplayTagContainer(ar, def.BlockedTags);
        SerializeGameplayTagContainer(ar, def.ActivationGrantedTags);
        ar << def.CooldownDuration << def.ResourceCost;
        ar << def.CostAttribute;
        u64 activationCount = def.ActivationEffects.size();
        ar << activationCount;
        if (ar.IsLoading())
            def.ActivationEffects.resize(activationCount);
        for (u64 i = 0; i < activationCount; ++i)
            SerializeGameplayEffect(ar, def.ActivationEffects[i]);
        u64 targetCount = def.TargetActivationEffects.size();
        ar << targetCount;
        if (ar.IsLoading())
            def.TargetActivationEffects.resize(targetCount);
        for (u64 i = 0; i < targetCount; ++i)
            SerializeGameplayEffect(ar, def.TargetActivationEffects[i]);
        ar << def.IsChanneled << def.IsToggled << def.ChannelDuration;
    }

    static void SerializeQuestObjective(FArchive& ar, QuestObjective& o)
    {
        ar << o.ObjectiveID << o.Description;
        ar << o.ObjectiveType;
        ar << o.TargetID;
        ar << o.RequiredCount << o.CurrentCount;
        ar << o.IsOptional << o.IsHidden << o.IsCompleted;
    }

    static void SerializeQuestRequirement(FArchive& ar, QuestRequirement& r)
    {
        ar << r.Type;
        ar << r.Target;
        ar << r.Value;
        ar << r.Comparison;
        u64 childCount = r.Children.size();
        ar << childCount;
        if (ar.IsLoading())
            r.Children.resize(childCount);
        for (u64 i = 0; i < childCount; ++i)
            SerializeQuestRequirement(ar, r.Children[i]);
        ar << r.Description;
    }

    static void SerializeQuestStage(FArchive& ar, QuestStage& s)
    {
        ar << s.StageID << s.Description;
        u64 objCount = s.Objectives.size();
        ar << objCount;
        if (ar.IsLoading())
            s.Objectives.resize(objCount);
        for (u64 i = 0; i < objCount; ++i)
            SerializeQuestObjective(ar, s.Objectives[i]);
        ar << s.RequireAllObjectives;
    }

    static void SerializeQuestBranchChoice(FArchive& ar, QuestBranchChoice& c)
    {
        ar << c.ChoiceID << c.Description << c.NextQuestID;
        ar << c.GrantedTags;
    }

    static void SerializeQuestRewards(FArchive& ar, QuestRewards& r)
    {
        ar << r.ExperiencePoints << r.Currency;
        ar << r.ItemRewards;
        ar << r.GrantedTags;
    }

    static void SerializeQuestDefinition(FArchive& ar, QuestDefinition& d)
    {
        ar << d.QuestID << d.Title << d.Description << d.Category;
        u64 stageCount = d.Stages.size();
        ar << stageCount;
        if (ar.IsLoading())
            d.Stages.resize(stageCount);
        for (u64 i = 0; i < stageCount; ++i)
            SerializeQuestStage(ar, d.Stages[i]);
        u64 reqCount = d.Requirements.size();
        ar << reqCount;
        if (ar.IsLoading())
            d.Requirements.resize(reqCount);
        for (u64 i = 0; i < reqCount; ++i)
            SerializeQuestRequirement(ar, d.Requirements[i]);
        u64 choiceCount = d.CompletionChoices.size();
        ar << choiceCount;
        if (ar.IsLoading())
            d.CompletionChoices.resize(choiceCount);
        for (u64 i = 0; i < choiceCount; ++i)
            SerializeQuestBranchChoice(ar, d.CompletionChoices[i]);
        SerializeQuestRewards(ar, d.CompletionRewards);
        ar << d.CanFail << d.TimeLimit;
        ar << d.FailOnTags;
        ar << d.IsRepeatable << d.RepeatCooldownSeconds;
    }

    static void SerializeActiveQuestState(FArchive& ar, QuestJournal::ActiveQuestState& s)
    {
        ar << s.QuestID;
        ar << s.Status;
        ar << s.CurrentStageIndex;
        u64 objCount = s.ObjectiveStates.size();
        ar << objCount;
        if (ar.IsLoading())
            s.ObjectiveStates.resize(objCount);
        for (u64 i = 0; i < objCount; ++i)
            SerializeQuestObjective(ar, s.ObjectiveStates[i]);
        ar << s.ElapsedTime;
        SerializeQuestDefinition(ar, s.Definition);
    }

    template<typename Hash, typename Equal>
    static void SerializeStringSet(FArchive& ar, std::unordered_set<std::string, Hash, Equal>& set)
    {
        if (ar.IsSaving())
        {
            u64 count = set.size();
            ar << count;
            for (auto& s : set)
            {
                std::string copy = s;
                ar << copy;
            }
        }
        else
        {
            set.clear();
            u64 count{};
            ar << count;
            for (u64 i = 0; i < count; ++i)
            {
                std::string s;
                ar << s;
                set.insert(std::move(s));
            }
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, InstancedMeshComponent& c)
    {
        // MeshSource / OverrideMaterial / merge cache are runtime Refs — rebuilt
        // from the primitive + placement asset handle on load.
        ar << c.Primitive;
        ar << c.PlacementAssetHandle;
        ar << c.FrustumCullPerInstance << c.CastShadows;
        ar << c.CullDistance;

        // Inline placement list — round-trip the per-instance transforms.
        u64 instanceCount = c.Instances.size();
        ar << instanceCount;
        if (ar.IsLoading())
            c.Instances.resize(instanceCount);
        for (u64 i = 0; i < instanceCount; ++i)
        {
            auto& inst = c.Instances[i];
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    ar << inst.Transform[row][col];
        }
    }

    // ========================================================================
    // Gameplay component serializers
    // ========================================================================

    void SaveGameComponentSerializer::Serialize(FArchive& ar, AnimationGraphComponent& c)
    {
        ar << c.AnimationGraphAssetHandle;
        SerializeAnimationParameterSet(ar, c.Parameters);
        // RuntimeGraph is a Ref that gets lazy-loaded from the asset on first tick.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, CinematicComponent& c)
    {
        ar << c.Sequence;
        ar << c.PlayOnStart;
        ar << c.Loop;
        ar << c.PlaybackSpeed;
        // RuntimeSequence + playhead/event state are runtime-only; the tick
        // loop rebuilds them when playback (re)starts.
        if (ar.IsLoading())
        {
            if (!std::isfinite(c.PlaybackSpeed))
            {
                c.PlaybackSpeed = 1.0f;
            }
            else
            {
                // Clamp to the inspector's authoring range so a corrupted save
                // can't inject an absurd time scale. Negative is valid (reverse
                // playback); the range is symmetric.
                c.PlaybackSpeed = std::clamp(c.PlaybackSpeed, -16.0f, 16.0f);
            }
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, BehaviorTreeComponent& c)
    {
        ar << c.BehaviorTreeAssetHandle;
        SerializeBlackboard(ar, c.Blackboard);
        // RuntimeTree + IsRunning are runtime state; the tick loop rebuilds them.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, StateMachineComponent& c)
    {
        ar << c.StateMachineAssetHandle;
        SerializeBlackboard(ar, c.Blackboard);
        // RuntimeFSM is a runtime Ref.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, GoapAgentComponent& c)
    {
        ar << c.Enabled;
        SerializeBlackboard(ar, c.Blackboard);
        // RuntimeAgent (actions/goals/world state) is a runtime Ref the gameplay
        // layer rebuilds after load.
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, PerceptibleComponent& c)
    {
        ar << c.Team;
        ar << c.IsPerceptible;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, PerceptionComponent& c)
    {
        // Authored config only — the runtime sensor result (HasVisibleTarget,
        // VisibleTarget, LastKnownPosition, ...) is recomputed by PerceptionSystem
        // on the first tick after load.
        ar << c.SightRange;
        ar << c.FovDegrees;
        ar << c.EyeOffset.x << c.EyeOffset.y << c.EyeOffset.z;
        ar << c.RequireLineOfSight;
        ar << c.PerceiverTeam;
        ar << c.DetectSameTeam;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, InventoryComponent& c)
    {
        SerializeInventory(ar, c.PlayerInventory);
        SerializeEquipmentSlots(ar, c.Equipment);
        ar << c.Currency;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ItemPickupComponent& c)
    {
        SerializeItemInstance(ar, c.Item);
        ar << c.PickupRadius;
        ar << c.AutoPickup;
        ar << c.DespawnTimer;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ItemContainerComponent& c)
    {
        SerializeInventory(ar, c.Contents);
        ar << c.IsShop;
        ar << c.LootTableID;
        ar << c.HasBeenLooted;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, QuestJournalComponent& c)
    {
        auto& j = c.Journal;
        if (ar.IsSaving())
        {
            // Active quests
            const auto& active = j.GetActiveQuestStates();
            u64 activeCount = active.size();
            ar << activeCount;
            for (auto& [id, state] : active)
            {
                std::string idCopy = id;
                QuestJournal::ActiveQuestState stateCopy = state;
                ar << idCopy;
                SerializeActiveQuestState(ar, stateCopy);
            }

            // Completed & failed IDs
            auto completedSet = j.GetCompletedQuestIDs();
            SerializeStringSet(ar, completedSet);
            auto failedSet = j.GetFailedQuestIDs();
            SerializeStringSet(ar, failedSet);

            // Tags
            auto tagSet = j.GetTags();
            SerializeStringSet(ar, tagSet);

            // Branch choices for completed quests
            auto branches = j.GetCompletedQuestBranches();
            ar << branches;

            // Cooldowns
            auto cooldowns = j.GetQuestCooldowns();
            ar << cooldowns;

            // Player state
            i32 level = j.GetPlayerLevel();
            ar << level;
            auto reputations = j.GetReputations();
            ar << reputations;
        }
        else
        {
            // Clear current state. We don't expose Reset() on QuestJournal so we
            // just write into a fresh component; the entt registry hands us one.
            u64 activeCount{};
            ar << activeCount;
            for (u64 i = 0; i < activeCount; ++i)
            {
                std::string id;
                ar << id;
                QuestJournal::ActiveQuestState state;
                SerializeActiveQuestState(ar, state);
                j.SetActiveQuestState(id, std::move(state));
            }

            std::unordered_set<std::string> completedSet;
            SerializeStringSet(ar, completedSet);
            std::unordered_set<std::string> failedSet;
            SerializeStringSet(ar, failedSet);
            std::unordered_set<std::string> tagSet;
            SerializeStringSet(ar, tagSet);

            std::unordered_map<std::string, std::string> branches;
            ar << branches;
            for (auto const& id : completedSet)
            {
                auto branchIt = branches.find(id);
                j.AddCompletedQuestID(id, branchIt != branches.end() ? branchIt->second : std::string{});
            }
            for (auto const& id : failedSet)
            {
                j.AddFailedQuestID(id);
            }
            for (auto const& tag : tagSet)
            {
                j.AddTag(tag);
            }

            std::unordered_map<std::string, f32> cooldowns;
            ar << cooldowns;
            for (const auto& [id, remaining] : cooldowns)
            {
                j.SetQuestCooldown(id, remaining);
            }

            i32 level{};
            ar << level;
            j.SetPlayerLevel(level);
            std::unordered_map<std::string, i32> reputations;
            ar << reputations;
            for (const auto& [faction, value] : reputations)
            {
                j.SetReputation(faction, value);
            }
        }
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, QuestGiverComponent& c)
    {
        ar << c.OfferedQuestIDs;
        ar << c.TurnInQuestIDs;
        ar << c.QuestMarkerIcon;
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, AbilityComponent& c)
    {
        SerializeAttributeSet(ar, c.Attributes);
        SerializeGameplayTagContainer(ar, c.OwnedTags);

        u64 abilityCount = c.Abilities.size();
        ar << abilityCount;
        if (ar.IsLoading())
            c.Abilities.resize(abilityCount);
        for (u64 i = 0; i < abilityCount; ++i)
        {
            SerializeGameplayAbilityDef(ar, c.Abilities[i].Definition);
            ar << c.Abilities[i].IsActive;
            ar << c.Abilities[i].ActiveTime;
            ar << c.Abilities[i].ChannelRemaining;
        }

        SerializeActiveEffectsContainer(ar, c.ActiveEffects);
        SerializeCooldownManager(ar, c.Cooldowns);
    }

    void SaveGameComponentSerializer::Serialize(FArchive& ar, ProgressionComponent& c)
    {
        ar << c.Level;
        ar << c.CurrentXP;
        ar << c.AttributePoints;
        ar << c.SkillPoints;
        ar << c.XPBounty;
        ar << c.HealOnLevelUp;

        SerializeStringSet(ar, c.UnlockedNodes);
        ar << c.AllocatedPoints;

        u64 curveHandle = static_cast<u64>(c.ExperienceCurveHandle);
        u64 classDbHandle = static_cast<u64>(c.ClassDatabaseHandle);
        u64 skillTreeHandle = static_cast<u64>(c.SkillTreeHandle);
        ar << curveHandle;
        ar << classDbHandle;
        ar << skillTreeHandle;
        ar << c.ClassID;
        ar << c.PendingXP;

        if (ar.IsLoading())
        {
            c.ExperienceCurveHandle = AssetHandle(curveHandle);
            c.ClassDatabaseHandle = AssetHandle(classDbHandle);
            c.SkillTreeHandle = AssetHandle(skillTreeHandle);

            // Defensive clamps: a corrupt save-file must not deliver negative
            // pools or a level below 1.
            c.Level = std::max(c.Level, 1);
            c.CurrentXP = std::max(c.CurrentXP, 0);
            c.AttributePoints = std::max(c.AttributePoints, 0);
            c.SkillPoints = std::max(c.SkillPoints, 0);
            c.XPBounty = std::max(c.XPBounty, 0);
            c.PendingXP = std::max(c.PendingXP, 0);
            std::erase_if(c.AllocatedPoints, [](const auto& entry)
                          { return entry.second <= 0; });

            // Runtime reconciliation (modifier sources, skill payloads) is
            // re-derived by ProgressionSystem on the next tick.
            c.RuntimeInitialized = false;
        }
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
        REGISTER_SAVE_COMPONENT(PhysicsJoint3DComponent);
        REGISTER_SAVE_COMPONENT(VehicleComponent);
        REGISTER_SAVE_COMPONENT(RagdollComponent);
        REGISTER_SAVE_COMPONENT(ClothComponent);
        REGISTER_SAVE_COMPONENT(TextComponent);
        REGISTER_SAVE_COMPONENT(ScriptComponent);
        REGISTER_SAVE_COMPONENT(AudioSourceComponent);
        REGISTER_SAVE_COMPONENT(AudioListenerComponent);
        REGISTER_SAVE_COMPONENT(VideoOverlayComponent);
        REGISTER_SAVE_COMPONENT(VideoSurfaceComponent);
        REGISTER_SAVE_COMPONENT(MaterialComponent);
        REGISTER_SAVE_COMPONENT(DirectionalLightComponent);
        REGISTER_SAVE_COMPONENT(PointLightComponent);
        REGISTER_SAVE_COMPONENT(SpotLightComponent);
        REGISTER_SAVE_COMPONENT(SphereAreaLightComponent);
        REGISTER_SAVE_COMPONENT(EnvironmentMapComponent);
        REGISTER_SAVE_COMPONENT(ProceduralSkyComponent);
        REGISTER_SAVE_COMPONENT(StarNestSkyComponent);
        REGISTER_SAVE_COMPONENT(TimeOfDayComponent);
        REGISTER_SAVE_COMPONENT(WeatherStateComponent);
        REGISTER_SAVE_COMPONENT(CloudscapeComponent);
        REGISTER_SAVE_COMPONENT(LightProbeComponent);
        REGISTER_SAVE_COMPONENT(LightProbeVolumeComponent);
        REGISTER_SAVE_COMPONENT(ReflectionProbeComponent);
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
        REGISTER_SAVE_COMPONENT(BuoyancyComponent);
        REGISTER_SAVE_COMPONENT(SnowDeformerComponent);
        REGISTER_SAVE_COMPONENT(VirtualMeshComponent);
        REGISTER_SAVE_COMPONENT(FluidComponent);
        REGISTER_SAVE_COMPONENT(FluidEmitterComponent);
        REGISTER_SAVE_COMPONENT(FluidKillVolumeComponent);
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
        REGISTER_SAVE_COMPONENT(SkeletonComponent);
        REGISTER_SAVE_COMPONENT(StreamingVolumeComponent);
        REGISTER_SAVE_COMPONENT(LuaScriptComponent);
        REGISTER_SAVE_COMPONENT(TileRendererComponent);
        REGISTER_SAVE_COMPONENT(DialogueComponent);
        REGISTER_SAVE_COMPONENT(NavMeshBoundsComponent);
        REGISTER_SAVE_COMPONENT(NavAgentComponent);
        REGISTER_SAVE_COMPONENT(NameplateComponent);
        REGISTER_SAVE_COMPONENT(IKTargetComponent);
        REGISTER_SAVE_COMPONENT(SpringBoneComponent);
        REGISTER_SAVE_COMPONENT(RetargetingComponent);
        REGISTER_SAVE_COMPONENT(FootIKComponent);
        REGISTER_SAVE_COMPONENT(LocomotionComponent);
        REGISTER_SAVE_COMPONENT(NoiseAnimationComponent);
        REGISTER_SAVE_COMPONENT(UIWorldAnchorComponent);
        REGISTER_SAVE_COMPONENT(MorphTargetComponent);
        REGISTER_SAVE_COMPONENT(InstancedMeshComponent);
        REGISTER_SAVE_COMPONENT(AnimationGraphComponent);
        REGISTER_SAVE_COMPONENT(CinematicComponent);
        REGISTER_SAVE_COMPONENT(BehaviorTreeComponent);
        REGISTER_SAVE_COMPONENT(StateMachineComponent);
        REGISTER_SAVE_COMPONENT(GoapAgentComponent);
        REGISTER_SAVE_COMPONENT(PerceptibleComponent);
        REGISTER_SAVE_COMPONENT(PerceptionComponent);
        REGISTER_SAVE_COMPONENT(InventoryComponent);
        REGISTER_SAVE_COMPONENT(ItemPickupComponent);
        REGISTER_SAVE_COMPONENT(ItemContainerComponent);
        REGISTER_SAVE_COMPONENT(QuestJournalComponent);
        REGISTER_SAVE_COMPONENT(QuestGiverComponent);
        REGISTER_SAVE_COMPONENT(AbilityComponent);
        REGISTER_SAVE_COMPONENT(ProgressionComponent);

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
