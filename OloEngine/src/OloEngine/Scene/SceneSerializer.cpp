#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Core/YAMLConverters.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Asset/AssetManager.h"

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
            if (meshComponent.m_MeshSource)
            {
                out << YAML::Key << "MeshSourceHandle" << YAML::Value << static_cast<u64>(meshComponent.m_MeshSource->GetHandle());
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

            out << YAML::EndMap; // SpotLightComponent
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
            // Note: m_CurrentClip, m_NextClip, m_BoneEntityIds are runtime data

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
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Scene" << YAML::Value << m_Scene->GetName();
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
                }

                if (auto pointLightComponent = entity["PointLightComponent"]; pointLightComponent)
                {
                    auto& pointLight = deserializedEntity.AddComponent<PointLightComponent>();
                    pointLight.m_Color = pointLightComponent["Color"].as<glm::vec3>(pointLight.m_Color);
                    pointLight.m_Intensity = pointLightComponent["Intensity"].as<f32>(pointLight.m_Intensity);
                    pointLight.m_Range = pointLightComponent["Range"].as<f32>(pointLight.m_Range);
                    pointLight.m_Attenuation = pointLightComponent["Attenuation"].as<f32>(pointLight.m_Attenuation);
                    pointLight.m_CastShadows = pointLightComponent["CastShadows"].as<bool>(pointLight.m_CastShadows);
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
                    // Note: Animation clips are runtime data, loaded from model
                }

                if (auto skelComponent = entity["SkeletonComponent"]; skelComponent)
                {
                    deserializedEntity.AddComponent<SkeletonComponent>();
                    // Note: Skeleton is loaded from model file at runtime
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
                }

                if (auto pointLightComponent = entity["PointLightComponent"]; pointLightComponent)
                {
                    auto& pointLight = deserializedEntity.AddComponent<PointLightComponent>();
                    pointLight.m_Color = pointLightComponent["Color"].as<glm::vec3>(pointLight.m_Color);
                    pointLight.m_Intensity = pointLightComponent["Intensity"].as<f32>(pointLight.m_Intensity);
                    pointLight.m_Range = pointLightComponent["Range"].as<f32>(pointLight.m_Range);
                    pointLight.m_Attenuation = pointLightComponent["Attenuation"].as<f32>(pointLight.m_Attenuation);
                    pointLight.m_CastShadows = pointLightComponent["CastShadows"].as<bool>(pointLight.m_CastShadows);
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
                }

                if (auto skelComponent = entity["SkeletonComponent"]; skelComponent)
                {
                    deserializedEntity.AddComponent<SkeletonComponent>();
                }
            }
        }

        return true;
    }
} // namespace OloEngine
