#include "OloEnginePCH.h"
#include "SaveGameSerializer.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Cinematic/CinematicComponent.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/SaveGame/ISaveable.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/ModelImporter.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <box2d/box2d.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <variant>

namespace OloEngine
{
    // ========================================================================
    // Macro helpers for component capture/restore
    // ========================================================================

    // Sentinel value: typeHash==0 signals end of an entity's component list
    static constexpr u32 kEndOfEntityMarker = 0;

    // Reserved typeHash for ISaveable custom state blob
    static constexpr u32 kSaveableTypeHash = 0x53415645; // "SAVE"

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
            if (cw.IsError())                                               \
            {                                                               \
                (writer).SetError();                                        \
                break;                                                      \
            }                                                               \
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
#define LOAD_COMPONENT(ComponentType, entity, typeHashVar, dataBuf)                                              \
    if ((typeHashVar) == Hash::GenerateFNVHash(#ComponentType))                                                  \
    {                                                                                                            \
        bool wasAdded = !(entity).HasComponent<ComponentType>();                                                 \
        auto& comp = wasAdded                                                                                    \
                         ? (entity).AddComponent<ComponentType>()                                                \
                         : (entity).GetComponent<ComponentType>();                                               \
        FMemoryReader cr(dataBuf);                                                                               \
        cr.ArIsSaveGame = true;                                                                                  \
        SaveGameComponentSerializer::Serialize(cr, comp);                                                        \
        if (cr.IsError() || cr.Tell() != static_cast<i64>((dataBuf).size()))                                     \
        {                                                                                                        \
            OLO_CORE_WARN("[SaveGameSerializer] Incomplete deserialization of " #ComponentType " ({}/{} bytes)", \
                          cr.Tell(), (dataBuf).size());                                                          \
            if (wasAdded)                                                                                        \
            {                                                                                                    \
                (entity).RemoveComponent<ComponentType>();                                                       \
            }                                                                                                    \
            loadFailed = true;                                                                                   \
        }                                                                                                        \
        else                                                                                                     \
        {                                                                                                        \
            matched = true;                                                                                      \
        }                                                                                                        \
    }

    // ========================================================================
    // Scene Settings serialization helpers
    // ========================================================================

    static void SerializePostProcessSettings(FArchive& ar, PostProcessSettings& s)
    {
        OLO_PROFILE_FUNCTION();
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
        // Auto-exposure / eye adaptation (save format v2+).
        ar << s.AutoExposureEnabled;
        ar << s.AutoExposureMinLogLuminance << s.AutoExposureMaxLogLuminance;
        ar << s.AutoExposureSpeedUp << s.AutoExposureSpeedDown;
        ar << s.AutoExposureCompensation;
        ar << s.AutoExposureMinExposure << s.AutoExposureMaxExposure;
        if (ar.IsLoading())
        {
            // Floats read from disk must be finite and ordered (CLAUDE.md
            // float-validation rule); shared with the scene-YAML loader.
            SanitizeAutoExposure(s);
        }
    }

    static void SerializeSnowSettings(FArchive& ar, SnowSettings& s)
    {
        OLO_PROFILE_FUNCTION();
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
        OLO_PROFILE_FUNCTION();
        ar << s.Enabled << s.Mode << s.Color << s.Density;
        ar << s.Start << s.End << s.HeightFalloff << s.HeightOffset << s.MaxOpacity;
        ar << s.EnableScattering << s.RayleighStrength << s.MieStrength << s.MieDirectionality;
        ar << s.RayleighColor << s.SunIntensity;
        ar << s.EnableVolumetric << s.VolumetricSamples << s.AbsorptionCoefficient;
        ar << s.EnableNoise << s.NoiseScale << s.NoiseSpeed << s.NoiseIntensity;
        ar << s.EnableLightShafts << s.LightShaftIntensity;
        // NOT SERIALIZED, deliberately: FogSettings' three volumetric
        // self-shadow fields (issue #723). They are authored scene state,
        // persisted by SceneSerializer, and RestoreSceneState seeds its settings
        // structs from the LIVE scene, so a field this archive does not carry
        // keeps the value the scene was loaded with.
    }

    static void SerializeWindSettings(FArchive& ar, WindSettings& s)
    {
        OLO_PROFILE_FUNCTION();
        ar << s.Enabled << s.Direction << s.Speed;
        ar << s.GustStrength << s.GustFrequency;
        ar << s.TurbulenceIntensity << s.TurbulenceScale;
    }

    static void SerializeSnowAccumulationSettings(FArchive& ar, SnowAccumulationSettings& s)
    {
        OLO_PROFILE_FUNCTION();
        ar << s.Enabled << s.AccumulationRate << s.MaxDepth << s.MeltRate << s.RestorationRate;
        ar << s.DisplacementScale;
        ar << s.ClipmapResolution << s.ClipmapExtent << s.NumClipmapRings;
        ar << s.SnowDensity;
    }

    static void SerializeSnowEjectaSettings(FArchive& ar, SnowEjectaSettings& s)
    {
        OLO_PROFILE_FUNCTION();
        ar << s.Enabled << s.ParticlesPerDeform << s.EjectaSpeed << s.SpeedVariance;
        ar << s.UpwardBias << s.LifetimeMin << s.LifetimeMax;
        ar << s.InitialSize << s.SizeVariance;
        ar << s.GravityScale << s.DragCoefficient;
        ar << s.Color << s.VelocityThreshold;
        ar << s.MaxParticles << s.WindInfluence;
        ar << s.NoiseStrength << s.NoiseFrequency;
        ar << s.GroundY << s.CollisionBounce << s.CollisionFriction;
    }

    static void SerializeSkinDiffusionSettings(FArchive& ar, SkinDiffusionSettings& s)
    {
        OLO_PROFILE_FUNCTION();
        ar << s.Enabled << s.Quality << s.DepthRejectionScale << s.RadiusScale;
        if (ar.IsLoading())
        {
            // Untrusted input: an unknown tier and a non-finite or wild float
            // fall back to the default or clamp, as in the scene YAML loader.
            if (!IsValidSkinDiffusionQuality(static_cast<i32>(s.Quality)))
            {
                s.Quality = SkinDiffusionQuality::Medium;
            }
            s.DepthRejectionScale = std::isfinite(s.DepthRejectionScale) ? std::clamp(s.DepthRejectionScale, 0.1f, 8.0f) : 2.0f;
            s.RadiusScale = std::isfinite(s.RadiusScale) ? std::clamp(s.RadiusScale, 0.0f, 4.0f) : 1.0f;
        }
    }

    static void SerializePrecipitationSettings(FArchive& ar, PrecipitationSettings& s)
    {
        OLO_PROFILE_FUNCTION();
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
        OLO_PROFILE_FUNCTION();
        ar << s.Enabled << s.DefaultLoadRadius << s.DefaultUnloadRadius;
        ar << s.MaxLoadedRegions << s.RegionDirectory;
        ar << s.MaxResidentMegabytes << s.MaxAdmittedMegabytesPerFrame; // #1365 byte budgets
        ar << s.RepresentationResidentMegabytes << s.RepresentationUploadMegabytesPerFrame << s.RepresentationStagingMegabytes;
        if (ar.IsLoading())
        {
            // A save file is untrusted input: a NaN, infinite or negative budget is
            // "no budget", exactly as the scene YAML loader treats it.
            s.MaxResidentMegabytes = SanitizeStreamingBudgetMegabytes(s.MaxResidentMegabytes);
            s.MaxAdmittedMegabytesPerFrame = SanitizeStreamingBudgetMegabytes(s.MaxAdmittedMegabytesPerFrame);
            s.RepresentationResidentMegabytes = SanitizeStreamingBudgetMegabytes(s.RepresentationResidentMegabytes);
            s.RepresentationUploadMegabytesPerFrame = SanitizeStreamingBudgetMegabytes(s.RepresentationUploadMegabytesPerFrame);
            s.RepresentationStagingMegabytes = SanitizeStreamingBudgetMegabytes(s.RepresentationStagingMegabytes);
        }
    }

    // The scene's dialogue variables: the flags and counters scripts set and
    // dialogue condition nodes read. Without them a Continue reset every branch
    // a conversation had unlocked. Written sorted by key so a save is
    // byte-stable; each value carries its variant index.
    static void SerializeDialogueVariables(FArchive& ar, DialogueVariables& variables)
    {
        OLO_PROFILE_FUNCTION();
        static constexpr u32 kMaxVariables = 65'536;
        enum : u8
        {
            kBool = 0,
            kInt = 1,
            kFloat = 2,
            kString = 3
        };
        static_assert(std::is_same_v<std::variant_alternative_t<kBool, DialogueVariableValue>, bool> &&
                          std::is_same_v<std::variant_alternative_t<kInt, DialogueVariableValue>, i32> &&
                          std::is_same_v<std::variant_alternative_t<kFloat, DialogueVariableValue>, f32> &&
                          std::is_same_v<std::variant_alternative_t<kString, DialogueVariableValue>, FString>,
                      "the save writes DialogueVariableValue's variant index");

        if (ar.IsLoading())
        {
            u32 count = 0;
            ar << count;
            if (ar.IsError() || count > kMaxVariables)
            {
                ar.SetError();
                return;
            }
            DialogueVariables loaded;
            for (u32 i = 0; i < count; ++i)
            {
                std::string key;
                u8 type = 0;
                ar << key << type;
                if (ar.IsError())
                    return;
                switch (type)
                {
                    case kBool:
                    {
                        bool value = false;
                        ar << value;
                        loaded.SetBool(key, value);
                        break;
                    }
                    case kInt:
                    {
                        i32 value = 0;
                        ar << value;
                        loaded.SetInt(key, value);
                        break;
                    }
                    case kFloat:
                    {
                        f32 value = 0.0f;
                        ar << value;
                        loaded.SetFloat(key, std::isfinite(value) ? value : 0.0f);
                        break;
                    }
                    case kString:
                    {
                        std::string value;
                        ar << value;
                        loaded.SetString(key, value);
                        break;
                    }
                    default:
                        ar.SetError();
                        return;
                }
                if (ar.IsError())
                    return;
            }
            variables = std::move(loaded);
            return;
        }

        if (variables.GetAll().size() > kMaxVariables)
        {
            ar.SetError();
            return;
        }
        TArray<const std::pair<const std::string, DialogueVariableValue>*> entries;
        entries.Reserve(static_cast<i32>(variables.GetAll().size()));
        for (const auto& entry : variables.GetAll())
            entries.Add(&entry);
        std::ranges::sort(entries, {}, [](const auto* entry)
                          { return std::string_view(entry->first); });
        u32 count = static_cast<u32>(entries.Num());
        ar << count;
        for (const auto* entry : entries)
        {
            std::string key = entry->first;
            u8 type = static_cast<u8>(entry->second.index());
            ar << key << type;
            std::visit([&ar](const auto& value)
                       {
                           auto copy = value;
                           ar << copy; },
                       entry->second);
        }
    }

    // ========================================================================
    // Framed settings blocks
    // ========================================================================

    // Each scene-settings struct is written as {u32 byteCount, payload}, the same
    // framing a component block has. The reader hands the payload to a bounded
    // sub-archive and requires it to be consumed exactly, so a truncated save, or
    // a struct whose writer and reader disagree by one field, fails naming the
    // struct instead of shifting every later read (#1498).
    template<typename SerializeFn>
    static void WriteSettingsBlock(FMemoryWriter& writer, SerializeFn&& serialize)
    {
        std::vector<u8> block;
        {
            FMemoryWriter blockWriter(block);
            blockWriter.ArIsSaveGame = true;
            serialize(blockWriter);
            if (blockWriter.IsError())
            {
                // A block that refused to write (e.g. more dialogue variables
                // than the reader accepts) fails the whole capture, as a
                // component block does, instead of saving a file no load reads.
                writer.SetError();
                return;
            }
        }
        u32 byteCount = static_cast<u32>(block.size());
        writer << byteCount;
        if (byteCount > 0)
        {
            writer.Serialize(block.data(), static_cast<i64>(byteCount));
        }
    }

    template<typename SerializeFn>
    [[nodiscard]] static bool ReadSettingsBlock(FMemoryReader& reader, const char* name, SerializeFn&& serialize)
    {
        u32 byteCount = 0;
        reader << byteCount;
        // The declared length is bounded by what the reader still holds, so a corrupt length
        // fails here instead of allocating it; the writer puts no cap on a block's size.
        if (reader.IsError() || static_cast<i64>(byteCount) > reader.TotalSize() - reader.Tell())
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Settings block '{}' declares {} bytes, more than the save holds "
                           "(truncated or corrupt save)",
                           name, byteCount);
            return false;
        }
        std::vector<u8> block(byteCount);
        if (byteCount > 0)
        {
            reader.Serialize(block.data(), static_cast<i64>(byteCount));
        }
        if (reader.IsError())
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Settings block '{}' is truncated: {} bytes declared", name, byteCount);
            return false;
        }
        FMemoryReader blockReader(block);
        blockReader.ArIsSaveGame = true;
        serialize(blockReader);
        if (blockReader.IsError() || blockReader.Tell() != static_cast<i64>(byteCount))
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Settings block '{}' read {} of {} bytes; the save does not match "
                           "this build's layout",
                           name, blockReader.Tell(), byteCount);
            return false;
        }
        return true;
    }

    // ========================================================================
    // CaptureSceneState
    // ========================================================================

    std::vector<u8> SaveGameSerializer::CaptureSceneState(Scene& scene, u32* outEntityCount)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsSaveGame = true;

        // --- Scene settings ---
        u32 settingsMarker = 0x53455453; // "SETS"
        writer << settingsMarker;

        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializePostProcessSettings(ar, scene.m_PostProcessSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeSnowSettings(ar, scene.m_SnowSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeFogSettings(ar, scene.m_FogSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeWindSettings(ar, scene.m_WindSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeSnowAccumulationSettings(ar, scene.m_SnowAccumulationSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeSnowEjectaSettings(ar, scene.m_SnowEjectaSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeSkinDiffusionSettings(ar, scene.m_SkinDiffusionSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializePrecipitationSettings(ar, scene.m_PrecipitationSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeStreamingSettings(ar, scene.m_StreamingSettings); });
        WriteSettingsBlock(writer, [&scene](FArchive& ar)
                           { SerializeDialogueVariables(ar, scene.m_DialogueVariables); });
        if (writer.IsError())
        {
            // A settings block refused to write. Stop before the entity pass and
            // its script ISaveable callbacks run for a save that cannot happen.
            return {};
        }

        // --- Entities ---
        u32 entitiesMarker = 0x454E5453; // "ENTS"
        writer << entitiesMarker;

        // Snapshot runtime physics velocities into component fields before serialization
        // 2D: read from Box2D runtime bodies
        if (b2World_IsValid(scene.m_PhysicsWorld))
        {
            auto rb2dView = scene.m_Registry.view<Rigidbody2DComponent>();
            for (auto e : rb2dView)
            {
                auto& rb2d = rb2dView.get<Rigidbody2DComponent>(e);
                if (b2Body_IsValid(rb2d.RuntimeBody))
                {
                    b2Vec2 linVel = b2Body_GetLinearVelocity(rb2d.RuntimeBody);
                    rb2d.LinearVelocity = { linVel.x, linVel.y };
                    rb2d.AngularVelocity = b2Body_GetAngularVelocity(rb2d.RuntimeBody);
                }
            }
        }
        // 3D: read from Jolt runtime bodies into temporaries, then write to component,
        // serialize, and restore the original "initial" values afterwards.
        struct Rigidbody3DVelocityBackup
        {
            entt::entity Entity;
            glm::vec3 LinearVelocity;
            glm::vec3 AngularVelocity;
        };
        std::vector<Rigidbody3DVelocityBackup> rb3dBackups;
        if (auto* joltScene = scene.GetPhysicsScene(); joltScene && joltScene->IsInitialized())
        {
            auto rb3dView = scene.m_Registry.view<Rigidbody3DComponent>();
            for (auto e : rb3dView)
            {
                auto& rb3d = rb3dView.get<Rigidbody3DComponent>(e);
                Entity ent = { e, &scene };
                if (auto body = joltScene->GetBody(ent))
                {
                    rb3dBackups.push_back({ e, rb3d.m_InitialLinearVelocity, rb3d.m_InitialAngularVelocity });
                    rb3d.m_InitialLinearVelocity = body->GetLinearVelocity();
                    rb3d.m_InitialAngularVelocity = body->GetAngularVelocity();
                }
            }
        }

        // Runtime-owned entities (the dialogue UI, captions, debris…) are rebuilt
        // by their systems after Continue, so the save never carries them, nor
        // anything parented under them (#1712): the same set Scene::IsRuntimeOwned
        // answers for. Collected downward from the marked roots through child
        // lists; then any entity whose parent link names a collected entity
        // joins too, so a child missing from its parent's list is not saved as
        // an orphan. No marked entity, no work.
        auto view = scene.GetAllEntitiesWith<IDComponent>();
        std::unordered_set<u64> runtimeOwned;
        {
            TArray<UUID> pending;
            const auto collectSubtrees = [&scene, &runtimeOwned, &pending]
            {
                while (!pending.IsEmpty())
                {
                    const UUID id = pending.Last();
                    pending.Pop(EAllowShrinking::No);
                    const auto owned = scene.TryGetEntityWithUUID(id);
                    // A dangling child UUID names no entity and is not counted;
                    // the insert doubles as the guard against a corrupt cycle.
                    if (!owned || !runtimeOwned.insert(static_cast<u64>(id)).second)
                        continue;
                    if (owned->HasComponent<RelationshipComponent>())
                        pending.Append(owned->GetComponent<RelationshipComponent>().m_Children);
                }
            };
            for (auto handle : scene.m_Registry.view<RuntimeOwnedTag>())
                pending.Add(scene.m_Registry.get<IDComponent>(handle).ID);
            collectSubtrees();
            for (bool grew = !runtimeOwned.empty(); grew;)
            {
                grew = false;
                for (auto [handle, id, relationship] : scene.m_Registry.view<IDComponent, RelationshipComponent>().each())
                {
                    if (static_cast<u64>(relationship.m_ParentHandle) != 0 &&
                        runtimeOwned.contains(static_cast<u64>(relationship.m_ParentHandle)) &&
                        !runtimeOwned.contains(static_cast<u64>(id.ID)))
                    {
                        pending.Add(id.ID);
                        grew = true;
                    }
                }
                collectSubtrees();
            }
        }
        const auto isRuntimeOwned = [&runtimeOwned](UUID child)
        {
            return runtimeOwned.contains(static_cast<u64>(child));
        };

        // A saved parent's child list is written without its runtime-owned
        // children, so the saved hierarchy names no missing entity. The live
        // list is put back as soon as that entity's components are written,
        // before any script callback can observe it.
        struct ScrubbedChildList
        {
            RelationshipComponent* Component = nullptr;
            TArray<UUID> Live;
            ScrubbedChildList() = default;
            ScrubbedChildList(const ScrubbedChildList&) = delete;
            ScrubbedChildList& operator=(const ScrubbedChildList&) = delete;
            ~ScrubbedChildList()
            {
                if (Component)
                    Component->m_Children = MoveTemp(Live);
            }
        };

        u32 entityCount = static_cast<u32>(view.size() - runtimeOwned.size());
        writer << entityCount;
        if (outEntityCount)
            *outEntityCount = entityCount;

        // Serialize each entity
        for (auto entityHandle : view)
        {
            Entity entity = { entityHandle, &scene };
            if (runtimeOwned.contains(static_cast<u64>(entity.GetUUID())))
            {
                continue;
            }

            // Write UUID first (always present via IDComponent)
            const auto& id = entity.GetComponent<IDComponent>();
            UUID uuid = id.ID;
            writer << uuid;

            {
                ScrubbedChildList scrubbed;
                if (!runtimeOwned.empty() && entity.HasComponent<RelationshipComponent>())
                {
                    auto& relationship = entity.GetComponent<RelationshipComponent>();
                    if (std::ranges::any_of(relationship.m_Children, isRuntimeOwned))
                    {
                        scrubbed.Component = &relationship;
                        scrubbed.Live = relationship.m_Children;
                        relationship.m_Children.RemoveAll(isRuntimeOwned);
                    }
                }

                // Serialize all present components. The SAVE_COMPONENT(...) enumeration
                // is generated by OloHeaderTool from every `struct *Component` with a
                // save serializer (see SaveGameComponentSerializer.cpp) — DO NOT hand-
                // edit. Add a Serialize() overload + REGISTER_SAVE_COMPONENT and the
                // next GenerateBindings run adds the component here automatically.
                // Guarded by SaveGameComponentSerializerCoverageTest.
#include "OloEngine/SaveGame/Generated/SaveGameComponentCapture.Generated.inl"
            }

            // ISaveable blob for ScriptComponent custom state
            if (entity.HasComponent<ScriptComponent>())
            {
                const auto& sc = entity.GetComponent<ScriptComponent>();
                std::vector<u8> saveableBlob;
                if (!CollectSaveableData(uuid, sc.ClassName, saveableBlob))
                {
                    OLO_CORE_ERROR("[SaveGameSerializer] CollectSaveableData failed for entity {}", static_cast<u64>(uuid));
                    writer.SetError();
                    break;
                }
                if (!saveableBlob.empty())
                {
                    u32 saveableHash = kSaveableTypeHash;
                    u32 dataSize = static_cast<u32>(saveableBlob.size());
                    writer << saveableHash << dataSize;
                    writer.Serialize(saveableBlob.data(), static_cast<i64>(dataSize));
                }
            }

            // End-of-entity sentinel
            u32 endMarker = kEndOfEntityMarker;
            writer << endMarker;
        }

        // Restore original initial velocities for 3D rigidbodies
        for (auto const& backup : rb3dBackups)
        {
            auto& rb3d = scene.m_Registry.get<Rigidbody3DComponent>(backup.Entity);
            rb3d.m_InitialLinearVelocity = backup.LinearVelocity;
            rb3d.m_InitialAngularVelocity = backup.AngularVelocity;
        }

        // If serialization failed, return empty buffer so callers detect the error
        if (writer.IsError())
        {
            return {};
        }

        return buffer;
    }

    // ========================================================================
    // RestoreSceneState
    // ========================================================================

    // Deserialize entities into a staging scene. Returns whether parsing succeeded.
    // Deferred ISaveable restore entry — collected during staging, applied post-commit.
    struct DeferredSaveableEntry
    {
        UUID EntityID;
        std::string ClassName;
        std::vector<u8> Data;
    };

    static std::string_view MeshImportPath(Entity entity)
    {
        return entity.HasComponent<AnimationStateComponent>() ? std::string_view(entity.GetComponent<AnimationStateComponent>().m_SourceFilePath) : std::string_view{};
    }

    // A bind-pose-only import made before #1715 has no saved source path.
    // Its UUID and the authored clipless rig are its surviving identity.
    static bool IsLegacyBindPose(Entity saved, Entity authored)
    {
        return !saved.HasComponent<AnimationStateComponent>() &&
               saved.HasComponent<SkeletonComponent>() && authored.HasComponent<SkeletonComponent>() &&
               authored.GetComponent<SkeletonComponent>().m_Skeleton &&
               (!authored.HasComponent<AnimationStateComponent>() ||
                authored.GetComponent<AnimationStateComponent>().m_AvailableClips.empty());
    }

    static bool SameMaterialOwner(Entity saved, Entity authored)
    {
        if (saved.HasComponent<ModelComponent>() != authored.HasComponent<ModelComponent>() ||
            saved.HasComponent<MeshComponent>() != authored.HasComponent<MeshComponent>())
            return false;
        if (saved.HasComponent<ModelComponent>() &&
            !FileSystem::SameSourcePath(saved.GetComponent<ModelComponent>().m_FilePath, authored.GetComponent<ModelComponent>().m_FilePath))
            return false;
        if (saved.HasComponent<MeshComponent>() &&
            saved.GetComponent<MeshComponent>().m_Primitive != authored.GetComponent<MeshComponent>().m_Primitive)
            return false;
        return FileSystem::SameSourcePath(MeshImportPath(saved), MeshImportPath(authored)) || IsLegacyBindPose(saved, authored);
    }

    static bool DeserializeEntitiesInto(Scene& staging, const Scene& authoredScene, FMemoryReader& reader,
                                        std::vector<DeferredSaveableEntry>& outDeferredSaveables)
    {
        OLO_PROFILE_FUNCTION();

        u32 entityCount = 0;
        reader << entityCount;
        if (reader.IsError())
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Failed to read entity count");
            return false;
        }

        if (static constexpr u32 kMaxEntityCount = 1'000'000; entityCount > kMaxEntityCount)
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Entity count {} exceeds maximum {}", entityCount, kMaxEntityCount);
            return false;
        }

        for (u32 i = 0; i < entityCount; ++i)
        {
            UUID uuid;
            reader << uuid;
            if (reader.IsError())
            {
                OLO_CORE_ERROR("[SaveGameSerializer] Failed to read UUID for entity {}", i);
                return false;
            }

            // Create entity with the saved UUID
            Entity entity = staging.CreateEntityWithUUID(uuid, "");

            bool loadFailed = false;
            std::optional<std::vector<u8>> materialData;
            constexpr u32 materialType = Hash::GenerateFNVHash("MaterialComponent");

            // Read component blocks until end-of-entity sentinel (typeHash == 0)
            while (true)
            {
                if (reader.AtEnd() || reader.IsError())
                {
                    OLO_CORE_ERROR("[SaveGameSerializer] Unexpected end of data at entity {} (missing end marker)", i);
                    return false;
                }

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

                // The save carries material factors. Read them over the authored maps,
                // shader and alpha settings after this entity's source identity
                // is known, independent of the order of its component blocks.
                if (typeHash == materialType)
                {
                    if (materialData)
                        return false;
                    materialData.emplace(std::move(compData));
                    continue;
                }

                // Match typeHash to component and deserialize
                bool matched = false;

#define TRY_LOAD_COMPONENT(ComponentType) \
    if (!matched)                         \
    LOAD_COMPONENT(ComponentType, entity, typeHash, compData)

                // The TRY_LOAD_COMPONENT(...) restore matcher is generated by
                // OloHeaderTool — the exact complement of the SAVE_COMPONENT capture
                // list above, so a captured component is always restorable. DO NOT
                // hand-edit; see SaveGameComponentSerializer.cpp. Order is irrelevant
                // (each stream block is matched by typeHash), so unlike the former
                // hand list IDComponent is just another TRY_LOAD_COMPONENT entry.
#include "OloEngine/SaveGame/Generated/SaveGameComponentRestore.Generated.inl"

#undef TRY_LOAD_COMPONENT

                // ISaveable blob: defer restore until after staging is committed
                if (!matched && typeHash == kSaveableTypeHash)
                {
                    if (entity.HasComponent<ScriptComponent>())
                    {
                        outDeferredSaveables.push_back({ uuid, entity.GetComponent<ScriptComponent>().ClassName, compData });
                    }
                    matched = true;
                }

                // Unknown component types are silently skipped (forward compatible)
            }

            if (materialData)
            {
                if (const auto authored = authoredScene.TryGetEntityWithUUID(uuid);
                    authored && authored->HasComponent<MaterialComponent>() && SameMaterialOwner(entity, *authored))
                    entity.AddComponent<MaterialComponent>(authored->GetComponent<MaterialComponent>());
                [[maybe_unused]] bool matched = false;
                LOAD_COMPONENT(MaterialComponent, entity, materialType, *materialData)
            }

            if (loadFailed)
            {
                OLO_CORE_ERROR("[SaveGameSerializer] Component deserialization failed for entity {} (UUID {})", i, static_cast<u64>(uuid));
                return false;
            }
        }

        return !reader.IsError();
    }

    static f32 RestoredClipTime(const AnimationStateComponent& state)
    {
        const f32 duration = state.m_CurrentClip ? state.m_CurrentClip->Duration : 0.0f;
        if (!std::isfinite(state.m_CurrentTime) || !std::isfinite(duration) || duration <= 0.0f)
            return 0.0f;
        if (!state.m_Loop)
            return std::clamp(state.m_CurrentTime, 0.0f, duration);
        const f32 time = std::fmod(state.m_CurrentTime, duration);
        return time < 0.0f ? time + duration : time;
    }

    static void RestoreImportedPose(Entity saved, Entity authored)
    {
        AnimationStateComponent sample;
        if (saved.HasComponent<AnimationStateComponent>() && authored.HasComponent<AnimationStateComponent>())
        {
            auto& state = saved.GetComponent<AnimationStateComponent>();
            ModelImporter::BindAnimationClips(state, authored.GetComponent<AnimationStateComponent>().m_AvailableClips);
            // ApplyClipRequest sets the selected index to the destination as
            // soon as a blend starts. The save omits the outgoing clip identity.
            if (state.m_Blending)
            {
                state.m_CurrentTime = state.m_NextTime;
                state.m_Loop = state.m_NextLoop;
            }
            state.m_Blending = false;
            state.m_BlendTime = state.m_BlendFactor = state.m_NextTime = 0.0f;
            state.m_CurrentTime = RestoredClipTime(state);
            if (!std::isfinite(state.m_BlendDuration) || state.m_BlendDuration < 0.001f)
                state.m_BlendDuration = 0.001f;

            // Evaluate the first frame even when paused. Keep queued requests
            // and root-motion deltas out of this zero-time pose evaluation.
            sample.m_CurrentClip = state.m_CurrentClip;
            sample.m_CurrentTime = state.m_CurrentTime;
            sample.m_Loop = state.m_Loop;
            sample.m_RootMotion = state.m_RootMotion;
        }
        if (!saved.HasComponent<SkeletonComponent>() || !authored.HasComponent<SkeletonComponent>())
            return;
        const auto& source = authored.GetComponent<SkeletonComponent>().m_Skeleton;
        if (!source)
            return;

        // Each actor owns its mutable pose and velocity history. Never carry
        // the old registry's bone-entity lookup cache into the staged scene.
        auto skeleton = Ref<Skeleton>::Create();
        static_cast<SkeletonData&>(*skeleton) = static_cast<const SkeletonData&>(*source);
        Animation::AnimationSystem::Update(sample, *skeleton, 0.0f);
        skeleton->ResetBoneHistory();
        saved.GetComponent<SkeletonComponent>().SetSkeleton(skeleton);
    }

    bool SaveGameSerializer::RestoreSceneState(Scene& scene, const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (scene.m_BlockCheckpointLoad)
        {
            OLO_CORE_WARN("[SaveGameSerializer] Cannot load a checkpoint during teardown or another checkpoint commit");
            return false;
        }
        if (data.empty())
        {
            return false;
        }

        FMemoryReader reader(data);
        reader.ArIsSaveGame = true;

        // --- Parse settings into local temporaries ---
        u32 settingsMarker = 0;
        reader << settingsMarker;
        if (settingsMarker != 0x53455453)
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Invalid settings marker");
            return false;
        }

        // SEEDED FROM THE LIVE SCENE, not default-constructed. Every one of
        // these is overwritten field by field by the reads below — except any
        // field this archive predates or deliberately does not carry, which
        // then retains the value the scene was loaded with. Default-
        // constructing instead would silently reset such a field to a global
        // default that has nothing to do with the scene the player is in.
        PostProcessSettings postProcess = scene.m_PostProcessSettings;
        SnowSettings snow = scene.m_SnowSettings;
        FogSettings fog = scene.m_FogSettings;
        WindSettings wind = scene.m_WindSettings;
        SnowAccumulationSettings snowAccum = scene.m_SnowAccumulationSettings;
        SnowEjectaSettings snowEjecta = scene.m_SnowEjectaSettings;
        SkinDiffusionSettings skinDiffusion = scene.m_SkinDiffusionSettings;
        PrecipitationSettings precipitation = scene.m_PrecipitationSettings;
        StreamingSettings streaming = scene.m_StreamingSettings;
        DialogueVariables dialogueVariables;

        const bool settingsRead =
            ReadSettingsBlock(reader, "PostProcess", [&](FArchive& ar)
                              { SerializePostProcessSettings(ar, postProcess); }) &&
            ReadSettingsBlock(reader, "Snow", [&](FArchive& ar)
                              { SerializeSnowSettings(ar, snow); }) &&
            ReadSettingsBlock(reader, "Fog", [&](FArchive& ar)
                              { SerializeFogSettings(ar, fog); }) &&
            ReadSettingsBlock(reader, "Wind", [&](FArchive& ar)
                              { SerializeWindSettings(ar, wind); }) &&
            ReadSettingsBlock(reader, "SnowAccumulation", [&](FArchive& ar)
                              { SerializeSnowAccumulationSettings(ar, snowAccum); }) &&
            ReadSettingsBlock(reader, "SnowEjecta", [&](FArchive& ar)
                              { SerializeSnowEjectaSettings(ar, snowEjecta); }) &&
            ReadSettingsBlock(reader, "SkinDiffusion", [&](FArchive& ar)
                              { SerializeSkinDiffusionSettings(ar, skinDiffusion); }) &&
            ReadSettingsBlock(reader, "Precipitation", [&](FArchive& ar)
                              { SerializePrecipitationSettings(ar, precipitation); }) &&
            ReadSettingsBlock(reader, "Streaming", [&](FArchive& ar)
                              { SerializeStreamingSettings(ar, streaming); }) &&
            ReadSettingsBlock(reader, "DialogueVariables", [&](FArchive& ar)
                              { SerializeDialogueVariables(ar, dialogueVariables); });
        if (!settingsRead)
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Failed to parse scene settings, scene is unchanged");
            return false;
        }

        // --- Entities marker ---
        u32 entitiesMarker = 0;
        reader << entitiesMarker;
        if (entitiesMarker != 0x454E5453)
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Invalid entities marker");
            return false;
        }

        // --- Deserialize entities into a staging scene ---
        // If deserialization fails, the real scene is untouched.
        Ref<Scene> staging = Ref<Scene>::Create();
        staging->m_ViewportWidth = scene.m_ViewportWidth;
        staging->m_ViewportHeight = scene.m_ViewportHeight;

        std::vector<DeferredSaveableEntry> deferredSaveables;
        if (!DeserializeEntitiesInto(*staging, scene, reader, deferredSaveables))
        {
            OLO_CORE_ERROR("[SaveGameSerializer] Entity deserialization failed, scene is unchanged");
            return false;
        }

        if (!reader.AtEnd())
        {
            OLO_CORE_ERROR("[SaveGameSerializer] {} trailing bytes after entity data — rejecting payload",
                           reader.TotalSize() - reader.Tell());
            return false;
        }

        // Continue loads the authored scene before applying the save. The save
        // carries geometry identities and gameplay flags, while the model and
        // primitive resources belong to that loaded scene. Retain matching
        // resources before destroying the old registry.
        auto models = staging->GetAllEntitiesWith<IDComponent, ModelComponent>();
        for (auto [handle, id, model] : models.each())
        {
            if (const auto original = scene.TryGetEntityWithUUID(id.ID); original && original->HasComponent<ModelComponent>())
            {
                const auto& source = original->GetComponent<ModelComponent>();
                if (!model.m_FilePath.empty() && FileSystem::SameSourcePath(model.m_FilePath, source.m_FilePath))
                {
                    model.m_Model = source.m_Model;
                }
            }
        }
        auto meshes = staging->GetAllEntitiesWith<IDComponent, MeshComponent>();
        for (auto [handle, id, mesh] : meshes.each())
        {
            if (const auto original = scene.TryGetEntityWithUUID(id.ID); original && original->HasComponent<MeshComponent>())
            {
                const auto& source = original->GetComponent<MeshComponent>();
                if (!source.m_MeshSource || mesh.m_Primitive != source.m_Primitive)
                {
                    continue;
                }

                // A saved source file is explicit identity. Never let a currently
                // registered mesh override a different (or removed) import path.
                Entity saved{ handle, staging.get() };
                const auto savedPath = MeshImportPath(saved);
                const bool legacyBindPose = IsLegacyBindPose(saved, *original);
                if (!legacyBindPose && !FileSystem::SameSourcePath(savedPath, MeshImportPath(*original)))
                {
                    continue;
                }

                // The save has no separate MeshSourceHandle field. Without an import
                // path, the scene supplies its registered mesh or matching primitive.
                const bool hasAssetHandle = static_cast<u64>(source.m_MeshSource->GetHandle()) != 0;
                if (legacyBindPose || !savedPath.empty() || hasAssetHandle || mesh.m_Primitive != MeshPrimitive::None)
                {
                    mesh.m_MeshSource = source.m_MeshSource;
                    RestoreImportedPose(saved, *original);
                }
            }
        }

        // --- All parsing succeeded — commit to real scene ---

        TGuardValue<bool> committing(scene.m_BlockCheckpointLoad, true);
        const bool restart = scene.StopStartupForCheckpoint();

        // Apply settings
        scene.m_PostProcessSettings = std::move(postProcess);
        scene.m_SnowSettings = std::move(snow);
        scene.m_FogSettings = std::move(fog);
        scene.m_WindSettings = std::move(wind);
        scene.m_SnowAccumulationSettings = std::move(snowAccum);
        scene.m_SnowEjectaSettings = std::move(snowEjecta);
        scene.m_SkinDiffusionSettings = std::move(skinDiffusion);
        scene.m_PrecipitationSettings = std::move(precipitation);
        scene.m_StreamingSettings = std::move(streaming);

        // Destroy all existing entities in the real scene (children before parents)
        {
            auto allEntities = scene.GetAllEntitiesWith<IDComponent>();
            std::vector<Entity> toDestroy;
            for (auto e : allEntities)
            {
                toDestroy.emplace_back(e, &scene);
            }

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

            std::ranges::sort(toDestroy,
                              [&getDepth](Entity& a, Entity& b)
                              {
                                  return getDepth(a) > getDepth(b);
                              });

            for (const auto& entity : toDestroy)
            {
                scene.DestroyEntity(entity);
            }
        }

        // Swap ECS registry and entity map from staging into the real scene.
        // After this swap the registries are owned by the correct Scene objects.
        // NOTE: Any Entity wrappers created during DeserializeEntitiesInto pointed
        // at the staging Scene, but those are temporaries local to that call and
        // are not cached.  Callers must NOT hold Entity objects across a
        // RestoreSceneState call — the swap invalidates them.
        if (!scene.IsRunning())
        {
            for (auto [entity, id, animation] : staging->m_Registry.view<IDComponent, AnimationStateComponent>().each())
                animation.m_RestoredPlaybackOwner = static_cast<u64>(id.ID);
        }
        std::swap(scene.m_Registry, staging->m_Registry);
        std::swap(scene.m_EntityMap, staging->m_EntityMap);

        // After the teardown above, whose script OnDestroy callbacks may still
        // write variables, and after StopStartupForCheckpoint's OnRuntimeStop,
        // which clears them.
        scene.m_DialogueVariables = std::move(dialogueVariables);

        // Apply deferred ISaveable restores now that entities live in the committed scene
        bool restored = true;
        for (auto& entry : deferredSaveables)
        {
            if (!RestoreSaveableData(entry.EntityID, entry.ClassName, entry.Data))
            {
                OLO_CORE_ERROR("[SaveGameSerializer] ISaveable restore failed for entity {} post-commit", static_cast<u64>(entry.EntityID));
                restored = false;
                break;
            }
        }

        if (restart)
        {
            // The replacement registry is live. Its startup callbacks may load the next checkpoint.
            TGuardValue<bool> committed(scene.m_BlockCheckpointLoad, false);
            scene.OnRuntimeStart();
        }
        return restored;
    }

#undef SAVE_COMPONENT
#undef LOAD_COMPONENT

} // namespace OloEngine
