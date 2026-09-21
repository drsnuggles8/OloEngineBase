#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"
#include "SystemScheduler.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Task/Task.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace OloEngine
{
    namespace
    {
        bool Contains(const TArray<FString>& names, std::string_view value)
        {
            return names.ContainsByPredicate([value](const FString& name)
                                             { return name.ToView() == value; });
        }

        // Default for the parallel path: enabled, unless the environment opts the
        // process out (the debugging/bisection lever — same systems, same derived
        // order, one thread). Mirrors the scheduler's other env knobs
        // (OLO_TASK_GRAPH_*); only the exact value "1" disables.
        bool ParallelEnabledDefault()
        {
            return !Levers::GameplaySchedulerSequential();
        }

        std::atomic<bool> s_ParallelExecutionEnabled{ ParallelEnabledDefault() };
    } // namespace

    void SystemScheduler::SetParallelExecutionEnabled(bool enabled)
    {
        s_ParallelExecutionEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool SystemScheduler::IsParallelExecutionEnabled()
    {
        return s_ParallelExecutionEnabled.load(std::memory_order_relaxed);
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::Reads(std::string_view resource)
    {
        m_Owner.m_Systems[m_Index].Reads.Emplace(resource);
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::Writes(std::string_view resource)
    {
        m_Owner.m_Systems[m_Index].Writes.Emplace(resource);
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::ReadsWrites(std::string_view resource)
    {
        Reads(resource);
        Writes(resource);
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::After(std::string_view systemName)
    {
        m_Owner.m_Systems[m_Index].After.Emplace(systemName);
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::Before(std::string_view systemName)
    {
        m_Owner.m_Systems[m_Index].Before.Emplace(systemName);
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::Parallelizable()
    {
        m_Owner.m_Systems[m_Index].Parallel = true;
        // The flag doesn't change the derived DAG, but m_AnyParallel is cached
        // during DeriveOrder — keep the cache coherent.
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder SystemScheduler::AddSystem(std::string name, ExecFn exec)
    {
        const u32 index = static_cast<u32>(m_Systems.Num());
        // TFunction stores this non-relocatable std::function on the heap, so
        // moving the node bytes never moves the callback target itself.
        m_Systems.Add(SystemNode{ FString(name), {}, {}, {}, {}, TFunction<void(Scene&, Timestep)>(std::move(exec)) });
        m_Built = false;
        return SystemBuilder{ *this, index };
    }

    void SystemScheduler::Build()
    {
        if (m_Built)
        {
            return;
        }
        DeriveOrder();
        m_Built = true;
    }

    void SystemScheduler::Execute(Scene& scene, Timestep ts)
    {
        Build();

        if (!m_AnyParallel || !IsParallelExecutionEnabled())
        {
            for (const u32 index : m_Order)
            {
                m_Systems[index].Exec(scene, ts);
            }
            return;
        }

        // Parallel path. The calling thread walks the derived order; every
        // UNMARKED system is a join-all barrier, so parallel-marked systems only
        // ever overlap each other (the audit contract in the header). Tasks are
        // engine tasks on the FScheduler pool; with zero workers running,
        // Tasks::Launch executes the body inline on this thread at launch, which
        // collapses back to exact sequential order. Wait() retracts a still-
        // queued task onto this thread, so the join can always make progress.
        TArray<Tasks::TTask<void>> inFlight;
        inFlight.SetNum(m_Systems.Num());
        std::exception_ptr firstError;
        std::mutex errorMutex;

        const auto join = [](Tasks::TTask<void>& task)
        {
            if (task.IsValid())
            {
                task.Wait();
                task = {};
            }
        };
        const auto joinAll = [&inFlight, &join]
        {
            for (auto& task : inFlight)
            {
                join(task);
            }
        };

        for (const u32 index : m_Order)
        {
            SystemNode& node = m_Systems[index];
            if (!node.Parallel)
            {
                joinAll();
                node.Exec(scene, ts);
                continue;
            }

            // Every main-thread predecessor already completed (it was a barrier).
            // A parallel predecessor may still be in flight — hand those to the
            // task graph as PREREQUISITES instead of blocking this thread on
            // them: the successor then starts the moment its predecessors
            // complete (Mass-style dispatch — UE feeds its solver's edges to the
            // task graph the same way). The handles stay live in inFlight until
            // a barrier joins them; the prerequisite holds its own reference.
            TArray<Tasks::TTask<void>> prerequisites;
            for (const u32 pred : m_Predecessors[index])
            {
                if (inFlight[pred].IsValid())
                {
                    prerequisites.Add(inFlight[pred]);
                }
            }

            auto body = [&node, &scene, ts, &firstError, &errorMutex]
            {
                try
                {
                    node.Exec(scene, ts);
                }
                catch (...)
                {
                    // Capture on the worker, rethrow on the caller after the
                    // final join — an exception must not vanish into the pool.
                    const std::scoped_lock lock(errorMutex);
                    if (!firstError)
                    {
                        firstError = std::current_exception();
                    }
                }
            };
            inFlight[index] = prerequisites.IsEmpty()
                                  ? Tasks::Launch(*node.Name, std::move(body), Tasks::ETaskPriority::Normal)
                                  : Tasks::Launch(*node.Name, std::move(body), prerequisites,
                                                  Tasks::ETaskPriority::Normal);
        }
        joinAll();

        if (firstError)
        {
            std::rethrow_exception(firstError);
        }
    }

    const TArray<FString>& SystemScheduler::GetOrderedNames()
    {
        Build();
        return m_OrderedNames;
    }

    bool SystemScheduler::DependsOn(std::string_view system, std::string_view ancestor)
    {
        Build();
        const auto resolveIndex = [this](std::string_view name) -> u32
        {
            const auto it = m_NameToIndex.find(std::string{ name });
            if (it == m_NameToIndex.end())
            {
                const std::string message = "SystemScheduler: DependsOn query names unknown system '" +
                                            std::string{ name } + "'";
                OLO_CORE_ERROR("{}", message);
                throw SystemSchedulerError(message);
            }
            return it->second;
        };
        const u32 target = resolveIndex(system);
        const u32 source = resolveIndex(ancestor);

        // BFS over the derived successor edges from `ancestor`.
        TArray<bool> visited;
        visited.Init(false, m_Systems.Num());
        TArray<u32> queue{ source };
        visited[source] = true;
        while (!queue.IsEmpty())
        {
            const u32 current = queue.Last();
            queue.Pop();
            for (const u32 next : m_Successors[current])
            {
                if (next == target)
                {
                    return true;
                }
                if (!visited[next])
                {
                    visited[next] = true;
                    queue.Add(next);
                }
            }
        }
        return false;
    }

    SystemScheduler::GraphSnapshot SystemScheduler::ExportGraph()
    {
        Build();

        // GraphSnapshot is the MCP/JSON DTO. Copy engine-owned strings at this
        // export boundary instead of retaining a second standard-container owner.
        const auto exportNames = [](const TArray<FString>& names)
        {
            std::vector<std::string> result;
            result.reserve(static_cast<sizet>(names.Num()));
            for (const FString& name : names)
            {
                result.push_back(name.ToStdString());
            }
            return result;
        };
        GraphSnapshot snapshot;
        snapshot.ParallelExecutionEnabled = m_AnyParallel && IsParallelExecutionEnabled();
        snapshot.Nodes.reserve(m_Order.Num());

        // Walk the DERIVED order, not m_Systems: the order is the thing this class
        // computes, so a reader should see the graph laid out the way it will run
        // rather than the way it happened to be registered.
        for (u32 position = 0; position < static_cast<u32>(m_Order.Num()); ++position)
        {
            const SystemNode& node = m_Systems[m_Order[position]];
            snapshot.Nodes.push_back(GraphNode{ node.Name.ToStdString(), exportNames(node.Reads), exportNames(node.Writes), exportNames(node.After), exportNames(node.Before),
                                                node.Parallel, position });
        }

        // m_Successors is already deduplicated (DeriveOrder's edgeSet) and already
        // includes the edges derived from the read/write declarations, so this is
        // the complete edge set the executor honours — not just the explicit
        // After()/Before() ones, which are the only edges a source file shows.
        for (u32 from = 0; from < static_cast<u32>(m_Successors.Num()); ++from)
        {
            for (const u32 to : m_Successors[from])
            {
                snapshot.Edges.push_back(GraphEdge{ m_Systems[from].Name.ToStdString(), m_Systems[to].Name.ToStdString() });
            }
        }
        return snapshot;
    }

    void SystemScheduler::DeriveOrder()
    {
        const u32 n = static_cast<u32>(m_Systems.Num());

        // Name -> index, catching duplicate registrations early (a duplicate makes
        // every by-name After/Before reference ambiguous).
        std::unordered_map<std::string, u32> nameToIndex;
        nameToIndex.reserve(n);
        for (u32 i = 0; i < n; ++i)
        {
            if (!nameToIndex.emplace(m_Systems[i].Name.ToStdString(), i).second)
            {
                const std::string message = "SystemScheduler: duplicate system name '" + m_Systems[i].Name.ToStdString() + "'";
                OLO_CORE_ERROR("{}", message);
                throw SystemSchedulerError(message);
            }
        }

        TArray<TArray<u32>> successors;
        successors.SetNum(static_cast<i32>(n));
        TArray<u32> inDegree;
        inDegree.Init(0, static_cast<i32>(n));
        std::unordered_set<u64> edgeSet; // dedup: from * n + to
        edgeSet.reserve(n * 4);

        const auto addEdge = [&](u32 from, u32 to)
        {
            if (from == to)
            {
                return;
            }
            const u64 key = static_cast<u64>(from) * n + to;
            if (edgeSet.insert(key).second)
            {
                successors[from].Add(to);
                ++inDegree[to];
            }
        };

        // ── Resource read/write edges (RAW / WAW / WAR), derived in registration
        // order exactly like the RenderGraph's per-resource hazard walk. Every
        // edge points forward in registration order, so these alone are always
        // acyclic. ─────────────────────────────────────────────────────────────
        std::unordered_set<std::string> resources;
        for (const SystemNode& node : m_Systems)
        {
            for (const FString& resource : node.Reads)
            {
                resources.insert(resource.ToStdString());
            }
            for (const FString& resource : node.Writes)
            {
                resources.insert(resource.ToStdString());
            }
        }
        TArray<u32> readersSinceWrite;
        for (const std::string& resource : resources)
        {
            i32 lastWriter = -1;
            readersSinceWrite.Reset();
            for (u32 s = 0; s < n; ++s)
            {
                const bool reads = Contains(m_Systems[s].Reads, resource);
                const bool writes = Contains(m_Systems[s].Writes, resource);
                if (reads)
                {
                    if (lastWriter >= 0)
                    {
                        addEdge(static_cast<u32>(lastWriter), s); // read-after-write
                    }
                    readersSinceWrite.Add(s);
                }
                if (writes)
                {
                    if (lastWriter >= 0)
                    {
                        addEdge(static_cast<u32>(lastWriter), s); // write-after-write
                    }
                    for (const u32 reader : readersSinceWrite)
                    {
                        addEdge(reader, s); // write-after-read (addEdge drops the self case)
                    }
                    lastWriter = static_cast<i32>(s);
                    readersSinceWrite.Reset();
                }
            }
        }

        // ── Explicit After()/Before() edges. These are the only source of a cycle,
        // and the only place a dangling (unregistered) reference can appear. ─────
        const auto resolve = [&](const FString& referencedName, const FString& owner) -> u32
        {
            const auto it = nameToIndex.find(referencedName.ToStdString());
            if (it == nameToIndex.end())
            {
                const std::string message = "SystemScheduler: system '" + owner.ToStdString() +
                                            "' references unknown system '" + referencedName.ToStdString() + "'";
                OLO_CORE_ERROR("{}", message);
                throw SystemSchedulerError(message);
            }
            return it->second;
        };
        for (u32 i = 0; i < n; ++i)
        {
            for (const FString& afterName : m_Systems[i].After)
            {
                addEdge(resolve(afterName, m_Systems[i].Name), i); // other -> this
            }
            for (const FString& beforeName : m_Systems[i].Before)
            {
                addEdge(i, resolve(beforeName, m_Systems[i].Name)); // this -> other
            }
        }

        // ── Kahn's algorithm with a registration-index tie-break. Popping the
        // lowest-index ready node yields the lexicographically-smallest valid
        // topological order; when every edge respects registration order that is
        // exactly registration order, so the derived sequence reproduces the
        // historical hard-coded one. ────────────────────────────────────────────
        std::set<u32> ready;
        for (u32 i = 0; i < n; ++i)
        {
            if (inDegree[i] == 0)
            {
                ready.insert(i);
            }
        }

        m_Order.Reset();
        m_Order.Reserve(static_cast<i32>(n));
        while (!ready.empty())
        {
            const u32 s = *ready.begin();
            ready.erase(ready.begin());
            m_Order.Add(s);
            for (const u32 successor : successors[s])
            {
                if (--inDegree[successor] == 0)
                {
                    ready.insert(successor);
                }
            }
        }

        if (static_cast<u32>(m_Order.Num()) != n)
        {
            std::string cycleNames;
            for (u32 i = 0; i < n; ++i)
            {
                if (inDegree[i] != 0)
                {
                    if (!cycleNames.empty())
                    {
                        cycleNames += ", ";
                    }
                    cycleNames += m_Systems[i].Name.ToStdString();
                }
            }
            const std::string message = "SystemScheduler: dependency cycle among systems { " + cycleNames + " }";
            OLO_CORE_ERROR("{}", message);
            throw SystemSchedulerError(message);
        }

        m_OrderedNames.Reset();
        m_OrderedNames.Reserve(static_cast<i32>(n));
        for (const u32 index : m_Order)
        {
            m_OrderedNames.Add(m_Systems[index].Name);
        }

        // Persist the derived DAG for DependsOn queries and the executor.
        m_Successors = std::move(successors);
        m_Predecessors.Reset();
        m_Predecessors.SetNum(static_cast<i32>(n));
        for (u32 from = 0; from < n; ++from)
        {
            for (const u32 to : m_Successors[from])
            {
                m_Predecessors[to].Add(from);
            }
        }
        m_NameToIndex = std::move(nameToIndex);
        m_AnyParallel = std::ranges::any_of(m_Systems, [](const SystemNode& node)
                                            { return node.Parallel; });
    }
} // namespace OloEngine
