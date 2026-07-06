#include "OloEnginePCH.h"
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
        bool Contains(const std::vector<std::string>& names, const std::string& value)
        {
            return std::ranges::find(names, value) != names.end();
        }

        // Default for the parallel path: enabled, unless the environment opts the
        // process out (the debugging/bisection lever — same systems, same derived
        // order, one thread). Mirrors the scheduler's other env knobs
        // (OLO_TASK_GRAPH_*); only the exact value "1" disables.
        bool ParallelEnabledDefault()
        {
            const char* env = std::getenv("OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL");
            return !(env && env[0] == '1' && env[1] == '\0');
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
        m_Owner.m_Systems[m_Index].Reads.emplace_back(resource);
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::Writes(std::string_view resource)
    {
        m_Owner.m_Systems[m_Index].Writes.emplace_back(resource);
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
        m_Owner.m_Systems[m_Index].After.emplace_back(systemName);
        m_Owner.m_Built = false;
        return *this;
    }

    SystemScheduler::SystemBuilder& SystemScheduler::SystemBuilder::Before(std::string_view systemName)
    {
        m_Owner.m_Systems[m_Index].Before.emplace_back(systemName);
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
        const u32 index = static_cast<u32>(m_Systems.size());
        m_Systems.push_back(SystemNode{ std::move(name), {}, {}, {}, {}, std::move(exec) });
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
        std::vector<Tasks::TTask<void>> inFlight(m_Systems.size());
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
            std::vector<Tasks::TTask<void>> prerequisites;
            for (const u32 pred : m_Predecessors[index])
            {
                if (inFlight[pred].IsValid())
                {
                    prerequisites.push_back(inFlight[pred]);
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
            inFlight[index] = prerequisites.empty()
                                  ? Tasks::Launch(node.Name.c_str(), std::move(body), Tasks::ETaskPriority::Normal)
                                  : Tasks::Launch(node.Name.c_str(), std::move(body), prerequisites,
                                                  Tasks::ETaskPriority::Normal);
        }
        joinAll();

        if (firstError)
        {
            std::rethrow_exception(firstError);
        }
    }

    const std::vector<std::string>& SystemScheduler::GetOrderedNames()
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
        std::vector<bool> visited(m_Systems.size(), false);
        std::vector<u32> queue{ source };
        visited[source] = true;
        while (!queue.empty())
        {
            const u32 current = queue.back();
            queue.pop_back();
            for (const u32 next : m_Successors[current])
            {
                if (next == target)
                {
                    return true;
                }
                if (!visited[next])
                {
                    visited[next] = true;
                    queue.push_back(next);
                }
            }
        }
        return false;
    }

    void SystemScheduler::DeriveOrder()
    {
        const u32 n = static_cast<u32>(m_Systems.size());

        // Name -> index, catching duplicate registrations early (a duplicate makes
        // every by-name After/Before reference ambiguous).
        std::unordered_map<std::string, u32> nameToIndex;
        nameToIndex.reserve(n);
        for (u32 i = 0; i < n; ++i)
        {
            if (!nameToIndex.emplace(m_Systems[i].Name, i).second)
            {
                const std::string message = "SystemScheduler: duplicate system name '" + m_Systems[i].Name + "'";
                OLO_CORE_ERROR("{}", message);
                throw SystemSchedulerError(message);
            }
        }

        std::vector<std::vector<u32>> successors(n);
        std::vector<u32> inDegree(n, 0);
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
                successors[from].push_back(to);
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
            resources.insert(node.Reads.begin(), node.Reads.end());
            resources.insert(node.Writes.begin(), node.Writes.end());
        }
        std::vector<u32> readersSinceWrite;
        for (const std::string& resource : resources)
        {
            i32 lastWriter = -1;
            readersSinceWrite.clear();
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
                    readersSinceWrite.push_back(s);
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
                    readersSinceWrite.clear();
                }
            }
        }

        // ── Explicit After()/Before() edges. These are the only source of a cycle,
        // and the only place a dangling (unregistered) reference can appear. ─────
        const auto resolve = [&](const std::string& referencedName, const std::string& owner) -> u32
        {
            const auto it = nameToIndex.find(referencedName);
            if (it == nameToIndex.end())
            {
                const std::string message = "SystemScheduler: system '" + owner +
                                            "' references unknown system '" + referencedName + "'";
                OLO_CORE_ERROR("{}", message);
                throw SystemSchedulerError(message);
            }
            return it->second;
        };
        for (u32 i = 0; i < n; ++i)
        {
            for (const std::string& afterName : m_Systems[i].After)
            {
                addEdge(resolve(afterName, m_Systems[i].Name), i); // other -> this
            }
            for (const std::string& beforeName : m_Systems[i].Before)
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

        m_Order.clear();
        m_Order.reserve(n);
        while (!ready.empty())
        {
            const u32 s = *ready.begin();
            ready.erase(ready.begin());
            m_Order.push_back(s);
            for (const u32 successor : successors[s])
            {
                if (--inDegree[successor] == 0)
                {
                    ready.insert(successor);
                }
            }
        }

        if (m_Order.size() != n)
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
                    cycleNames += m_Systems[i].Name;
                }
            }
            const std::string message = "SystemScheduler: dependency cycle among systems { " + cycleNames + " }";
            OLO_CORE_ERROR("{}", message);
            throw SystemSchedulerError(message);
        }

        m_OrderedNames.clear();
        m_OrderedNames.reserve(n);
        for (const u32 index : m_Order)
        {
            m_OrderedNames.push_back(m_Systems[index].Name);
        }

        // Persist the derived DAG for DependsOn queries and the executor.
        m_Successors = std::move(successors);
        m_Predecessors.assign(n, {});
        for (u32 from = 0; from < n; ++from)
        {
            for (const u32 to : m_Successors[from])
            {
                m_Predecessors[to].push_back(from);
            }
        }
        m_NameToIndex = std::move(nameToIndex);
        m_AnyParallel = std::ranges::any_of(m_Systems, [](const SystemNode& node)
                                            { return node.Parallel; });
    }
} // namespace OloEngine
