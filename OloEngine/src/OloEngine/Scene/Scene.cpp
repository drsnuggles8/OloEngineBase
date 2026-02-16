#include "OloEnginePCH.h"
#include "Scene.h"
#include "Entity.h"

#include "Components.h"
#include "Prefab.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Animation/BoneEntityUtils.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/UI/UILayoutSystem.h"
#include "OloEngine/UI/UIRenderer.h"
#include "OloEngine/UI/UIInputSystem.h"
#include "OloEngine/Particle/ParticleRenderer.h"
#include "OloEngine/Particle/TrailRenderer.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/MouseCodes.h"

#include <glm/glm.hpp>
#include <ranges>

// Box2D
#include "box2d/box2d.h"

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/MotionType.h>

namespace OloEngine
{
    [[nodiscard("Store this!")]] static b2BodyType Rigidbody2DTypeToBox2DBody(const Rigidbody2DComponent::BodyType bodyType)
    {
        switch (bodyType)
        {
            using enum OloEngine::Rigidbody2DComponent::BodyType;
            case Static:
                return b2_staticBody;
            case Dynamic:
                return b2_dynamicBody;
            case Kinematic:
                return b2_kinematicBody;
        }

        OLO_CORE_ASSERT(false, "Unknown body type");
        return b2_staticBody;
    }

    Scene::Scene()
        : m_JoltScene(std::make_unique<JoltScene>(this))
    {
    }

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
    static void CopyComponent(entt::registry& dstRegistry, entt::registry& srcRegistry, const TMap<UUID, entt::entity>& enttMap)
    {
        ([&]()
         {
			auto view = srcRegistry.view<Component>();
			for (auto entity : view)
			{
				entt::entity dstEntity = enttMap.FindChecked(srcRegistry.get<IDComponent>(entity).ID);

				const auto& srcComponent = srcRegistry.get<Component>(entity);
				dstRegistry.emplace_or_replace<Component>(dstEntity, srcComponent);
			} }(), ...);
    }

    template<typename... Component>
    static void CopyComponent(ComponentGroup<Component...>, entt::registry& dst, entt::registry& src, const TMap<UUID, entt::entity>& enttMap)
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
			} }(), ...);
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
        TMap<UUID, entt::entity> enttMap;

        // Create entities in new scene
        for (const auto idView = srcSceneRegistry.view<IDComponent>(); auto e : std::ranges::reverse_view(idView))
        {
            const UUID uuid = srcSceneRegistry.get<IDComponent>(e).ID;
            const auto& name = srcSceneRegistry.get<TagComponent>(e).Tag;
            const Entity newEntity = newScene->CreateEntityWithUUID(uuid, name);
            enttMap.Add(uuid, static_cast<entt::entity>(newEntity));
        }

        // Copy components (except IDComponent and TagComponent)
        CopyComponent(AllComponents{}, dstSceneRegistry, srcSceneRegistry, enttMap);

        return newScene;
    }

    [[nodiscard]] Entity Scene::CreateEntity(const std::string& name)
    {
        return CreateEntityWithUUID(UUID(), name);
    }

    [[nodiscard]] Entity Scene::CreateEntityWithUUID(const UUID uuid, const std::string& name)
    {
        auto entity = Entity{ m_Registry.create(), this };
        auto& idComponent = entity.AddComponent<IDComponent>();
        idComponent.ID = uuid;

        entity.AddComponent<TransformComponent>();
        // RelationshipComponent will be added on-demand when needed
        // entity.AddComponent<RelationshipComponent>();

        auto& tag = entity.AddComponent<TagComponent>();
        tag.Tag = name.empty() ? "Entity" : name;

        m_EntityMap.Add(uuid, entity);

        return entity;
    }

    [[nodiscard]] Entity Scene::Instantiate(AssetHandle prefabHandle)
    {
        return InstantiateWithUUID(prefabHandle, UUID());
    }

    [[nodiscard]] Entity Scene::InstantiateWithUUID(AssetHandle prefabHandle, UUID uuid)
    {
        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(prefabHandle);
        if (!prefab)
        {
            OLO_CORE_ERROR("Scene::InstantiateWithUUID - Failed to load prefab with handle {}", prefabHandle);
            return {};
        }

        // Generate UUID if not provided
        if (!uuid)
            uuid = UUID();

        // Check for UUID collision and resolve if necessary
        if (m_EntityMap.Contains(uuid))
        {
            UUID originalUuid = uuid;
#ifdef OLO_DEBUG
            OLO_CORE_ASSERT(false, "Scene::InstantiateWithUUID - UUID collision detected! UUID {} already exists in scene", static_cast<u64>(uuid));
#else
            OLO_CORE_WARN("Scene::InstantiateWithUUID - UUID collision detected! UUID {} already exists, generating new UUID", static_cast<u64>(uuid));

            // Generate new unique UUID
            do
            {
                uuid = UUID();
            } while (m_EntityMap.Contains(uuid));

            OLO_CORE_WARN("Scene::InstantiateWithUUID - Resolved collision: original UUID {} replaced with new UUID {}", static_cast<u64>(originalUuid), static_cast<u64>(uuid));
#endif
        }

        // Create a new entity from the prefab
        Entity entity = prefab->Instantiate(*this, uuid);

        // Add PrefabComponent to track the prefab source
        if (entity)
        {
            auto& prefabComponent = entity.AddComponent<PrefabComponent>();
            prefabComponent.m_PrefabID = prefabHandle;
            prefabComponent.m_PrefabEntityID = uuid;
        }

        return entity;
    }

    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity || !entity.HasComponent<IDComponent>())
            return;

        UUID entityUUID = entity.GetUUID();
        m_Registry.destroy(entity);
        m_EntityMap.Remove(entityUUID);
    }

    void Scene::OnRuntimeStart()
    {
        m_IsRunning = true;

        OnPhysics2DStart();
        OnPhysics3DStart();

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

        // Start animations
        {
            auto animView = m_Registry.view<AnimationStateComponent>();
            for (auto e : animView)
            {
                auto& animState = animView.get<AnimationStateComponent>(e);
                if (animState.m_CurrentClip)
                {
                    animState.m_IsPlaying = true;
                    animState.m_CurrentTime = 0.0f;
                }
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
        OnPhysics3DStop();
    }

    void Scene::OnSimulationStart()
    {
        OnPhysics2DStart();
        OnPhysics3DStart();
    }

    void Scene::OnSimulationStop()
    {
        OnPhysics2DStop();
        OnPhysics3DStop();
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

            // Update animations
            {
                auto animView = m_Registry.view<AnimationStateComponent, SkeletonComponent>();
                for (auto e : animView)
                {
                    auto& animState = animView.get<AnimationStateComponent>(e);
                    auto& skelComp = animView.get<SkeletonComponent>(e);

                    if (animState.m_IsPlaying && animState.m_CurrentClip && skelComp.m_Skeleton)
                    {
                        Animation::AnimationSystem::Update(animState, *skelComp.m_Skeleton, ts.GetSeconds());
                    }
                }
            }

            // Physics
            {
                const i32 velocityIterations = 6;
                // const i32 positionIterations = 2; // TODO: Use this parameter when implementing position iterations
                b2World_Step(m_PhysicsWorld, ts.GetSeconds(), velocityIterations);

                // Update 3D physics
                if (m_JoltScene)
                {
                    m_JoltScene->Simulate(ts.GetSeconds());
                }

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

                // Retrieve transforms from Jolt 3D physics
                for (const auto view = m_Registry.view<Rigidbody3DComponent, TransformComponent>(); const auto e : view)
                {
                    Entity entity = { e, this };
                    auto& transform = entity.GetComponent<TransformComponent>();
                    auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();

                    if (rb3d.m_RuntimeBodyToken != 0 && rb3d.m_Type != BodyType3D::Static && m_JoltScene)
                    {
                        // Get the body from JoltScene and sync transforms
                        auto body = m_JoltScene->GetBody(entity);
                        if (body)
                        {
                            auto pos = body->GetPosition();
                            auto rot = body->GetRotation();

                            transform.Translation = pos;
                            transform.Rotation = glm::eulerAngles(rot);
                        }
                    }
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

        // Update particle systems
        {
            // Quick camera position lookup for LOD
            glm::vec3 camPos(0.0f);
            for (const auto camView = m_Registry.view<TransformComponent, CameraComponent>(); const auto camEntity : camView)
            {
                const auto& [camTransform, cam] = camView.get<TransformComponent, CameraComponent>(camEntity);
                if (cam.Primary)
                {
                    camPos = camTransform.Translation;
                    break;
                }
            }

            for (auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>(); auto entity : view)
            {
                auto& transform = view.get<TransformComponent>(entity);
                auto& psc = view.get<ParticleSystemComponent>(entity);
                // Provide Jolt scene for raycast collision
                psc.System.SetJoltScene(m_JoltScene.get());
                psc.System.UpdateLOD(camPos, transform.Translation);
                psc.System.Update(ts, transform.Translation);
            }
        }

        // Find the primary camera
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
            // Render 3D if 3D mode is enabled
            if (m_Is3DModeEnabled)
            {
                RenderScene3D(*mainCamera, cameraTransform);
            }

            // Always render 2D (sprites, circles, text overlay on top of 3D)
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
                for (const auto view = m_Registry.view<TransformComponent, TextComponent>(); const auto entity : view)
                {
                    const auto [transform, text] = view.get<TransformComponent, TextComponent>(entity);

                    Renderer2D::DrawString(text.TextString, transform.GetTransform(), text, static_cast<int>(entity));
                }
            }

            // Draw particles
            {
                for (auto view = m_Registry.view<ParticleSystemComponent>(); auto entity : view)
                {
                    auto& psc = view.get<ParticleSystemComponent>(entity);
                    ParticleRenderer::RenderParticles2D(psc.System.GetPool(), psc.Texture, static_cast<int>(entity));
                }
            }

            Renderer2D::EndScene();
        }

        // Process UI input during runtime
        {
            const glm::vec2 mousePos = Input::GetMousePosition();
            const bool mouseDown = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
            const bool mousePressed = mouseDown && !m_PreviousMouseButtonDown;
            m_PreviousMouseButtonDown = mouseDown;
            UIInputSystem::ProcessInput(*this, mousePos, mouseDown, mousePressed);
        }

        RenderUIOverlay();
    }

    void Scene::OnUpdateSimulation(const Timestep ts, EditorCamera const& camera)
    {
        if (!m_IsPaused || m_StepFrames-- > 0)
        {

            // Physics
            {
                const i32 velocityIterations = 6;
                // const i32 positionIterations = 2; // TODO: Use this parameter when implementing position iterations
                b2World_Step(m_PhysicsWorld, ts.GetSeconds(), velocityIterations);

                // Update 3D physics
                if (m_JoltScene)
                {
                    m_JoltScene->Simulate(ts.GetSeconds());
                }

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

                // Retrieve transforms from Jolt 3D physics
                for (const auto view = m_Registry.view<Rigidbody3DComponent, TransformComponent>(); const auto e : view)
                {
                    Entity entity = { e, this };
                    auto& transform = entity.GetComponent<TransformComponent>();
                    auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();

                    if (rb3d.m_RuntimeBodyToken != 0 && rb3d.m_Type != BodyType3D::Static && m_JoltScene)
                    {
                        // Get the body from JoltScene and sync transforms
                        auto body = m_JoltScene->GetBody(entity);
                        if (body)
                        {
                            auto pos = body->GetPosition();
                            auto rot = body->GetRotation();

                            transform.Translation = pos;
                            transform.Rotation = glm::eulerAngles(rot);
                        }
                    }
                }
            }
        }

        // Render
        RenderScene(camera);
    }

    void Scene::OnUpdateEditor([[maybe_unused]] Timestep const ts, EditorCamera const& camera)
    {
        // Update particle systems so they preview in the editor
        {
            const glm::vec3 camPos = camera.GetPosition();
            for (auto view = m_Registry.view<TransformComponent, ParticleSystemComponent>(); auto entity : view)
            {
                auto& transform = view.get<TransformComponent>(entity);
                auto& psc = view.get<ParticleSystemComponent>(entity);
                psc.System.UpdateLOD(camPos, transform.Translation);
                psc.System.Update(ts, transform.Translation);
            }
        }

        // Render based on mode
        if (m_Is3DModeEnabled)
        {
            RenderScene3D(camera);
        }
        else
        {
            RenderScene(camera);
        }

        // UI overlay renders on top of both 2D and 3D scenes
        RenderUIOverlay();
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

    [[nodiscard]] Entity Scene::GetPrimaryCameraEntity()
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
    void Scene::OnComponentAdded<MeshComponent>(Entity, MeshComponent&) {}
    template<>
    void Scene::OnComponentAdded<ModelComponent>(Entity, ModelComponent&) {}
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
    template<>
    void Scene::OnComponentAdded<DirectionalLightComponent>(Entity, DirectionalLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<PointLightComponent>(Entity, PointLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<SpotLightComponent>(Entity, SpotLightComponent&) {}
    template<>
    void Scene::OnComponentAdded<EnvironmentMapComponent>(Entity, EnvironmentMapComponent&) {}

    [[nodiscard]] Entity Scene::FindEntityByName(std::string_view name)
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

    [[nodiscard]] Entity Scene::GetEntityByUUID(UUID uuid)
    {
        OLO_CORE_ASSERT(m_EntityMap.Contains(uuid));
        return { m_EntityMap.FindChecked(uuid), this };
    }

    [[nodiscard]] Entity Scene::FindEntityByName(std::string_view name) const
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

    [[nodiscard]] Entity Scene::GetEntityByUUID(UUID uuid) const
    {
        OLO_CORE_ASSERT(m_EntityMap.Contains(uuid));
        // SAFETY: this is const Scene*, but Entity requires non-const Scene*
        // This is safe because Entity lookup only reads entity data
        return { m_EntityMap.FindChecked(uuid), const_cast<Scene*>(this) };
    }

    [[nodiscard]] Entity Scene::GetPrimaryCameraEntity() const
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
    std::vector<glm::mat4> Scene::GetModelSpaceBoneTransforms(const std::vector<UUID>& boneEntityIds, const MeshSource& meshSource) const
    {
        return BoneEntityUtils::GetModelSpaceBoneTransforms(boneEntityIds, &meshSource, this);
    }

    std::vector<UUID> Scene::FindBoneEntityIds(Entity rootEntity, const Skeleton& skeleton) const
    {
        return BoneEntityUtils::FindBoneEntityIds(rootEntity, &skeleton, this);
    }

    glm::mat4 Scene::FindRootBoneTransform(Entity entity, const std::vector<UUID>& boneEntityIds) const
    {
        return BoneEntityUtils::FindRootBoneTransform(entity, boneEntityIds, this);
    }

    void Scene::BuildBoneEntityIds(Entity entity)
    {
        // Build bone entity IDs for the given entity, using itself as the root entity
        // This is useful when the entity is both the mesh and the root of the bone hierarchy
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

    [[nodiscard]] std::optional<Entity> Scene::TryGetEntityWithUUID(UUID id) const
    {
        if (auto* entityPtr = m_EntityMap.Find(id))
        {
            // FIXME: const_cast usage should be minimized. Consider updating Entity class
            // to accept const Scene* for read-only operations or create a const-Entity view.
            // This cast is currently necessary as Entity requires non-const Scene* for API consistency.
            return Entity{ *entityPtr, const_cast<Scene*>(this) };
        }
        return std::nullopt;
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

    void Scene::OnPhysics3DStart()
    {
        // Ensure JoltScene was properly initialized in constructor
        OLO_CORE_ASSERT(m_JoltScene, "JoltScene should be initialized in constructor");

        m_JoltScene->Initialize();

        if (!m_JoltScene->IsInitialized())
        {
            OLO_CORE_ERROR("Failed to initialize 3D physics system");
            return;
        }

        // Create physics bodies for all entities with Rigidbody3DComponent
        auto view = m_Registry.view<Rigidbody3DComponent, TransformComponent>();
        for (auto entity : view)
        {
            Entity ent = { entity, this };

            // Create the physics body - JoltScene will handle shape creation based on components
            auto body = m_JoltScene->CreateBody(ent);
            if (body)
            {
                auto& rb3d = ent.GetComponent<Rigidbody3DComponent>();
                // Store only the body token for safe runtime access
                rb3d.m_RuntimeBodyToken = static_cast<std::uint64_t>(body->GetBodyID().GetIndexAndSequenceNumber());
            }
        }
    }

    void Scene::OnPhysics3DStop()
    {
        // Early return if JoltScene is null to avoid null dereference
        if (!m_JoltScene)
        {
            return;
        }

        // Clean up all physics bodies
        auto view = m_Registry.view<Rigidbody3DComponent>();
        for (auto entity : view)
        {
            Entity ent = { entity, this };
            auto& rb3d = ent.GetComponent<Rigidbody3DComponent>();

            if (rb3d.m_RuntimeBodyToken != 0)
            {
                // Destroy the body using the entity
                m_JoltScene->DestroyBody(ent);
                rb3d.m_RuntimeBodyToken = 0;
            }
        }

        m_JoltScene->Shutdown();
    }

    void Scene::RenderUIOverlay()
    {
        OLO_PROFILE_FUNCTION();
        if (m_ViewportWidth == 0 || m_ViewportHeight == 0)
        {
            return;
        }

        UILayoutSystem::ResolveLayout(*this, m_ViewportWidth, m_ViewportHeight);

        glm::mat4 uiProjection = glm::ortho(0.0f, static_cast<f32>(m_ViewportWidth),
                                            static_cast<f32>(m_ViewportHeight), 0.0f,
                                            -1.0f, 1.0f);
        UIRenderer::BeginScene(uiProjection);

        // Build sorted draw order based on UICanvasComponent::m_SortOrder
        auto resolvedView = GetAllEntitiesWith<UIResolvedRectComponent>();
        std::vector<entt::entity> uiEntities;
        for (const auto entity : resolvedView)
        {
            uiEntities.push_back(entity);
        }
        std::sort(uiEntities.begin(), uiEntities.end(),
                  [this](entt::entity a, entt::entity b)
                  {
                      i32 sortA = 0;
                      i32 sortB = 0;
                      if (m_Registry.all_of<UICanvasComponent>(a))
                      {
                          sortA = m_Registry.get<UICanvasComponent>(a).m_SortOrder;
                      }
                      if (m_Registry.all_of<UICanvasComponent>(b))
                      {
                          sortB = m_Registry.get<UICanvasComponent>(b).m_SortOrder;
                      }
                      if (sortA != sortB)
                      {
                          return sortA < sortB;
                      }
                      return static_cast<u32>(a) < static_cast<u32>(b);
                  });

        for (const auto entity : uiEntities)
        {
            auto& resolved = m_Registry.get<UIResolvedRectComponent>(entity);
            const int eid = static_cast<int>(entity);

            if (m_Registry.all_of<UIPanelComponent>(entity))
            {
                UIRenderer::DrawPanel(resolved.m_Position, resolved.m_Size, m_Registry.get<UIPanelComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIButtonComponent>(entity))
            {
                UIRenderer::DrawButton(resolved.m_Position, resolved.m_Size, m_Registry.get<UIButtonComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIImageComponent>(entity))
            {
                UIRenderer::DrawImage(resolved.m_Position, resolved.m_Size, m_Registry.get<UIImageComponent>(entity), eid);
            }

            if (m_Registry.all_of<UITextComponent>(entity))
            {
                UIRenderer::DrawUIText(resolved.m_Position, resolved.m_Size, m_Registry.get<UITextComponent>(entity), eid);
            }

            if (m_Registry.all_of<UISliderComponent>(entity))
            {
                UIRenderer::DrawSlider(resolved.m_Position, resolved.m_Size, m_Registry.get<UISliderComponent>(entity), eid);
            }

            if (m_Registry.all_of<UICheckboxComponent>(entity))
            {
                UIRenderer::DrawCheckbox(resolved.m_Position, resolved.m_Size, m_Registry.get<UICheckboxComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIProgressBarComponent>(entity))
            {
                UIRenderer::DrawProgressBar(resolved.m_Position, resolved.m_Size, m_Registry.get<UIProgressBarComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIInputFieldComponent>(entity))
            {
                UIRenderer::DrawInputField(resolved.m_Position, resolved.m_Size, m_Registry.get<UIInputFieldComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIScrollViewComponent>(entity))
            {
                UIRenderer::DrawScrollView(resolved.m_Position, resolved.m_Size, m_Registry.get<UIScrollViewComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIDropdownComponent>(entity))
            {
                UIRenderer::DrawDropdown(resolved.m_Position, resolved.m_Size, m_Registry.get<UIDropdownComponent>(entity), eid);
            }

            if (m_Registry.all_of<UIToggleComponent>(entity))
            {
                UIRenderer::DrawToggle(resolved.m_Position, resolved.m_Size, m_Registry.get<UIToggleComponent>(entity), eid);
            }
        }

        UIRenderer::EndScene();
    }

    void Scene::RenderScene(EditorCamera const& camera)
    {
        Renderer2D::BeginScene(camera);

        // Draw sprites
        {
            for (const auto view = m_Registry.view<TransformComponent, SpriteRendererComponent>(); const auto entity : view)
            {
                const auto [transform, sprite] = view.get<TransformComponent, SpriteRendererComponent>(entity);

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

        // Draw particles
        {
            for (auto view = m_Registry.view<ParticleSystemComponent>(); auto entity : view)
            {
                auto& psc = view.get<ParticleSystemComponent>(entity);
                ParticleRenderer::RenderParticles2D(psc.System.GetPool(), psc.Texture, static_cast<int>(entity));
            }
        }

        Renderer2D::EndScene();
    }

    // Static cached default material for 3D rendering
    static Ref<Material> s_DefaultMaterial = nullptr;

    static Material& GetDefaultMaterial()
    {
        if (!s_DefaultMaterial)
        {
            s_DefaultMaterial = Material::CreatePBR("Default", glm::vec3(0.8f, 0.8f, 0.8f), 0.0f, 0.5f);
        }
        return *s_DefaultMaterial;
    }

    void Scene::RenderScene3D(EditorCamera const& camera)
    {
        OLO_PROFILE_FUNCTION();

        Renderer3D::BeginScene(camera);

        // Render skybox from EnvironmentMapComponent (first one found)
        {
            auto view = m_Registry.view<EnvironmentMapComponent>();
            for (auto entity : view)
            {
                auto& envMapComp = view.get<EnvironmentMapComponent>(entity);
                if (!envMapComp.m_EnableSkybox)
                    continue;

                // Lazy load environment map from file path if not already loaded
                if (!envMapComp.m_EnvironmentMap && !envMapComp.m_FilePath.empty())
                {
                    if (envMapComp.m_IsCubemapFolder)
                    {
                        // Load 6 cubemap face textures from folder
                        std::string basePath = envMapComp.m_FilePath;
                        // Ensure path ends with separator
                        if (!basePath.empty() && basePath.back() != '/' && basePath.back() != '\\')
                            basePath += '/';

                        std::vector<std::string> skyboxFaces = {
                            basePath + "right.jpg",
                            basePath + "left.jpg",
                            basePath + "top.jpg",
                            basePath + "bottom.jpg",
                            basePath + "front.jpg",
                            basePath + "back.jpg"
                        };

                        auto skyboxCubemap = TextureCubemap::Create(skyboxFaces);
                        if (skyboxCubemap)
                        {
                            envMapComp.m_EnvironmentMap = EnvironmentMap::CreateFromCubemap(skyboxCubemap);
                        }
                    }
                    else
                    {
                        // Load as HDR/EXR equirectangular environment map
                        envMapComp.m_EnvironmentMap = EnvironmentMap::CreateFromEquirectangular(envMapComp.m_FilePath);
                    }
                }

                if (envMapComp.m_EnvironmentMap && envMapComp.m_EnvironmentMap->GetEnvironmentMap())
                {
                    auto* skyboxPacket = Renderer3D::DrawSkybox(envMapComp.m_EnvironmentMap->GetEnvironmentMap());
                    if (skyboxPacket)
                    {
                        Renderer3D::SubmitPacket(skyboxPacket);
                    }
                }
                break; // Only use first environment map
            }
        }

        // Collect and set scene lights from light components
        // Note: We need to pass a Ref<Scene>, so we use a workaround since Scene doesn't inherit from enable_shared_from_this
        // For now, we'll collect lights manually here
        {
            // Set first directional light as the main scene light
            auto dirLightView = m_Registry.view<TransformComponent, DirectionalLightComponent>();
            for (auto entity : dirLightView)
            {
                const auto& [transform, dirLight] = dirLightView.get<TransformComponent, DirectionalLightComponent>(entity);
                Light light;
                light.Type = LightType::Directional;
                light.Direction = dirLight.m_Direction;
                light.Ambient = dirLight.m_Color * 0.1f;
                light.Diffuse = dirLight.m_Color * dirLight.m_Intensity;
                light.Specular = dirLight.m_Color * dirLight.m_Intensity;
                Renderer3D::SetLight(light);
                Renderer3D::SetViewPosition(camera.GetPosition());
                break; // Only use first directional light for now
            }
        }

        // Draw mesh entities (skip animated entities - they're rendered separately)
        {
            auto view = m_Registry.view<TransformComponent, MeshComponent>();
            for (auto entity : view)
            {
                // Skip entities with SkeletonComponent - they're rendered by the animated mesh path
                if (m_Registry.all_of<SkeletonComponent>(entity))
                {
                    continue;
                }

                const auto [transform, mesh] = view.get<TransformComponent, MeshComponent>(entity);

                if (!mesh.m_MeshSource)
                {
                    continue;
                }

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(static_cast<u32>(entity));

                // Draw each submesh with entity ID
                if (mesh.m_MeshSource && !mesh.m_MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < mesh.m_MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(mesh.m_MeshSource, i);
                        auto* packet = Renderer3D::DrawMesh(submesh, transform.GetTransform(), material, true, entityID);
                        if (packet)
                            Renderer3D::SubmitPacket(packet);
                    }
                }
            }
        }

        // Draw submesh entities (if they have their own transforms)
        {
            auto view = m_Registry.view<TransformComponent, SubmeshComponent>();
            for (auto entity : view)
            {
                const auto [transform, submesh] = view.get<TransformComponent, SubmeshComponent>(entity);

                if (!submesh.m_Mesh || !submesh.m_Visible)
                {
                    continue;
                }

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(static_cast<u32>(entity));

                auto* packet = Renderer3D::DrawMesh(submesh.m_Mesh, transform.GetTransform(), material, true, entityID);
                if (packet)
                    Renderer3D::SubmitPacket(packet);
            }
        }

        // Draw model entities (full models with materials from file)
        {
            auto view = m_Registry.view<TransformComponent, ModelComponent>();
            for (auto entity : view)
            {
                const auto [transform, model] = view.get<TransformComponent, ModelComponent>(entity);

                if (!model.m_Model || !model.m_Visible)
                {
                    continue;
                }

                // Model::DrawParallel uses the model's own materials loaded from file
                // Pass entity ID for mouse picking support
                model.m_Model->DrawParallel(transform.GetTransform(), static_cast<int>(entity));
            }
        }

        // Draw animated mesh entities (entities with MeshComponent + SkeletonComponent)
        {
            auto view = m_Registry.view<TransformComponent, MeshComponent, SkeletonComponent>();
            for (auto entity : view)
            {
                const auto [transform, mesh, skeleton] = view.get<TransformComponent, MeshComponent, SkeletonComponent>(entity);

                if (!mesh.m_MeshSource || !skeleton.m_Skeleton)
                {
                    continue;
                }

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Get bone matrices from skeleton
                const auto& boneMatrices = skeleton.m_Skeleton->m_FinalBoneMatrices;

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(static_cast<u32>(entity));

                // Draw each submesh as an animated mesh
                if (!mesh.m_MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < mesh.m_MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(mesh.m_MeshSource, i);
                        auto* packet = Renderer3D::DrawAnimatedMesh(submesh, transform.GetTransform(), material, boneMatrices, false, entityID);
                        if (packet)
                            Renderer3D::SubmitPacket(packet);
                    }
                }

                // Draw skeleton visualization if enabled
                if (m_SkeletonVisualization.ShowSkeleton && skeleton.m_Skeleton)
                {
                    Renderer3D::DrawSkeleton(
                        *skeleton.m_Skeleton,
                        transform.GetTransform(),
                        m_SkeletonVisualization.ShowBones,
                        m_SkeletonVisualization.ShowJoints,
                        m_SkeletonVisualization.JointSize,
                        m_SkeletonVisualization.BoneThickness);
                }
            }
        }

        // TODO: Implement infinite grid as a proper command packet
        // The grid currently uses immediate-mode OpenGL (glDrawArrays) which conflicts
        // with the deferred command buffer system. Grid rendering has been disabled
        // until it can be properly integrated.
        Renderer3D::DrawInfiniteGrid(1.0f);

        // Draw world axis helper at origin
        Renderer3D::DrawWorldAxisHelper(3.0f);

        // Draw light visualization gizmos
        {
            auto view = m_Registry.view<TransformComponent, DirectionalLightComponent>();
            for (auto entity : view)
            {
                const auto& [tc, dirLight] = view.get<TransformComponent, DirectionalLightComponent>(entity);

                // Draw the directional light gizmo with arrow and sun icon
                Renderer3D::DrawDirectionalLightGizmo(
                    tc.Translation,
                    dirLight.m_Direction,
                    dirLight.m_Color,
                    dirLight.m_Intensity);
            }
        }
        {
            auto view = m_Registry.view<TransformComponent, PointLightComponent>();
            for (auto entity : view)
            {
                const auto& [tc, pointLight] = view.get<TransformComponent, PointLightComponent>(entity);

                // Draw the point light gizmo with range sphere
                Renderer3D::DrawPointLightGizmo(
                    tc.Translation,
                    pointLight.m_Range,
                    pointLight.m_Color,
                    true // Show range sphere
                );
            }
        }
        {
            auto view = m_Registry.view<TransformComponent, SpotLightComponent>();
            for (auto entity : view)
            {
                const auto& [tc, spotLight] = view.get<TransformComponent, SpotLightComponent>(entity);

                // Draw the spot light gizmo with cone visualization
                Renderer3D::DrawSpotLightGizmo(
                    tc.Translation,
                    spotLight.m_Direction,
                    spotLight.m_Range,
                    spotLight.m_InnerCutoff,
                    spotLight.m_OuterCutoff,
                    spotLight.m_Color);
            }
        }

        // Draw audio source gizmos
        {
            auto view = m_Registry.view<TransformComponent, AudioSourceComponent>();
            for (auto entity : view)
            {
                const auto& [tc, audioSource] = view.get<TransformComponent, AudioSourceComponent>(entity);

                // Only draw gizmo if spatialization is enabled
                if (audioSource.Config.Spatialization)
                {
                    Renderer3D::DrawAudioSourceGizmo(
                        tc.Translation,
                        audioSource.Config.MinDistance,
                        audioSource.Config.MaxDistance,
                        glm::vec3(0.2f, 0.6f, 1.0f) // Blue color for audio
                    );
                }
            }
        }

        // Draw camera frustum gizmos for scene cameras (only in editor mode)
        {
            auto view = m_Registry.view<TransformComponent, CameraComponent>();
            for (auto entity : view)
            {
                const auto& [transform, cameraComp] = view.get<TransformComponent, CameraComponent>(entity);
                const SceneCamera& sceneCamera = cameraComp.Camera;

                // Calculate aspect ratio (use current viewport aspect if not fixed)
                f32 aspectRatio = (m_ViewportHeight > 0) ? static_cast<f32>(m_ViewportWidth) / static_cast<f32>(m_ViewportHeight) : 1.778f;

                if (sceneCamera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                {
                    Renderer3D::DrawCameraFrustum(
                        transform.GetTransform(),
                        sceneCamera.GetPerspectiveVerticalFOV(),
                        aspectRatio,
                        sceneCamera.GetPerspectiveNearClip(),
                        sceneCamera.GetPerspectiveFarClip(),
                        glm::vec3(0.9f, 0.9f, 0.3f), // Yellow-ish color for frustum
                        true,                        // isPerspective
                        0.0f                         // orthoSize (not used for perspective)
                    );
                }
                else
                {
                    // Orthographic camera
                    Renderer3D::DrawCameraFrustum(
                        transform.GetTransform(),
                        0.0f, // fov (not used for ortho)
                        aspectRatio,
                        sceneCamera.GetOrthographicNearClip(),
                        sceneCamera.GetOrthographicFarClip(),
                        glm::vec3(0.3f, 0.9f, 0.9f), // Cyan color for ortho frustum
                        false,                       // isPerspective
                        sceneCamera.GetOrthographicSize());
                }
            }
        }

        // Draw 3D collider gizmos (green wireframes)
        {
            // Box colliders
            auto boxView = m_Registry.view<TransformComponent, BoxCollider3DComponent>();
            for (auto entity : boxView)
            {
                const auto& [tc, boxCollider] = boxView.get<TransformComponent, BoxCollider3DComponent>(entity);
                glm::vec3 position = tc.Translation + boxCollider.m_Offset;
                glm::quat rotation = glm::quat(tc.Rotation);
                Renderer3D::DrawBoxColliderGizmo(position, boxCollider.m_HalfExtents, rotation);
            }

            // Sphere colliders
            auto sphereView = m_Registry.view<TransformComponent, SphereCollider3DComponent>();
            for (auto entity : sphereView)
            {
                const auto& [tc, sphereCollider] = sphereView.get<TransformComponent, SphereCollider3DComponent>(entity);
                glm::vec3 position = tc.Translation + sphereCollider.m_Offset;
                Renderer3D::DrawSphereColliderGizmo(position, sphereCollider.m_Radius);
            }

            // Capsule colliders
            auto capsuleView = m_Registry.view<TransformComponent, CapsuleCollider3DComponent>();
            for (auto entity : capsuleView)
            {
                const auto& [tc, capsuleCollider] = capsuleView.get<TransformComponent, CapsuleCollider3DComponent>(entity);
                glm::vec3 position = tc.Translation + capsuleCollider.m_Offset;
                glm::quat rotation = glm::quat(tc.Rotation);
                Renderer3D::DrawCapsuleColliderGizmo(position, capsuleCollider.m_Radius, capsuleCollider.m_HalfHeight, rotation);
            }
        }

        // Draw particles as 3D billboards
        {
            glm::vec3 camRight = camera.GetRightDirection();
            glm::vec3 camUp = camera.GetUpDirection();
            for (auto view = m_Registry.view<ParticleSystemComponent>(); auto entity : view)
            {
                auto& psc = view.get<ParticleSystemComponent>(entity);
                ParticleRenderer::RenderParticles3D(psc.System.GetPool(), camRight, camUp, psc.Texture);

                // Render trails if enabled
                if (psc.System.TrailModule.Enabled)
                {
                    TrailRenderer::RenderTrails3D(psc.System.GetPool(), psc.System.GetTrailData(),
                                                  psc.System.TrailModule, camRight, camUp, psc.Texture);
                }
            }
        }

        Renderer3D::EndScene();
    }

    void Scene::RenderScene3D(Camera const& camera, const glm::mat4& cameraTransform)
    {
        OLO_PROFILE_FUNCTION();

        Renderer3D::BeginScene(camera, cameraTransform);

        // Render skybox from EnvironmentMapComponent (first one found)
        {
            auto view = m_Registry.view<EnvironmentMapComponent>();
            for (auto entity : view)
            {
                auto& envMapComp = view.get<EnvironmentMapComponent>(entity);
                if (!envMapComp.m_EnableSkybox)
                    continue;

                // Lazy load environment map from file path if not already loaded
                if (!envMapComp.m_EnvironmentMap && !envMapComp.m_FilePath.empty())
                {
                    if (envMapComp.m_IsCubemapFolder)
                    {
                        // Load 6 cubemap face textures from folder
                        std::string basePath = envMapComp.m_FilePath;
                        if (!basePath.empty() && basePath.back() != '/' && basePath.back() != '\\')
                            basePath += '/';

                        std::vector<std::string> skyboxFaces = {
                            basePath + "right.jpg",
                            basePath + "left.jpg",
                            basePath + "top.jpg",
                            basePath + "bottom.jpg",
                            basePath + "front.jpg",
                            basePath + "back.jpg"
                        };

                        auto skyboxCubemap = TextureCubemap::Create(skyboxFaces);
                        if (skyboxCubemap)
                        {
                            envMapComp.m_EnvironmentMap = EnvironmentMap::CreateFromCubemap(skyboxCubemap);
                        }
                    }
                    else
                    {
                        envMapComp.m_EnvironmentMap = EnvironmentMap::CreateFromEquirectangular(envMapComp.m_FilePath);
                    }
                }

                if (envMapComp.m_EnvironmentMap && envMapComp.m_EnvironmentMap->GetEnvironmentMap())
                {
                    auto* skyboxPacket = Renderer3D::DrawSkybox(envMapComp.m_EnvironmentMap->GetEnvironmentMap());
                    if (skyboxPacket)
                    {
                        Renderer3D::SubmitPacket(skyboxPacket);
                    }
                }
                break; // Only use first environment map
            }
        }

        // Collect and set scene lights from light components
        {
            // Set first directional light as the main scene light
            auto dirLightView = m_Registry.view<TransformComponent, DirectionalLightComponent>();
            for (auto entity : dirLightView)
            {
                const auto& [transform, dirLight] = dirLightView.get<TransformComponent, DirectionalLightComponent>(entity);
                Light light;
                light.Type = LightType::Directional;
                light.Direction = dirLight.m_Direction;
                light.Ambient = dirLight.m_Color * 0.1f;
                light.Diffuse = dirLight.m_Color * dirLight.m_Intensity;
                light.Specular = dirLight.m_Color * dirLight.m_Intensity;
                Renderer3D::SetLight(light);
                // Extract camera position from transform
                Renderer3D::SetViewPosition(glm::vec3(cameraTransform[3]));
                break; // Only use first directional light for now
            }
        }

        // Draw mesh entities (skip animated entities - they're rendered separately)
        {
            auto view = m_Registry.view<TransformComponent, MeshComponent>();
            for (auto entity : view)
            {
                // Skip entities with SkeletonComponent - they're rendered by the animated mesh path
                if (m_Registry.all_of<SkeletonComponent>(entity))
                {
                    continue;
                }

                const auto [transform, mesh] = view.get<TransformComponent, MeshComponent>(entity);

                if (!mesh.m_MeshSource)
                {
                    continue;
                }

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(static_cast<u32>(entity));

                // Draw each submesh with entity ID
                if (mesh.m_MeshSource && !mesh.m_MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < mesh.m_MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(mesh.m_MeshSource, i);
                        auto* packet = Renderer3D::DrawMesh(submesh, transform.GetTransform(), material, true, entityID);
                        if (packet)
                            Renderer3D::SubmitPacket(packet);
                    }
                }
            }
        }

        // Draw submesh entities (if they have their own transforms)
        {
            auto view = m_Registry.view<TransformComponent, SubmeshComponent>();
            for (auto entity : view)
            {
                const auto [transform, submesh] = view.get<TransformComponent, SubmeshComponent>(entity);

                if (!submesh.m_Mesh || !submesh.m_Visible)
                {
                    continue;
                }

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(static_cast<u32>(entity));

                auto* packet = Renderer3D::DrawMesh(submesh.m_Mesh, transform.GetTransform(), material, true, entityID);
                if (packet)
                    Renderer3D::SubmitPacket(packet);
            }
        }

        // Draw model entities (full models with materials from file)
        {
            auto view = m_Registry.view<TransformComponent, ModelComponent>();
            for (auto entity : view)
            {
                const auto [transform, model] = view.get<TransformComponent, ModelComponent>(entity);

                if (!model.m_Model || !model.m_Visible)
                {
                    continue;
                }

                // Model::DrawParallel uses the model's own materials loaded from file
                // Pass entity ID for mouse picking support
                model.m_Model->DrawParallel(transform.GetTransform(), static_cast<int>(entity));
            }
        }

        // Draw animated mesh entities (entities with MeshComponent + SkeletonComponent)
        {
            auto view = m_Registry.view<TransformComponent, MeshComponent, SkeletonComponent>();
            for (auto entity : view)
            {
                const auto [transform, mesh, skeleton] = view.get<TransformComponent, MeshComponent, SkeletonComponent>(entity);

                if (!mesh.m_MeshSource || !skeleton.m_Skeleton)
                {
                    continue;
                }

                // Get material or use cached default
                const Material& material = m_Registry.all_of<MaterialComponent>(entity)
                                               ? m_Registry.get<MaterialComponent>(entity).m_Material
                                               : GetDefaultMaterial();

                // Get bone matrices from skeleton
                const auto& boneMatrices = skeleton.m_Skeleton->m_FinalBoneMatrices;

                // Convert entt entity to int for entity ID picking
                i32 entityID = static_cast<i32>(static_cast<u32>(entity));

                // Draw each submesh as an animated mesh
                if (!mesh.m_MeshSource->GetSubmeshes().IsEmpty())
                {
                    for (i32 i = 0; i < mesh.m_MeshSource->GetSubmeshes().Num(); ++i)
                    {
                        auto submesh = Ref<Mesh>::Create(mesh.m_MeshSource, i);
                        auto* packet = Renderer3D::DrawAnimatedMesh(submesh, transform.GetTransform(), material, boneMatrices, false, entityID);
                        if (packet)
                            Renderer3D::SubmitPacket(packet);
                    }
                }
            }
        }

        // Draw particles as 3D billboards
        {
            glm::vec3 camRight = glm::normalize(glm::vec3(cameraTransform[0]));
            glm::vec3 camUp = glm::normalize(glm::vec3(cameraTransform[1]));
            for (auto view = m_Registry.view<ParticleSystemComponent>(); auto entity : view)
            {
                auto& psc = view.get<ParticleSystemComponent>(entity);
                ParticleRenderer::RenderParticles3D(psc.System.GetPool(), camRight, camUp, psc.Texture);

                // Render trails if enabled
                if (psc.System.TrailModule.Enabled)
                {
                    TrailRenderer::RenderTrails3D(psc.System.GetPool(), psc.System.GetTrailData(),
                                                  psc.System.TrailModule, camRight, camUp, psc.Texture);
                }
            }
        }

        Renderer3D::EndScene();
    }

} // namespace OloEngine

// Template specializations for component callbacks
template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::IDComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::IDComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TransformComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TransformComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CameraComponent>([[maybe_unused]] OloEngine::Entity entity, OloEngine::CameraComponent& component)
{
    if ((m_ViewportWidth > 0) && (m_ViewportHeight > 0))
    {
        component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
    }
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ScriptComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ScriptComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::SpriteRendererComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::SpriteRendererComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CircleRendererComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CircleRendererComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TagComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TagComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::Rigidbody2DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::Rigidbody2DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::BoxCollider2DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::BoxCollider2DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CircleCollider2DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CircleCollider2DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TextComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TextComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::AudioSourceComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::AudioSourceComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::AudioListenerComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::AudioListenerComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::RelationshipComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::RelationshipComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::PrefabComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::PrefabComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::Rigidbody3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::Rigidbody3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::BoxCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::BoxCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::SphereCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::SphereCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CapsuleCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CapsuleCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::CharacterController3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::CharacterController3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::MeshCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::MeshCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ConvexMeshCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ConvexMeshCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::TriangleMeshCollider3DComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::TriangleMeshCollider3DComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UICanvasComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UICanvasComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIRectTransformComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIRectTransformComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIResolvedRectComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIResolvedRectComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIImageComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIImageComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIPanelComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIPanelComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UITextComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UITextComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIButtonComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIButtonComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UISliderComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UISliderComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UICheckboxComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UICheckboxComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIProgressBarComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIProgressBarComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIInputFieldComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIInputFieldComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIScrollViewComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIScrollViewComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIDropdownComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIDropdownComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIGridLayoutComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIGridLayoutComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::UIToggleComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::UIToggleComponent& component)
{
}

template<>
void OloEngine::Scene::OnComponentAdded<OloEngine::ParticleSystemComponent>([[maybe_unused]] OloEngine::Entity entity, [[maybe_unused]] OloEngine::ParticleSystemComponent& component)
{
}
