#include "OloEnginePCH.h"
#include "Scene.h"
#include "Entity.h"

#include "Components.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Animation/BoneEntityUtils.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <glm/glm.hpp>
#include <ranges>

// Box2D
#include "box2d/box2d.h"

namespace OloEngine
{
	[[nodiscard("Store this!")]] static b2BodyType Rigidbody2DTypeToBox2DBody(const Rigidbody2DComponent::BodyType bodyType)
	{
		switch (bodyType)
		{
			using enum OloEngine::Rigidbody2DComponent::BodyType;
			case Static:    return b2_staticBody;
			case Dynamic:   return b2_dynamicBody;
			case Kinematic: return b2_kinematicBody;
		}

		OLO_CORE_ASSERT(false, "Unknown body type");
		return b2_staticBody;
	}

	Scene::Scene()
	= default;

	Ref<Scene> Scene::Create()
	{
		return Ref<Scene>(new Scene());
	}

	Scene::~Scene()
	{
		if (b2World_IsValid(m_PhysicsWorld))
		{
			b2DestroyWorld(m_PhysicsWorld);
			m_PhysicsWorld = b2_nullWorldId;
		}
	}

	template<typename... Component>
	static void CopyComponent(entt::registry& dstRegistry, entt::registry& srcRegistry, const std::unordered_map<UUID, entt::entity>& enttMap)
	{
		([&]()
		{
			auto view = srcRegistry.view<Component>();
			for (auto entity : view)
			{
				entt::entity dstEntity = enttMap.at(srcRegistry.get<IDComponent>(entity).ID);

				const auto& srcComponent = srcRegistry.get<Component>(entity);
				dstRegistry.emplace_or_replace<Component>(dstEntity, srcComponent);
			}
		}(), ...);
	}

	template<typename... Component>
	static void CopyComponent(ComponentGroup<Component...>, entt::registry& dst, entt::registry& src, const std::unordered_map<UUID, entt::entity>& enttMap)
	{
		CopyComponent<Component...>(dst, src, enttMap);
	}

	template<typename... Component>
	static void CopyComponentIfExists(Entity dst, Entity src)
	{
		([&]()
		{
			if (src.HasComponent<Component>())
			{
				dst.AddOrReplaceComponent<Component>(src.GetComponent<Component>());
			}
		}(), ...);
	}

	template<typename... Component>
	static void CopyComponentIfExists(ComponentGroup<Component...>, Entity dst, Entity src)
	{
		CopyComponentIfExists<Component...>(dst, src);
	}

	Ref<Scene> Scene::Copy(Ref<Scene>& other)
	{
		Ref<Scene> newScene = Ref<Scene>::Create();

		newScene->m_ViewportWidth = other->m_ViewportWidth;
		newScene->m_ViewportHeight = other->m_ViewportHeight;

		auto& srcSceneRegistry = other->m_Registry;
		auto& dstSceneRegistry = newScene->m_Registry;
		std::unordered_map<UUID, entt::entity> enttMap;

		// Create entities in new scene
		for (const auto idView = srcSceneRegistry.view<IDComponent>(); auto e : std::ranges::reverse_view(idView))
		{
			const UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
			const auto& name = srcSceneRegistry.get<TagComponent>(e).Tag;
			const Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
			enttMap[uuid] = static_cast<entt::entity>(newEntity);
		}

		// Copy components (except IDComponent and TagComponent)
		CopyComponent(AllComponents{}, dstSceneRegistry, srcSceneRegistry, enttMap);

		return newScene;
	}

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(const UUID uuid, const std::string & name)
	{
		auto entity = Entity { m_Registry.create(), this };
		auto& idComponent = entity.AddComponent<IDComponent>();
		idComponent.ID = uuid;

		entity.AddComponent<TransformComponent>();
		entity.AddComponent<RelationshipComponent>();

		auto& tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;

		m_EntityMap.emplace(uuid, entity);

		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		if (!entity || !entity.HasComponent<IDComponent>())
			return;
			
		UUID entityUUID = entity.GetUUID();
		m_Registry.destroy(entity);
		m_EntityMap.erase(entityUUID);
	}

	void Scene::OnRuntimeStart()
	{
		m_IsRunning = true;

		OnPhysics2DStart();


		for (auto listenerView = m_Registry.group<AudioListenerComponent>(entt::get<TransformComponent>); auto&& [e, ac, tc] : listenerView.each())
		{
			ac.Listener = Ref<AudioListener>::Create();
			if (ac.Active)
			{
				const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
				const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
				ac.Listener->SetConfig(ac.Config);
				ac.Listener->SetPosition(tc.Translation);
				ac.Listener->SetDirection(-forward);
				break;
			}
		}

		for (auto sourceView = m_Registry.group<AudioSourceComponent>(entt::get<TransformComponent>); auto&& [e, ac, tc] : sourceView.each())
		{
			if (ac.Source)
			{
				const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
				const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
				ac.Source->SetConfig(ac.Config);
				ac.Source->SetPosition(tc.Translation);
				ac.Source->SetDirection(forward);
				if (ac.Config.PlayOnAwake)
				{
					ac.Source->Play();
				}
			}
		}

		// Scripting
		{
			ScriptEngine::OnRuntimeStart(this);
			// Instantiate all script entities
			for (const auto scriptView = m_Registry.view<ScriptComponent>(); const auto e : scriptView)
			{
				Entity entity = { e, this };
				ScriptEngine::OnCreateEntity(entity);
			}
		}
	}

	void Scene::OnRuntimeStop()
	{
		m_IsRunning = false;

		ScriptEngine::OnRuntimeStop();

		for (auto view = m_Registry.view<AudioSourceComponent>(); auto&& [e, ac] : view.each())
		{
			if (ac.Source)
				ac.Source->Stop();
		}

		OnPhysics2DStop();

	}

	void Scene::OnSimulationStart()
	{
		OnPhysics2DStart();
	}

	void Scene::OnSimulationStop()
	{
		OnPhysics2DStop();
	}

	void Scene::OnUpdateRuntime(Timestep const ts)
	{
		if (!m_IsPaused || m_StepFrames-- > 0)
		{
			// Update scripts
			{
				// C# Entity OnUpdate
				for (auto scriptView = m_Registry.view<ScriptComponent>(); auto e : scriptView)
				{
					Entity entity = { e, this };
					ScriptEngine::OnUpdateEntity(entity, ts);
				}
			}

			// Physics
			{
				const i32 velocityIterations = 6;
				// const i32 positionIterations = 2; // TODO: Use this parameter when implementing position iterations
				b2World_Step(m_PhysicsWorld, ts.GetSeconds(), velocityIterations);

				// Retrieve transform from Box2D
				for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
				{
					Entity entity = { e, this };
					auto& transform = entity.GetComponent<TransformComponent>();
					auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

					b2Vec2 position = b2Body_GetPosition(rb2d.RuntimeBody);
					b2Rot rotation = b2Body_GetRotation(rb2d.RuntimeBody);

					transform.Translation.x = position.x;
					transform.Translation.y = position.y;
					transform.Rotation.z = b2Rot_GetAngle(rotation);
				}
			}

			auto listenerView = m_Registry.group<AudioListenerComponent>(entt::get<TransformComponent>);
			for (auto&& [e, ac, tc] : listenerView.each())
			{
				if (ac.Active)
				{
					const glm::mat4 inverted = glm::inverse(Entity(e, this).GetLocalTransform());
					const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
					ac.Listener->SetPosition(tc.Translation);
					ac.Listener->SetDirection(-forward);
					break;
				}
			}

			auto sourceView = m_Registry.group<AudioSourceComponent>(entt::get<TransformComponent>);
			for (auto&& [e, ac, tc] : sourceView.each())
			{
				if (ac.Source)
				{
					Entity entity = { e, this };
					const glm::mat4 inverted = glm::inverse(entity.GetLocalTransform());
					const glm::vec3 forward = normalize(glm::vec3(inverted[2]));
					ac.Source->SetPosition(tc.Translation);
					ac.Source->SetDirection(forward);
				}
			}
		}

		// Render 2D
		Camera const* mainCamera = nullptr;
		glm::mat4 cameraTransform;
		{
			for (const auto view = m_Registry.view<TransformComponent, CameraComponent>(); const auto entity : view)
			{
				const auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = transform.GetTransform();
					break;
				}
			}
		}

		if (mainCamera)
		{
			Renderer2D::BeginScene(*mainCamera, cameraTransform);

			// Draw sprites
			{
				for (const auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>); const auto entity : group)
				{
					const auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);
					Renderer2D::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(entity));
				}
			}

			// Draw circles
			{
				for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
				{
					const auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

					Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(entity));
				}
			}

			// Draw text
			{
				for (const auto view = m_Registry.view<TransformComponent, TextComponent>();  const auto entity : view)
				{
					const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);

					Renderer2D::DrawString(text.TextString, transform.GetTransform(), text, static_cast<int>(entity));
				}
			}

			Renderer2D::EndScene();
		}
	}

	void Scene::OnUpdateSimulation(const Timestep ts, EditorCamera const& camera)
	{
		if (!m_IsPaused || m_StepFrames-- > 0)		{

			// Physics
			{
				const i32 velocityIterations = 6;
				// const i32 positionIterations = 2; // TODO: Use this parameter when implementing position iterations
				b2World_Step(m_PhysicsWorld, ts.GetSeconds(), velocityIterations);

				// Retrieve transform from Box2D
				for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
				{
					Entity entity = { e, this };
					auto& transform = entity.GetComponent<TransformComponent>();
					auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

					b2Vec2 position = b2Body_GetPosition(rb2d.RuntimeBody);
					b2Rot rotation = b2Body_GetRotation(rb2d.RuntimeBody);

					transform.Translation.x = position.x;
					transform.Translation.y = position.y;
					transform.Rotation.z = b2Rot_GetAngle(rotation);
				}
			}
		}

		// Render
		RenderScene(camera);
	}

	void Scene::OnUpdateEditor([[maybe_unused]] Timestep const ts, EditorCamera const& camera)
	{
		// Render
		RenderScene(camera);
	}

	void Scene::OnViewportResize(const u32 width, const u32 height)
	{
		if ((m_ViewportWidth == width) && (m_ViewportHeight == height))
		{
			return;
		}

		m_ViewportWidth = width;
		m_ViewportHeight = height;

		// Resize our non-FixedAspectRatio cameras
		for (const auto view = m_Registry.view<CameraComponent>(); auto const entity : view)
		{
			auto& cameraComponent = view.get<CameraComponent>(entity);
			if (!cameraComponent.FixedAspectRatio)
			{
				cameraComponent.Camera.SetViewportSize(width, height);
			}
		}
	}

	void Scene::Step(int frames)
	{
		m_StepFrames = frames;
	}

	void Scene::DuplicateEntity(Entity entity)
	{
		const Entity newEntity = CreateEntity(entity.GetName());

		CopyComponentIfExists(AllComponents{}, newEntity, entity);
	}

	void Scene::SetName(std::string_view name)
	{
		m_Name = name;
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		for (const auto view = m_Registry.view<CameraComponent>(); auto const entity : view)
		{
			if (const auto& camera = view.get<CameraComponent>(entity); camera.Primary)
			{
				return Entity{ entity, this };
			}
		}
		return {};
	}


// Animation/ECS explicit specializations
template<>
void Scene::OnComponentAdded<AnimatedMeshComponent>(Entity, AnimatedMeshComponent&) {}
template<>
void Scene::OnComponentAdded<MeshComponent>(Entity, MeshComponent&) {}
template<>
void Scene::OnComponentAdded<SubmeshComponent>(Entity, SubmeshComponent&) {}
template<>
void Scene::OnComponentAdded<AnimationStateComponent>(Entity, AnimationStateComponent&) {}
template<>
void Scene::OnComponentAdded<Skeleton>(Entity, Skeleton&) {}
// If you use SkeletonComponent as a struct, add this too:
template<>
void Scene::OnComponentAdded<SkeletonComponent>(Entity, SkeletonComponent&) {}
template<>
void Scene::OnComponentAdded<MaterialComponent>(Entity, MaterialComponent&) {}

	Entity Scene::FindEntityByName(std::string_view name)
	{
		for (auto view = m_Registry.view<TagComponent>(); auto entity : view)
		{
			const TagComponent& tc = view.get<TagComponent>(entity);
			if (tc.Tag == name)
			{
				return Entity{ entity, this };
			}
		}
		return {};
	}

	Entity Scene::GetEntityByUUID(UUID uuid)
	{
		OLO_CORE_ASSERT(m_EntityMap.contains(uuid));
		return { m_EntityMap.at(uuid), this };
	}

	Entity Scene::FindEntityByName(std::string_view name) const
	{
		for (auto view = m_Registry.view<TagComponent>(); auto entity : view)
		{
			const TagComponent& tc = view.get<TagComponent>(entity);
			if (tc.Tag == name)
			{
				// SAFETY: this is const Scene*, but Entity requires non-const Scene*
				// This is safe because name search only reads entity data
				return Entity{ entity, const_cast<Scene*>(this) };
			}
		}
		return {};
	}

	Entity Scene::GetEntityByUUID(UUID uuid) const
	{
		OLO_CORE_ASSERT(m_EntityMap.contains(uuid));
		// SAFETY: this is const Scene*, but Entity requires non-const Scene*
		// This is safe because Entity lookup only reads entity data
		return { m_EntityMap.at(uuid), const_cast<Scene*>(this) };
	}

	Entity Scene::GetPrimaryCameraEntity() const
	{
		for (const auto view = m_Registry.view<CameraComponent>(); auto const entity : view)
		{
			if (const auto& camera = view.get<CameraComponent>(entity); camera.Primary)
			{
				// SAFETY: this is const Scene*, but Entity requires non-const Scene*
				// This is safe because camera lookup only reads entity data
				return Entity{ entity, const_cast<Scene*>(this) };
			}
		}
		return {};
	}

	// Bone entity management methods (Hazel-style)
	std::vector<glm::mat4> Scene::GetModelSpaceBoneTransforms(const std::vector<UUID>& boneEntityIds, Ref<MeshSource> meshSource)
	{
		return BoneEntityUtils::GetModelSpaceBoneTransforms(boneEntityIds, meshSource, this);
	}

	std::vector<UUID> Scene::FindBoneEntityIds(Entity entity, Entity rootEntity, const Skeleton* skeleton) const
	{
		return BoneEntityUtils::FindBoneEntityIds(rootEntity, skeleton, const_cast<Scene*>(this));
	}

	glm::mat3 Scene::FindRootBoneTransform(Entity entity, const std::vector<UUID>& boneEntityIds) const
	{
		return BoneEntityUtils::FindRootBoneTransform(entity, boneEntityIds, const_cast<Scene*>(this));
	}

	void Scene::BuildBoneEntityIds(Entity entity)
	{
		// This method doesn't exist in BoneEntityUtils, so we'll implement it here
		// For now, just call BuildMeshBoneEntityIds with the entity as both parameters
		BuildMeshBoneEntityIds(entity, entity);
	}

	void Scene::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity)
	{
		BoneEntityUtils::BuildMeshBoneEntityIds(entity, rootEntity, this);
	}

	void Scene::BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity)
	{
		BoneEntityUtils::BuildAnimationBoneEntityIds(entity, rootEntity, this);
	}

	Entity Scene::TryGetEntityWithUUID(UUID id) const
	{
		if (m_EntityMap.find(id) != m_EntityMap.end())
		{
			return Entity{ m_EntityMap.at(id), const_cast<Scene*>(this) };
		}
		return {};
	}

	Entity Scene::GetEntityWithUUID(UUID id) const
	{
		OLO_CORE_ASSERT(m_EntityMap.contains(id), "Entity with UUID not found");
		return Entity{ m_EntityMap.at(id), const_cast<Scene*>(this) };
	}

	void Scene::OnPhysics2DStart()
	{
		b2WorldDef worldDef = b2DefaultWorldDef();
		worldDef.gravity = { 0.0f, -9.8f };
		m_PhysicsWorld = b2CreateWorld(&worldDef);

		for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
		{
			Entity entity = { e, this };
			auto const& transform = entity.GetComponent<TransformComponent>();
			auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

			b2BodyDef bodyDef = b2DefaultBodyDef();
			bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
			bodyDef.position = { transform.Translation.x, transform.Translation.y };
			bodyDef.rotation = b2MakeRot(transform.Rotation.z);

			b2BodyId body = b2CreateBody(m_PhysicsWorld, &bodyDef);
			b2Body_SetFixedRotation(body, rb2d.FixedRotation);
			rb2d.RuntimeBody = body;

			if (entity.HasComponent<BoxCollider2DComponent>())
			{
				auto const& bc2d = entity.GetComponent<BoxCollider2DComponent>();

				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = bc2d.Density;
				shapeDef.friction = bc2d.Friction;
				shapeDef.restitution = bc2d.Restitution;

				b2Polygon polygon = b2MakeBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y);
				b2CreatePolygonShape(body, &shapeDef, &polygon);
			}

			if (entity.HasComponent<CircleCollider2DComponent>())
			{
				auto const& cc2d = entity.GetComponent<CircleCollider2DComponent>();

				b2ShapeDef shapeDef = b2DefaultShapeDef();
				shapeDef.density = cc2d.Density;
				shapeDef.friction = cc2d.Friction;
				shapeDef.restitution = cc2d.Restitution;

				b2Circle circle = { b2Vec2(cc2d.Offset.x, cc2d.Offset.y), transform.Scale.x * cc2d.Radius };
				b2CreateCircleShape(body, &shapeDef, &circle);
			}
		}
	}

	void Scene::OnPhysics2DStop()
	{
		if (b2World_IsValid(m_PhysicsWorld))
		{
			b2DestroyWorld(m_PhysicsWorld);
			m_PhysicsWorld = b2_nullWorldId;
		}
	}

	void Scene::RenderScene(EditorCamera const& camera)
	{
		Renderer2D::BeginScene(camera);

		// Draw sprites
		{
			for (const auto group = m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>); const auto entity : group)
			{
				const auto [transform, sprite] = group.get<TransformComponent, SpriteRendererComponent>(entity);

				Renderer2D::DrawSprite(transform.GetTransform(), sprite, static_cast<int>(entity));
			}
		}

		// Draw circles
		{
			for (const auto view = m_Registry.view<TransformComponent, CircleRendererComponent>(); const auto entity : view)
			{
				const auto [transform, circle] = view.get<TransformComponent, CircleRendererComponent>(entity);

				Renderer2D::DrawCircle(transform.GetTransform(), circle.Color, circle.Thickness, circle.Fade, static_cast<int>(entity));
			}
		}

		// Draw text
		{
			for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
			{
				const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);
				Renderer2D::DrawString(text.TextString, transform.GetTransform(), text, static_cast<int>(entity));
			}
		}
		
		Renderer2D::EndScene();
	}

	template<>
	void Scene::OnComponentAdded<IDComponent>(Entity, IDComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<TransformComponent>(Entity, TransformComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<CameraComponent>(Entity, CameraComponent& component)
	{
		if ((m_ViewportWidth > 0) && (m_ViewportHeight > 0))
		{
			component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
		}
	}

	template<>
	void Scene::OnComponentAdded<ScriptComponent>(Entity, ScriptComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<SpriteRendererComponent>(Entity, SpriteRendererComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<CircleRendererComponent>(Entity, CircleRendererComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<TagComponent>(Entity, TagComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<Rigidbody2DComponent>(Entity, Rigidbody2DComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<BoxCollider2DComponent>(Entity, BoxCollider2DComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<CircleCollider2DComponent>(Entity, CircleCollider2DComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<TextComponent>(Entity, TextComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<AudioSourceComponent>(Entity, AudioSourceComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<AudioListenerComponent>(Entity, AudioListenerComponent&)
	{
	}

	template<>
	void Scene::OnComponentAdded<RelationshipComponent>(Entity, RelationshipComponent&)
	{
	}
}
