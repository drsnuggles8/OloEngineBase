#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptVM.h"

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    class Scene;
} // namespace OloEngine

namespace OloEngine::VisualScript
{
    //==============================================================================
    /// Per-scene owner of every live graph instance (issue #634).
    ///
    /// Lives on Scene for the duration of a runtime session, alongside
    /// DialogueSystem. It is runtime-only: never serialized, never copied with the
    /// scene, and torn down at OnRuntimeStop so a stop/play cycle cannot carry a
    /// stale instance (or a stale GameplayEventBus subscription) into the next.
    ///
    /// Three deliberate design points, each of which is a hazard if reversed:
    ///
    /// 1. **Iteration is over a snapshot of entity UUIDs, not a live EnTT view.**
    ///    A graph may add or remove a component on any entity, which is a
    ///    structural change to that component's pool. Snapshotting means such a
    ///    change can never invalidate the iteration we are inside.
    /// 2. **Entity create/destroy still goes through Scene's deferred command
    ///    queue**, and Update() brackets itself with FlushPendingEntityCommands()
    ///    exactly as Scene::UpdateScripts does — so a graph spawn is visible to
    ///    physics, transform propagation and rendering in the SAME tick.
    ///    See docs/agent-rules/script-structural-command-safe-point.md.
    /// 3. **Bus subscriptions are per SCENE, not per entity.** GameplayEventBus
    ///    has no unsubscribe: a handler registered per graph instance would
    ///    dangle the moment that entity died. One handler per scene, fanning out
    ///    to whichever instances are alive at delivery time, has no such window.
    class VisualScriptSystem
    {
      public:
        explicit VisualScriptSystem(Scene* scene);
        ~VisualScriptSystem();

        VisualScriptSystem(const VisualScriptSystem&) = delete;
        VisualScriptSystem& operator=(const VisualScriptSystem&) = delete;
        VisualScriptSystem(VisualScriptSystem&&) = delete;
        VisualScriptSystem& operator=(VisualScriptSystem&&) = delete;

        //-- Scene lifecycle -------------------------------------------------------
        void OnRuntimeStart();
        void OnRuntimeStop();

        /// The gameplay-scheduler node body. Unmarked (join-all barrier): it
        /// performs structural registry changes through the deferred queue and
        /// publishes to the synchronous GameplayEventBus.
        void Update(f32 deltaSeconds);

        //-- Triggers in -----------------------------------------------------------
        /// Queues a named custom event. `target` of 0 broadcasts to every graph.
        /// Never dispatches inline — callers are typically mid-iteration.
        void QueueCustomEvent(std::string name, PinValue payload, UUID target, UUID sender);
        /// Queues a physics contact. `isTrigger` picks OnTriggerEnter over
        /// OnCollisionEnter.
        void QueueContact(UUID entity, UUID other, bool isTrigger);
        /// Queues a GameplayEventBus bridge event (Event.OnGameplayEvent).
        void QueueGameplayEvent(std::string name, PinValue payload, UUID subject);

        //-- Script-facing blackboard access --------------------------------------
        [[nodiscard]] bool TryGetVariable(UUID entity, std::string_view name, PinValue& outValue) const;
        bool SetVariable(UUID entity, std::string_view name, const PinValue& value);

        //-- Component lifecycle ---------------------------------------------------
        /// Drops an entity's instance. Called from Scene::OnComponentRemoved so the
        /// VM state dies with the component rather than lingering until stop.
        void DestroyInstance(UUID entity);
        /// Recompiles every instance built from `handle` — the hot-reload path.
        void NotifyGraphReloaded(AssetHandle handle);

        /// Installs a pre-compiled plan directly, bypassing the AssetManager.
        /// Test-only seam: the Functional harness mounts no project, so an
        /// AssetHandle cannot resolve, yet what those tests exercise is the
        /// system's tick/dispatch behaviour rather than asset loading. The
        /// entity's component keeps its null graph handle, which is exactly the
        /// case SyncInstances leaves alone.
        void InstallInstanceForTesting(UUID entity, const Ref<VisualScriptPlan>& plan);

        //-- Diagnostics (tests + the future editor debugger) ----------------------
        [[nodiscard]] sizet GetInstanceCount() const
        {
            return m_Instances.size();
        }
        [[nodiscard]] const VisualScriptInstance* FindInstance(UUID entity) const;
        /// Mutable access, for the editor debugger only — it needs to arm
        /// breakpoints, turn tracing on and resume a paused graph. Everything
        /// else reads through the const overload.
        [[nodiscard]] VisualScriptInstance* FindInstanceForDebug(UUID entity);
        [[nodiscard]] const std::deque<std::string>& GetLog() const
        {
            return m_Log;
        }
        void ClearLog()
        {
            m_Log.clear();
        }
        /// Errors reported by every live instance since the last clear.
        [[nodiscard]] std::vector<std::string> CollectErrors() const;

      private:
        struct QueuedEvent
        {
            std::string m_Key;
            PinValue m_Payload{};
            UUID m_Target{ 0 };
            UUID m_Other{ 0 };
        };

        [[nodiscard]] Ref<VisualScriptPlan> GetOrCompilePlan(AssetHandle handle);
        void SyncInstances();
        void DrainInbox(RuntimeContext& runtime);
        void DrainOutbox();
        void SubscribeToGameplayBus();
        /// Fires Event.OnEndPlay on an instance and then drops it. Every removal
        /// path goes through here so teardown can never be skipped.
        void EndAndErase(UUID entity);
        [[nodiscard]] RuntimeContext MakeRuntimeContext(f32 deltaSeconds);

        Scene* m_Scene = nullptr;
        std::unordered_map<UUID, VisualScriptInstance> m_Instances;
        /// Which asset each instance was built from, so a hot-reload knows which
        /// instances to rebuild without re-reading every component.
        std::unordered_map<UUID, AssetHandle> m_InstanceSource;
        std::unordered_map<AssetHandle, Ref<VisualScriptPlan>> m_Plans;
        /// Graphs whose compile failed. Kept so a broken asset produces ONE error
        /// rather than one per tick per entity for the rest of the session.
        std::unordered_set<AssetHandle> m_FailedPlans;

        std::vector<QueuedEvent> m_Inbox;
        std::vector<EmittedEvent> m_Outbox;
        /// Bounded log ring — Print in an OnUpdate would otherwise grow without
        /// limit over a long session.
        std::deque<std::string> m_Log;
        /// Scratch reused across ticks so the per-tick entity sweep does not
        /// allocate; cleared, not shrunk.
        std::vector<UUID> m_EntityScratch;
        u64 m_RandomState = 0;
        bool m_Running = false;

        static constexpr sizet kMaxLogEntries = 512;
        /// How many times per tick an emitted event may itself emit further
        /// events before the rest are deferred to the next tick. Without a cap,
        /// two graphs pinging each other would spin the frame.
        static constexpr u32 kMaxEventRoundsPerTick = 4;
    };

} // namespace OloEngine::VisualScript
