#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Core/YAMLConverters.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Project/Project.h"

#include <fstream>

#include <yaml-cpp/yaml.h>


namespace OloEngine
{
#define WRITE_SCRIPT_FIELD(FieldType, Type)   \
	case ScriptFieldType::FieldType:          \
		out << scriptField.GetValue<Type>();  \
		break

#define READ_SCRIPT_FIELD(FieldType, Type)            \
	case ScriptFieldType::FieldType:                  \
	{                                                 \
		Type fieldData = scriptField["Data"].as<Type>();   \
		fieldInstance.SetValue(fieldData);                 \
		break;                                        \
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
			case Static:    return "Static";
			case Dynamic:   return "Dynamic";
			case Kinematic: return "Kinematic";
		}

		OLO_CORE_ASSERT(false, "Unknown body type");
		return {};
	}

	static Rigidbody2DComponent::BodyType RigidBody2DBodyTypeFromString(const std::string_view bodyTypeString)
	{
		using enum OloEngine::Rigidbody2DComponent::BodyType;
		if (bodyTypeString == "Static") { return Static; }
		if (bodyTypeString == "Dynamic") { return Dynamic; }
		if (bodyTypeString == "Kinematic") { return Kinematic; }

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

		if (entity.HasComponent<PrefabComponent>())
		{
			out << YAML::Key << "PrefabComponent";
			out << YAML::BeginMap; // PrefabComponent

			auto const& prefabComponent = entity.GetComponent<PrefabComponent>();
			out << YAML::Key << "PrefabID" << YAML::Value << prefabComponent.m_PrefabID;
			out << YAML::Key << "PrefabEntityID" << YAML::Value << prefabComponent.m_PrefabEntityID;

			out << YAML::EndMap; // PrefabComponent
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

				SerializeEntity(out, entity);
			});
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
		catch (YAML::ParserException const e)
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

				if (auto prefabComponent = entity["PrefabComponent"]; prefabComponent)
				{
					auto& pc = deserializedEntity.AddComponent<PrefabComponent>();
					pc.m_PrefabID = prefabComponent["PrefabID"].as<u64>();
					pc.m_PrefabEntityID = prefabComponent["PrefabEntityID"].as<u64>();
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

				SerializeEntity(out, entity);
			});
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
			}
		}

		return true;
	}
}
