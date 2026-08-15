#include "OloEnginePCH.h"
#include "VisualScriptSystem.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Inventory/InventoryEvents.h"
#include "OloEngine/Gameplay/Progression/ProgressionEvents.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptEvents.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptGraph.h"

#include <algorithm>
#include <utility>

namespace OloEngine::VisualScript
{
    namespace
    {
        // Seed derived from nothing wall-clock: the RNG stream must reproduce
        // across a replay of the same scene (issue #452). Per-scene state, so two
        // scenes in one process do not share a sequence.
        constexpr u64 kRandomSeed = 0x853C49E6748FEA9Bull;
    } // namespace

    VisualScriptSystem::VisualScriptSystem(Scene* scene)
        : m_Scene(scene), m_RandomState(kRandomSeed)
    {
    }

    VisualScriptSystem::~VisualScriptSystem() = default;

    RuntimeContext VisualScriptSystem::MakeRuntimeContext(f32 deltaSeconds)
    {
        RuntimeContext runtime;
        runtime.m_Scene = m_Scene;
        runtime.m_DeltaTime = deltaSeconds;
        runtime.m_EventOutbox = &m_Outbox;
        runtime.m_RandomState = &m_RandomState;
        // The log sink is a plain vector; the deque ring is filled from it after
        // the pass so a single Print cannot make the ring reallocate mid-tick.
        runtime.m_LogSink = nullptr;
        return runtime;
    }

    Ref<VisualScriptPlan> VisualScriptSystem::GetOrCompilePlan(AssetHandle handle)
    {
        if (static_cast<u64>(handle) == 0 || m_FailedPlans.contains(handle))
        {
            return nullptr;
        }
        if (const auto it = m_Plans.find(handle); it != m_Plans.end())
        {
            return it->second;
        }

        Ref<VisualScriptAsset> asset = AssetManager::GetAsset<VisualScriptAsset>(handle);
        if (!asset)
        {
            OLO_CORE_WARN("[VisualScript] Graph asset {} could not be loaded", static_cast<u64>(handle));
            m_FailedPlans.insert(handle);
            return nullptr;
        }

        std::vector<CompileDiagnostic> errors;
        Ref<VisualScriptPlan> plan = VisualScriptPlan::Compile(*asset, errors);
        if (!plan)
        {
            // Remembered as failed so a broken graph logs once, not once per
            // entity per tick for the rest of the session.
            m_FailedPlans.insert(handle);
            OLO_CORE_ERROR("[VisualScript] Graph {} failed to compile ({} error(s)):", static_cast<u64>(handle), errors.size());
            for (const CompileDiagnostic& error : errors)
            {
                OLO_CORE_ERROR("[VisualScript]   {} node {}: {}", error.m_Graph, error.m_Node, error.m_Message);
            }
            return nullptr;
        }

        m_Plans.emplace(handle, plan);
        return plan;
    }

    void VisualScriptSystem::SubscribeToGameplayBus()
    {
        if (m_Scene == nullptr)
        {
            return;
        }
        GameplayEventBus& bus = m_Scene->GetGameplayEvents();

        // ONE handler per event type per SCENE, capturing `this`. Safe because
        // the bus is Clear()ed on Scene::OnRuntimeStop and this system is
        // destroyed in the same call — the bus never outlives the capture.
        // A per-instance subscription would be unsafe: GameplayEventBus has no
        // unsubscribe, so an entity destroyed mid-session would leave a dangling
        // handler behind.
        //
        // Each handler only QUEUES: Publish is synchronous and typically fires
        // from inside another system's entity iteration.
        bus.Subscribe<VisualScriptCustomEvent>([this](const VisualScriptCustomEvent& event)
                                               { QueueCustomEvent(event.m_Name, PinValue::MakeString(event.m_Payload), event.m_Target, event.m_Sender); });

        // The bridged set is curated, not exhaustive: each entry decides what the
        // graph-visible payload IS, which a generic forwarder cannot. Adding a
        // row here is the whole cost of exposing another engine event to graphs.
        bus.Subscribe<QuestStartedEvent>([this](const QuestStartedEvent& event)
                                         { QueueGameplayEvent("QuestStarted", PinValue::MakeString(event.QuestID), event.EntityID); });
        bus.Subscribe<QuestCompletedEvent>([this](const QuestCompletedEvent& event)
                                           { QueueGameplayEvent("QuestCompleted", PinValue::MakeString(event.QuestID), event.EntityID); });
        bus.Subscribe<ItemAddedEvent>([this](const ItemAddedEvent& event)
                                      { QueueGameplayEvent("ItemAdded", PinValue::MakeString(event.ItemDefinitionID), event.EntityID); });
        bus.Subscribe<ExperienceGainedEvent>([this](const ExperienceGainedEvent& event)
                                             { QueueGameplayEvent("ExperienceGained", PinValue::MakeInt(event.Amount), event.EntityID); });
        bus.Subscribe<LevelUpEvent>([this](const LevelUpEvent& event)
                                    { QueueGameplayEvent("LevelUp", PinValue::MakeInt(event.NewLevel), event.EntityID); });
    }

    void VisualScriptSystem::OnRuntimeStart()
    {
        OLO_PROFILE_FUNCTION();
        m_Running = true;
        m_RandomState = kRandomSeed;
        SubscribeToGameplayBus();
        NodeRegistry::EnsureStandardLibrary();
    }

    void VisualScriptSystem::OnRuntimeStop()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Running)
        {
            return;
        }

        RuntimeContext runtime = MakeRuntimeContext(0.0f);
        for (auto& [entity, instance] : m_Instances)
        {
            instance.EndPlay(runtime);
        }
        // Whatever OnEndPlay emitted has nowhere to go — the session is over and
        // dispatching now would run graph code against a scene being torn down.
        m_Outbox.clear();
        m_Inbox.clear();
        m_Instances.clear();
        m_InstanceSource.clear();
        m_Plans.clear();
        m_FailedPlans.clear();
        m_Running = false;
    }

    void VisualScriptSystem::SyncInstances()
    {
        if (m_Scene == nullptr)
        {
            return;
        }

        m_EntityScratch.clear();
        for (auto view = m_Scene->GetAllEntitiesWith<VisualScriptComponent>(); const auto e : view)
        {
            Entity entity{ e, m_Scene };
            const auto& component = view.get<VisualScriptComponent>(e);
            const UUID id = entity.GetUUID();

            // A disabled component drops its instance, so re-enabling restarts
            // the graph from OnBeginPlay. Keeping a suspended instance around
            // instead would mean a Delay parked before the disable fires the
            // moment it comes back, which is not what "disabled" reads as.
            if (!component.m_Enabled)
            {
                // Fires OnEndPlay before dropping the instance, so re-enabling
                // genuinely restarts from OnBeginPlay rather than resuming a
                // half-torn-down graph.
                EndAndErase(id);
                continue;
            }
            m_EntityScratch.push_back(id);

            // No graph assigned yet — the state a freshly added component is in,
            // and also how a test installs a pre-compiled plan directly
            // (InstallInstanceForTesting). Leave any existing instance alone.
            if (static_cast<u64>(component.m_Graph) == 0)
            {
                continue;
            }

            const auto existing = m_InstanceSource.find(id);
            if (existing != m_InstanceSource.end() && existing->second == component.m_Graph)
            {
                continue;
            }

            // Either brand new, or the author repointed the component at a
            // different graph — in both cases the old instance's state is
            // meaningless against the new plan.
            Ref<VisualScriptPlan> plan = GetOrCompilePlan(component.m_Graph);
            if (!plan)
            {
                m_Instances.erase(id);
                m_InstanceSource.erase(id);
                continue;
            }

            VisualScriptInstance instance(plan, id);
            instance.ApplyVariableOverrides(component.m_VariableOverrides);
            m_Instances.insert_or_assign(id, std::move(instance));
            m_InstanceSource.insert_or_assign(id, component.m_Graph);
        }

        // Drop instances whose entity or component is gone. Iterating the map and
        // erasing through the iterator (rather than collecting first) is safe:
        // nothing here re-enters the map.
        for (auto it = m_Instances.begin(); it != m_Instances.end();)
        {
            const bool stillPresent = std::ranges::find(m_EntityScratch, it->first) != m_EntityScratch.end();
            if (stillPresent)
            {
                ++it;
                continue;
            }
            RuntimeContext teardown = MakeRuntimeContext(0.0f);
            it->second.EndPlay(teardown);
            m_InstanceSource.erase(it->first);
            it = m_Instances.erase(it);
        }
    }

    void VisualScriptSystem::Update(f32 deltaSeconds)
    {
        OLO_PROFILE_FUNCTION();
        if (m_Scene == nullptr || !m_Running)
        {
            return;
        }

        // ── The graph spawn/destroy safe point ────────────────────────────────
        // Same two-drain shape as Scene::UpdateScripts, and load-bearing for the
        // same reasons: the leading drain applies anything queued since last
        // tick's trailing drain (a bus subscriber, a dialogue action), and the
        // trailing drain makes a graph's spawn visible to physics, transform
        // propagation and rendering in THIS tick. Both sit outside every
        // iteration below. This system must stay UNMARKED in the scheduler —
        // applying structural registry changes from a worker is exactly what the
        // queue exists to prevent.
        m_Scene->FlushPendingEntityCommands();

        SyncInstances();

        RuntimeContext runtime = MakeRuntimeContext(deltaSeconds);
        std::vector<std::string> logSink;
        runtime.m_LogSink = &logSink;

        // Snapshot the ids, then work them. A graph may add or remove a component
        // on any entity; walking a live EnTT view here would let that structural
        // change invalidate the iterator underneath us.
        //
        // BeginPlay for EVERY new instance runs BEFORE the inbox drain, and both
        // run before any OnUpdate. The order is load-bearing: DispatchEvent is a
        // no-op on an instance that has not begun play, so draining first would
        // silently swallow anything queued before an entity's first tick — a
        // GameplayEventBus event published during scene setup, or a Lua OnUpdate
        // that fired earlier in THIS tick (Scripts runs before this node).
        for (const UUID id : m_EntityScratch)
        {
            if (const auto it = m_Instances.find(id); it != m_Instances.end() && !it->second.HasBegunPlay())
            {
                it->second.BeginPlay(runtime);
            }
        }

        DrainInbox(runtime);

        for (const UUID id : m_EntityScratch)
        {
            if (const auto it = m_Instances.find(id); it != m_Instances.end())
            {
                it->second.Tick(runtime);
            }
        }

        // Events emitted this tick are delivered this tick, up to a bounded
        // number of rounds — two graphs pinging each other would otherwise spin
        // the frame. Leftovers stay queued for the next tick.
        for (u32 round = 0; round < kMaxEventRoundsPerTick && !m_Outbox.empty(); ++round)
        {
            DrainOutbox();
            DrainInbox(runtime);
        }

        for (std::string& line : logSink)
        {
            m_Log.push_back(std::move(line));
            if (m_Log.size() > kMaxLogEntries)
            {
                m_Log.pop_front();
            }
        }

        m_Scene->FlushPendingEntityCommands();
    }

    void VisualScriptSystem::DrainInbox(RuntimeContext& runtime)
    {
        if (m_Inbox.empty())
        {
            return;
        }
        // Swap out: dispatching an event can queue more, and those belong to the
        // next round rather than extending the vector we are walking.
        std::vector<QueuedEvent> batch;
        batch.swap(m_Inbox);

        for (const QueuedEvent& event : batch)
        {
            IncomingEvent incoming;
            incoming.m_Key = event.m_Key;
            incoming.m_Payload = event.m_Payload;
            incoming.m_OtherEntity = event.m_Other;

            if (static_cast<u64>(event.m_Target) != 0)
            {
                if (const auto it = m_Instances.find(event.m_Target); it != m_Instances.end())
                {
                    (void)it->second.DispatchEvent(incoming, runtime);
                }
                continue;
            }
            for (auto& [id, instance] : m_Instances)
            {
                (void)instance.DispatchEvent(incoming, runtime);
            }
        }
    }

    void VisualScriptSystem::DrainOutbox()
    {
        if (m_Outbox.empty())
        {
            return;
        }
        std::vector<EmittedEvent> batch;
        batch.swap(m_Outbox);

        for (EmittedEvent& event : batch)
        {
            if (event.m_PublishToBus && m_Scene != nullptr)
            {
                // Bus route ONLY — deliberately not also queued here. This system
                // subscribes to VisualScriptCustomEvent itself, so Publish comes
                // straight back into QueueCustomEvent; doing both would fire every
                // matching Event.CustomEvent node twice per publish. The bus round
                // trip carries the same name/payload/target, so graphs still
                // receive it, and non-graph subscribers see it exactly once.
                //
                // Publishing here is safe: DrainOutbox runs between iterations,
                // never inside one. A node calling Publish directly would not be.
                VisualScriptCustomEvent busEvent;
                busEvent.m_Name = event.m_Name;
                busEvent.m_Payload = event.m_Payload.AsString();
                busEvent.m_Target = event.m_Target;
                m_Scene->GetGameplayEvents().Publish(busEvent);
                continue;
            }

            QueuedEvent queued;
            queued.m_Key = VisualScriptPlan::MakeEventKey("Custom", event.m_Name);
            queued.m_Payload = event.m_Payload;
            queued.m_Target = event.m_Target;
            m_Inbox.push_back(std::move(queued));
        }
    }

    void VisualScriptSystem::QueueCustomEvent(std::string name, PinValue payload, UUID target, UUID sender)
    {
        if (name.empty())
        {
            return;
        }
        QueuedEvent event;
        event.m_Key = VisualScriptPlan::MakeEventKey("Custom", name);
        event.m_Payload = std::move(payload);
        event.m_Target = target;
        event.m_Other = sender;
        m_Inbox.push_back(std::move(event));
    }

    void VisualScriptSystem::QueueGameplayEvent(std::string name, PinValue payload, UUID subject)
    {
        if (name.empty())
        {
            return;
        }
        QueuedEvent event;
        event.m_Key = VisualScriptPlan::MakeEventKey("Bus", name);
        event.m_Payload = std::move(payload);
        // Bus events broadcast: any graph listening for the name should hear it,
        // not just the entity the event happens to name.
        event.m_Target = UUID(0);
        event.m_Other = subject;
        m_Inbox.push_back(std::move(event));
    }

    void VisualScriptSystem::QueueContact(UUID entity, UUID other, bool isTrigger)
    {
        QueuedEvent event;
        event.m_Key = VisualScriptPlan::MakeEventKey(
            "Engine", isTrigger ? NodeTypes::kOnTriggerEnter : NodeTypes::kOnCollisionEnter);
        event.m_Target = entity;
        event.m_Other = other;
        m_Inbox.push_back(std::move(event));
    }

    bool VisualScriptSystem::TryGetVariable(UUID entity, std::string_view name, PinValue& outValue) const
    {
        const auto it = m_Instances.find(entity);
        if (it == m_Instances.end())
        {
            return false;
        }
        bool found = false;
        PinValue value = it->second.GetVariable(name, &found);
        if (!found)
        {
            return false;
        }
        outValue = std::move(value);
        return true;
    }

    bool VisualScriptSystem::SetVariable(UUID entity, std::string_view name, const PinValue& value)
    {
        const auto it = m_Instances.find(entity);
        return it != m_Instances.end() && it->second.SetVariable(name, value);
    }

    void VisualScriptSystem::InstallInstanceForTesting(UUID entity, const Ref<VisualScriptPlan>& plan)
    {
        if (!plan)
        {
            return;
        }
        m_Instances.insert_or_assign(entity, VisualScriptInstance(plan, entity));
    }

    void VisualScriptSystem::EndAndErase(UUID entity)
    {
        const auto it = m_Instances.find(entity);
        if (it != m_Instances.end())
        {
            // Event.OnEndPlay's whole contract is "fires when the runtime stops OR
            // the component is removed". Erasing without firing it means a graph's
            // teardown branch silently never runs — the failure looks like the
            // teardown logic being wrong rather than never invoked.
            RuntimeContext runtime = MakeRuntimeContext(0.0f);
            it->second.EndPlay(runtime);
            m_Instances.erase(it);
        }
        m_InstanceSource.erase(entity);
    }

    void VisualScriptSystem::DestroyInstance(UUID entity)
    {
        EndAndErase(entity);
    }

    void VisualScriptSystem::NotifyGraphReloaded(AssetHandle handle)
    {
        if (static_cast<u64>(handle) == 0)
        {
            return;
        }
        // Drop the cached plan AND the failure memo — a reload is exactly the
        // moment a previously broken graph may have been fixed.
        m_Plans.erase(handle);
        m_FailedPlans.erase(handle);

        // Forget every instance built from it. SyncInstances rebuilds them on the
        // next tick, which also re-runs OnBeginPlay — the right behaviour for a
        // graph whose logic just changed.
        for (auto it = m_InstanceSource.begin(); it != m_InstanceSource.end();)
        {
            if (it->second == handle)
            {
                m_Instances.erase(it->first);
                it = m_InstanceSource.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    const VisualScriptInstance* VisualScriptSystem::FindInstance(UUID entity) const
    {
        const auto it = m_Instances.find(entity);
        return it == m_Instances.end() ? nullptr : &it->second;
    }

    VisualScriptInstance* VisualScriptSystem::FindInstanceForDebug(UUID entity)
    {
        const auto it = m_Instances.find(entity);
        return it == m_Instances.end() ? nullptr : &it->second;
    }

    std::vector<std::string> VisualScriptSystem::CollectErrors() const
    {
        std::vector<std::string> errors;
        for (const auto& [id, instance] : m_Instances)
        {
            for (const std::string& error : instance.GetErrors())
            {
                errors.push_back("entity " + std::to_string(static_cast<u64>(id)) + ": " + error);
            }
        }
        return errors;
    }

} // namespace OloEngine::VisualScript
