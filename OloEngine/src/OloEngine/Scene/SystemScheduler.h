#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timestep.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Scene;

    // Thrown when the declared system graph is invalid: a dependency cycle, a
    // before/after reference to an unregistered system, or a duplicate system
    // name. The gameplay system set is authored once and fixed at build time, so
    // any of these is a programmer error that must fail LOUDLY rather than run the
    // systems in a silently-wrong order (issue #453, plan step 6). It is an
    // exception (not just an assert) so it fires in every build config and is
    // directly testable with EXPECT_THROW.
    class SystemSchedulerError : public std::runtime_error
    {
      public:
        explicit SystemSchedulerError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    // Declarative scheduler for the per-tick gameplay systems that make up
    // Scene::SimulateRuntimeStep. Each system declares the named resources it
    // reads / writes plus optional explicit before/after ordering, and the
    // execution order is DERIVED by a deterministic topological sort rather than
    // being an implicit property of source-code call order.
    //
    // The read/write model mirrors the RenderGraph's resource-hazard derivation
    // (RGBuilder / RenderGraph::UpdateDependencyGraph): for each resource, in
    // registration order, a reader is ordered after the last writer (read-after-
    // write), a writer after the previous writer (write-after-write), and a writer
    // after every reader of the current version (write-after-read). Because those
    // edges always point forward in registration order, they can never by
    // themselves create a cycle — registration order is always a valid topological
    // order, so a fully-unconstrained set of systems runs in registration order.
    //
    // The derivation therefore reproduces the historical hard-coded sequence
    // bit-for-bit when systems are registered in that sequence (the tie-break is
    // the registration index), while a *real* data dependency is now encoded in
    // the graph — so a future reordering that violates one is caught by the sort
    // instead of silently corrupting a frame. Explicit After()/Before() edges
    // express the handful of ordering constraints that are documented but not a
    // clean single-resource data flow (e.g. "rebuild the spatial index after
    // navigation has moved agents").
    //
    // Only explicit After()/Before() edges can introduce a cycle; when they do,
    // Build() throws SystemSchedulerError naming the systems it could not order.
    class SystemScheduler
    {
      public:
        // Invoked with (scene, ts) when the system runs. Takes the Scene by
        // reference (captures nothing instance-specific) so one built scheduler is
        // safe to share across every Scene / tick.
        using ExecFn = std::function<void(Scene&, Timestep)>;

        // Fluent constraint declaration for a single registered system. Holds an
        // index (not a pointer) into the owner so it survives the owner's internal
        // vector growth between AddSystem() calls.
        class SystemBuilder
        {
          public:
            SystemBuilder& Reads(std::string_view resource);
            SystemBuilder& Writes(std::string_view resource);
            // Convenience for a system that both reads the current value and
            // overwrites it (a read-modify-write on one resource, e.g. physics on
            // the entity transforms).
            SystemBuilder& ReadsWrites(std::string_view resource);
            // This system runs AFTER the named system (edge other -> this).
            SystemBuilder& After(std::string_view systemName);
            // This system runs BEFORE the named system (edge this -> other).
            SystemBuilder& Before(std::string_view systemName);
            // Marks this system safe to execute on a worker thread. The executor
            // guarantees a parallel-marked system only ever overlaps OTHER
            // parallel-marked systems (every unmarked system is a join-all
            // barrier), so the audit obligation is exactly: this system's body
            // must be thread-safe against the other marked systems — no GL/GPU
            // calls, no EnTT structural changes, no unsynchronized shared
            // singletons, and (for #452 determinism) no draws from the seeded
            // game-thread RNG stream. Do NOT mark a system without that audit;
            // see the schedule in Scene::GetGameplayScheduler for worked
            // examples of both outcomes.
            SystemBuilder& Parallelizable();

          private:
            friend class SystemScheduler;
            SystemBuilder(SystemScheduler& owner, u32 index)
                : m_Owner(owner), m_Index(index)
            {
            }
            SystemScheduler& m_Owner;
            u32 m_Index;
        };

        // Register a system. Marks the derived order stale (a later Build() /
        // Execute() re-derives it). Returns a builder for chaining constraints.
        SystemBuilder AddSystem(std::string name, ExecFn exec);

        // Derive + cache the execution order. Throws SystemSchedulerError on a
        // cycle, a dangling before/after reference, or a duplicate system name.
        // Idempotent: a no-op once built until the next AddSystem().
        void Build();

        // Run every system exactly once per call. Builds first if needed.
        //
        // Sequential mode (no system marked Parallelizable, parallel execution
        // globally disabled): every system runs on the calling thread in derived
        // order — bit-for-bit the historical behavior.
        //
        // Parallel mode: the calling thread walks the same derived order;
        // Parallelizable systems are dispatched as engine tasks
        // (Tasks::Launch onto the FScheduler work-stealing pool), unmarked
        // systems first JOIN every in-flight task and then run inline. Declared
        // edges between two parallel systems become task-graph PREREQUISITES
        // (the successor is launched immediately but only starts once its
        // predecessors completed — the walk never blocks on a launch), and ALL
        // tasks are joined before Execute returns, so callers (and the render
        // pass after the tick) never observe a half-finished tick. With zero
        // workers running, Tasks::Launch degrades to inline execution on the
        // calling thread in the same order — headless hosts need no special
        // case. An exception thrown by a parallel system is captured and
        // rethrown on the calling thread after the join.
        void Execute(Scene& scene, Timestep ts);

        // Process-global kill-switch for the parallel path (default: enabled,
        // unless the environment sets OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1).
        // Sequential mode is the debugging/bisection lever: same systems, same
        // derived order, one thread. Thread-safe.
        static void SetParallelExecutionEnabled(bool enabled);
        static bool IsParallelExecutionEnabled();

        // Derived order as system names, for tests / diagnostics. Builds if needed.
        const std::vector<std::string>& GetOrderedNames();

        // True when `system` transitively depends on `ancestor` in the derived
        // graph — i.e. an edge path ancestor -> … -> system exists, so `system`
        // can never start before `ancestor` has finished, under BOTH the
        // sequential and the parallel executor. Tests use this to pin the
        // documented cross-subsystem seams: a position check on the sequential
        // order can be satisfied by the registration-order tie-break even when
        // the edge is missing, which is precisely the silent gap that becomes a
        // race once systems run concurrently. Throws SystemSchedulerError for an
        // unknown name (a typo must not pass as "no dependency"). Builds if needed.
        bool DependsOn(std::string_view system, std::string_view ancestor);

        sizet SystemCount() const
        {
            return m_Systems.size();
        }

      private:
        struct SystemNode
        {
            std::string Name;
            std::vector<std::string> Reads;
            std::vector<std::string> Writes;
            std::vector<std::string> After;
            std::vector<std::string> Before;
            ExecFn Exec;
            bool Parallel = false;
        };

        // Topologically sort m_Systems into m_Order (indices) + m_OrderedNames,
        // persisting the derived adjacency for DependsOn / the executor.
        void DeriveOrder();

        std::vector<SystemNode> m_Systems;
        std::vector<u32> m_Order;                // indices into m_Systems, in exec order
        std::vector<std::string> m_OrderedNames; // cache of the names in exec order
        // Derived DAG, filled by DeriveOrder (indices into m_Systems):
        std::vector<std::vector<u32>> m_Successors;   // edge from -> to
        std::vector<std::vector<u32>> m_Predecessors; // inverse of m_Successors
        std::unordered_map<std::string, u32> m_NameToIndex;
        bool m_AnyParallel = false; // cached: any system marked Parallelizable
        bool m_Built = false;
    };
} // namespace OloEngine
