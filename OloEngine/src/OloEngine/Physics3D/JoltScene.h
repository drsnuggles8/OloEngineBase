#pragma once

#include "Physics3DTypes.h"
#include "SceneQueries.h"
#include "JoltUtils.h"
#include "JoltLayerInterface.h"
#include "JoltContactListener.h"
#include "JoltBody.h"
#include "JoltCharacterController.h"
#include "EntityExclusionUtils.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Entity.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/Reference.h>
#include "JoltJobSystemAdapter.h"
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
// Full type (not a forward decl): ClothRuntime holds a JPH::Ref<SoftBodySharedSettings>
// member, so its implicit destructor instantiates ~Ref → Release(), which Clang requires
// the complete type for wherever this header is included (issue #460).
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace JPH
{
    class VehicleConstraint; // Forward declaration — full type only needed in JoltScene.cpp
} // namespace JPH

namespace OloEngine
{

    class Scene; // Forward declaration

    class JoltScene : public SceneQueries
    {
      public:
        JoltScene(Scene* scene);
        ~JoltScene();

        // Initialization
        void Initialize();
        void Shutdown();
        bool IsInitialized() const
        {
            return m_Initialized;
        }

        // Simulation.
        //
        // Simulate() is the synchronous whole-frame tick (editor Simulate mode,
        // and the scheduler's sequential fallback). It is composed of the phase
        // methods below, which the runtime gameplay scheduler also drives
        // directly so the ECS-free world phase can run inside an engine task
        // while game-thread systems execute in the physics shadow (issue #453):
        //
        //   PreSimulate()                       — GAME THREAD. Drains the queued
        //       contact events into gameplay (Scene::OnContactEvent) — handlers
        //       may grow script/registry access, so this must never move off the
        //       game thread.
        //   BeginSteps(dt) -> N                 — GAME THREAD. Advances the fixed-
        //       step accumulator (with the spiral-of-death clamp) and CONSUMES
        //       the time for the N steps it authorizes this frame.
        //   Step(fixedDt), N times              — one full fixed step, composed of:
        //       StepCharacterAndVehiclePhase()  — GAME THREAD. Character
        //           controllers Pre/Simulate + vehicle driver-input push; both
        //           read the ECS registry (TransformComponent, VehicleComponent,
        //           entity lookups) and character contact callbacks fire here.
        //       StepWorldPhase()                — WORKER-SAFE. The Jolt world
        //           update (whose jobs already fan out to the task pool), the
        //           #281 outstanding-task drain, error throttling, and character
        //           PostSimulate (pure Jolt-internal state). Touches NO ECS
        //           state; Jolt contact callbacks only enqueue under a mutex.
        //       StepJointBreakPhase()           — GAME THREAD. Break-impulse scan
        //           (reads PhysicsJoint3DComponent, publishes JointBrokeEvent,
        //           destroys constraints). Must run before the NEXT world update
        //           overwrites the per-step constraint impulses.
        //   SynchronizeTransforms()             — GAME THREAD. Writes body poses
        //       back to ECS TransformComponents (declared further below).
        //
        // The async single-step split (kick: char/vehicle phase → task: world
        // phase → fence: joint-break + transform sync) is exactly equivalent to
        // Step() when N == 1; Scene::KickPhysicsStep falls back to whole-frame
        // synchronous stepping for N != 1 (idle or hitch catch-up frames).
        void Simulate(f32 deltaTime);
        void PreSimulate();
        [[nodiscard]] u32 BeginSteps(f32 deltaTime);
        void Step(f32 fixedTimeStep);
        void StepCharacterAndVehiclePhase(f32 fixedTimeStep);
        void StepWorldPhase(f32 fixedTimeStep);
        void StepJointBreakPhase(f32 fixedTimeStep);

        // Gravity
        glm::vec3 GetGravity() const;
        void SetGravity(const glm::vec3& gravity);

        // Body management
        Ref<JoltBody> CreateBody(Entity entity);
        void DestroyBody(Entity entity);
        Ref<JoltBody> GetBody(Entity entity);
        Ref<JoltBody> GetBodyByEntityID(UUID entityID);

        // Entity lookup by body ID (for character controller integration)
        Entity GetEntityByBodyID(const JPH::BodyID& bodyID);

        // Terrain collision (static height-field bodies). A terrain entity carries no
        // Rigidbody3DComponent / JoltBody wrapper, so its collision is a raw static JPH
        // body tracked separately, keyed by the owning TerrainComponent entity. The
        // caller (Scene) builds the JPH::HeightFieldShape from the terrain's CPU heights
        // and the entity transform, then hands it here. The body's UserData is the entity
        // UUID and it is registered in the BodyID→entity reverse map, so raycasts and
        // contacts resolve back to the terrain entity. CreateTerrainBody is idempotent —
        // it replaces any existing terrain body for the entity. Returns the new BodyID
        // (invalid on failure). DestroyTerrainBody removes it by entity UUID.
        JPH::BodyID CreateTerrainBody(Entity entity, const JPH::Ref<JPH::Shape>& shape,
                                      const glm::vec3& position, const glm::quat& rotation);
        void DestroyTerrainBody(UUID entityID);
        bool HasTerrainBody(UUID entityID) const
        {
            return m_TerrainBodies.contains(entityID);
        }

        // Per-tile terrain collision for STREAMED terrain (issue #469). A streaming
        // TerrainComponent owns many static height-field bodies at once — one per
        // loaded tile — instead of the single body above, so they are tracked in a
        // separate map keyed by (owning terrain entity UUID, tile grid X, tile grid Z).
        // Each tile body is otherwise identical to the single-tile terrain body: a raw
        // static JPH body on NON_MOVING, UserData = the OWNING TERRAIN ENTITY UUID (so a
        // raycast against any tile resolves the terrain entity), entered in the
        // BodyID→entity reverse map. The caller (Scene) builds the tile's HeightFieldShape
        // from that tile's CPU heights and places it at the tile's world origin. Create is
        // idempotent per (entity, x, z). Returns the new BodyID (invalid on failure).
        JPH::BodyID CreateTerrainTileBody(Entity terrainEntity, i32 tileX, i32 tileZ,
                                          const JPH::Ref<JPH::Shape>& shape,
                                          const glm::vec3& position, const glm::quat& rotation);
        void DestroyTerrainTileBody(UUID terrainEntityID, i32 tileX, i32 tileZ);
        bool HasTerrainTileBody(UUID terrainEntityID, i32 tileX, i32 tileZ) const;
        // Grid coords of every tile body currently tracked for a terrain. The per-frame
        // streaming reconcile uses this to destroy the tiles that are no longer loaded.
        [[nodiscard]] std::vector<std::pair<i32, i32>> GetTerrainTileCoords(UUID terrainEntityID) const;
        // Destroy every tile body owned by a terrain (streaming disabled at runtime, or
        // the terrain entity destroyed). Cheap no-op when the terrain owns no tile bodies.
        void DestroyTerrainTilesForEntity(UUID terrainEntityID);

        // Partial in-place update of a SINGLE-TILE terrain body's height field after a
        // sculpt / erosion edit (issue #469). Mutates the body's JPH::HeightFieldShape via
        // HeightFieldShape::SetHeights over the dirty rect [regionX, regionX+regionWidth) ×
        // [regionZ, regionZ+regionHeight) (in heightmap sample coords, X = column / world
        // X, Z = row / world Z), snapping it outward to the shape's block-size grid (Jolt
        // requires block alignment) and edge-replicating any sample the pad added beyond
        // `resolution`, then NotifyShapeChanged to refresh the broadphase bounds.
        // `fullHeights` / `resolution` are the terrain's full CPU field (row-major,
        // normalized [0,1], index = z*resolution + x) — the SAME field the shape was built
        // from, now edited. MUST be called on the game thread OUTSIDE the physics step:
        // SetHeights mutates shared shape data and races concurrent collision queries
        // (see HeightFieldShape::SetHeights docs). Returns false if the terrain has no
        // single-tile body, the body's shape is not a height field, or the input is bad.
        bool UpdateTerrainBodyHeights(UUID terrainEntityID, u32 regionX, u32 regionZ,
                                      u32 regionWidth, u32 regionHeight,
                                      const std::vector<f32>& fullHeights, u32 resolution);

        // Cloth / soft-body management (issue #460). Like terrain bodies, a cloth entity
        // carries no Rigidbody3DComponent / JoltBody wrapper — its Jolt soft body is a raw
        // JPH body tracked separately, keyed by the owning ClothComponent entity. The caller
        // (Scene) builds the JPH::SoftBodySharedSettings from the cloth grid + entity
        // transform (JoltShapes::CreateClothSharedSettings), then hands it here. The body's
        // UserData is the entity UUID and it is registered in the BodyID→entity reverse map.
        // CreateClothBody is idempotent — it replaces any existing cloth body for the entity.
        // Returns the new BodyID (invalid on failure). The soft body is created on the MOVING
        // layer so it falls under gravity and collides with static/rigid geometry.
        JPH::BodyID CreateClothBody(Entity entity, const JPH::Ref<JPH::SoftBodySharedSettings>& settings,
                                    u32 iterations, f32 linearDamping, f32 pressure);
        void DestroyClothBody(UUID entityID);
        bool HasClothBody(UUID entityID) const
        {
            return m_Cloths.contains(entityID);
        }
        [[nodiscard("cloth count query result must be used")]] u32 GetClothCount() const
        {
            return static_cast<u32>(m_Cloths.size());
        }
        // Read the current world-space particle positions of a cloth soft body into
        // outWorldPositions (row-major, same order as the generating grid). GPU-free — runs
        // headless / on the dedicated server. Returns false if there is no cloth body for the
        // entity or the body lock fails; on success outWorldPositions has one entry per
        // particle. Called each frame by Scene to drive the deforming render mesh and by the
        // functional tests to assert the cloth drapes / rests on the floor.
        [[nodiscard("cloth vertex readback result must be used")]] bool GetClothVertices(UUID entityID, std::vector<glm::vec3>& outWorldPositions) const;

        // Live world-space centre-of-mass position of a cloth's soft body, read directly from
        // Jolt (JPH::Body::GetCenterOfMassPosition) rather than the cached ECS
        // WorldTransformComponent, which can be up to one physics tick stale by the time
        // ClothWindSystem samples it (PropagateTransforms runs after PhysicsKick/PhysicsFence
        // in the gameplay scheduler — issue #460 wind-coupling slice). Mirrors how
        // BuoyancySystem samples a rigid body's live JoltBody::GetPosition() instead of its
        // ECS transform. Returns false (outPosition untouched) if entityID has no live cloth
        // body.
        [[nodiscard("cloth position query result must be used")]] bool GetClothWorldPosition(UUID entityID, glm::vec3& outPosition) const;

        // Report the particle indices a cloth's soft body has pinned (inverse mass 0) —
        // the set JoltShapes::CreateClothSharedSettings fixed per ClothAttachment. Scene
        // captures these once at physics start to know which vertices to drive kinematically
        // from a skeleton bone (issue #460 cape slice). Returns false (outIndices cleared)
        // if entityID has no live cloth body or the body lock fails; the returned indices
        // are into the same row-major particle order as GetClothVertices.
        [[nodiscard("cloth pinned-index query result must be used")]] bool GetClothPinnedVertexIndices(UUID entityID, std::vector<u32>& outIndices) const;

        // Drive a cloth's pinned (kinematic, inverse-mass-0) particles toward per-vertex
        // world-space target positions by setting their velocity (issue #460 cape slice) —
        // Jolt's recommended way to kinematically control a soft-body vertex (SoftBodyVertex:
        // "at run-time you should only modify the inverse mass and/or velocity"). velocity =
        // (target - current) / dt lands each vertex on its target across the frame's
        // sub-steps (IntegratePositions advances every particle by mVelocity*dt and a pinned
        // particle keeps that velocity — no gravity/damping touches it). vertexIndices and
        // targetWorldPositions must be 1:1; only particles with inverse mass 0 are touched
        // (a free particle is never perturbed). Wakes the body so a moving attachment keeps
        // it simulating. No-op if entityID has no cloth body, dt is non-positive/non-finite,
        // or the arrays mismatch.
        void DriveClothAttachment(UUID entityID, const std::vector<u32>& vertexIndices,
                                  const std::vector<glm::vec3>& targetWorldPositions, f32 dt);

        // Queue a uniform whole-body force (Newtons, world space) on a cloth's soft body —
        // e.g. wind (ClothWindSystem, issue #460). Delegates to JPH::BodyInterface::AddForce,
        // which the soft-body sub-stepper divides evenly across every particle and resets
        // after PhysicsSystem::Update, so the caller must queue it before Simulate()/
        // BeginSteps() runs for the frame it should apply to (same timing contract as
        // BuoyancySystem's rigid-body forces). No-op if entityID has no cloth body or the
        // force is non-finite.
        void ApplyClothWindForce(UUID entityID, const glm::vec3& force);

        // Two-body constraint (joint) management. CreateConstraint builds the
        // Jolt constraint for the PhysicsJoint3DComponent on `entity` (both
        // endpoint bodies must already exist) and sets the component's
        // m_RuntimeConstraintToken on success; it is called from the runtime-start
        // second pass and from the runtime-add hook. DestroyConstraint releases it.
        bool CreateConstraint(Entity entity);
        void DestroyConstraint(Entity entity);
        [[nodiscard("constraint count query result must be used")]] u32 GetConstraintCount() const
        {
            return static_cast<u32>(m_Constraints.size());
        }

        // (Re)build the collision filtering for joints that opted out of letting
        // their two connected bodies collide (PhysicsJoint3DComponent::
        // m_CollideConnected == false). Idempotent: each call resets bodies a
        // previous call grouped, then assigns a single shared collision group +
        // a fresh GroupFilterTable that disables exactly the authored no-collide
        // pairs. Call it after the constraint pass at runtime start and after a
        // joint is added/removed at runtime. Safe to call when no joint opts out
        // (it just clears any prior filtering).
        void ApplyJointCollisionFilters();

        // Vehicle (wheeled) management. CreateVehicle builds the Jolt
        // VehicleConstraint + WheeledVehicleController for the VehicleComponent on
        // `entity` (the chassis body must already exist), registers it as both a
        // constraint and a step listener, and sets the component's
        // m_RuntimeVehicleToken on success; it is called from the runtime-start
        // pass and the runtime-add hook. DestroyVehicle removes the step listener,
        // releases the constraint, and clears the token. Both are idempotent.
        bool CreateVehicle(Entity entity);
        void DestroyVehicle(Entity entity);
        [[nodiscard("vehicle count query result must be used")]] u32 GetVehicleCount() const
        {
            return static_cast<u32>(m_Vehicles.size());
        }

        // Ragdoll (skeleton-driven SwingTwist chain) management, issue #308 item 5.
        // CreateRagdoll expands the RagdollComponent on `entity` into a chain of
        // Rigidbody3D + collider + SwingTwist-joint components authored onto the
        // skeleton's bone entities; it does NOT touch Jolt itself, so it must run
        // before the body/constraint creation passes (see Scene::OnPhysics3DStart).
        // DestroyRagdoll removes exactly the components it generated, restoring the
        // authored scene. Both are idempotent. CreateRagdolls builds every enabled
        // ragdoll in the scene; DestroyAllRagdolls tears them all down. The plural
        // forms are driven by Scene::OnPhysics3DStart / OnPhysics3DStop.
        bool CreateRagdoll(Entity entity);
        void DestroyRagdoll(Entity entity);
        // Authoring pass (no Jolt objects created here): expand every enabled
        // RagdollComponent into per-bone Rigidbody3D + collider + SwingTwist-joint
        // components. MUST run BEFORE Initialize / the body + constraint passes.
        void CreateRagdolls();
        // Remove every generated ragdoll component (restoring the authored scene).
        void DestroyAllRagdolls();
        [[nodiscard("ragdoll count query result must be used")]] u32 GetRagdollCount() const
        {
            return static_cast<u32>(m_Ragdolls.size());
        }

        // Character controller management
        Ref<JoltCharacterController> CreateCharacterController(Entity entity, const ContactCallbackFn& contactCallback = nullptr);
        void DestroyCharacterController(Entity entity);
        Ref<JoltCharacterController> GetCharacterController(Entity entity);
        Ref<JoltCharacterController> GetCharacterControllerByEntityID(UUID entityID);

        // Layer interface access for character controllers
        // DEPRECATED: Use GetJoltSystem() instead. This method may return nullptr if not initialized.
        JPH::PhysicsSystem* GetPhysicsSystem() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing PhysicsSystem");
            return m_JoltSystem.get();
        }

        // Scene lifecycle
        void OnRuntimeStart();
        void OnRuntimeStop();
        void OnSimulationStart() const;
        void OnSimulationStop() const;

        // Scene queries (implementing SceneQueries interface)
        bool CastRay(const RayCastInfo& rayInfo, SceneQueryHit& outHit) override;
        bool CastShape(const ShapeCastInfo& shapeCastInfo, SceneQueryHit& outHit) override;
        bool CastBox(const BoxCastInfo& boxCastInfo, SceneQueryHit& outHit) override;
        bool CastSphere(const SphereCastInfo& sphereCastInfo, SceneQueryHit& outHit) override;
        bool CastCapsule(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit& outHit) override;
        i32 OverlapShape(const ShapeOverlapInfo& overlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 OverlapBox(const BoxOverlapInfo& boxOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 OverlapSphere(const SphereOverlapInfo& sphereOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 OverlapCapsule(const CapsuleOverlapInfo& capsuleOverlapInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastRayMultiple(const RayCastInfo& rayInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastShapeMultiple(const ShapeCastInfo& shapeCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastBoxMultiple(const BoxCastInfo& boxCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastSphereMultiple(const SphereCastInfo& sphereCastInfo, SceneQueryHit* outHits, i32 maxHits) override;
        i32 CastCapsuleMultiple(const CapsuleCastInfo& capsuleCastInfo, SceneQueryHit* outHits, i32 maxHits) override;

        // Radial impulse
        void AddRadialImpulse(const glm::vec3& origin, f32 radius, f32 strength, EFalloffMode falloff, bool velocityChange);

        // Entity teleportation
        void Teleport(Entity entity, const glm::vec3& targetPosition, const glm::quat& targetRotation, bool force = false);

        // Floating-origin rebase (issue #429 / #613): shift every physics body,
        // static terrain height-field, cloth soft body, character controller, AND
        // world-anchored constraint anchor by `delta` in one pass, preserving
        // linear/angular velocities, rotations, and sleep state (SetPosition with
        // DontActivate). Called by Scene::RebaseOrigin on the game thread with the
        // simulation idle (between ticks). Two-body joints between real bodies stay
        // consistent implicitly (both endpoints translate together); world-anchored
        // ones need the explicit ShiftWorldAnchoredConstraints pass. Cloth vertices
        // are stored relative to the soft body's COM, so shifting the body origin
        // moves the whole cloth (see the m_Cloths loop). With #613 nothing physics-
        // side is left behind, so the rebase no longer needs to be deferred.
        void ShiftOrigin(const glm::vec3& delta);

        // Floating-origin rebase support: true if any live constraint holds an
        // ABSOLUTE world-space anchor — a pulley (its two pivot points are world
        // fixed points) or a single-body joint realised against the shared fixed
        // world body (its world-side anchor is an absolute point). Historically
        // this gated a rebase *deferral*; as of #613 ShiftWorldAnchoredConstraints
        // translates those anchors during the shift, so the predicate is retained
        // only for diagnostics / tests (the presence of such a constraint no longer
        // blocks a rebase). Two-body joints between real bodies are NOT reported:
        // their WorldSpace settings are converted to body-local frames at Create(),
        // so both endpoints translate together and the constraint is preserved.
        [[nodiscard]] bool HasWorldAnchoredConstraints() const;

        // Floating-origin rebase (issue #613): translate every world-anchored
        // constraint's ABSOLUTE anchor by `delta`, so a coordinate shift keeps the
        // body↔anchor geometry identical instead of yanking the body. Called by
        // ShiftOrigin AFTER the bodies have moved. For single-body-to-world joints
        // (Fixed/Point/Distance/Hinge/Slider/Cone/SwingTwist/SixDOF/Path) the
        // world-side anchor is shifted exactly in place via the constraint's own
        // NotifyShapeChanged(sFixedToWorld, -delta) — preserving the joint's rest
        // state, motors and warm-start. A pulley's fixed pivots have no runtime
        // setter, so its authored PhysicsJoint3DComponent::m_PulleyFixedPointA/B are
        // shifted and the constraint is rebuilt (DestroyConstraint + CreateConstraint
        // from the already-shifted transforms). Two-body joints and vehicles need
        // nothing here.
        void ShiftWorldAnchoredConstraints(const glm::vec3& delta);

        // Transform synchronization
        void SynchronizeTransforms();

        // Contact events
        void OnContactEvent(ContactType type, UUID entityA, UUID entityB) const;

        // Jolt system access
        // NOTE: These methods require Initialize() to have been called successfully
        JPH::BodyInterface& GetBodyInterface()
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing BodyInterface");
            return m_JoltSystem->GetBodyInterface();
        }
        const JPH::BodyInterface& GetBodyInterface() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing BodyInterface");
            return m_JoltSystem->GetBodyInterface();
        }
        const JPH::BodyLockInterface& GetBodyLockInterface() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing BodyLockInterface");
            return m_JoltSystem->GetBodyLockInterface();
        }
        JPH::PhysicsSystem& GetJoltSystem()
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing PhysicsSystem");
            return *m_JoltSystem;
        }
        const JPH::PhysicsSystem& GetJoltSystem() const
        {
            OLO_CORE_ASSERT(m_JoltSystem, "JoltScene not initialized - call Initialize() before accessing PhysicsSystem");
            return *m_JoltSystem;
        }

        // Pointer version for null-checking (prefer reference versions above when possible)
        JPH::PhysicsSystem* GetJoltSystemPtr() const
        {
            return m_JoltSystem.get();
        }

        // Debug info
        u32 GetBodyCount() const
        {
            return m_JoltSystem ? m_JoltSystem->GetNumBodies() : 0;
        }
        u32 GetActiveBodyCount() const
        {
            return m_JoltSystem ? m_JoltSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
        }
        // The system limits InitializeJolt() actually passed to JPH::PhysicsSystem::Init,
        // read from PhysicsSettings at init time (0 before Initialize()). Exposed so tests
        // can verify a custom PhysicsSettings is honoured rather than silently ignored in
        // favor of a hardcoded constant (issue #523).
        u32 GetMaxBodies() const
        {
            return m_AppliedMaxBodies;
        }
        u32 GetMaxBodyPairs() const
        {
            return m_AppliedMaxBodyPairs;
        }
        u32 GetMaxContactConstraints() const
        {
            return m_AppliedMaxContactConstraints;
        }
        // The fixed timestep InitializeJolt() actually seeded from PhysicsSettings
        // (defaults to 1/60 before Initialize(), or if PhysicsSettings::m_FixedTimestep
        // was non-positive/non-finite). Exposed so tests can verify it, mirroring the
        // GetMaxBodies()-style accessors above (issue #540).
        f32 GetFixedTimeStep() const
        {
            return m_FixedTimeStep;
        }
        // The solver/sleep tuning InitializeJolt() actually passed to
        // JPH::PhysicsSystem::SetPhysicsSettings, read from PhysicsSettings at init
        // time. Exposed so tests can verify solver/sleep settings are honoured by the
        // live sim rather than only reaching the never-stepped Physics3DSystem (issue #540).
        JPH::PhysicsSettings GetAppliedPhysicsSettings() const
        {
            return m_JoltSystem ? m_JoltSystem->GetPhysicsSettings() : JPH::PhysicsSettings{};
        }

        // Read-only snapshot of the entity pairs whose bodies are currently in
        // contact, deduplicated per entity pair. Empty when the contact listener
        // is absent (physics not initialized). Backs the read-only MCP
        // olo_physics_contacts diagnostics tool. Thread-safe (listener-locked).
        [[nodiscard("contact-pair snapshot must be used")]] std::vector<std::pair<UUID, UUID>> GetActiveContactPairs() const;

      private:
        void CreateRigidBodies();
        // Second pass after CreateRigidBodies(): build every authored joint's
        // Jolt constraint now that all bodies exist.
        void CreateConstraints();
        // Remove and release every tracked constraint. Must run before the
        // bodies they reference are destroyed.
        void DestroyAllConstraints();
        // Remove every tracked static terrain height-field body. Called on runtime
        // stop / shutdown while m_JoltSystem is still alive.
        void DestroyAllTerrainBodies();
        // Remove every tracked cloth soft body. Called on runtime stop / shutdown while
        // m_JoltSystem is still alive (before the bodies' shared settings are released).
        void DestroyAllClothBodies();
        // Second pass after CreateRigidBodies(): build every authored vehicle's
        // Jolt VehicleConstraint now that the chassis bodies exist.
        void CreateVehicles();
        // Remove (and unregister the step listener for) every tracked vehicle.
        // Must run before the chassis bodies they reference are destroyed.
        void DestroyAllVehicles();
        // Before each simulation step, push the authored driver input from every
        // VehicleComponent into its WheeledVehicleController (and wake the chassis
        // when there is input), so the vehicle's OnStep listener acts on it.
        void UpdateVehicleControllers();
        // After a simulation step, read back each breakable constraint's
        // accumulated impulse (force/torque = impulse / dt), and for any that
        // exceeds its authored PhysicsJoint3DComponent break threshold: remove
        // the constraint, clear the component's m_RuntimeConstraintToken, and
        // publish a JointBrokeEvent on the Scene's GameplayEventBus.
        void BreakOverstressedJoints(f32 stepDeltaTime);
        void SynchronizeBody(Ref<JoltBody> body) const;

        // Internal Jolt setup
        void InitializeJolt();
        void ShutdownJolt();

        // Scene query helpers - legacy vector-based interface (O(n) performance)
        // ⚠️  DEPRECATED: These methods have O(n) performance due to linear entity exclusion checks.
        // Migration: Use the ExcludedEntitySet overloads below for O(1) performance with repeated queries.
        // Performance Note: Each query performs std::find() over the excluded entities vector, causing
        // significant performance degradation with large exclusion lists or frequent queries.
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        bool PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                              f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit& outHit);
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        i32 PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits);
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        i32 PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                u32 layerMask, const std::vector<UUID>& excludedEntities, SceneQueryHit* outHits, i32 maxHits);
        [[deprecated("Use ExcludedEntitySet overloads for O(1) performance - this vector-based API has O(n) lookup cost")]]
        bool IsEntityExcluded(UUID entityID, const std::vector<UUID>& excludedEntities);

        // Scene query helpers - optimized O(1) ExcludedEntitySet interface
        // ✅ PREFERRED: These methods provide O(1) entity exclusion checks for optimal performance.
        // Performance Note: Uses std::unordered_set for constant-time entity lookup during queries.
        bool PerformShapeCast(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                              f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit& outHit) const;
        i32 PerformShapeCastMultiple(JPH::Ref<JPH::Shape> shape, const glm::vec3& start, const glm::vec3& direction,
                                     f32 maxDistance, u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits);
        i32 PerformShapeOverlap(JPH::Ref<JPH::Shape> shape, const glm::vec3& position, const glm::quat& rotation,
                                u32 layerMask, const ExcludedEntitySet& excludedEntitySet, SceneQueryHit* outHits, i32 maxHits);
        bool IsEntityExcluded(UUID entityID, const ExcludedEntitySet& excludedEntitySet);

        void FillHitInfo(const JPH::RayCastResult& hit, const JPH::RRayCast& ray, SceneQueryHit& outHit) const;
        void FillHitInfo(const JPH::ShapeCastResult& hit, const JPH::RShapeCast& shapeCast, SceneQueryHit& outHit) const;

      private:
        Scene* m_Scene;
        bool m_Initialized = false;

        // Jolt physics system
        std::unique_ptr<JPH::TempAllocator> m_TempAllocator;
        std::unique_ptr<JoltJobSystemAdapter> m_JobSystem;
        std::unique_ptr<JPH::PhysicsSystem> m_JoltSystem;

        // Layer interfaces
        std::unique_ptr<BroadPhaseLayerInterface> m_BroadPhaseLayerInterface;
        std::unique_ptr<ObjectVsBroadPhaseLayerFilter> m_ObjectVsBroadPhaseLayerFilter;
        std::unique_ptr<ObjectLayerPairFilter> m_ObjectLayerPairFilter;

        // Contact listener
        std::unique_ptr<JoltContactListener> m_ContactListener;

        // Body management
        std::unordered_map<UUID, Ref<JoltBody>> m_Bodies;
        std::unordered_map<JPH::BodyID, UUID> m_BodyIDToEntity; // Reverse lookup for efficient GetEntityByBodyID
        std::vector<Ref<JoltBody>> m_BodiesToSync;

        // Static terrain height-field collision bodies, keyed by the owning
        // TerrainComponent entity. These are raw JPH bodies (no JoltBody wrapper), so
        // they live outside m_Bodies; they are still entered in m_BodyIDToEntity so
        // queries resolve them. Created/destroyed by Create/DestroyTerrainBody.
        std::unordered_map<UUID, JPH::BodyID> m_TerrainBodies;

        // Per-tile terrain height-field bodies for STREAMED terrain (issue #469), keyed by
        // (owning terrain entity UUID, tile grid X, tile grid Z). Same raw-static-body
        // convention as m_TerrainBodies (entered in m_BodyIDToEntity under the owning
        // terrain entity UUID). Created/destroyed by Create/DestroyTerrainTileBody as the
        // TerrainStreamer loads / evicts tiles.
        struct TerrainTileKey
        {
            UUID Terrain;
            i32 X = 0;
            i32 Z = 0;

            bool operator==(const TerrainTileKey& other) const
            {
                return Terrain == other.Terrain && X == other.X && Z == other.Z;
            }
        };
        struct TerrainTileKeyHash
        {
            sizet operator()(const TerrainTileKey& k) const
            {
                sizet h = std::hash<u64>{}(static_cast<u64>(k.Terrain));
                h ^= std::hash<i32>{}(k.X) * 0x9E3779B97F4A7C15ULL + 0x9E3779B9ULL + (h << 6) + (h >> 2);
                h ^= std::hash<i32>{}(k.Z) * 0x9E3779B97F4A7C15ULL + 0x9E3779B9ULL + (h << 6) + (h >> 2);
                return h;
            }
        };
        std::unordered_map<TerrainTileKey, JPH::BodyID, TerrainTileKeyHash> m_TerrainTileBodies;

        // Cloth soft bodies (issue #460), keyed by the owning ClothComponent entity. Like
        // terrain bodies these are raw JPH bodies (no JoltBody wrapper), so they live
        // outside m_Bodies; they are still entered in m_BodyIDToEntity so queries resolve
        // them. The JPH::Ref keeps the SoftBodySharedSettings alive for the body's lifetime.
        // Created/destroyed by Create/DestroyClothBody.
        struct ClothRuntime
        {
            JPH::BodyID m_BodyID;
            JPH::Ref<JPH::SoftBodySharedSettings> m_Settings;
        };
        std::unordered_map<UUID, ClothRuntime> m_Cloths;

        // Joints (two-body constraints), keyed by the owning entity (the one
        // carrying the PhysicsJoint3DComponent). The JPH::Ref keeps the
        // constraint alive while it is registered with m_JoltSystem.
        std::unordered_map<UUID, JPH::Ref<JPH::Constraint>> m_Constraints;

        // Wheeled vehicles, keyed by the owning entity (the chassis carrying the
        // VehicleComponent). The JPH::Ref keeps the VehicleConstraint alive while
        // it is registered with m_JoltSystem; it owns its WheeledVehicleController
        // and VehicleCollisionTester. The constraint is ALSO registered as a step
        // listener (raw pointer, not ref-counted), so DestroyVehicle must
        // RemoveStepListener before releasing the ref.
        std::unordered_map<UUID, JPH::Ref<JPH::VehicleConstraint>> m_Vehicles;

        // Ragdolls (issue #308 item 5), keyed by the owning entity (the one
        // carrying the RagdollComponent). A ragdoll holds no Jolt object of its
        // own — it authors Rigidbody3D + collider + SwingTwist-joint components
        // onto the skeleton's bone entities, which the normal body/constraint
        // passes then realise. We only track which components we generated on
        // which bone entities so DestroyRagdoll can remove exactly those (and
        // their Jolt body/constraint, via the OnComponentRemoved hooks) on stop,
        // leaving any pre-authored bone bodies/joints untouched.
        struct RagdollRuntime
        {
            std::vector<UUID> m_GeneratedBodyEntities;     // got a generated Rigidbody3DComponent
            std::vector<UUID> m_GeneratedColliderEntities; // got a generated SphereCollider3DComponent
            std::vector<UUID> m_GeneratedJointEntities;    // got a generated PhysicsJoint3DComponent
        };
        std::unordered_map<UUID, RagdollRuntime> m_Ragdolls;

        // Collision filtering for joints whose m_CollideConnected == false (see
        // ApplyJointCollisionFilters). m_JointGroupFilter is the single shared
        // GroupFilterTable referenced by every filtered body's CollisionGroup;
        // holding a Ref here keeps it alive and makes its lifetime explicit.
        // m_JointCollisionBodies is the set of body-owning entities that were
        // assigned that group, so the next rebuild can reset them first.
        JPH::Ref<JPH::GroupFilterTable> m_JointGroupFilter;
        std::unordered_set<UUID> m_JointCollisionBodies;

        // Character controller management
        std::unordered_map<UUID, Ref<JoltCharacterController>> m_CharacterControllers;
        std::vector<Ref<JoltCharacterController>> m_CharacterControllersToUpdate;

        // Simulation settings
        f32 m_FixedTimeStep = 1.0f / 60.0f;
        f32 m_Accumulator = 0.0f;
        i32 m_CollisionSteps = 1;
        i32 m_IntegrationSubSteps = 1;

        // Throttling state for the JPH::EPhysicsUpdateError overflow log (see Step()):
        // logs immediately on the first occurrence / on an error-type transition, then
        // only every kPhysicsUpdateErrorLogInterval steps while the same error persists,
        // instead of spamming every fixed step (issue #523).
        JPH::EPhysicsUpdateError m_LastPhysicsUpdateError = JPH::EPhysicsUpdateError::None;
        u32 m_PhysicsUpdateErrorRepeatCount = 0;

        // System limits actually passed to JPH::PhysicsSystem::Init (from PhysicsSettings),
        // cached for the GetMax*() diagnostics accessors above.
        u32 m_AppliedMaxBodies = 0;
        u32 m_AppliedMaxBodyPairs = 0;
        u32 m_AppliedMaxContactConstraints = 0;

        // Constants
        // CollisionGroup group id reserved for joint "collide connected" filtering.
        // The engine uses CollisionGroup for nothing else, so every filtered body
        // shares this group id and a unique sub-group id (see ApplyJointCollisionFilters).
        static constexpr u32 s_JointCollisionGroupID = 0;
        static constexpr u32 s_NumBodyMutexes = 0;    // Autodetect
        static constexpr u32 s_MaxStepsPerFrame = 10; // Prevent "spiral of death" in fixed timestep
        // Baseline temp-allocator scratch size, tuned for the *old* hardcoded
        // s_MaxContactConstraints (10240) that used to be the only value Jolt ever saw.
        // Jolt's per-step scratch allocations (contact-constraint solving buffers etc.)
        // scale with the constraint capacity passed to PhysicsSystem::Init, and
        // JPH::TempAllocatorImpl is a fixed-size ring buffer — handing Jolt a bigger
        // maxContactConstraints without growing this proportionally silently corrupts
        // memory the moment a step's scratch usage exceeds the buffer (reproduced: raising
        // PhysicsSettings::m_MaxContactConstraints from 10240 to 32768 with this constant
        // still at 10 MB crashed JoltScene::Step with SEH 0xc0000005, even in a trivial
        // 2-body test). InitializeJolt() scales the actual allocator size against this
        // baseline — see the maxContactConstraints ratio there. Issue #523.
        static constexpr u32 kBaselineTempAllocatorSize = 10 * 1024 * 1024; // 10 MB
        static constexpr u32 kBaselineMaxContactConstraints = 10240;
        static constexpr u32 s_JobSystemMaxJobs = 2048;
        static constexpr u32 s_JobSystemMaxBarriers = 8;
        // How often (in fixed steps) to re-log a persisting physics update error.
        static constexpr u32 kPhysicsUpdateErrorLogInterval = 300; // ~5s at a 60Hz fixed step
    };

} // namespace OloEngine
