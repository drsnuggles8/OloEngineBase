#include "OloEnginePCH.h"
#include "TestTempDir.h"

// OLO_TEST_LAYER: Functional
//
// =============================================================================
// LuaSpawnsAndDestroysEntitiesTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Lua scripting × Scene's deferred entity-command queue × the gameplay
//   system scheduler × Prefab instantiation × the Lua OnCreate lifecycle
//   (issue #643).
//
// Runtime spawning is the one script capability that mutates the EnTT registry
// STRUCTURALLY, and it is requested from inside `Scene::UpdateScripts`, which
// is itself iterating the LuaScriptComponent pool. Applying a spawn or destroy
// inline there invalidates the live iterator — a corruption that does not
// surface at the call site but much later, in an unrelated system or an
// unrelated test. So every script spawn/destroy is queued and drained at a safe
// point, and this test pins that contract end to end through a real
// `Scene::OnUpdateRuntime`:
//
//   * a script spawn is DEFERRED (invisible during the OnUpdate that asked for
//     it) but LANDS IN THE SAME TICK (visible once the tick returns),
//   * the pre-allocated UUID handed back is the entity's real, final ID,
//   * a spawned prefab's whole hierarchy arrives, at the requested transform,
//   * a spawned entity carrying a script gets OnCreate fired — the
//     OnRuntimeStart sweep only covers entities that existed at start,
//   * a script may destroy the entity it is running on without a
//     use-after-free, and destroying a prefab root takes its children,
//   * a double destroy is a no-op, not a double free.
//
// The sustained-churn body runs TWICE — once under the parallel executor, once
// with SystemScheduler::SetParallelExecutionEnabled(false), the mode
// OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1 selects — because the sequential path's
// registration-order tie-break can mask a hazard that only appears when the
// marked systems actually overlap.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/SystemScheduler.h"
#include "OloEngine/Asset/AssetManager.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* stem)
    {
        const auto path = OloEngine::Tests::TempFile(std::string("olo_spawn_") + stem + ".lua");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        return path;
    }

    // Render a u64 handle as a Lua source literal.
    //
    // Lua 5.4's integers are SIGNED 64-bit, and its lexer parses a decimal
    // literal above LUA_MAXINTEGER as a FLOAT — which then fails sol2's
    // "fits exactly an integer" check on the way into a u64 parameter. Half of
    // all randomly generated AssetHandles/UUIDs land above that bound, so a
    // handle written out as an unsigned decimal makes this test fail one run in
    // two. Emitting the same bit pattern as a signed decimal always lexes as an
    // integer, and sol2 bit-casts it straight back to u64 on the way in.
    //
    // Real scripts don't hit this: they get handles from a binding return value
    // or a component field, both of which cross as lua_Integer already. It is
    // specifically hard-coding a large id in Lua source that breaks.
    [[nodiscard]] std::string LuaIdLiteral(u64 value)
    {
        return std::to_string(static_cast<i64>(value));
    }

    // RAII flip of the process-wide parallel-executor toggle, so a failing
    // assertion can't leave sequential mode latched for every later test.
    class ScopedSchedulerMode
    {
      public:
        explicit ScopedSchedulerMode(bool parallel)
            : m_Previous(SystemScheduler::IsParallelExecutionEnabled())
        {
            SystemScheduler::SetParallelExecutionEnabled(parallel);
        }
        ScopedSchedulerMode(const ScopedSchedulerMode&) = delete;
        ScopedSchedulerMode& operator=(const ScopedSchedulerMode&) = delete;
        ScopedSchedulerMode(ScopedSchedulerMode&&) = delete;
        ScopedSchedulerMode& operator=(ScopedSchedulerMode&&) = delete;
        ~ScopedSchedulerMode()
        {
            SystemScheduler::SetParallelExecutionEnabled(m_Previous);
        }

      private:
        bool m_Previous;
    };

    // Count live entities whose tag equals `tag`, by walking the registry
    // rather than trusting the name cache — that is what makes this a
    // registry-integrity assertion. A spawn that half-registered, or a destroy
    // that left the entity behind, shows up here.
    [[nodiscard]] sizet CountEntitiesNamed(Scene& scene, const std::string& tag)
    {
        sizet count = 0;
        for (auto view = scene.GetAllEntitiesWith<TagComponent>(); auto e : view)
        {
            if (view.get<TagComponent>(e).Tag == tag)
                ++count;
        }
        return count;
    }

    [[nodiscard]] sizet CountLiveEntities(Scene& scene)
    {
        sizet count = 0;
        for (auto view = scene.GetAllEntitiesWith<IDComponent>(); auto e : view)
        {
            (void)e;
            ++count;
        }
        return count;
    }
} // namespace

class LuaSpawnsAndDestroysEntitiesTest : public FunctionalTest
{
  protected:
    static constexpr u64 kSpawnerUUID = 0xA11CE;

    void BuildScene() override
    {
        // A prefab has to resolve through AssetManager, so mount the throwaway
        // project first. No staged files needed — the prefab is memory-only.
        EnableAssetManager({});
        EnableLua();

        BuildProjectilePrefab();

        m_Spawner = GetScene().CreateEntityWithUUID(UUID{ kSpawnerUUID }, "Spawner");
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        for (const auto& path : m_ScriptPaths)
            std::filesystem::remove(path, ec);
    }

    // Author a two-level prefab: root "Projectile" with one child
    // "ProjectileTrail". The child is what proves hierarchy instantiation and
    // hierarchical destroy.
    void BuildProjectilePrefab()
    {
        // The source entities live in the test scene only long enough to be
        // copied into the prefab's own scene; they are removed afterwards so
        // they can't be mistaken for spawned instances by the counters.
        Entity source = GetScene().CreateEntity("Projectile");
        source.GetComponent<TransformComponent>().Translation = { 7.0f, 8.0f, 9.0f };
        Entity child = GetScene().CreateEntity("ProjectileTrail");
        child.SetParent(source);

        m_Prefab = Ref<Prefab>::Create();
        // Prefab::Create asserts on a zero handle, so register before authoring.
        m_PrefabHandle = AssetManager::AddMemoryOnlyAsset<Prefab>(m_Prefab);
        ASSERT_NE(static_cast<u64>(m_PrefabHandle), 0ULL)
            << "AddMemoryOnlyAsset returned a zero handle — the test asset manager is not mounted.";
        m_Prefab->Create(source, /*serialize=*/false);

        GetScene().DestroyEntityAndChildren(source);
        ASSERT_EQ(CountEntitiesNamed(GetScene(), "Projectile"), 0u)
            << "prefab source entity survived teardown — later instance counts would be off by one.";
        ASSERT_EQ(CountEntitiesNamed(GetScene(), "ProjectileTrail"), 0u)
            << "prefab source child survived teardown — DestroyEntityAndChildren is not recursing.";
    }

    void AttachSpawnerScript(const std::string& src, const char* stem)
    {
        const auto path = WriteScript(src, stem);
        m_ScriptPaths.push_back(path);
        RegisterLuaScript(m_Spawner, path);
    }

    Entity m_Spawner;
    Ref<Prefab> m_Prefab;
    AssetHandle m_PrefabHandle{};
    std::vector<std::filesystem::path> m_ScriptPaths;
};

// -----------------------------------------------------------------------------
// The core contract: deferred within the OnUpdate, applied within the tick.
// -----------------------------------------------------------------------------
TEST_F(LuaSpawnsAndDestroysEntitiesTest, SpawnIsDeferredWithinOnUpdateButLandsInTheSameTick)
{
    // The script spawns once and immediately records what it can see about the
    // new entity. `is_valid` must already be true (the handle is logically
    // live) while `get_name` must be nil (nothing exists in the registry yet).
    // It reports both by stamping the spawner's own transform, which C++ reads.
    AttachSpawnerScript(R"(
local script = {}
script.done = false

function script.OnUpdate(entityID, ts)
    if script.done then return end
    script.done = true

    local id = Scene.CreateEntity("Bullet", vec3.new(1.0, 2.0, 3.0))

    local sawValid = entity_utils.is_valid(id)
    local sawNoEntityYet = (entity_utils.get_name(id) == nil)

    entity_utils.set_translation(entityID, vec3.new(
        sawValid and 1.0 or 0.0,
        sawNoEntityYet and 1.0 or 0.0,
        0.0))
end

return script
)",
                        "deferred");

    ASSERT_EQ(CountEntitiesNamed(GetScene(), "Bullet"), 0u);

    RunFrames(1);

    const auto& probe = m_Spawner.GetComponent<TransformComponent>().Translation;
    EXPECT_FLOAT_EQ(probe.x, 1.0f)
        << "entity_utils.is_valid said false for a handle the script had just been given — a script "
           "cannot tell its own fresh spawn from a dead entity.";
    EXPECT_FLOAT_EQ(probe.y, 1.0f)
        << "the spawned entity was already resolvable inside the OnUpdate that requested it — the "
           "spawn is being applied INLINE, in the middle of the script-pool iteration.";

    EXPECT_EQ(CountEntitiesNamed(GetScene(), "Bullet"), 1u)
        << "the spawn did not land within the tick that requested it — the drain at the end of "
           "Scene::UpdateScripts is not running, or is running before the script callbacks.";

    // Registry integrity: findable by name AND resolvable through the UUID map.
    Entity bullet = GetScene().FindEntityByName("Bullet");
    ASSERT_TRUE(static_cast<bool>(bullet)) << "spawned entity is not findable by name.";
    EXPECT_TRUE(GetScene().TryGetEntityWithUUID(bullet.GetUUID()).has_value())
        << "spawned entity is missing from the scene's UUID map — CreateEntityWithUUID was bypassed.";

    const auto& translation = bullet.GetComponent<TransformComponent>().Translation;
    EXPECT_FLOAT_EQ(translation.x, 1.0f);
    EXPECT_FLOAT_EQ(translation.y, 2.0f);
    EXPECT_FLOAT_EQ(translation.z, 3.0f)
        << "the spawn translation passed to Scene.CreateEntity was not applied.";

    // Nothing further should spawn on later ticks (the script self-gates).
    RunFrames(5);
    EXPECT_EQ(CountEntitiesNamed(GetScene(), "Bullet"), 1u)
        << "a queued command was applied more than once.";
    EXPECT_EQ(GetScene().GetPendingEntityCommandCount(), 0u)
        << "the command queue is not empty after the drain.";
}

// -----------------------------------------------------------------------------
// The UUID handed back at request time must be the entity's real one.
// -----------------------------------------------------------------------------
TEST_F(LuaSpawnsAndDestroysEntitiesTest, ReturnedUUIDResolvesToTheSpawnedEntityNextTick)
{
    AttachSpawnerScript(R"(
local script = {}
script.spawned_id = 0

function script.OnUpdate(entityID, ts)
    if script.spawned_id == 0 then
        script.spawned_id = Scene.CreateEntity("Turret", vec3.new(0, 0, 0))
        return
    end
    -- Next tick: the pre-allocated handle must resolve to the real entity, and
    -- be usable for real work rather than just lookups.
    if entity_utils.get_name(script.spawned_id) ~= nil then
        entity_utils.set_translation(script.spawned_id, vec3.new(42.0, 0, 0))
    end
end

return script
)",
                        "uuid");

    RunFrames(3);

    Entity turret = GetScene().FindEntityByName("Turret");
    ASSERT_TRUE(static_cast<bool>(turret));
    EXPECT_FLOAT_EQ(turret.GetComponent<TransformComponent>().Translation.x, 42.0f)
        << "the UUID returned by Scene.CreateEntity did not resolve to the spawned entity on a later "
           "tick — the pre-allocated handle is not the one CreateEntityWithUUID used.";
}

// -----------------------------------------------------------------------------
// Prefab instantiation: hierarchy + transform override.
// -----------------------------------------------------------------------------
TEST_F(LuaSpawnsAndDestroysEntitiesTest, PrefabSpawnBringsItsChildrenAtTheRequestedTransform)
{
    AttachSpawnerScript(std::string(R"(
local script = {}
script.done = false

function script.OnUpdate(entityID, ts)
    if script.done then return end
    script.done = true
    Scene.Instantiate()") +
                            LuaIdLiteral(m_PrefabHandle) +
                            R"(, vec3.new(10.0, 20.0, 30.0), vec3.new(0, 0, 0), vec3.new(2.0, 2.0, 2.0))
end

return script
)",
                        "prefab");

    RunFrames(1);

    ASSERT_EQ(CountEntitiesNamed(GetScene(), "Projectile"), 1u)
        << "the prefab root did not spawn.";
    EXPECT_EQ(CountEntitiesNamed(GetScene(), "ProjectileTrail"), 1u)
        << "the prefab's child did not come with it — Prefab::Instantiate's recursive child pass is "
           "not reached from the deferred command.";

    Entity root = GetScene().FindEntityByName("Projectile");
    ASSERT_TRUE(static_cast<bool>(root));

    const auto& transform = root.GetComponent<TransformComponent>();
    EXPECT_FLOAT_EQ(transform.Translation.x, 10.0f);
    EXPECT_FLOAT_EQ(transform.Translation.y, 20.0f);
    EXPECT_FLOAT_EQ(transform.Translation.z, 30.0f)
        << "the requested spawn transform did not override the prefab's authored (7,8,9).";
    EXPECT_FLOAT_EQ(transform.Scale.x, 2.0f)
        << "the requested spawn scale was not applied.";

    ASSERT_EQ(root.Children().size(), 1u);
    EXPECT_TRUE(root.HasComponent<PrefabComponent>())
        << "the spawned root is not marked as a prefab instance.";
}

// -----------------------------------------------------------------------------
// Lifecycle: a runtime-spawned scripted entity must receive OnCreate.
// -----------------------------------------------------------------------------
TEST_F(LuaSpawnsAndDestroysEntitiesTest, SpawnedPrefabWithScriptGetsOnCreateFired)
{
    // A second prefab whose root carries a LuaScriptComponent. Its script
    // stamps a sentinel on the spawner from OnCreate — the only way that value
    // can appear is if the drain fires the lifecycle for a runtime spawn.
    const auto spawnedScript = WriteScript(std::string(R"(
local script = {}
function script.OnCreate(entityID)
    entity_utils.set_translation()") +
                                               std::to_string(kSpawnerUUID) + R"(, vec3.new(777.0, 0, 0))
end
return script
)",
                                           "prefab_oncreate");
    m_ScriptPaths.push_back(spawnedScript);

    Entity source = GetScene().CreateEntity("ScriptedProjectile");
    source.AddComponent<LuaScriptComponent>().ScriptFile = spawnedScript.string();

    Ref<Prefab> scriptedPrefab = Ref<Prefab>::Create();
    const AssetHandle scriptedHandle = AssetManager::AddMemoryOnlyAsset<Prefab>(scriptedPrefab);
    ASSERT_NE(static_cast<u64>(scriptedHandle), 0ULL);
    scriptedPrefab->Create(source, /*serialize=*/false);
    GetScene().DestroyEntityAndChildren(source);

    AttachSpawnerScript(std::string(R"(
local script = {}
script.done = false
function script.OnUpdate(entityID, ts)
    if script.done then return end
    script.done = true
    Scene.Instantiate()") +
                            LuaIdLiteral(scriptedHandle) + R"()
end
return script
)",
                        "prefab_lifecycle");

    ASSERT_FLOAT_EQ(m_Spawner.GetComponent<TransformComponent>().Translation.x, 0.0f);

    RunFrames(1);

    ASSERT_EQ(CountEntitiesNamed(GetScene(), "ScriptedProjectile"), 1u)
        << "the scripted prefab did not spawn.";
    EXPECT_FLOAT_EQ(m_Spawner.GetComponent<TransformComponent>().Translation.x, 777.0f)
        << "OnCreate did not fire for the runtime-spawned scripted entity — the OnRuntimeStart sweep "
           "only covers entities that existed at start, so the drain must fire it.";
}

// -----------------------------------------------------------------------------
// Destroy: self-destroy, hierarchy, idempotence.
// -----------------------------------------------------------------------------
TEST_F(LuaSpawnsAndDestroysEntitiesTest, ScriptCanDestroyItsOwnEntityWithoutUseAfterFree)
{
    // A script destroying the entity it is running on is the classic
    // use-after-free in an inline implementation: Scene::UpdateScripts is
    // mid-iteration over the LuaScriptComponent pool this entity lives in, and
    // LuaScriptEngine::OnUpdateEntity still touches the entity after the
    // callback returns. Deferral is what makes it safe.
    AttachSpawnerScript(R"(
local script = {}
script.ticks = 0
function script.OnUpdate(entityID, ts)
    script.ticks = script.ticks + 1
    if script.ticks == 1 then
        entity_utils.destroy(entityID)
        -- Logically gone from this instant, even though the registry still
        -- holds it until the drain.
        if entity_utils.is_valid(entityID) then
            error("entity reported valid after its own destroy was requested")
        end
    end
end
return script
)",
                        "self_destroy");

    ASSERT_TRUE(GetScene().TryGetEntityWithUUID(UUID{ kSpawnerUUID }).has_value());

    RunFrames(1);

    EXPECT_FALSE(GetScene().TryGetEntityWithUUID(UUID{ kSpawnerUUID }).has_value())
        << "the self-destroy did not take effect within the tick.";
    EXPECT_EQ(CountEntitiesNamed(GetScene(), "Spawner"), 0u);

    // Keep ticking: the destroyed entity must not be revisited and the command
    // must not replay.
    RunFrames(5);
    EXPECT_EQ(GetScene().GetPendingEntityCommandCount(), 0u);
}

TEST_F(LuaSpawnsAndDestroysEntitiesTest, DestroyingAPrefabRootTakesItsChildren)
{
    AttachSpawnerScript(std::string(R"(
local script = {}
script.ticks = 0
script.root_id = 0
function script.OnUpdate(entityID, ts)
    script.ticks = script.ticks + 1
    if script.ticks == 1 then
        script.root_id = Scene.Instantiate()") +
                            LuaIdLiteral(m_PrefabHandle) + R"(, vec3.new(0, 0, 0))
    elseif script.ticks == 2 then
        Scene.DestroyEntity(script.root_id)
        -- Idempotent: a second request must be a no-op, not a double free.
        Scene.DestroyEntity(script.root_id)
    end
end
return script
)",
                        "hierarchy_destroy");

    RunFrames(1);
    ASSERT_EQ(CountEntitiesNamed(GetScene(), "Projectile"), 1u);
    ASSERT_EQ(CountEntitiesNamed(GetScene(), "ProjectileTrail"), 1u);

    RunFrames(1);
    EXPECT_EQ(CountEntitiesNamed(GetScene(), "Projectile"), 0u)
        << "the prefab root survived the destroy request.";
    EXPECT_EQ(CountEntitiesNamed(GetScene(), "ProjectileTrail"), 0u)
        << "the prefab's child was orphaned rather than destroyed — a script-driven spawn/despawn "
           "loop would leak one entity per cycle.";

    RunFrames(5);
    EXPECT_EQ(GetScene().GetPendingEntityCommandCount(), 0u);
}

// -----------------------------------------------------------------------------
// Sustained spawn/destroy churn across many ticks, under BOTH executors.
// -----------------------------------------------------------------------------
class LuaSpawnChurnTest : public LuaSpawnsAndDestroysEntitiesTest
{
  protected:
    // Spawn one projectile per tick and destroy the previous one, for many
    // ticks. Steady state is exactly one live projectile hierarchy plus the
    // spawner; anything else means a command was dropped, replayed, or applied
    // out of order.
    void RunChurn(u32 ticks)
    {
        AttachSpawnerScript(std::string(R"(
local script = {}
script.prev = 0
function script.OnUpdate(entityID, ts)
    if script.prev ~= 0 then
        Scene.DestroyEntity(script.prev)
    end
    script.prev = Scene.Instantiate()") +
                                LuaIdLiteral(m_PrefabHandle) + R"(, vec3.new(0, 0, 0))
end
return script
)",
                            "churn");

        RunFrames(ticks);

        EXPECT_EQ(CountEntitiesNamed(GetScene(), "Projectile"), 1u)
            << "expected exactly one live projectile after " << ticks
            << " spawn/destroy ticks — the command queue dropped, replayed or reordered work.";
        EXPECT_EQ(CountEntitiesNamed(GetScene(), "ProjectileTrail"), 1u)
            << "child count drifted from the root count — hierarchy destroy is leaking.";
        EXPECT_EQ(GetScene().GetPendingEntityCommandCount(), 0u);

        // Spawner + one root + one child, and nothing else: proves no orphans
        // accumulated across the churn.
        EXPECT_EQ(CountLiveEntities(GetScene()), 3u)
            << "the scene accumulated entities across churn ticks — destroyed hierarchies are "
               "leaving members behind.";
    }
};

TEST_F(LuaSpawnChurnTest, SustainedChurnKeepsTheRegistryConsistentUnderTheParallelExecutor)
{
    ScopedSchedulerMode mode{ true };
    ASSERT_TRUE(SystemScheduler::IsParallelExecutionEnabled())
        << "could not enable the parallel executor — this test would silently duplicate the "
           "sequential one.";
    RunChurn(30);
}

TEST_F(LuaSpawnChurnTest, SustainedChurnKeepsTheRegistryConsistentUnderTheSequentialExecutor)
{
    // The mode OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1 selects. Running the same
    // body both ways is the point: the sequential registration-order tie-break
    // can hide an ordering hazard that only bites when systems overlap.
    ScopedSchedulerMode mode{ false };
    ASSERT_FALSE(SystemScheduler::IsParallelExecutionEnabled());
    RunChurn(30);
}
