#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include <algorithm>
#include <fstream>
#include <unordered_set>

namespace OloEngine
{
    void RenderGraph::Init(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing RenderGraph with dimensions: {}x{}", width, height);

        // Initialize all passes
        for (auto& [name, pass] : m_PassLookup)
        {
            pass->SetupFramebuffer(width, height);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Shutting down RenderGraph");

        // Clear all pass references
        m_PassLookup.clear();
        m_Dependencies.clear();
        m_FramebufferConnections.clear();
        m_InsertionOrder.clear();
        m_PassOrder.clear();
        m_FinalPassName.clear();
        m_ImportedResources.clear();
        m_ResourceRegistry.clear();
        m_RegisteredResources.clear();
        m_ResourceRegistryDiagnostics.clear();
        m_TextureHandlesByName.clear();
        m_BufferHandlesByName.clear();
        m_FramebufferHandlesByName.clear();
        m_TextureHandleSlots.clear();
        m_BufferHandleSlots.clear();
        m_FramebufferHandleSlots.clear();
        m_FreeTextureHandleIndices.clear();
        m_FreeBufferHandleIndices.clear();
        m_FreeFramebufferHandleIndices.clear();
        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::ResetTopology()
    {
        OLO_PROFILE_FUNCTION();

        // Wipe topology bookkeeping but leave pass framebuffers / internal
        // state alone — the passes themselves are owned externally and will
        // be re-registered by the caller as part of the new topology.
        m_PassLookup.clear();
        m_Dependencies.clear();
        m_FramebufferConnections.clear();
        m_InsertionOrder.clear();
        m_PassOrder.clear();
        m_FinalPassName.clear();
        m_CachedPipes.clear();
        m_CachedExecutionOrder.clear();
        m_DependencyGraphDirty = true;
        m_ImportedResources.clear();
        m_ResourceRegistry.clear();
        m_RegisteredResources.clear();
        m_ResourceRegistryDiagnostics.clear();
        m_TextureHandlesByName.clear();
        m_BufferHandlesByName.clear();
        m_FramebufferHandlesByName.clear();

        auto invalidateSlots = [](auto& slots, auto& freeIndices)
        {
            freeIndices.clear();
            freeIndices.reserve(slots.size());
            for (u32 i = 0; i < static_cast<u32>(slots.size()); ++i)
            {
                auto& slot = slots[i];
                slot.Alive = false;
                slot.Name.clear();
                if (slot.Generation == 0)
                    slot.Generation = 1;
                ++slot.Generation;
                freeIndices.push_back(i);
            }
        };

        invalidateSlots(m_TextureHandleSlots, m_FreeTextureHandleIndices);
        invalidateSlots(m_BufferHandleSlots, m_FreeBufferHandleIndices);
        invalidateSlots(m_FramebufferHandleSlots, m_FreeFramebufferHandleIndices);

        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::AddPass(const Ref<RenderPass>& pass)
    {
        OLO_PROFILE_FUNCTION();

        std::string name = pass->GetName();
        OLO_CORE_INFO("Adding RenderPass to graph: {}", name);

        // Track insertion order so topological sort has a stable tie-break
        // when multiple passes have no dependencies between them. Without
        // this, the unordered_map iteration order leaks into m_PassOrder
        // and varies between platforms/standard-library versions, causing
        // the hazard validator to classify RAW vs WAR differently.
        if (!m_PassLookup.contains(name))
            m_InsertionOrder.push_back(name);
        m_PassLookup[name] = pass;
        m_DependencyGraphDirty = true;
        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::ImportResource(std::string_view name, const RGResourceDesc& desc)
    {
        auto importedDesc = desc;
        importedDesc.Imported = true;
        if (importedDesc.DebugName.empty())
            importedDesc.DebugName = std::string(name);

        m_ImportedResources[std::string(name)] = std::move(importedDesc);
        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::ClearImportedResources()
    {
        m_ImportedResources.clear();
        m_ResourceRegistryDirty = true;
    }

    const std::vector<RenderGraph::ResourceInfo>& RenderGraph::GetRegisteredResources() const
    {
        EnsureResourceRegistryBuilt();
        return m_RegisteredResources;
    }

    const RenderGraph::ResourceInfo* RenderGraph::FindRegisteredResource(std::string_view name) const
    {
        EnsureResourceRegistryBuilt();

        if (auto it = m_ResourceRegistry.find(std::string(name)); it != m_ResourceRegistry.end())
            return &it->second;

        return nullptr;
    }

    RGTextureHandle RenderGraph::GetTextureHandle(std::string_view name) const
    {
        EnsureResourceRegistryBuilt();

        if (auto it = m_TextureHandlesByName.find(std::string(name)); it != m_TextureHandlesByName.end())
            return it->second;

        return {};
    }

    RGBufferHandle RenderGraph::GetBufferHandle(std::string_view name) const
    {
        EnsureResourceRegistryBuilt();

        if (auto it = m_BufferHandlesByName.find(std::string(name)); it != m_BufferHandlesByName.end())
            return it->second;

        return {};
    }

    RGFramebufferHandle RenderGraph::GetFramebufferHandle(std::string_view name) const
    {
        EnsureResourceRegistryBuilt();

        if (auto it = m_FramebufferHandlesByName.find(std::string(name)); it != m_FramebufferHandlesByName.end())
            return it->second;

        return {};
    }

    bool RenderGraph::IsTextureHandleCurrent(RGTextureHandle handle) const
    {
        EnsureResourceRegistryBuilt();

        if (!handle.IsValid() || handle.Index >= m_TextureHandleSlots.size())
            return false;

        const auto& slot = m_TextureHandleSlots[handle.Index];
        return slot.Alive && slot.Generation == handle.Generation;
    }

    bool RenderGraph::IsBufferHandleCurrent(RGBufferHandle handle) const
    {
        EnsureResourceRegistryBuilt();

        if (!handle.IsValid() || handle.Index >= m_BufferHandleSlots.size())
            return false;

        const auto& slot = m_BufferHandleSlots[handle.Index];
        return slot.Alive && slot.Generation == handle.Generation;
    }

    bool RenderGraph::IsFramebufferHandleCurrent(RGFramebufferHandle handle) const
    {
        EnsureResourceRegistryBuilt();

        if (!handle.IsValid() || handle.Index >= m_FramebufferHandleSlots.size())
            return false;

        const auto& slot = m_FramebufferHandleSlots[handle.Index];
        return slot.Alive && slot.Generation == handle.Generation;
    }

    void RenderGraph::ConnectPass(const std::string& outputPass, const std::string& inputPass)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_PassLookup.contains(outputPass))
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Output pass '{}' not found!", outputPass);
            return;
        }

        if (!m_PassLookup.contains(inputPass))
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Input pass '{}' not found!", inputPass);
            return;
        }

        OLO_CORE_INFO("Connecting passes (with framebuffer piping): {} -> {}", outputPass, inputPass);

        // Add dependency for execution ordering (avoid duplicates)
        auto& deps = m_Dependencies[inputPass];
        if (std::find(deps.begin(), deps.end(), outputPass) == deps.end())
        {
            deps.push_back(outputPass);
        }
        // Mark for framebuffer piping (avoid duplicates)
        auto& conns = m_FramebufferConnections[outputPass];
        if (std::find(conns.begin(), conns.end(), inputPass) == conns.end())
        {
            conns.push_back(inputPass);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::AddExecutionDependency(const std::string& beforePass, const std::string& afterPass)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_PassLookup.contains(beforePass))
        {
            OLO_CORE_ERROR("RenderGraph::AddExecutionDependency: Pass '{}' not found!", beforePass);
            return;
        }

        if (!m_PassLookup.contains(afterPass))
        {
            OLO_CORE_ERROR("RenderGraph::AddExecutionDependency: Pass '{}' not found!", afterPass);
            return;
        }

        OLO_CORE_INFO("Adding execution dependency (ordering only): {} -> {}", beforePass, afterPass);

        // Only add dependency for execution ordering, no framebuffer piping (avoid duplicates)
        auto& deps = m_Dependencies[afterPass];
        if (std::find(deps.begin(), deps.end(), beforePass) == deps.end())
        {
            deps.push_back(beforePass);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Cycle detected — m_PassOrder is empty. Abort execution;
                // running with a partial topo order would execute the wrong
                // subset of passes in the wrong order. Keep the dirty flag
                // set so a corrected graph can retry.
                OLO_CORE_ERROR("RenderGraph::Execute: aborting because dependency graph rebuild failed");
                return;
            }
            ResolveFinalPass();
            RebuildExecutionCache();
            m_DependencyGraphDirty = false;
        }

        // First pass: Connect framebuffers between passes that use framebuffer piping
        for (const auto& pipe : m_CachedPipes)
        {
            Ref<Framebuffer> outputFramebuffer = pipe.OutputPass->GetTarget();
            if (outputFramebuffer)
            {
                for (auto* inputPass : pipe.InputPasses)
                {
                    inputPass->SetInputFramebuffer(outputFramebuffer);
                }
            }
        }

        // Second pass: Execute passes in order (zero-overhead — no lookups, no branching)
        RGCommandContext commandContext;
        for (auto* pass : m_CachedExecutionOrder)
        {
            commandContext.BeginPass(pass->GetName());
            pass->Execute(commandContext);
            commandContext.EndPass();
        }
    }

    void RenderGraph::RebuildExecutionCache()
    {
        OLO_PROFILE_FUNCTION();

        // Build framebuffer piping cache with validation
        m_CachedPipes.clear();
        for (const auto& [outputPassName, inputPassNames] : m_FramebufferConnections)
        {
            auto outputIt = m_PassLookup.find(outputPassName);
            if (outputIt == m_PassLookup.end() || !outputIt->second)
            {
                OLO_CORE_ERROR("RenderGraph: Output pass '{}' not found or null in framebuffer piping — skipping connection", outputPassName);
                continue;
            }

            FramebufferPipe pipe;
            pipe.OutputPass = outputIt->second.Raw();

            for (const auto& inputPassName : inputPassNames)
            {
                auto inputIt = m_PassLookup.find(inputPassName);
                if (inputIt == m_PassLookup.end() || !inputIt->second)
                {
                    OLO_CORE_ERROR("RenderGraph: Input pass '{}' not found or null in framebuffer piping — skipping", inputPassName);
                    continue;
                }
                pipe.InputPasses.push_back(inputIt->second.Raw());
            }

            m_CachedPipes.push_back(std::move(pipe));
        }

        // Build execution order cache with validation
        m_CachedExecutionOrder.clear();
        m_CachedExecutionOrder.reserve(m_PassOrder.size());

        for (const auto& passName : m_PassOrder)
        {
            auto it = m_PassLookup.find(passName);
            if (it == m_PassLookup.end() || !it->second)
            {
                OLO_CORE_ERROR("RenderGraph: Pass '{}' is null or missing from lookup — it will be skipped during execution!", passName);
                continue;
            }
            m_CachedExecutionOrder.push_back(it->second.Raw());
        }

        OLO_CORE_INFO("RenderGraph: Execution cache rebuilt — {} passes, {} framebuffer pipes",
                      m_CachedExecutionOrder.size(), m_CachedPipes.size());
    }

    void RenderGraph::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        for (auto& [name, pass] : m_PassLookup)
        {
            pass->ResizeFramebuffer(width, height);
        }
    }

    void RenderGraph::SetFinalPass(const std::string& passName)
    {
        m_FinalPassName = passName;
    }

    std::vector<Ref<RenderPass>> RenderGraph::GetAllPasses() const
    {
        std::vector<Ref<RenderPass>> result;
        result.reserve(m_PassLookup.size());
        for (const auto& [name, pass] : m_PassLookup)
        {
            result.push_back(pass);
        }
        return result;
    }

    bool RenderGraph::IsFinalPass(const std::string& passName) const
    {
        return passName == m_FinalPassName;
    }

    std::vector<RenderGraph::ConnectionInfo> RenderGraph::GetConnections() const
    {
        std::vector<ConnectionInfo> result;
        for (const auto& [input, outputs] : m_Dependencies)
        {
            for (const auto& output : outputs)
            {
                result.push_back({ output, input, 0 });
            }
        }
        return result;
    }

    std::vector<RenderGraph::PassSubmissionInfo> RenderGraph::GetPassSubmissionInfo() const
    {
        std::vector<PassSubmissionInfo> result;
        result.reserve(m_PassLookup.size());

        const auto appendPassInfo = [&](const std::string& name)
        {
            auto it = m_PassLookup.find(name);
            if (it == m_PassLookup.end() || !it->second)
                return;

            const auto& pass = it->second;
            result.push_back(PassSubmissionInfo{
                .PassName = name,
                .Submission = pass->GetSubmissionModel(),
                .DeclaresResources = !pass->GetReads().empty() || !pass->GetWrites().empty(),
            });
        };

        std::unordered_set<std::string> visited;
        visited.reserve(m_InsertionOrder.size());

        for (const auto& name : m_InsertionOrder)
        {
            appendPassInfo(name);
            visited.insert(name);
        }

        for (const auto& [name, pass] : m_PassLookup)
        {
            if (!visited.contains(name))
                appendPassInfo(name);
        }

        return result;
    }

    bool RenderGraph::UpdateDependencyGraph()
    {
        OLO_PROFILE_FUNCTION();

        m_PassOrder.clear();

        // Topological sort to determine execution order
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> inProgress;

        std::function<bool(const std::string&)> visit = [&](const std::string& node)
        {
            if (inProgress.contains(node))
            {
                OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Cycle detected in graph!");
                return false;
            }

            if (visited.contains(node))
            {
                return true;
            }

            inProgress.insert(node);

            if (m_Dependencies.contains(node))
            {
                for (const auto& dep : m_Dependencies[node])
                {
                    if (!visit(dep))
                        return false;
                }
            }

            visited.insert(node);
            inProgress.erase(node);
            m_PassOrder.push_back(node);

            return true;
        };

        // Visit all nodes in insertion order so ties are broken
        // deterministically (independent of std::unordered_map hashing).
        for (const auto& name : m_InsertionOrder)
        {
            if (!m_PassLookup.contains(name))
                continue;
            if (!visited.contains(name))
            {
                if (!visit(name))
                {
                    OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Failed to build execution order!");
                    m_PassOrder.clear();
                    return false;
                }
            }
        }

        OLO_CORE_INFO("RenderGraph execution order updated with {} passes", m_PassOrder.size());
        return true;
    }

    void RenderGraph::ResolveFinalPass()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FinalPassName.empty())
        {
            // If no final pass was explicitly set, try to find a pass with no dependents
            // Iterate m_PassOrder (deterministic) instead of m_PassLookup (unordered_map)
            for (auto it = m_PassOrder.rbegin(); it != m_PassOrder.rend(); ++it)
            {
                const auto& name = *it;
                if (!m_FramebufferConnections.contains(name) ||
                    m_FramebufferConnections[name].empty())
                {
                    m_FinalPassName = name;
                    OLO_CORE_INFO("RenderGraph: Auto-selected final pass: {}", name);
                    break;
                }
            }
        }

        if (m_FinalPassName.empty())
        {
            OLO_CORE_WARN("RenderGraph: Could not determine final pass!");
        }
    }

    // =========================================================================
    // Resource-aware hazard validation
    // =========================================================================
    // Algorithm: walk passes in topological order, maintain a per-resource
    // state (last writer + live readers since that writer), and for every
    // declared access check that the current pass transitively depends on
    // any prior conflicting accesses.
    //
    // Transitive closure is built on-demand from m_Dependencies (which holds
    // every edge — ConnectPass + AddExecutionDependency both append here).
    std::vector<RenderGraph::Hazard> RenderGraph::ValidateResourceHazards()
    {
        OLO_PROFILE_FUNCTION();

        EnsureResourceRegistryBuilt();

        std::vector<Hazard> hazards = m_ResourceRegistryDiagnostics;

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Partial topo order is useless — running the remaining
                // validator over an incomplete m_PassOrder would produce
                // misleading "missing dependency" reports for passes the
                // cycle excluded. Surface a synthetic Cycle hazard so
                // callers can distinguish "no hazards" from "could not
                // validate" (the empty-vector overload used to conflate
                // both).
                OLO_CORE_ERROR("RenderGraph::ValidateResourceHazards: aborting (graph has a cycle)");
                Hazard h;
                h.Kind = HazardKind::Cycle;
                h.Message = "RenderGraph contains a cycle; resource hazard validation aborted";
                hazards.push_back(std::move(h));
                return hazards;
            }
            // Note: we deliberately leave m_DependencyGraphDirty set so the
            // first Execute() after validation still runs ResolveFinalPass +
            // RebuildExecutionCache. The validator only needs topo order.
        }

        // Build transitive dependency closure: for each pass P, closure[P]
        // is the set of all passes that must execute before P.
        std::unordered_map<std::string, std::unordered_set<std::string>> closure;
        closure.reserve(m_PassOrder.size());
        for (const auto& passName : m_PassOrder)
        {
            std::unordered_set<std::string>& cls = closure[passName];
            std::vector<std::string> frontier;
            auto depsIt = m_Dependencies.find(passName);
            if (depsIt != m_Dependencies.end())
            {
                frontier.insert(frontier.end(), depsIt->second.begin(), depsIt->second.end());
            }
            while (!frontier.empty())
            {
                const std::string parent = std::move(frontier.back());
                frontier.pop_back();
                if (!cls.insert(parent).second)
                    continue;
                auto parentDeps = m_Dependencies.find(parent);
                if (parentDeps == m_Dependencies.end())
                    continue;
                for (const auto& grand : parentDeps->second)
                {
                    if (!cls.contains(grand))
                        frontier.push_back(grand);
                }
            }
        }

        auto dependsOn = [&closure](const std::string& later, const std::string& earlier) -> bool
        {
            auto it = closure.find(later);
            if (it == closure.end())
                return false;
            return it->second.contains(earlier);
        };

        struct ResourceState
        {
            std::string LastWriter;                      // empty => never written
            std::unordered_set<std::string> LiveReaders; // readers since lastWriter
        };
        std::unordered_map<std::string, ResourceState> state;

        for (const auto& passName : m_PassOrder)
        {
            auto passIt = m_PassLookup.find(passName);
            if (passIt == m_PassLookup.end() || !passIt->second)
                continue;
            const RenderPass& pass = *passIt->second;

            // Record reads first — a same-pass read+write on the same
            // resource isn't a hazard against itself.
            for (const ResourceHandle& r : pass.GetReads())
            {
                ResourceState& st = state[r.Name];
                if (!st.LastWriter.empty() && st.LastWriter != passName && !dependsOn(passName, st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::ReadAfterWrite;
                    h.Resource = r.Name;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "RAW: pass '" + passName + "' reads resource '" + r.Name +
                                "' written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                    hazards.push_back(std::move(h));
                }
                st.LiveReaders.insert(passName);
            }

            for (const ResourceHandle& w : pass.GetWrites())
            {
                ResourceState& st = state[w.Name];

                if (!st.LastWriter.empty() && st.LastWriter != passName && !dependsOn(passName, st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::WriteAfterWrite;
                    h.Resource = w.Name;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "WAW: pass '" + passName + "' writes resource '" + w.Name +
                                "' previously written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                    hazards.push_back(std::move(h));
                }

                for (const auto& reader : st.LiveReaders)
                {
                    if (reader == passName)
                        continue;
                    if (!dependsOn(passName, reader))
                    {
                        Hazard h;
                        h.Kind = HazardKind::WriteAfterRead;
                        h.Resource = w.Name;
                        h.Producer = reader;
                        h.Consumer = passName;
                        h.Message = "WAR: pass '" + passName + "' overwrites resource '" + w.Name +
                                    "' still live for reader '" + reader +
                                    "' without declaring a dependency";
                        OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                        hazards.push_back(std::move(h));
                    }
                }

                st.LastWriter = passName;
                st.LiveReaders.clear();
            }
        }

        return hazards;
    }

    void RenderGraph::EnsureResourceRegistryBuilt() const
    {
        if (!m_ResourceRegistryDirty)
            return;

        auto isTextureKind = [](const ResourceHandle::Kind kind)
        {
            return kind == ResourceHandle::Kind::Texture2D ||
                   kind == ResourceHandle::Kind::Texture2DArray ||
                   kind == ResourceHandle::Kind::TextureCube ||
                   kind == ResourceHandle::Kind::TextureCubeArray;
        };

        auto isBufferKind = [](const ResourceHandle::Kind kind)
        {
            return kind == ResourceHandle::Kind::UniformBuffer ||
                   kind == ResourceHandle::Kind::StorageBuffer;
        };

        m_ResourceRegistry.clear();
        m_RegisteredResources.clear();
        m_ResourceRegistryDiagnostics.clear();
        for (auto& info : m_RegisteredResources)
        {
            info.TextureHandle = {};
            info.BufferHandle = {};
            info.FramebufferHandle = {};
        }

        auto appendUnique = [](std::vector<std::string>& names, const std::string& value)
        {
            if (std::find(names.begin(), names.end(), value) == names.end())
                names.push_back(value);
        };

        for (const auto& [name, desc] : m_ImportedResources)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.empty())
                info.Desc.DebugName = name;
            info.Desc.Imported = true;
            m_ResourceRegistry[name] = std::move(info);
        }

        auto registerDeclaration = [this, &appendUnique](const std::string& passName,
                                                         const ResourceHandle& handle,
                                                         const bool isWrite)
        {
            auto [it, inserted] = m_ResourceRegistry.try_emplace(handle.Name);
            auto& info = it->second;
            if (inserted)
            {
                info.Name = handle.Name;
                info.Desc = RGResourceDesc::FromLegacy(handle.Type, handle.Name);
            }
            else if (info.Desc.DebugName.empty())
            {
                info.Desc.DebugName = handle.Name;
            }

            const auto declaredKind = handle.Type;
            const auto existingKind = info.Desc.Kind;
            if (existingKind == ResourceHandle::Kind::Unknown && declaredKind != ResourceHandle::Kind::Unknown)
            {
                info.Desc.Kind = declaredKind;
            }
            else if (existingKind != ResourceHandle::Kind::Unknown &&
                     declaredKind != ResourceHandle::Kind::Unknown &&
                     existingKind != declaredKind)
            {
                const auto priorPass = !info.Producers.empty()
                                           ? info.Producers.front()
                                           : (!info.Consumers.empty() ? info.Consumers.front() : std::string{});

                Hazard h;
                h.Kind = HazardKind::ResourceKindMismatch;
                h.Resource = handle.Name;
                h.Producer = priorPass;
                h.Consumer = passName;
                h.Message = "Kind mismatch: resource '" + handle.Name +
                            "' was previously declared as '" + std::string(ToString(existingKind)) +
                            "' but pass '" + passName + "' declares it as '" +
                            std::string(ToString(declaredKind)) + "'";
                OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                m_ResourceRegistryDiagnostics.push_back(std::move(h));
            }

            if (isWrite)
                appendUnique(info.Producers, passName);
            else
                appendUnique(info.Consumers, passName);
        };

        for (const auto& passName : m_InsertionOrder)
        {
            auto passIt = m_PassLookup.find(passName);
            if (passIt == m_PassLookup.end() || !passIt->second)
                continue;

            const auto& reads = passIt->second->GetReads();
            for (const auto& read : reads)
            {
                registerDeclaration(passName, read, false);
            }

            const auto& writes = passIt->second->GetWrites();
            for (const auto& write : writes)
            {
                registerDeclaration(passName, write, true);
            }
        }

        m_RegisteredResources.reserve(m_ResourceRegistry.size());
        for (const auto& [name, info] : m_ResourceRegistry)
        {
            m_RegisteredResources.push_back(info);
        }
        std::sort(m_RegisteredResources.begin(), m_RegisteredResources.end(),
                  [](const ResourceInfo& lhs, const ResourceInfo& rhs)
                  {
                      return lhs.Name < rhs.Name;
                  });

        auto reconcileTypeHandles = [](auto& handlesByName,
                                       auto& slots,
                                       auto& freeIndices,
                                       const std::unordered_set<std::string>& activeNames)
        {
            for (auto it = handlesByName.begin(); it != handlesByName.end();)
            {
                if (activeNames.contains(it->first))
                {
                    ++it;
                    continue;
                }

                const auto staleHandle = it->second;
                if (staleHandle.IsValid() && staleHandle.Index < slots.size())
                {
                    auto& slot = slots[staleHandle.Index];
                    slot.Alive = false;
                    slot.Name.clear();
                    if (slot.Generation == 0)
                        slot.Generation = 1;
                    ++slot.Generation;
                    freeIndices.push_back(staleHandle.Index);
                }

                it = handlesByName.erase(it);
            }
        };

        std::unordered_set<std::string> activeTextureNames;
        std::unordered_set<std::string> activeBufferNames;
        std::unordered_set<std::string> activeFramebufferNames;
        for (const auto& info : m_RegisteredResources)
        {
            if (isTextureKind(info.Desc.Kind))
                activeTextureNames.insert(info.Name);
            else if (isBufferKind(info.Desc.Kind))
                activeBufferNames.insert(info.Name);
            else if (info.Desc.Kind == ResourceHandle::Kind::Framebuffer)
                activeFramebufferNames.insert(info.Name);
        }

        reconcileTypeHandles(m_TextureHandlesByName, m_TextureHandleSlots, m_FreeTextureHandleIndices, activeTextureNames);
        reconcileTypeHandles(m_BufferHandlesByName, m_BufferHandleSlots, m_FreeBufferHandleIndices, activeBufferNames);
        reconcileTypeHandles(m_FramebufferHandlesByName, m_FramebufferHandleSlots, m_FreeFramebufferHandleIndices, activeFramebufferNames);

        auto allocateHandle = [](const std::string& name,
                                 auto& handlesByName,
                                 auto& slots,
                                 auto& freeIndices,
                                 auto makeHandle)
        {
            if (auto it = handlesByName.find(name); it != handlesByName.end())
                return it->second;

            u32 index = 0;
            if (!freeIndices.empty())
            {
                index = freeIndices.back();
                freeIndices.pop_back();

                auto& slot = slots[index];
                slot.Alive = true;
                slot.Name = name;
                if (slot.Generation == 0)
                    slot.Generation = 1;

                auto handle = makeHandle(index, slot.Generation);
                handlesByName[name] = handle;
                return handle;
            }

            index = static_cast<u32>(slots.size());
            slots.push_back({ 1, true, name });
            auto handle = makeHandle(index, 1);
            handlesByName[name] = handle;
            return handle;
        };

        for (auto& info : m_RegisteredResources)
        {
            if (isTextureKind(info.Desc.Kind))
            {
                info.TextureHandle = allocateHandle(info.Name,
                                                    m_TextureHandlesByName,
                                                    m_TextureHandleSlots,
                                                    m_FreeTextureHandleIndices,
                                                    [](u32 index, u32 generation)
                                                    {
                                                        return RGTextureHandle{ index, generation };
                                                    });
            }
            else if (isBufferKind(info.Desc.Kind))
            {
                info.BufferHandle = allocateHandle(info.Name,
                                                   m_BufferHandlesByName,
                                                   m_BufferHandleSlots,
                                                   m_FreeBufferHandleIndices,
                                                   [](u32 index, u32 generation)
                                                   {
                                                       return RGBufferHandle{ index, generation };
                                                   });
            }
            else if (info.Desc.Kind == ResourceHandle::Kind::Framebuffer)
            {
                info.FramebufferHandle = allocateHandle(info.Name,
                                                        m_FramebufferHandlesByName,
                                                        m_FramebufferHandleSlots,
                                                        m_FreeFramebufferHandleIndices,
                                                        [](u32 index, u32 generation)
                                                        {
                                                            return RGFramebufferHandle{ index, generation };
                                                        });
            }
        }

        m_ResourceRegistryDirty = false;
    }

    bool RenderGraph::DumpToDot(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();

        std::ofstream out(filePath);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("RenderGraph::DumpToDot: failed to open '{}' for writing", filePath);
            return false;
        }

        out << "// OloEngine RenderGraph — generated by RenderGraph::DumpToDot\n";
        out << "// Render with:  dot -Tsvg " << filePath << " -o graph.svg\n";
        out << "digraph RenderGraph {\n";
        out << "    rankdir=LR;\n";
        out << "    node [shape=box, style=\"rounded,filled\", fillcolor=\"#e8f0fe\", fontname=\"Helvetica\"];\n";
        out << "    edge [fontname=\"Helvetica\", fontsize=10];\n\n";

        // Nodes in insertion order; final pass is double-ringed.
        for (const auto& name : m_InsertionOrder)
        {
            const bool isFinal = (name == m_FinalPassName);
            out << "    \"" << name << "\"";
            if (isFinal)
                out << " [peripheries=2, fillcolor=\"#d4edda\"]";
            out << ";\n";
        }
        out << "\n";

        // Solid edges = framebuffer piping (ConnectPass).
        for (const auto& [producer, consumers] : m_FramebufferConnections)
        {
            for (const auto& consumer : consumers)
            {
                out << "    \"" << producer << "\" -> \"" << consumer
                    << "\" [color=\"#1a73e8\", label=\"fb\"];\n";
            }
        }

        // Dashed grey edges = ordering-only (AddExecutionDependency), but
        // suppress ones that are already expressed as framebuffer pipes to
        // avoid double-drawing.
        for (const auto& [consumer, producers] : m_Dependencies)
        {
            for (const auto& producer : producers)
            {
                // Skip if this edge is already a framebuffer pipe
                auto it = m_FramebufferConnections.find(producer);
                if (it != m_FramebufferConnections.end())
                {
                    const auto& pipedConsumers = it->second;
                    if (std::find(pipedConsumers.begin(), pipedConsumers.end(), consumer) != pipedConsumers.end())
                        continue;
                }
                out << "    \"" << producer << "\" -> \"" << consumer
                    << "\" [style=dashed, color=\"#5f6368\", label=\"order\"];\n";
            }
        }

        out << "}\n";
        out.close();

        OLO_CORE_INFO("RenderGraph::DumpToDot: wrote {} passes, {} framebuffer edges, "
                      "{} dependency groups to '{}'",
                      m_InsertionOrder.size(),
                      m_FramebufferConnections.size(),
                      m_Dependencies.size(),
                      filePath);
        return true;
    }
} // namespace OloEngine
