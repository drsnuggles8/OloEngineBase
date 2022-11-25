#include "OloEnginePCH.h"
#include "Scene.h"
#include "Entity.h"

#include "Components.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"

#include <glm/glm.hpp>
#include <ranges>

// Box2D
#include "b2_world.h"
#include "b2_body.h"
#include "b2_fixture.h"
#include "b2_polygon_shape.h"
#include "b2_circle_shape.h"

namespace OloEngine {

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

	Scene::~Scene()
	{
		delete m_PhysicsWorld;
	}

	template<typename... Component>
	static void CopyComponent(entt::registry& dstRegistry, entt::registry& srcRegistry, const std::unordered_map<UUID, entt::entity>& enttMap)
	{
		([&]()
		{
			auto view = srcRegistry.view<Component>();
			for (auto it = view.rbegin(); it != view.rend(); it++)
			{
				entt::entity dstEntity = enttMap.at(srcRegistry.get<IDComponent>(*it).ID);

				const auto& srcComponent = srcRegistry.get<Component>(*it);
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

	Ref<Scene> Scene::Copy(Ref<Scene> const& other)
	{
		Ref<Scene> newScene = CreateRef<Scene>();

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

		auto& tag = entity.AddComponent<TagComponent>();
		tag.Tag = name.empty() ? "Entity" : name;

		m_EntityMap.emplace(uuid, entity);

		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		m_Registry.destroy(entity);
		m_EntityMap.erase(entity.GetUUID());
	}

	void Scene::OnRuntimeStart()
	{
		m_IsRunning = true;

		OnPhysics2DStart();

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

		OnPhysics2DStop();

		ScriptEngine::OnRuntimeStop();
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
				const int32_t velocityIterations = 6;
				const int32_t positionIterations = 2;
				m_PhysicsWorld->Step(ts, velocityIterations, positionIterations);

				// Retrieve transform from Box2D
				for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
				{
					Entity entity = { e, this };
					auto& transform = entity.GetComponent<TransformComponent>();
					auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

					auto const* const body = static_cast<b2Body*>(rb2d.RuntimeBody);

					const auto& position = body->GetPosition();
					transform.Translation.x = position.x;
					transform.Translation.y = position.y;
					transform.Rotation.z = body->GetAngle();
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

			Renderer2D::EndScene();
		}
	}

	void Scene::OnUpdateSimulation(const Timestep ts, EditorCamera const& camera)
	{
		if (!m_IsPaused || m_StepFrames-- > 0)
		{

			// Physics
			{
				const int32_t velocityIterations = 6;
				const int32_t positionIterations = 2;
				m_PhysicsWorld->Step(ts, velocityIterations, positionIterations);

				// Retrieve transform from Box2D
				for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
				{
					Entity entity = { e, this };
					auto& transform = entity.GetComponent<TransformComponent>();
					auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

					auto const* const body = static_cast<b2Body*>(rb2d.RuntimeBody);
					const auto& position = body->GetPosition();
					transform.Translation.x = position.x;
					transform.Translation.y = position.y;
					transform.Rotation.z = body->GetAngle();
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

		// TODO(olbu): Implement these as tests, rest from Renderer2D.cpp too
		// Renderer2D::DrawLine(glm::vec3(0.0f), glm::vec3(5.0f), glm::vec4(1, 0, 1, 1));
		// Renderer2D::DrawRect(glm::vec3(0.0f), glm::vec2(5.0f), glm::vec4(1, 1, 1, 1));

	void Scene::OnViewportResize(const uint32_t width, const uint32_t height)
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

	template<typename T>
	void Scene::OnComponentAdded(Entity entity, T& component)
	{
		static_assert(0 == sizeof(T));
	}

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
		// TODO(OLBU): Maybe should be assert
		if (m_EntityMap.contains(uuid))
		{
			return { m_EntityMap.at(uuid), this };
		}

		return {};
	}

	void Scene::OnPhysics2DStart()
	{
		m_PhysicsWorld = new b2World({ 0.0f, -9.8f });

		for (const auto view = m_Registry.view<Rigidbody2DComponent>(); const auto e : view)
		{
			Entity entity = { e, this };
			auto const& transform = entity.GetComponent<TransformComponent>();
			auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();

			b2BodyDef bodyDef;
			bodyDef.type = Rigidbody2DTypeToBox2DBody(rb2d.Type);
			bodyDef.position.Set(transform.Translation.x, transform.Translation.y);
			bodyDef.angle = transform.Rotation.z;

			b2Body* const body = m_PhysicsWorld->CreateBody(&bodyDef);
			body->SetFixedRotation(rb2d.FixedRotation);
			rb2d.RuntimeBody = body;

			if (entity.HasComponent<BoxCollider2DComponent>())
			{
				auto const& bc2d = entity.GetComponent<BoxCollider2DComponent>();

				b2PolygonShape boxShape;
				boxShape.SetAsBox(bc2d.Size.x * transform.Scale.x, bc2d.Size.y * transform.Scale.y);

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &boxShape;
				fixtureDef.density = bc2d.Density;
				fixtureDef.friction = bc2d.Friction;
				fixtureDef.restitution = bc2d.Restitution;
				fixtureDef.restitutionThreshold = bc2d.RestitutionThreshold;
				body->CreateFixture(&fixtureDef);
			}

			if (entity.HasComponent<CircleCollider2DComponent>())
			{
				auto const& cc2d = entity.GetComponent<CircleCollider2DComponent>();

				b2CircleShape circleShape;
				circleShape.m_p.Set(cc2d.Offset.x, cc2d.Offset.y);
				circleShape.m_radius = transform.Scale.x * cc2d.Radius;

				b2FixtureDef fixtureDef;
				fixtureDef.shape = &circleShape;
				fixtureDef.density = cc2d.Density;
				fixtureDef.friction = cc2d.Friction;
				fixtureDef.restitution = cc2d.Restitution;
				fixtureDef.restitutionThreshold = cc2d.RestitutionThreshold;
				body->CreateFixture(&fixtureDef);
			}
		}
	}

	void Scene::OnPhysics2DStop()
	{
		delete m_PhysicsWorld;
		m_PhysicsWorld = nullptr;
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

}
