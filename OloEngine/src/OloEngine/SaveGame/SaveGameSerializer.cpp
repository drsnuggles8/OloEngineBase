#include "OloEnginePCH.h"
#include "SaveGameSerializer.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

namespace OloEngine
{
    // ========================================================================
    // Macro helpers for component capture/restore
    // ========================================================================

    // Sentinel value: typeHash==0 signals end of an entity's component list
    static constexpr u32 kEndOfEntityMarker = 0;

// Save: serialize a component with typeHash + dataSize for skip-ability
#define SAVE_COMPONENT(ComponentType, entity, writer)                       \
    if ((entity).HasComponent<ComponentType>())                             \
    {                                                                       \
        auto& comp = (entity).GetComponent<ComponentType>();                \
        constexpr u32 typeHash = Hash::GenerateFNVHash(#ComponentType);     \
        /* Serialize to temp buffer to measure size */                      \
        std::vector<u8> compBuf;                                            \
        {                                                                   \
            FMemoryWriter cw(compBuf);                                      \
            cw.ArIsSaveGame = true;                                         \
            SaveGameComponentSerializer::Serialize(cw, comp);               \
        }                                                                   \
        u32 dataSize = static_cast<u32>(compBuf.size());                    \
        u32 hashToWrite = typeHash;                                         \
        (writer) << hashToWrite << dataSize;                                \
        if (dataSize > 0)                                                   \
        {                                                                   \
            (writer).Serialize(compBuf.data(), static_cast<i64>(dataSize)); \
        }                                                                   \
    }

// Load: try to match typeHash and deserialize, or skip
#define LOAD_COMPONENT(ComponentType, entity, typeHashVar, dataBuf) \
    if ((typeHashVar) == Hash::GenerateFNVHash(#ComponentType))     \
    {                                                               \
        auto& comp = (entity).HasComponent<ComponentType>()         \
                         ? (entity).GetComponent<ComponentType>()   \
                         : (entity).AddComponent<ComponentType>();  \
        FMemoryReader cr(dataBuf);                                  \
        cr.ArIsSaveGame = true;                                     \
        SaveGameComponentSerializer::Serialize(cr, comp);           \
        matched = true;                                             \
    }

    // ========================================================================
    // Scene Settings serialization helpers
    // ========================================================================

    static void SerializePostProcessSettings(FArchive& ar, PostProcessSettings& s)
    {
        ar << s.Tonemap << s.Exposure << s.Gamma;
        ar << s.BloomEnabled << s.BloomThreshold << s.BloomIntensity << s.BloomIterations;
        ar << s.VignetteEnabled << s.VignetteIntensity << s.VignetteSmoothness;
        ar << s.ChromaticAberrationEnabled << s.ChromaticAberrationIntensity;
        ar << s.FXAAEnabled;
        ar << s.DOFEnabled << s.DOFFocusDistance << s.DOFFocusRange << s.DOFBokehRadius;
        ar << s.MotionBlurEnabled << s.MotionBlurStrength << s.MotionBlurSamples;
        ar << s.ColorGradingEnabled;
        ar << s.SSAOEnabled << s.SSAORadius << s.SSAOBias << s.SSAOIntensity;
        ar << s.SSAOSamples << s.SSAODebugView;
    }

    static void SerializeSnowSettings(FArchive& ar, SnowSettings& s)
    {
        ar << s.Enabled;
        ar << s.HeightStart << s.HeightFull << s.SlopeStart << s.SlopeFull;
        ar << s.Albedo << s.Roughness;
        ar << s.SSSColor << s.SSSIntensity;
        ar << s.SparkleIntensity << s.SparkleDensity << s.SparkleScale;
        ar << s.NormalPerturbStrength << s.WindDriftFactor;
        ar << s.SSSBlurEnabled << s.SSSBlurRadius << s.SSSBlurFalloff;
    }

    static void SerializeFogSettings(FArchive& ar, FogSettings& s)
    {
        ar << s.Enabled << s.Mode << s.Color << s.Density;
        ar << s.Start << s.End << s.HeightFalloff << s.HeightOffset << s.MaxOpacity;
        ar << s.EnableScattering << s.RayleighStrength << s.MieStrength << s.MieDirectionality;
        ar << s.RayleighColor << s.SunIntensity;
        ar << s.EnableVolumetric << s.VolumetricSamples << s.AbsorptionCoefficient;
        ar << s.EnableNoise << s.NoiseScale << s.NoiseSpeed << s.NoiseIntensity;
        ar << s.EnableLightShafts << s.LightShaftIntensity;
    }

    static void SerializeWindSettings(FArchive& ar, WindSettings& s)
    {
        ar << s.Enabled << s.Direction << s.Speed;
        ar << s.GustStrength << s.GustFrequency;
        ar << s.TurbulenceIntensity << s.TurbulenceScale;
    }

    static void SerializeSnowAccumulationSettings(FArchive& ar, SnowAccumulationSettings& s)
    {
        ar << s.Enabled << s.AccumulationRate << s.MaxDepth << s.MeltRate << s.RestorationRate;
        ar << s.DisplacementScale;
        ar << s.ClipmapResolution << s.ClipmapExtent << s.NumClipmapRings;
        ar << s.SnowDensity;
    }

    static void SerializeSnowEjectaSettings(FArchive& ar, SnowEjectaSettings& s)
    {
        ar << s.Enabled << s.ParticlesPerDeform << s.EjectaSpeed << s.SpeedVariance;
        ar << s.UpwardBias << s.LifetimeMin << s.LifetimeMax;
        ar << s.InitialSize << s.SizeVariance;
        ar << s.GravityScale << s.DragCoefficient;
        ar << s.Color << s.VelocityThreshold;
        ar << s.MaxParticles << s.WindInfluence;
        ar << s.NoiseStrength << s.NoiseFrequency;
        ar << s.GroundY << s.CollisionBounce << s.CollisionFriction;
    }

    static void SerializePrecipitationSettings(FArchive& ar, PrecipitationSettings& s)
    {
        ar << s.Enabled << s.Type << s.Intensity << s.TransitionSpeed;
        ar << s.BaseEmissionRate << s.MaxParticlesNearField << s.MaxParticlesFarField;
        ar << s.NearFieldExtent << s.NearFieldParticleSize << s.NearFieldSizeVariance;
        ar << s.NearFieldSpeedMin << s.NearFieldSpeedMax << s.NearFieldLifetime;
        ar << s.FarFieldExtent << s.FarFieldParticleSize;
        ar << s.FarFieldSpeedMin << s.FarFieldSpeedMax << s.FarFieldLifetime << s.FarFieldAlphaMultiplier;
        ar << s.GravityScale << s.WindInfluence << s.DragCoefficient;
        ar << s.TurbulenceStrength << s.TurbulenceFrequency;
        ar << s.GroundCollisionEnabled << s.GroundY << s.CollisionBounce << s.CollisionFriction;
        ar << s.FeedAccumulation << s.AccumulationFeedRate;
        ar << s.ScreenStreaksEnabled << s.ScreenStreakIntensity << s.ScreenStreakLength;
        ar << s.LensImpactsEnabled << s.LensImpactRate << s.LensImpactLifetime << s.LensImpactSize;
        ar << s.LODNearDistance << s.LODFarDistance << s.FrameBudgetMs;
        ar << s.ParticleColor << s.ColorVariance << s.RotationSpeed;
    }

    static void SerializeStreamingSettings(FArchive& ar, StreamingSettings& s)
    {
        ar << s.Enabled << s.DefaultLoadRadius << s.DefaultUnloadRadius;
        ar << s.MaxLoadedRegions << s.RegionDirectory;
    }

    // ========================================================================
    // CaptureSceneState
    // ========================================================================

    std::vector<u8> SaveGameSerializer::CaptureSceneState(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsSaveGame = true;

        // --- Scene settings ---
        constexpr u32 settingsMarker = 0x53455453; // "SETS"
        writer << const_cast<u32&>(settingsMarker);

        SerializePostProcessSettings(writer, scene.m_PostProcessSettings);
        SerializeSnowSettings(writer, scene.m_SnowSettings);
        SerializeFogSettings(writer, scene.m_FogSettings);
        SerializeWindSettings(writer, scene.m_WindSettings);
        SerializeSnowAccumulationSettings(writer, scene.m_SnowAccumulationSettings);
        SerializeSnowEjectaSettings(writer, scene.m_SnowEjectaSettings);
        SerializePrecipitationSettings(writer, scene.m_PrecipitationSettings);
        SerializeStreamingSettings(writer, scene.m_StreamingSettings);

        // --- Entities ---
        constexpr u32 entitiesMarker = 0x454E5453; // "ENTS"
        writer << const_cast<u32&>(entitiesMarker);

        // Count entities with IDComponent (all real entities)
        auto view = scene.GetAllEntitiesWith<IDComponent>();
        u32 entityCount = 0;
        for ([[maybe_unused]] auto entity : view)
        {
            ++entityCount;
        }
        writer << entityCount;

        // Serialize each entity
        for (auto entityHandle : view)
        {
            Entity entity = { entityHandle, &scene };

            // Write UUID first (always present via IDComponent)
            auto& id = entity.GetComponent<IDComponent>();
            UUID uuid = id.ID;
            writer << uuid;

            // Serialize all present components
            SAVE_COMPONENT(IDComponent, entity, writer);
            SAVE_COMPONENT(TagComponent, entity, writer);
            SAVE_COMPONENT(PrefabComponent, entity, writer);
            SAVE_COMPONENT(TransformComponent, entity, writer);
            SAVE_COMPONENT(RelationshipComponent, entity, writer);
            SAVE_COMPONENT(SpriteRendererComponent, entity, writer);
            SAVE_COMPONENT(CircleRendererComponent, entity, writer);
            SAVE_COMPONENT(CameraComponent, entity, writer);
            SAVE_COMPONENT(Rigidbody2DComponent, entity, writer);
            SAVE_COMPONENT(BoxCollider2DComponent, entity, writer);
            SAVE_COMPONENT(CircleCollider2DComponent, entity, writer);
            SAVE_COMPONENT(Rigidbody3DComponent, entity, writer);
            SAVE_COMPONENT(BoxCollider3DComponent, entity, writer);
            SAVE_COMPONENT(SphereCollider3DComponent, entity, writer);
            SAVE_COMPONENT(CapsuleCollider3DComponent, entity, writer);
            SAVE_COMPONENT(MeshCollider3DComponent, entity, writer);
            SAVE_COMPONENT(ConvexMeshCollider3DComponent, entity, writer);
            SAVE_COMPONENT(TriangleMeshCollider3DComponent, entity, writer);
            SAVE_COMPONENT(CharacterController3DComponent, entity, writer);
            SAVE_COMPONENT(TextComponent, entity, writer);
            SAVE_COMPONENT(ScriptComponent, entity, writer);
            SAVE_COMPONENT(AudioSourceComponent, entity, writer);
            SAVE_COMPONENT(AudioListenerComponent, entity, writer);
            SAVE_COMPONENT(MaterialComponent, entity, writer);
            SAVE_COMPONENT(DirectionalLightComponent, entity, writer);
            SAVE_COMPONENT(PointLightComponent, entity, writer);
            SAVE_COMPONENT(SpotLightComponent, entity, writer);
            SAVE_COMPONENT(EnvironmentMapComponent, entity, writer);
            SAVE_COMPONENT(LightProbeComponent, entity, writer);
            SAVE_COMPONENT(LightProbeVolumeComponent, entity, writer);
            SAVE_COMPONENT(UICanvasComponent, entity, writer);
            SAVE_COMPONENT(UIRectTransformComponent, entity, writer);
            SAVE_COMPONENT(UIImageComponent, entity, writer);
            SAVE_COMPONENT(UIPanelComponent, entity, writer);
            SAVE_COMPONENT(UITextComponent, entity, writer);
            SAVE_COMPONENT(UIButtonComponent, entity, writer);
            SAVE_COMPONENT(UISliderComponent, entity, writer);
            SAVE_COMPONENT(UICheckboxComponent, entity, writer);
            SAVE_COMPONENT(UIProgressBarComponent, entity, writer);
            SAVE_COMPONENT(UIInputFieldComponent, entity, writer);
            SAVE_COMPONENT(UIScrollViewComponent, entity, writer);
            SAVE_COMPONENT(UIDropdownComponent, entity, writer);
            SAVE_COMPONENT(UIGridLayoutComponent, entity, writer);
            SAVE_COMPONENT(UIToggleComponent, entity, writer);
            SAVE_COMPONENT(ParticleSystemComponent, entity, writer);
            SAVE_COMPONENT(TerrainComponent, entity, writer);
            SAVE_COMPONENT(FoliageComponent, entity, writer);
            SAVE_COMPONENT(WaterComponent, entity, writer);
            SAVE_COMPONENT(SnowDeformerComponent, entity, writer);
            SAVE_COMPONENT(FogVolumeComponent, entity, writer);
            SAVE_COMPONENT(DecalComponent, entity, writer);
            SAVE_COMPONENT(LODGroupComponent, entity, writer);
            SAVE_COMPONENT(NetworkIdentityComponent, entity, writer);
            SAVE_COMPONENT(NetworkInterestComponent, entity, writer);
            SAVE_COMPONENT(PhaseComponent, entity, writer);
            SAVE_COMPONENT(InstancePortalComponent, entity, writer);
            SAVE_COMPONENT(NetworkLODComponent, entity, writer);
            SAVE_COMPONENT(SubmeshComponent, entity, writer);
            SAVE_COMPONENT(MeshComponent, entity, writer);
            SAVE_COMPONENT(ModelComponent, entity, writer);
            SAVE_COMPONENT(AnimationStateComponent, entity, writer);
            SAVE_COMPONENT(StreamingVolumeComponent, entity, writer);

            // End-of-entity sentinel
            u32 endMarker = kEndOfEntityMarker;
            writer << endMarker;
        }

        return buffer;
    }

    // ========================================================================
    // RestoreSceneState
    // ========================================================================

    bool SaveGameSerializer::RestoreSceneState(Scene& scene, const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.empty())
        {
            return false;
        }

        FMemoryReader reader(data);
        reader.ArIsSaveGame = true;

        // --- Scene settings ---
        u32 settingsMarker = 0;
        reader << settingsMarker;
        if (settingsMarker != 0x53455453)
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Invalid settings marker");
            return false;
        }

        SerializePostProcessSettings(reader, scene.m_PostProcessSettings);
        SerializeSnowSettings(reader, scene.m_SnowSettings);
        SerializeFogSettings(reader, scene.m_FogSettings);
        SerializeWindSettings(reader, scene.m_WindSettings);
        SerializeSnowAccumulationSettings(reader, scene.m_SnowAccumulationSettings);
        SerializeSnowEjectaSettings(reader, scene.m_SnowEjectaSettings);
        SerializePrecipitationSettings(reader, scene.m_PrecipitationSettings);
        SerializeStreamingSettings(reader, scene.m_StreamingSettings);

        // --- Entities ---
        u32 entitiesMarker = 0;
        reader << entitiesMarker;
        if (entitiesMarker != 0x454E5453)
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Invalid entities marker");
            return false;
        }

        // Destroy all existing entities (children before parents)
        {
            auto allEntities = scene.GetAllEntitiesWith<IDComponent>();
            std::vector<Entity> toDestroy;
            for (auto e : allEntities)
            {
                toDestroy.emplace_back(e, &scene);
            }

            // Sort by depth: entities with parents come first (children before parents)
            auto getDepth = [](Entity& ent) -> u32
            {
                u32 depth = 0;
                Entity current = ent;
                while (current.GetParentUUID() != UUID(0))
                {
                    ++depth;
                    current = current.GetParent();
                    if (!current)
                    {
                        break;
                    }
                }
                return depth;
            };

            std::sort(toDestroy.begin(), toDestroy.end(),
                      [&getDepth](Entity& a, Entity& b)
                      {
                          return getDepth(a) > getDepth(b);
                      });

            for (auto& entity : toDestroy)
            {
                scene.DestroyEntity(entity);
            }
        }

        u32 entityCount = 0;
        reader << entityCount;

        for (u32 i = 0; i < entityCount; ++i)
        {
            UUID uuid;
            reader << uuid;

            // Create entity with the saved UUID
            Entity entity = scene.CreateEntityWithUUID(uuid, "");

            // Read component blocks until end-of-entity sentinel (typeHash == 0)
            while (!reader.AtEnd())
            {
                u32 typeHash = 0;
                reader << typeHash;

                // End-of-entity sentinel
                if (typeHash == kEndOfEntityMarker)
                {
                    break;
                }

                u32 dataSize = 0;
                reader << dataSize;

                // Sanity limit
                if (reader.IsError() || dataSize > 1024 * 1024 * 100)
                {
                    OLO_CORE_ERROR("[SaveGameSerializer] Corrupt component block at entity {}", i);
                    return false;
                }

                // Read component data blob
                std::vector<u8> compData(dataSize);
                if (dataSize > 0)
                {
                    reader.Serialize(compData.data(), static_cast<i64>(dataSize));
                }

                if (reader.IsError())
                {
                    OLO_CORE_ERROR("[SaveGameSerializer] Read error at entity {}", i);
                    return false;
                }

                // Match typeHash to component and deserialize
                bool matched = false;

#define TRY_LOAD_COMPONENT(ComponentType) \
    if (!matched)                         \
    LOAD_COMPONENT(ComponentType, entity, typeHash, compData)

                LOAD_COMPONENT(IDComponent, entity, typeHash, compData);
                TRY_LOAD_COMPONENT(TagComponent);
                TRY_LOAD_COMPONENT(PrefabComponent);
                TRY_LOAD_COMPONENT(TransformComponent);
                TRY_LOAD_COMPONENT(RelationshipComponent);
                TRY_LOAD_COMPONENT(SpriteRendererComponent);
                TRY_LOAD_COMPONENT(CircleRendererComponent);
                TRY_LOAD_COMPONENT(CameraComponent);
                TRY_LOAD_COMPONENT(Rigidbody2DComponent);
                TRY_LOAD_COMPONENT(BoxCollider2DComponent);
                TRY_LOAD_COMPONENT(CircleCollider2DComponent);
                TRY_LOAD_COMPONENT(Rigidbody3DComponent);
                TRY_LOAD_COMPONENT(BoxCollider3DComponent);
                TRY_LOAD_COMPONENT(SphereCollider3DComponent);
                TRY_LOAD_COMPONENT(CapsuleCollider3DComponent);
                TRY_LOAD_COMPONENT(MeshCollider3DComponent);
                TRY_LOAD_COMPONENT(ConvexMeshCollider3DComponent);
                TRY_LOAD_COMPONENT(TriangleMeshCollider3DComponent);
                TRY_LOAD_COMPONENT(CharacterController3DComponent);
                TRY_LOAD_COMPONENT(TextComponent);
                TRY_LOAD_COMPONENT(ScriptComponent);
                TRY_LOAD_COMPONENT(AudioSourceComponent);
                TRY_LOAD_COMPONENT(AudioListenerComponent);
                TRY_LOAD_COMPONENT(MaterialComponent);
                TRY_LOAD_COMPONENT(DirectionalLightComponent);
                TRY_LOAD_COMPONENT(PointLightComponent);
                TRY_LOAD_COMPONENT(SpotLightComponent);
                TRY_LOAD_COMPONENT(EnvironmentMapComponent);
                TRY_LOAD_COMPONENT(LightProbeComponent);
                TRY_LOAD_COMPONENT(LightProbeVolumeComponent);
                TRY_LOAD_COMPONENT(UICanvasComponent);
                TRY_LOAD_COMPONENT(UIRectTransformComponent);
                TRY_LOAD_COMPONENT(UIImageComponent);
                TRY_LOAD_COMPONENT(UIPanelComponent);
                TRY_LOAD_COMPONENT(UITextComponent);
                TRY_LOAD_COMPONENT(UIButtonComponent);
                TRY_LOAD_COMPONENT(UISliderComponent);
                TRY_LOAD_COMPONENT(UICheckboxComponent);
                TRY_LOAD_COMPONENT(UIProgressBarComponent);
                TRY_LOAD_COMPONENT(UIInputFieldComponent);
                TRY_LOAD_COMPONENT(UIScrollViewComponent);
                TRY_LOAD_COMPONENT(UIDropdownComponent);
                TRY_LOAD_COMPONENT(UIGridLayoutComponent);
                TRY_LOAD_COMPONENT(UIToggleComponent);
                TRY_LOAD_COMPONENT(ParticleSystemComponent);
                TRY_LOAD_COMPONENT(TerrainComponent);
                TRY_LOAD_COMPONENT(FoliageComponent);
                TRY_LOAD_COMPONENT(WaterComponent);
                TRY_LOAD_COMPONENT(SnowDeformerComponent);
                TRY_LOAD_COMPONENT(FogVolumeComponent);
                TRY_LOAD_COMPONENT(DecalComponent);
                TRY_LOAD_COMPONENT(LODGroupComponent);
                TRY_LOAD_COMPONENT(NetworkIdentityComponent);
                TRY_LOAD_COMPONENT(NetworkInterestComponent);
                TRY_LOAD_COMPONENT(PhaseComponent);
                TRY_LOAD_COMPONENT(InstancePortalComponent);
                TRY_LOAD_COMPONENT(NetworkLODComponent);
                TRY_LOAD_COMPONENT(SubmeshComponent);
                TRY_LOAD_COMPONENT(MeshComponent);
                TRY_LOAD_COMPONENT(ModelComponent);
                TRY_LOAD_COMPONENT(AnimationStateComponent);
                TRY_LOAD_COMPONENT(StreamingVolumeComponent);

#undef TRY_LOAD_COMPONENT

                // Unknown component types are silently skipped (forward compatible)
            }
        }

        return !reader.IsError();
    }

#undef SAVE_COMPONENT
#undef LOAD_COMPONENT

} // namespace OloEngine
