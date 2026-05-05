#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace OloEngine
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }

        bool IsRenderGraphDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_RENDERGRAPH_DIAGNOSTICS");
            return enabled;
        }
    } // namespace

    void RenderGraph::Init(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Initializing RenderGraph with dimensions: {}x{}", width, height);

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
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Shutting down RenderGraph");

        m_TransientPool.Clear();

        // Clear all pass references
        m_PassLookup.clear();
        m_Dependencies.clear();
        m_ExplicitDependencies.clear();
        m_InsertionOrder.clear();
        m_PassOrder.clear();
        m_FinalPassName.clear();
        m_HasExplicitFinalPass = false;
        m_ReachablePasses.clear();
        m_CulledPasses.clear();
        m_PassAccessDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BarrierDiagnostics.clear();
        m_LastPassTimings.clear();
        m_FallbackActivations.clear();
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
        m_PhysicalTextures.clear();
        m_PhysicalFramebuffers.clear();
        m_PhysicalBuffers.clear();
        m_PlaceholderResolveWarningsThisFrame.clear();
        m_TextureExtracts.clear();
        m_HistoryTextureExtracts.clear();
        m_FramebufferExtracts.clear();
        m_TemporalHistoryContracts.clear();
        m_Blackboard.Reset();
        m_TransientResourceDescs.clear();
        m_TransientPlan.clear();
        m_TransientFramebufferOverrides.clear();
        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::ResetTopology()
    {
        OLO_PROFILE_FUNCTION();

        m_TransientPool.Clear();

        // Wipe topology bookkeeping but leave pass framebuffers / internal
        // state alone — the passes themselves are owned externally and will
        // be re-registered by the caller as part of the new topology.
        m_PassLookup.clear();
        m_Dependencies.clear();
        m_ExplicitDependencies.clear();
        m_InsertionOrder.clear();
        m_PassOrder.clear();
        m_FinalPassName.clear();
        m_HasExplicitFinalPass = false;
        m_ReachablePasses.clear();
        m_CulledPasses.clear();
        m_PassAccessDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BarrierDiagnostics.clear();
        m_LastPassTimings.clear();
        m_FallbackActivations.clear();
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

        // Clear physical resources that were bound via ImportTexture/ImportFramebuffer.
        // The slot entries are already invalidated above; clearing the physical vectors
        // releases Framebuffer refs so the destructor runs at the right time.
        for (auto& phys : m_PhysicalFramebuffers)
            phys.FB.Reset();
        m_PhysicalTextures.clear();
        m_PhysicalFramebuffers.clear();
        m_PhysicalBuffers.clear();
        m_PlaceholderResolveWarningsThisFrame.clear();
        m_TextureExtracts.clear();
        m_HistoryTextureExtracts.clear();
        m_FramebufferExtracts.clear();
        m_TemporalHistoryContracts.clear();
        m_Blackboard.Reset();
        m_TransientResourceDescs.clear();
        m_TransientPlan.clear();
        m_TransientFramebufferOverrides.clear();

        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::AddPass(const Ref<RenderPass>& pass)
    {
        OLO_PROFILE_FUNCTION();

        std::string name = pass->GetName();
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Adding RenderPass to graph: {}", name);

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

    // =========================================================================
    // Phase B — Typed import / resolve / extract
    // =========================================================================

    RGTextureHandle RenderGraph::AllocateTextureHandle(std::string_view name, u32 textureID, bool isHistory)
    {
        RGTextureHandle handle;

        // Prefer stable handle reuse by name so imported texture resources keep
        // a deterministic slot across frames (matching framebuffer behavior).
        // Without this, repeated ImportTexture(name, ...) churns slots and can
        // leave stale handle copies around during graph rebuild transitions.
        if (const auto existingIt = m_TextureHandlesByName.find(std::string(name));
            existingIt != m_TextureHandlesByName.end() &&
            existingIt->second.Index < m_TextureHandleSlots.size())
        {
            handle.Index = existingIt->second.Index;
            auto& slot = m_TextureHandleSlots[handle.Index];
            if (slot.Generation == 0)
                slot.Generation = 1;
            ++slot.Generation;
            slot.Alive = true;
            slot.Name = std::string(name);
            handle.Generation = slot.Generation;

            // If this slot was previously marked free, remove it from the free
            // list before reusing it to avoid duplicate allocations of the same index.
            const auto freeIt = std::find(m_FreeTextureHandleIndices.begin(),
                                          m_FreeTextureHandleIndices.end(),
                                          handle.Index);
            if (freeIt != m_FreeTextureHandleIndices.end())
                m_FreeTextureHandleIndices.erase(freeIt);

            if (handle.Index >= m_PhysicalTextures.size())
                m_PhysicalTextures.resize(static_cast<sizet>(handle.Index) + 1u);

            auto& phys = m_PhysicalTextures[handle.Index];
            phys.TextureID = textureID;
            phys.IsHistory = isHistory;

            m_TextureHandlesByName[std::string(name)] = handle;
            return handle;
        }

        if (!m_FreeTextureHandleIndices.empty())
        {
            handle.Index = m_FreeTextureHandleIndices.back();
            m_FreeTextureHandleIndices.pop_back();
            auto& slot = m_TextureHandleSlots[handle.Index];
            slot.Alive = true;
            slot.Name = std::string(name);
            handle.Generation = slot.Generation;

            if (handle.Index >= m_PhysicalTextures.size())
                m_PhysicalTextures.resize(static_cast<sizet>(handle.Index) + 1u);

            auto& phys = m_PhysicalTextures[handle.Index];
            phys.TextureID = textureID;
            phys.IsHistory = isHistory;
        }
        else
        {
            handle.Index = static_cast<u32>(m_TextureHandleSlots.size());
            handle.Generation = 1;

            HandleSlot slot;
            slot.Generation = 1;
            slot.Alive = true;
            slot.Name = std::string(name);
            m_TextureHandleSlots.push_back(std::move(slot));

            PhysicalTexture phys;
            phys.TextureID = textureID;
            phys.IsHistory = isHistory;
            m_PhysicalTextures.push_back(std::move(phys));
        }

        m_TextureHandlesByName[std::string(name)] = handle;
        return handle;
    }

    RGFramebufferHandle RenderGraph::AllocateFramebufferHandle(std::string_view name, const Ref<Framebuffer>& fb)
    {
        RGFramebufferHandle handle;

        // PREFERRED PATH: stable handle reuse by name. If we already have a
        // slot bound to this name (regardless of Alive flag), reuse the same
        // slot index. This keeps imported framebuffer handles stable across
        // frames — without this, every Import* call creates a brand-new slot
        // (because ClearImportedResources only clears the descriptor map),
        // leaking thousands of slots and causing stale-handle ghost
        // rendering when consumers cache handles across frames.
        if (const auto existingIt = m_FramebufferHandlesByName.find(std::string(name));
            existingIt != m_FramebufferHandlesByName.end() &&
            existingIt->second.Index < m_FramebufferHandleSlots.size())
        {
            handle.Index = existingIt->second.Index;
            auto& slot = m_FramebufferHandleSlots[handle.Index];
            // Bump generation so any handle copies held by callers from a
            // previous frame won't accidentally resolve via the old gen — they
            // just become invalid until the caller re-imports.
            ++slot.Generation;
            slot.Alive = true;
            slot.Name = std::string(name);
            handle.Generation = slot.Generation;

            const auto freeIt = std::find(m_FreeFramebufferHandleIndices.begin(),
                                          m_FreeFramebufferHandleIndices.end(),
                                          handle.Index);
            if (freeIt != m_FreeFramebufferHandleIndices.end())
                m_FreeFramebufferHandleIndices.erase(freeIt);

            if (handle.Index >= m_PhysicalFramebuffers.size())
                m_PhysicalFramebuffers.resize(static_cast<sizet>(handle.Index) + 1u);

            m_PhysicalFramebuffers[handle.Index].FB = fb;
            m_FramebufferHandlesByName[std::string(name)] = handle;
            return handle;
        }

        if (!m_FreeFramebufferHandleIndices.empty())
        {
            handle.Index = m_FreeFramebufferHandleIndices.back();
            m_FreeFramebufferHandleIndices.pop_back();
            auto& slot = m_FramebufferHandleSlots[handle.Index];
            slot.Alive = true;
            slot.Name = std::string(name);
            handle.Generation = slot.Generation;

            if (handle.Index >= m_PhysicalFramebuffers.size())
                m_PhysicalFramebuffers.resize(static_cast<sizet>(handle.Index) + 1u);

            m_PhysicalFramebuffers[handle.Index].FB = fb;
        }
        else
        {
            // CRITICAL: keep slots & physicals in lockstep. If a prior
            // operation cleared one but not the other, push_back here
            // would leave handle.Index OOB on the smaller vector.
            const sizet slotsBefore = m_FramebufferHandleSlots.size();
            const sizet physBefore = m_PhysicalFramebuffers.size();
            if (slotsBefore != physBefore)
            {
                OLO_CORE_ERROR("RG-FB-WRITE [SIZE MISMATCH BEFORE PUSH]: slots.size={} physicals.size={} freeFB.size={} — vectors out of sync!",
                               slotsBefore, physBefore, m_FreeFramebufferHandleIndices.size());
                // Re-sync to keep things from crashing.
                const sizet target = std::max(slotsBefore, physBefore);
                m_FramebufferHandleSlots.resize(target);
                m_PhysicalFramebuffers.resize(target);
            }

            handle.Index = static_cast<u32>(m_FramebufferHandleSlots.size());
            handle.Generation = 1;

            HandleSlot slot;
            slot.Generation = 1;
            slot.Alive = true;
            slot.Name = std::string(name);
            m_FramebufferHandleSlots.push_back(std::move(slot));

            PhysicalFramebuffer phys;
            phys.FB = fb;
            m_PhysicalFramebuffers.push_back(std::move(phys));
        }

        m_FramebufferHandlesByName[std::string(name)] = handle;
        return handle;
    }

    RGBufferHandle RenderGraph::AllocateBufferHandle(std::string_view name, u32 bufferID)
    {
        RGBufferHandle handle;

        // Stable handle reuse by name, mirroring textures/framebuffers.
        if (const auto existingIt = m_BufferHandlesByName.find(std::string(name));
            existingIt != m_BufferHandlesByName.end() &&
            existingIt->second.Index < m_BufferHandleSlots.size())
        {
            handle.Index = existingIt->second.Index;
            auto& slot = m_BufferHandleSlots[handle.Index];
            if (slot.Generation == 0)
                slot.Generation = 1;
            ++slot.Generation;
            slot.Alive = true;
            slot.Name = std::string(name);
            handle.Generation = slot.Generation;

            const auto freeIt = std::find(m_FreeBufferHandleIndices.begin(),
                                          m_FreeBufferHandleIndices.end(),
                                          handle.Index);
            if (freeIt != m_FreeBufferHandleIndices.end())
                m_FreeBufferHandleIndices.erase(freeIt);

            if (handle.Index >= m_PhysicalBuffers.size())
                m_PhysicalBuffers.resize(static_cast<sizet>(handle.Index) + 1u);

            m_PhysicalBuffers[handle.Index].BufferID = bufferID;
            m_BufferHandlesByName[std::string(name)] = handle;
            return handle;
        }

        if (!m_FreeBufferHandleIndices.empty())
        {
            handle.Index = m_FreeBufferHandleIndices.back();
            m_FreeBufferHandleIndices.pop_back();
            auto& slot = m_BufferHandleSlots[handle.Index];
            slot.Alive = true;
            slot.Name = std::string(name);
            handle.Generation = slot.Generation;

            if (handle.Index >= m_PhysicalBuffers.size())
                m_PhysicalBuffers.resize(static_cast<sizet>(handle.Index) + 1u);

            m_PhysicalBuffers[handle.Index].BufferID = bufferID;
        }
        else
        {
            handle.Index = static_cast<u32>(m_BufferHandleSlots.size());
            handle.Generation = 1;

            HandleSlot slot;
            slot.Generation = 1;
            slot.Alive = true;
            slot.Name = std::string(name);
            m_BufferHandleSlots.push_back(std::move(slot));

            PhysicalBuffer phys;
            phys.BufferID = bufferID;
            m_PhysicalBuffers.push_back(std::move(phys));
        }

        m_BufferHandlesByName[std::string(name)] = handle;
        return handle;
    }

    RGTextureHandle RenderGraph::ImportTexture(std::string_view name, u32 textureID,
                                               const RGResourceDesc& desc)
    {
        RGResourceDesc importDesc = desc;
        importDesc.Imported = true;
        if (importDesc.Kind == ResourceHandle::Kind::Unknown)
            importDesc.Kind = ResourceHandle::Kind::Texture2D;
        if (importDesc.DebugName.empty())
            importDesc.DebugName = std::string(name);

        m_ImportedResources[std::string(name)] = importDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, textureID, false);
    }

    RGFramebufferHandle RenderGraph::ImportFramebuffer(std::string_view name,
                                                       const Ref<Framebuffer>& fb,
                                                       const RGResourceDesc& desc)
    {
        RGResourceDesc importDesc = desc;
        importDesc.Imported = true;
        if (importDesc.Kind == ResourceHandle::Kind::Unknown)
            importDesc.Kind = ResourceHandle::Kind::Framebuffer;
        if (importDesc.DebugName.empty())
            importDesc.DebugName = std::string(name);

        m_ImportedResources[std::string(name)] = importDesc;
        m_ResourceRegistryDirty = true;

        return AllocateFramebufferHandle(name, fb);
    }

    RGBufferHandle RenderGraph::ImportBuffer(std::string_view name, u32 bufferID,
                                             const RGResourceDesc& desc)
    {
        RGResourceDesc importDesc = desc;
        importDesc.Imported = true;
        if (importDesc.Kind == ResourceHandle::Kind::Unknown)
            importDesc.Kind = ResourceHandle::Kind::UniformBuffer;
        if (importDesc.DebugName.empty())
            importDesc.DebugName = std::string(name);

        m_ImportedResources[std::string(name)] = importDesc;
        m_ResourceRegistryDirty = true;

        return AllocateBufferHandle(name, bufferID);
    }

    RGTextureHandle RenderGraph::ImportHistory(std::string_view name, u32 textureID,
                                               const RGResourceDesc& desc)
    {
        if (textureID == 0)
            return {};

        RGResourceDesc importDesc = desc;
        importDesc.Imported = true;
        if (importDesc.Kind == ResourceHandle::Kind::Unknown)
            importDesc.Kind = ResourceHandle::Kind::Texture2D;
        if (importDesc.DebugName.empty())
            importDesc.DebugName = std::string(name);

        m_ImportedResources[std::string(name)] = importDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, textureID, /*isHistory=*/true);
    }

    u32 RenderGraph::ResolveTexture(RGTextureHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_PhysicalTextures.size())
            return 0;
        if (handle.Index >= m_TextureHandleSlots.size())
            return 0;
        const auto& slot = m_TextureHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return 0;

        if (const auto descIt = m_ImportedResources.find(slot.Name); descIt != m_ImportedResources.end())
        {
            const auto& desc = descIt->second;
            if (desc.IsPlaceholder && !m_PlaceholderResolveWarningsThisFrame.contains(slot.Name))
            {
                OLO_CORE_WARN("RenderGraph: resolving placeholder texture resource '{}' (reason: {})",
                              slot.Name,
                              desc.PlaceholderReason.empty() ? "unspecified" : desc.PlaceholderReason);
                m_PlaceholderResolveWarningsThisFrame.insert(slot.Name);
            }
        }

        return m_PhysicalTextures[handle.Index].TextureID;
    }

    Ref<Framebuffer> RenderGraph::ResolveFramebuffer(RGFramebufferHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_PhysicalFramebuffers.size())
            return nullptr;
        if (handle.Index >= m_FramebufferHandleSlots.size())
            return nullptr;
        const auto& slot = m_FramebufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return nullptr;

        if (const auto descIt = m_ImportedResources.find(slot.Name); descIt != m_ImportedResources.end())
        {
            const auto& desc = descIt->second;
            if (desc.IsPlaceholder && !m_PlaceholderResolveWarningsThisFrame.contains(slot.Name))
            {
                OLO_CORE_WARN("RenderGraph: resolving placeholder framebuffer resource '{}' (reason: {})",
                              slot.Name,
                              desc.PlaceholderReason.empty() ? "unspecified" : desc.PlaceholderReason);
                m_PlaceholderResolveWarningsThisFrame.insert(slot.Name);
            }
        }

        return m_PhysicalFramebuffers[handle.Index].FB;
    }

    u32 RenderGraph::ResolveBuffer(RGBufferHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_PhysicalBuffers.size())
            return 0;
        if (handle.Index >= m_BufferHandleSlots.size())
            return 0;
        const auto& slot = m_BufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return 0;

        if (const auto descIt = m_ImportedResources.find(slot.Name); descIt != m_ImportedResources.end())
        {
            const auto& desc = descIt->second;
            if (desc.IsPlaceholder && !m_PlaceholderResolveWarningsThisFrame.contains(slot.Name))
            {
                OLO_CORE_WARN("RenderGraph: resolving placeholder buffer resource '{}' (reason: {})",
                              slot.Name,
                              desc.PlaceholderReason.empty() ? "unspecified" : desc.PlaceholderReason);
                m_PlaceholderResolveWarningsThisFrame.insert(slot.Name);
            }
        }

        return m_PhysicalBuffers[handle.Index].BufferID;
    }

    std::string_view RenderGraph::GetResourceName(RGTextureHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_TextureHandleSlots.size())
            return {};

        const auto& slot = m_TextureHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return {};

        return slot.Name;
    }

    std::string_view RenderGraph::GetResourceName(RGFramebufferHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_FramebufferHandleSlots.size())
            return {};

        const auto& slot = m_FramebufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return {};

        return slot.Name;
    }

    std::string_view RenderGraph::GetResourceName(RGBufferHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_BufferHandleSlots.size())
            return {};

        const auto& slot = m_BufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return {};

        return slot.Name;
    }

    void RenderGraph::ExtractTexture(RGTextureHandle handle, std::function<void(u32)> callback)
    {
        if (!handle.IsValid() || !callback)
            return;
        m_TextureExtracts.push_back({ handle, std::move(callback) });
    }

    void RenderGraph::ExtractHistoryTexture(std::string_view historyResource,
                                            RGTextureHandle sourceHandle,
                                            std::function<void(u32)> callback)
    {
        if (!sourceHandle.IsValid() || !callback || historyResource.empty())
            return;

        const auto sourceResource = std::string(GetResourceName(sourceHandle));
        m_TemporalHistoryContracts.push_back(TemporalHistoryContract{
            .HistoryResource = std::string(historyResource),
            .SourceResource = sourceResource,
            .HistoryImported = IsHistoryTextureResource(historyResource),
            .SourceReachable = IsResourceReachableForExtraction(sourceResource),
        });
        m_HistoryTextureExtracts.push_back(HistoryTextureExtract{
            .HistoryResource = std::string(historyResource),
            .SourceHandle = sourceHandle,
            .Callback = std::move(callback),
        });
    }

    void RenderGraph::ExtractFramebuffer(RGFramebufferHandle handle,
                                         std::function<void(Ref<Framebuffer>)> callback)
    {
        if (!handle.IsValid() || !callback)
            return;
        m_FramebufferExtracts.push_back({ handle, std::move(callback) });
    }

    void RenderGraph::FlushExtractions()
    {
        OLO_PROFILE_FUNCTION();

        auto diagnoseExtractionResource = [this](std::string_view resourceName, std::string_view passName) -> bool
        {
            if (resourceName.empty())
            {
                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                    .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                    .PassName = std::string(passName),
                    .Resource = {},
                    .Message = "Extraction requested with stale or invalid handle",
                });
                return false;
            }

            if (IsResourceReachableForExtraction(resourceName))
                return true;

            m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                .Kind = BarrierDiagnosticKind::ExtractionOfCulledResource,
                .PassName = std::string(passName),
                .Resource = std::string(resourceName),
                .Message = "Extraction requested for resource produced only by culled/unreachable passes",
            });
            return false;
        };

        for (const auto& extract : m_TextureExtracts)
        {
            if (!IsTextureHandleCurrent(extract.Handle))
            {
                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                    .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                    .PassName = "<extract-texture>",
                    .Resource = {},
                    .Message = "Extraction requested with stale or invalid texture handle",
                });
                continue;
            }

            const auto resourceName = GetResourceName(extract.Handle);
            if (!diagnoseExtractionResource(resourceName, "<extract-texture>"))
                continue;
            extract.Callback(ResolveTexture(extract.Handle));
        }

        for (const auto& extract : m_HistoryTextureExtracts)
        {
            if (!IsTextureHandleCurrent(extract.SourceHandle))
            {
                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                    .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                    .PassName = "<extract-history>",
                    .Resource = {},
                    .Message = "History extraction requested with stale or invalid source handle",
                });
                continue;
            }

            const auto sourceResource = GetResourceName(extract.SourceHandle);
            if (!diagnoseExtractionResource(sourceResource, "<extract-history>"))
                continue;
            if (!IsHistoryTextureResource(extract.HistoryResource))
            {
                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                    .Kind = BarrierDiagnosticKind::InvalidHistoryContract,
                    .PassName = "<extract-history>",
                    .Resource = extract.HistoryResource,
                    .Message = "History extraction requested for resource that was not imported via ImportHistory",
                });
                continue;
            }

            extract.Callback(ResolveTexture(extract.SourceHandle));
        }

        for (const auto& extract : m_FramebufferExtracts)
        {
            if (!IsFramebufferHandleCurrent(extract.Handle))
            {
                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                    .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                    .PassName = "<extract-framebuffer>",
                    .Resource = {},
                    .Message = "Extraction requested with stale or invalid framebuffer handle",
                });
                continue;
            }

            const auto resourceName = GetResourceName(extract.Handle);
            if (!diagnoseExtractionResource(resourceName, "<extract-framebuffer>"))
                continue;
            extract.Callback(ResolveFramebuffer(extract.Handle));
        }

        m_TextureExtracts.clear();
        m_HistoryTextureExtracts.clear();
        m_FramebufferExtracts.clear();
    }

    // -------------------------------------------------------------------
    // Phase C — Transient resource allocation (builder support)
    // -------------------------------------------------------------------

    RGTextureHandle RenderGraph::AllocateTransientTextureHandle(std::string_view name, const RGResourceDesc& desc)
    {
        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::Texture2D;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = std::string(name);

        m_TransientResourceDescs[std::string(name)] = transientDesc;
        m_ResourceRegistryDirty = true;

        if (m_FreeTextureHandleIndices.empty())
        {
            u32 index = static_cast<u32>(m_TextureHandleSlots.size());
            m_TextureHandleSlots.emplace_back();
            m_TextureHandleSlots[index].Alive = true;
            m_TextureHandleSlots[index].Name = std::string(name);
            m_PhysicalTextures.emplace_back();
            return { index, static_cast<u32>(m_TextureHandleSlots[index].Generation) };
        }

        u32 index = m_FreeTextureHandleIndices.back();
        m_FreeTextureHandleIndices.pop_back();
        auto& slot = m_TextureHandleSlots[index];
        slot.Generation++;
        slot.Alive = true;
        slot.Name = std::string(name);
        if (index >= m_PhysicalTextures.size())
            m_PhysicalTextures.resize(static_cast<sizet>(index) + 1u);
        return { index, slot.Generation };
    }

    RGFramebufferHandle RenderGraph::AllocateTransientFramebufferHandle(std::string_view name, const RGResourceDesc& desc)
    {
        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::Framebuffer;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = std::string(name);

        m_TransientResourceDescs[std::string(name)] = transientDesc;
        m_ResourceRegistryDirty = true;

        if (m_FreeFramebufferHandleIndices.empty())
        {
            u32 index = static_cast<u32>(m_FramebufferHandleSlots.size());
            m_FramebufferHandleSlots.emplace_back();
            m_FramebufferHandleSlots[index].Alive = true;
            m_FramebufferHandleSlots[index].Name = std::string(name);
            m_PhysicalFramebuffers.emplace_back();
            return { index, static_cast<u32>(m_FramebufferHandleSlots[index].Generation) };
        }

        u32 index = m_FreeFramebufferHandleIndices.back();
        m_FreeFramebufferHandleIndices.pop_back();
        auto& slot = m_FramebufferHandleSlots[index];
        slot.Generation++;
        slot.Alive = true;
        slot.Name = std::string(name);
        if (index >= m_PhysicalFramebuffers.size())
            m_PhysicalFramebuffers.resize(static_cast<sizet>(index) + 1u);
        return { index, slot.Generation };
    }

    RGFramebufferHandle RenderGraph::DeclareTransientFramebuffer(std::string_view name, const RGResourceDesc& desc,
                                                                  const Ref<Framebuffer>& ownerFB)
    {
        // Register the descriptor so EnsureResourceRegistryBuilt() creates a
        // stable typed handle.  Then return that handle.  If this is called
        // before any EnsureResourceRegistryBuilt() the AllocateTransient path
        // creates a "ghost" slot; the subsequent GetFramebufferHandle call
        // triggers EnsureResourceRegistryBuilt which promotes it to a stable
        // named slot that MaterializeTransientResources() can find.
        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::Framebuffer;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = std::string(name);

        m_TransientResourceDescs[std::string(name)] = transientDesc;
        m_ResourceRegistryDirty = true;

        // EnsureResourceRegistryBuilt is called inside GetFramebufferHandle
        // which creates and caches the stable handle in m_FramebufferHandlesByName.
        const auto handle = GetFramebufferHandle(name);

        // If the caller provided a pass-owned FB, register it as the override
        // immediately so no separate OverrideTransientFramebuffer call is needed.
        // MaterializeTransientResources() will re-apply it after pool allocation.
        if (ownerFB)
            OverrideTransientFramebuffer(name, ownerFB);

        return handle;
    }

    void RenderGraph::OverrideTransientFramebuffer(std::string_view name, const Ref<Framebuffer>& fb)
    {
        // Do not override imported resources (their physical slot is owned externally).
        if (m_ImportedResources.contains(std::string(name)))
            return;

        // Persist override so it can be re-applied after transient materialization.
        m_TransientFramebufferOverrides[std::string(name)] = fb;

        if (const auto it = m_FramebufferHandlesByName.find(std::string(name));
            it != m_FramebufferHandlesByName.end())
        {
            const auto handle = it->second;
            if (handle.IsValid() && handle.Index < m_PhysicalFramebuffers.size())
                m_PhysicalFramebuffers[handle.Index].FB = fb;
        }
    }

    RGBufferHandle RenderGraph::AllocateTransientBufferHandle(std::string_view name, const RGResourceDesc& desc)
    {
        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::StorageBuffer;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = std::string(name);

        m_TransientResourceDescs[std::string(name)] = transientDesc;
        m_ResourceRegistryDirty = true;

        if (m_FreeBufferHandleIndices.empty())
        {
            u32 index = static_cast<u32>(m_BufferHandleSlots.size());
            m_BufferHandleSlots.emplace_back();
            m_BufferHandleSlots[index].Alive = true;
            m_BufferHandleSlots[index].Name = std::string(name);
            m_PhysicalBuffers.emplace_back();
            return { index, static_cast<u32>(m_BufferHandleSlots[index].Generation) };
        }

        u32 index = m_FreeBufferHandleIndices.back();
        m_FreeBufferHandleIndices.pop_back();
        auto& slot = m_BufferHandleSlots[index];
        slot.Generation++;
        slot.Alive = true;
        slot.Name = std::string(name);
        if (index >= m_PhysicalBuffers.size())
            m_PhysicalBuffers.resize(static_cast<sizet>(index) + 1u);
        return { index, slot.Generation };
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

        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Connecting passes (ordering only): {} -> {}", outputPass, inputPass);
        AddExecutionDependency(outputPass, inputPass);
    }

    void RenderGraph::AddExecutionDependency(const std::string& beforePass, const std::string& afterPass, bool persistent)
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

        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Adding execution dependency (ordering only): {} -> {}", beforePass, afterPass);

        // Only add dependency for execution ordering, no framebuffer piping (avoid duplicates)
        auto& deps = m_Dependencies[afterPass];
        if (std::find(deps.begin(), deps.end(), beforePass) == deps.end())
        {
            deps.push_back(beforePass);
        }

        if (persistent)
        {
            auto& explicitDeps = m_ExplicitDependencies[afterPass];
            if (std::find(explicitDeps.begin(), explicitDeps.end(), beforePass) == explicitDeps.end())
            {
                explicitDeps.push_back(beforePass);
            }
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();

        m_LastPassTimings.clear();
        m_LastPassTimings.reserve(m_CachedExecutionOrder.size());
        m_FallbackActivations.clear();
        m_PlaceholderResolveWarningsThisFrame.clear();

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Cycle detected — m_PassOrder is empty. Abort execution;
                // running with a partial topo order would execute the wrong
                // subset of passes in the wrong order. Keep the dirty flag
                // set so a corrected graph can retry.
                OLO_CORE_ERROR("RenderGraph::Execute: aborting because dependency graph rebuild failed");
                m_TransientPool.ReleaseAll();
                m_TransientPool.Trim(m_TransientPoolMaxBucketSize);
                return;
            }
            ResolveFinalPass();
            RebuildExecutionCache();
            m_DependencyGraphDirty = false;

            // Phase E: Compute backward reachability from final output.
            // This determines which passes are needed for the final output.
            ComputeReachability();
            ComputeBarrierPlan();
            RebuildTransientPlan();

            // Cache the submission plan after barrier planning so
            // the execution loop below is a simple sequential walk over a pre-built
            // IR rather than repeating inline barrier-map lookups every frame.
            m_CachedSubmissionPlan = GetSubmissionPlan();
            LogSubmissionPlanIfChanged();
        }

        MaterializeTransientResources();

        // Execute passes in order (zero-overhead — no lookups, no branching)
        RGCommandContext commandContext;
        commandContext.SetRenderGraph(this);
        // Walk the pre-built submission-plan IR.
        // Each command kind maps to a distinct action; barrier placement and
        // async-compute batch boundaries are encoded in the plan so this loop
        // requires no topology lookups or per-frame map probes.
        for (const auto& cmd : m_CachedSubmissionPlan)
        {
            switch (cmd.CommandKind)
            {
                case SubmissionCommand::Kind::BatchBegin:
                {
                    commandContext.BeginAsyncBatch(cmd.BatchIndex);
                    if (m_BatchEventHook)
                        m_BatchEventHook(cmd.BatchIndex, true);
                    break;
                }
                case SubmissionCommand::Kind::BatchEnd:
                {
                    commandContext.EndAsyncBatch(cmd.BatchIndex);
                    if (m_BatchEventHook)
                        m_BatchEventHook(cmd.BatchIndex, false);
                    break;
                }
                case SubmissionCommand::Kind::MemoryBarrier:
                {
                    if (m_RuntimeBarrierExecutionEnabled)
                        commandContext.MemoryBarrier(cmd.Barriers);
                    break;
                }
                case SubmissionCommand::Kind::Pass:
                {
                    if (!IsPassReachable(cmd.PassName))
                    {
                        break;
                    }

                    const auto passIt = m_PassLookup.find(cmd.PassName);
                    if (passIt == m_PassLookup.end() || !passIt->second)
                        break;

                    auto* pass = passIt->second.Raw();
                    commandContext.BeginPass(cmd.PassName);
                    const auto executeStart = std::chrono::steady_clock::now();
                    pass->Execute(commandContext);
                    const auto executeEnd = std::chrono::steady_clock::now();
                    commandContext.EndPass();

                    const auto elapsedMs = std::chrono::duration<f64, std::milli>(executeEnd - executeStart).count();
                    m_LastPassTimings.push_back(PassTiming{
                        .PassName = cmd.PassName,
                        .CpuMs = elapsedMs,
                    });

                    // Debug post-pass hook — fires after EndPass() but before the
                    // next pass begins. Lets debug tooling snapshot intermediate
                    // resource state (see RenderGraphFrameCapture).
                    if (m_PostPassHook)
                        m_PostPassHook(cmd.PassName, *this);
                    break;
                }
            }
        }
        commandContext.SetRenderGraph(nullptr);

        // Third pass: fire extraction callbacks queued by passes during Execute().
        FlushExtractions();
        m_TransientPool.ReleaseAll();
        m_TransientPool.Trim(m_TransientPoolMaxBucketSize);
    }

    void RenderGraph::RecordFallbackActivation(const std::string_view passName, const std::string_view reason) const
    {
        if (passName.empty() || reason.empty())
            return;

        auto existing = std::find_if(m_FallbackActivations.begin(), m_FallbackActivations.end(),
                                     [passName, reason](const FallbackActivation& activation)
                                     {
                                         return activation.PassName == passName &&
                                                activation.Reason == reason;
                                     });

        if (existing != m_FallbackActivations.end())
        {
            ++existing->Count;
            return;
        }

        m_FallbackActivations.push_back(FallbackActivation{
            .PassName = std::string(passName),
            .Reason = std::string(reason),
            .Count = 1,
        });
    }

    ImageFormat RenderGraph::ToImageFormat(const RGResourceFormat format)
    {
        switch (format)
        {
            case RGResourceFormat::R8UNorm:
                return ImageFormat::R8;
            case RGResourceFormat::R32Float:
                return ImageFormat::R32F;
            case RGResourceFormat::RG16Float:
                return ImageFormat::RG16F;
            case RGResourceFormat::RGBA8UNorm:
                return ImageFormat::RGBA8;
            case RGResourceFormat::RGBA16Float:
                return ImageFormat::RGBA16F;
            case RGResourceFormat::Depth24Stencil8:
                return ImageFormat::DEPTH24STENCIL8;
            case RGResourceFormat::Depth32Float:
                return ImageFormat::R32F;
            case RGResourceFormat::Unknown:
            default:
                return ImageFormat::None;
        }
    }

    FramebufferTextureFormat RenderGraph::ToFramebufferFormat(const RGResourceFormat format)
    {
        switch (format)
        {
            case RGResourceFormat::RGBA8UNorm:
                return FramebufferTextureFormat::RGBA8;
            case RGResourceFormat::RGBA16Float:
                return FramebufferTextureFormat::RGBA16F;
            case RGResourceFormat::RG16Float:
                return FramebufferTextureFormat::RG16F;
            case RGResourceFormat::RGBA32Float:
                return FramebufferTextureFormat::RGBA32F;
            case RGResourceFormat::Depth24Stencil8:
                return FramebufferTextureFormat::DEPTH24STENCIL8;
            case RGResourceFormat::Depth32Float:
                return FramebufferTextureFormat::DEPTH_COMPONENT32F;
            case RGResourceFormat::R32Float:
            case RGResourceFormat::R8UNorm:
            case RGResourceFormat::Unknown:
            default:
                return FramebufferTextureFormat::None;
        }
    }

    void RenderGraph::MaterializeTransientResources()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_RuntimeTransientMaterializationEnabled)
            return;

        if (m_TransientPlan.empty())
            return;

        if (RendererAPI::GetAPI() == RendererAPI::API::None)
            return;

        std::unordered_map<std::string, Ref<Texture>> textureAliases;
        std::unordered_map<std::string, Ref<Framebuffer>> framebufferAliases;
        std::unordered_map<std::string, Ref<StorageBuffer>> bufferAliases;

        textureAliases.reserve(m_TransientPlan.size());
        framebufferAliases.reserve(m_TransientPlan.size());
        bufferAliases.reserve(m_TransientPlan.size());

        for (const auto& entry : m_TransientPlan)
        {
            // Guard: never let transient resource planning touch imported
            // resources. If a resource name appears in m_ImportedResources,
            // its physical slot is owned by the importer (e.g.
            // Renderer3D::SetupFrameBlackboard) and must not be reset or
            // overwritten by transient pool allocations. Without this guard
            // a name collision between a transient descriptor and an
            // imported resource silently corrupts the imported physical
            // slot — consumers reading through the blackboard handle then
            // see a blank transient FB instead of the real imported one.
            if (m_ImportedResources.contains(entry.Resource))
            {
                continue;
            }

            const auto descriptorIt = m_TransientResourceDescs.find(entry.Resource);
            if (descriptorIt == m_TransientResourceDescs.end())
                continue;

            const auto& desc = descriptorIt->second;
            if (!entry.WillAllocate)
            {
                if (const auto texIt = m_TextureHandlesByName.find(entry.Resource);
                    texIt != m_TextureHandlesByName.end() &&
                    texIt->second.Index < m_PhysicalTextures.size())
                {
                    m_PhysicalTextures[texIt->second.Index].TextureID = 0;
                }

                if (const auto fbIt = m_FramebufferHandlesByName.find(entry.Resource);
                    fbIt != m_FramebufferHandlesByName.end() &&
                    fbIt->second.Index < m_PhysicalFramebuffers.size())
                {
                    m_PhysicalFramebuffers[fbIt->second.Index].FB.Reset();
                }

                if (const auto bufferIt = m_BufferHandlesByName.find(entry.Resource);
                    bufferIt != m_BufferHandlesByName.end() &&
                    bufferIt->second.Index < m_PhysicalBuffers.size())
                {
                    m_PhysicalBuffers[bufferIt->second.Index].BufferID = 0;
                }

                continue;
            }

            const auto aliasKey = entry.AliasGroup + "#" + std::to_string(entry.AliasSlot);

            switch (desc.Kind)
            {
                case ResourceHandle::Kind::Texture2D:
                case ResourceHandle::Kind::Texture2DArray:
                case ResourceHandle::Kind::TextureCube:
                case ResourceHandle::Kind::TextureCubeArray:
                {
                    auto textureIt = textureAliases.find(aliasKey);
                    if (textureIt == textureAliases.end())
                    {
                        TextureSpecification spec;
                        spec.Width = desc.Width;
                        spec.Height = desc.Height;
                        spec.Format = ToImageFormat(desc.Format);
                        spec.GenerateMips = desc.MipLevels > 1;
                        spec.MipLevels = desc.MipLevels;

                        auto transientTexture = m_TransientPool.AcquireTexture(spec);
                        textureIt = textureAliases.emplace(aliasKey, transientTexture).first;
                    }

                    if (const auto texHandleIt = m_TextureHandlesByName.find(entry.Resource);
                        texHandleIt != m_TextureHandlesByName.end() &&
                        texHandleIt->second.Index < m_PhysicalTextures.size())
                    {
                        m_PhysicalTextures[texHandleIt->second.Index].TextureID = textureIt->second ? textureIt->second->GetRendererID() : 0;
                    }
                    break;
                }
                case ResourceHandle::Kind::Framebuffer:
                {
                    auto framebufferIt = framebufferAliases.find(aliasKey);
                    if (framebufferIt == framebufferAliases.end())
                    {
                        FramebufferSpecification spec;
                        spec.Width = desc.Width;
                        spec.Height = desc.Height;
                        spec.Samples = std::max(desc.Samples, 1u);
                        spec.SwapChainTarget = false;

                        if (!desc.Attachments.empty())
                        {
                            // MRT path: build one attachment spec per entry in Attachments.
                            std::vector<FramebufferTextureSpecification> attachSpecs;
                            attachSpecs.reserve(desc.Attachments.size());
                            for (const auto fmt : desc.Attachments)
                            {
                                const auto af = ToFramebufferFormat(fmt);
                                if (af != FramebufferTextureFormat::None)
                                    attachSpecs.push_back(FramebufferTextureSpecification{ af });
                            }
                            if (!attachSpecs.empty())
                            {
                                spec.Attachments.Attachments = std::move(attachSpecs);
                            }
                        }
                        else
                        {
                            // Single-attachment path (existing behaviour).
                            const auto attachmentFormat = ToFramebufferFormat(desc.Format);
                            if (attachmentFormat != FramebufferTextureFormat::None)
                            {
                                spec.Attachments = FramebufferAttachmentSpecification{
                                    FramebufferTextureSpecification{ attachmentFormat }
                                };
                            }
                        }

                        auto transientFramebuffer = m_TransientPool.AcquireFramebuffer(spec);
                        framebufferIt = framebufferAliases.emplace(aliasKey, transientFramebuffer).first;
                    }

                    if (const auto fbHandleIt = m_FramebufferHandlesByName.find(entry.Resource);
                        fbHandleIt != m_FramebufferHandlesByName.end() &&
                        fbHandleIt->second.Index < m_PhysicalFramebuffers.size())
                    {
                        m_PhysicalFramebuffers[fbHandleIt->second.Index].FB = framebufferIt->second;
                    }
                    break;
                }
                case ResourceHandle::Kind::UniformBuffer:
                case ResourceHandle::Kind::StorageBuffer:
                {
                    auto bufferIt = bufferAliases.find(aliasKey);
                    if (bufferIt == bufferAliases.end())
                    {
                        auto transientBuffer = m_TransientPool.AcquireBuffer(desc.Width);
                        bufferIt = bufferAliases.emplace(aliasKey, transientBuffer).first;
                    }

                    if (const auto bufferHandleIt = m_BufferHandlesByName.find(entry.Resource);
                        bufferHandleIt != m_BufferHandlesByName.end() &&
                        bufferHandleIt->second.Index < m_PhysicalBuffers.size())
                    {
                        m_PhysicalBuffers[bufferHandleIt->second.Index].BufferID = bufferIt->second ? bufferIt->second->GetRendererID() : 0;
                    }
                    break;
                }
                case ResourceHandle::Kind::Unknown:
                default:
                    break;
            }
        }

        // Re-apply pass-owned framebuffer overrides after transient pool
        // materialization. This keeps transient handles like AOApplyColor /
        // BloomColor wired to their real pass targets instead of pool-allocated
        // alias framebuffers.
        for (const auto& [name, fb] : m_TransientFramebufferOverrides)
        {
            if (const auto fbHandleIt = m_FramebufferHandlesByName.find(name);
                fbHandleIt != m_FramebufferHandlesByName.end() &&
                fbHandleIt->second.Index < m_PhysicalFramebuffers.size())
            {
                m_PhysicalFramebuffers[fbHandleIt->second.Index].FB = fb;
            }
        }
    }

    void RenderGraph::RebuildExecutionCache()
    {
        OLO_PROFILE_FUNCTION();

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

        if (IsRenderGraphDiagnosticsEnabled())
        {
            OLO_CORE_TRACE("RenderGraph: Execution cache rebuilt - {} passes",
                           m_CachedExecutionOrder.size());
        }
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
        m_HasExplicitFinalPass = true;
        m_DependencyGraphDirty = true;
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
                .WorkType = pass->GetPassWorkType(),
                .AsyncComputeCandidate = pass->IsAsyncComputeCandidate(),
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

        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("RenderGraph execution order updated with {} passes", m_PassOrder.size());

        // Hoist independent AsyncComputeCandidate passes before graphics.
        HoistComputePasses();

        return true;
    }

    void RenderGraph::HoistComputePasses()
    {
        OLO_PROFILE_FUNCTION();

        // Fast-path: skip the reorder when no async-compute candidate exists.
        bool hasCandidate = false;
        for (const auto& name : m_PassOrder)
        {
            auto it = m_PassLookup.find(name);
            if (it != m_PassLookup.end() && it->second && it->second->IsAsyncComputeCandidate())
            {
                hasCandidate = true;
                break;
            }
        }
        if (!hasCandidate)
            return;

        // Modified Kahn's algorithm over m_PassOrder:
        //   when multiple passes are simultaneously ready (in-degree == 0),
        //   all AsyncComputeCandidate passes are drained before any graphics
        //   pass advances.  The result is still a valid topological order.
        std::unordered_set<std::string_view> passSet;
        passSet.reserve(m_PassOrder.size());
        for (const auto& name : m_PassOrder)
            passSet.insert(name);

        std::unordered_map<std::string, u32> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> successors;
        inDegree.reserve(m_PassOrder.size());

        for (const auto& name : m_PassOrder)
        {
            inDegree.emplace(name, 0u);
            auto it = m_Dependencies.find(name);
            if (it == m_Dependencies.end())
                continue;
            for (const auto& dep : it->second)
            {
                if (!passSet.contains(dep))
                    continue;
                successors[dep].push_back(name);
                ++inDegree[name];
            }
        }

        // Augment the local inDegree / successors with resource-access-derived
        // edges so that the hoist does not move a compute pass before a pass
        // that produces one of its inputs.  This is necessary because derived
        // edges are not always present in m_Dependencies at the time this
        // function is called (e.g. from ValidateResourceHazards before the
        // first BuildFrameGraph run has added them).
        {
            // Build lastWriter from both legacy DeclareWrite and graph-native
            // access declarations (m_PassAccessDeclarations).
            std::unordered_map<std::string, std::string> lastWriter;
            lastWriter.reserve(m_PassOrder.size() * 4u);

            for (const auto& prodName : m_PassOrder)
            {
                // Legacy DeclareWrite declarations
                if (const auto pIt = m_PassLookup.find(prodName);
                    pIt != m_PassLookup.end() && pIt->second)
                {
                    for (const auto& w : pIt->second->GetWrites())
                        lastWriter[w.Name] = prodName;
                }
                // Graph-native builder.Write() access declarations
                if (const auto aIt = m_PassAccessDeclarations.find(prodName);
                    aIt != m_PassAccessDeclarations.end())
                {
                    for (const auto& access : aIt->second)
                    {
                        if (access.IsWrite && !access.ResourceName.empty())
                            lastWriter[access.ResourceName] = prodName;
                    }
                }
            }

            // For each pass, add an implicit producer→consumer edge for every
            // resource it reads whose last writer is a different pass.
            for (const auto& consName : m_PassOrder)
            {
                auto addImplicitEdge = [&](const std::string& producer)
                {
                    if (producer == consName || !passSet.contains(producer))
                        return;
                    // Skip if the edge is already present to avoid double-counting.
                    auto& succVec = successors[producer];
                    if (std::find(succVec.begin(), succVec.end(), consName) != succVec.end())
                        return;
                    succVec.push_back(consName);
                    ++inDegree[consName];
                };

                // Legacy DeclareRead declarations
                if (const auto pIt = m_PassLookup.find(consName);
                    pIt != m_PassLookup.end() && pIt->second)
                {
                    for (const auto& r : pIt->second->GetReads())
                    {
                        if (const auto wIt = lastWriter.find(r.Name); wIt != lastWriter.end())
                            addImplicitEdge(wIt->second);
                    }
                }
                // Graph-native builder.Read() access declarations
                if (const auto aIt = m_PassAccessDeclarations.find(consName);
                    aIt != m_PassAccessDeclarations.end())
                {
                    for (const auto& access : aIt->second)
                    {
                        if (!access.IsWrite && !access.ResourceName.empty())
                        {
                            if (const auto wIt = lastWriter.find(access.ResourceName);
                                wIt != lastWriter.end())
                                addImplicitEdge(wIt->second);
                        }
                    }
                }
            }
        }

        std::deque<std::string> computeReady;
        std::deque<std::string> graphicsReady;

        auto classify = [&](const std::string& passName)
        {
            auto it = m_PassLookup.find(passName);
            if (it != m_PassLookup.end() && it->second && it->second->IsAsyncComputeCandidate())
                computeReady.push_back(passName);
            else
                graphicsReady.push_back(passName);
        };

        for (const auto& name : m_PassOrder)
        {
            if (inDegree[name] == 0)
                classify(name);
        }

        std::vector<std::string> reordered;
        reordered.reserve(m_PassOrder.size());

        while (!computeReady.empty() || !graphicsReady.empty())
        {
            // Drain all ready compute passes before advancing any graphics pass.
            while (!computeReady.empty())
            {
                std::string name = std::move(computeReady.front());
                computeReady.pop_front();
                reordered.push_back(name);

                auto succIt = successors.find(name);
                if (succIt != successors.end())
                {
                    for (const auto& succ : succIt->second)
                    {
                        if (--inDegree[succ] == 0)
                            classify(succ);
                    }
                }
            }

            if (!graphicsReady.empty())
            {
                std::string name = std::move(graphicsReady.front());
                graphicsReady.pop_front();
                reordered.push_back(name);

                auto succIt = successors.find(name);
                if (succIt != successors.end())
                {
                    for (const auto& succ : succIt->second)
                    {
                        if (--inDegree[succ] == 0)
                            classify(succ);
                    }
                }
            }
        }

        // Commit the reordered sequence only when it's complete.
        // Defensive guard: the DFS above already validated no cycles, but
        // be conservative in case of any unforeseen edge case.
        if (reordered.size() == m_PassOrder.size())
        {
            m_PassOrder = std::move(reordered);
            if (IsRenderGraphDiagnosticsEnabled())
                OLO_CORE_TRACE("RenderGraph: Compute-hoist applied to execution order");
        }
    }

    // Async-compute batch query.
    // Partitions the hoisted execution order into contiguous runs of
    // AsyncComputeCandidate passes.  Each batch records the non-batch passes
    // it must wait for (WaitPasses) and the non-batch passes that must wait
    // for it (SignalPasses) — the fence-sync metadata needed by explicit-
    // barrier backends (Vulkan semaphores, DX12 fence Signal/Wait).
    std::vector<RenderGraph::AsyncComputeBatch> RenderGraph::GetAsyncComputeBatches() const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<AsyncComputeBatch> batches;

        // --- Step 1: group consecutive AsyncComputeCandidate passes ---
        AsyncComputeBatch current;
        for (const auto& passName : m_PassOrder)
        {
            const auto it = m_PassLookup.find(passName);
            const bool isCandidate = (it != m_PassLookup.end()) && it->second &&
                                     it->second->IsAsyncComputeCandidate();

            if (isCandidate)
            {
                current.ComputePasses.push_back(passName);
            }
            else
            {
                if (!current.ComputePasses.empty())
                {
                    batches.push_back(std::move(current));
                    current = {};
                }
            }
        }
        if (!current.ComputePasses.empty())
            batches.push_back(std::move(current));

        if (batches.empty())
            return batches;

        // --- Step 2: build successor map (A → passes that depend on A) ---
        std::unordered_set<std::string_view> passSet;
        passSet.reserve(m_PassOrder.size());
        for (const auto& name : m_PassOrder)
            passSet.insert(name);

        std::unordered_map<std::string, std::vector<std::string>> successors;
        successors.reserve(m_PassOrder.size());
        for (const auto& name : m_PassOrder)
        {
            const auto depIt = m_Dependencies.find(name);
            if (depIt == m_Dependencies.end())
                continue;
            for (const auto& dep : depIt->second)
            {
                if (passSet.contains(dep))
                    successors[dep].push_back(name);
            }
        }

        // --- Step 3: fill WaitPasses / SignalPasses for each batch ---
        for (auto& batch : batches)
        {
            std::unordered_set<std::string> batchSet(batch.ComputePasses.begin(),
                                                     batch.ComputePasses.end());
            std::unordered_set<std::string> waitSet;
            std::unordered_set<std::string> signalSet;

            for (const auto& computePass : batch.ComputePasses)
            {
                // WaitPasses: direct predecessors not in this batch
                if (const auto depIt = m_Dependencies.find(computePass);
                    depIt != m_Dependencies.end())
                {
                    for (const auto& dep : depIt->second)
                    {
                        if (!batchSet.contains(dep) && passSet.contains(dep))
                            waitSet.insert(dep);
                    }
                }

                // SignalPasses: direct successors not in this batch
                if (const auto sucIt = successors.find(computePass); sucIt != successors.end())
                {
                    for (const auto& succ : sucIt->second)
                    {
                        if (!batchSet.contains(succ))
                            signalSet.insert(succ);
                    }
                }
            }

            batch.WaitPasses = std::vector<std::string>(waitSet.begin(), waitSet.end());
            batch.SignalPasses = std::vector<std::string>(signalSet.begin(), signalSet.end());
        }

        // --- Step 4: fill InputResources / OutputResources for each batch ---
        // Build a pass-order index for fast boundary detection.
        std::unordered_map<std::string, sizet> passOrderIndex;
        passOrderIndex.reserve(m_PassOrder.size());
        for (sizet i = 0; i < m_PassOrder.size(); ++i)
            passOrderIndex[m_PassOrder[i]] = i;

        for (auto& batch : batches)
        {
            const std::unordered_set<std::string> batchSet(batch.ComputePasses.begin(),
                                                           batch.ComputePasses.end());

            // Find the inclusive pass-order range [batchStart, batchEnd].
            sizet batchStart = m_PassOrder.size();
            sizet batchEnd = 0;
            for (const auto& cp : batch.ComputePasses)
            {
                if (const auto idxIt = passOrderIndex.find(cp); idxIt != passOrderIndex.end())
                {
                    batchStart = std::min(batchStart, idxIt->second);
                    batchEnd = std::max(batchEnd, idxIt->second);
                }
            }
            if (batchStart > batchEnd)
                continue;

            // Collect all resources read / written by batch passes.
            std::unordered_set<std::string> batchReadResources;
            std::unordered_set<std::string> batchWrittenResources;
            for (const auto& cp : batch.ComputePasses)
            {
                if (const auto accessIt = m_PassAccessDeclarations.find(cp);
                    accessIt != m_PassAccessDeclarations.end())
                {
                    for (const auto& acc : accessIt->second)
                    {
                        if (acc.IsWrite)
                            batchWrittenResources.insert(acc.ResourceName);
                        else
                            batchReadResources.insert(acc.ResourceName);
                    }
                }
            }

            // InputResources: for each resource read by the batch, scan passes
            // *before* batchStart to find the last external writer.
            std::unordered_map<std::string, std::string> inputByResource;
            for (sizet i = 0; i < batchStart; ++i)
            {
                const auto& passName = m_PassOrder[i];
                if (batchSet.contains(passName))
                    continue;
                if (const auto accessIt = m_PassAccessDeclarations.find(passName);
                    accessIt != m_PassAccessDeclarations.end())
                {
                    for (const auto& acc : accessIt->second)
                    {
                        if (acc.IsWrite && batchReadResources.contains(acc.ResourceName))
                            inputByResource[acc.ResourceName] = passName; // last writer wins
                    }
                }
            }

            // OutputResources: for each resource written by the batch, scan passes
            // *after* batchEnd to find the first external reader.
            std::unordered_map<std::string, std::string> outputByResource;
            for (sizet i = batchEnd + 1; i < m_PassOrder.size(); ++i)
            {
                const auto& passName = m_PassOrder[i];
                if (batchSet.contains(passName))
                    continue;
                if (const auto accessIt = m_PassAccessDeclarations.find(passName);
                    accessIt != m_PassAccessDeclarations.end())
                {
                    for (const auto& acc : accessIt->second)
                    {
                        if (!acc.IsWrite && batchWrittenResources.contains(acc.ResourceName) &&
                            !outputByResource.contains(acc.ResourceName))
                        {
                            outputByResource[acc.ResourceName] = passName; // first reader wins
                        }
                    }
                }
            }

            // Convert to sorted vectors for deterministic ordering.
            batch.InputResources.reserve(inputByResource.size());
            for (auto& [res, externalPass] : inputByResource)
                batch.InputResources.push_back({ res, externalPass });
            std::sort(batch.InputResources.begin(), batch.InputResources.end(),
                      [](const BatchResourceDependency& a, const BatchResourceDependency& b)
                      { return a.ResourceName < b.ResourceName; });

            batch.OutputResources.reserve(outputByResource.size());
            for (auto& [res, externalPass] : outputByResource)
                batch.OutputResources.push_back({ res, externalPass });
            std::sort(batch.OutputResources.begin(), batch.OutputResources.end(),
                      [](const BatchResourceDependency& a, const BatchResourceDependency& b)
                      { return a.ResourceName < b.ResourceName; });
        }

        return batches;
    }

    // Submission-plan IR.
    // Merges the hoisted execution order, the barrier plan (Phase E), and the
    // async-compute batch boundaries into a single linearised
    // command stream that a backend can replay without touching the graph.
    std::vector<RenderGraph::SubmissionCommand> RenderGraph::GetSubmissionPlan() const
    {
        OLO_PROFILE_FUNCTION();

        const auto mapWorkTypeToLane = [](const RenderPass::PassWorkType workType)
        {
            switch (workType)
            {
                case RenderPass::PassWorkType::Compute:
                    return QueueLane::Compute;
                case RenderPass::PassWorkType::Copy:
                    return QueueLane::Copy;
                case RenderPass::PassWorkType::Graphics:
                default:
                    return QueueLane::Graphics;
            }
        };

        std::vector<SubmissionCommand> plan;
        plan.reserve(m_PassOrder.size() * 2); // rough upper bound

        // Build a set of passes that are members of some async batch so we can
        // quickly look up which batch (if any) a pass belongs to.
        const auto batches = GetAsyncComputeBatches();

        std::unordered_map<u32, const AsyncComputeBatch*> batchByIndex;
        batchByIndex.reserve(batches.size());
        for (u32 batchIdx = 0; batchIdx < static_cast<u32>(batches.size()); ++batchIdx)
            batchByIndex.emplace(batchIdx, &batches[batchIdx]);

        // Map: passName → batch index (only for passes inside a batch)
        std::unordered_map<std::string, u32> passToBatch;
        for (u32 batchIdx = 0; batchIdx < static_cast<u32>(batches.size()); ++batchIdx)
        {
            for (const auto& passName : batches[batchIdx].ComputePasses)
                passToBatch.emplace(passName, batchIdx);
        }

        // Build a map: passName → barrier flags (from Phase E barrier plan)
        std::unordered_map<std::string, MemoryBarrierFlags> barrierForPass;
        for (const auto& planned : m_PlannedBarriers)
        {
            // Barriers are keyed on the pass AFTER which they should fire — i.e.
            // the consumer pass that triggered them. Insert before that pass.
            auto& flags = barrierForPass[planned.BeforePass];
            flags = flags | planned.Flags;
        }

        // Walk the execution order and emit commands.
        u32 currentBatch = std::numeric_limits<u32>::max();

        for (const auto& passName : m_PassOrder)
        {
            const auto batchIt = passToBatch.find(passName);
            const bool inBatch = (batchIt != passToBatch.end());
            const u32 batchIdx = inBatch ? batchIt->second : std::numeric_limits<u32>::max();

            // --- Batch-boundary open ---
            if (inBatch && batchIdx != currentBatch)
            {
                // Close the previous batch (if any) before opening a new one.
                if (currentBatch != std::numeric_limits<u32>::max())
                {
                    SubmissionCommand end;
                    end.CommandKind = SubmissionCommand::Kind::BatchEnd;
                    end.BatchIndex = currentBatch;
                    end.Lane = QueueLane::Compute;
                    plan.push_back(std::move(end));
                }

                SubmissionCommand begin;
                begin.CommandKind = SubmissionCommand::Kind::BatchBegin;
                begin.BatchIndex = batchIdx;
                begin.Lane = QueueLane::Compute;
                if (const auto batchInfoIt = batchByIndex.find(batchIdx); batchInfoIt != batchByIndex.end())
                {
                    begin.WaitPasses = batchInfoIt->second->WaitPasses;
                    begin.InputResources = batchInfoIt->second->InputResources;
                }
                plan.push_back(std::move(begin));
                currentBatch = batchIdx;
            }

            // --- Batch-boundary close (returning to graphics after a batch) ---
            if (!inBatch && currentBatch != std::numeric_limits<u32>::max())
            {
                SubmissionCommand end;
                end.CommandKind = SubmissionCommand::Kind::BatchEnd;
                end.BatchIndex = currentBatch;
                end.Lane = QueueLane::Compute;
                if (const auto batchInfoIt = batchByIndex.find(currentBatch); batchInfoIt != batchByIndex.end())
                {
                    end.SignalPasses = batchInfoIt->second->SignalPasses;
                    end.OutputResources = batchInfoIt->second->OutputResources;
                }
                plan.push_back(std::move(end));
                currentBatch = std::numeric_limits<u32>::max();
            }

            auto passWorkType = RenderPass::PassWorkType::Graphics;
            if (const auto passIt = m_PassLookup.find(passName); passIt != m_PassLookup.end() && passIt->second)
                passWorkType = passIt->second->GetPassWorkType();

            const auto passLane = mapWorkTypeToLane(passWorkType);

            // --- Memory barrier before this pass (if any) ---
            if (const auto barIt = barrierForPass.find(passName); barIt != barrierForPass.end())
            {
                SubmissionCommand barrier;
                barrier.CommandKind = SubmissionCommand::Kind::MemoryBarrier;
                barrier.Barriers = barIt->second;
                barrier.Lane = passLane;
                plan.push_back(std::move(barrier));
            }

            // --- Pass command ---
            SubmissionCommand passCmd;
            passCmd.CommandKind = SubmissionCommand::Kind::Pass;
            passCmd.PassName = passName;
            passCmd.WorkType = passWorkType;
            passCmd.Lane = passLane;
            plan.push_back(std::move(passCmd));
        }

        // Close any trailing open batch.
        if (currentBatch != std::numeric_limits<u32>::max())
        {
            SubmissionCommand end;
            end.CommandKind = SubmissionCommand::Kind::BatchEnd;
            end.BatchIndex = currentBatch;
            end.Lane = QueueLane::Compute;
            if (const auto batchInfoIt = batchByIndex.find(currentBatch); batchInfoIt != batchByIndex.end())
            {
                end.SignalPasses = batchInfoIt->second->SignalPasses;
                end.OutputResources = batchInfoIt->second->OutputResources;
            }
            plan.push_back(std::move(end));
        }

        return plan;
    }

    // -------------------------------------------------------------------
    // Explicit resource transition records
    // -------------------------------------------------------------------
    std::vector<RenderGraph::ResourceTransition> RenderGraph::GetResourceTransitions() const
    {
        if (m_PlannedBarriers.empty())
            return {};

        // Build a pass → execution-order-index map for the backward producer scan.
        std::unordered_map<std::string, std::size_t> passOrderIdx;
        passOrderIdx.reserve(m_PassOrder.size());
        for (std::size_t i = 0; i < m_PassOrder.size(); ++i)
            passOrderIdx.emplace(m_PassOrder[i], i);

        // Build a pass → queue lane map so transitions can
        // detect when the producer and consumer are on different queue lanes.
        const auto passToLane = [&](const std::string& passName) -> QueueLane
        {
            if (const auto it = m_PassLookup.find(passName); it != m_PassLookup.end() && it->second)
            {
                switch (it->second->GetPassWorkType())
                {
                    case RenderPass::PassWorkType::Compute:
                        return QueueLane::Compute;
                    case RenderPass::PassWorkType::Copy:
                        return QueueLane::Copy;
                    case RenderPass::PassWorkType::Graphics:
                    default:
                        return QueueLane::Graphics;
                }
            }
            return QueueLane::Graphics;
        };

        std::vector<ResourceTransition> transitions;
        transitions.reserve(m_PlannedBarriers.size());

        for (const auto& barrier : m_PlannedBarriers)
        {
            ResourceTransition t;
            t.ResourceName = barrier.Resource;
            t.ConsumerPass = barrier.BeforePass;
            t.Flags = barrier.Flags;
            t.Range = barrier.Range;

            // Determine the consumer's read usage from its declared accesses.
            // Default to ShaderSample when the declaration is absent (e.g.
            // old-style passes that do not call DeclareRead/DeclareWrite).
            t.ToUsage = RGReadUsage::ShaderSample;
            if (const auto dit = m_PassAccessDeclarations.find(barrier.BeforePass);
                dit != m_PassAccessDeclarations.end())
            {
                for (const auto& decl : dit->second)
                {
                    if (decl.ResourceName == barrier.Resource && !decl.IsWrite)
                    {
                        t.ToUsage = decl.ReadUsage;
                        break;
                    }
                }
            }

            // Walk the execution order from the start up to (but not including)
            // the consumer to find the LAST writer — that is the producer for
            // this transition.  When no writer is found the resource is imported
            // externally (initial state) so ProducerPass stays "external".
            t.ProducerPass = "external";
            t.FromUsage = RGWriteUsage::RenderTarget;

            const auto consumerIdxIt = passOrderIdx.find(barrier.BeforePass);
            if (consumerIdxIt != passOrderIdx.end())
            {
                const std::size_t consumerIdx = consumerIdxIt->second;
                for (std::size_t i = 0; i < consumerIdx; ++i)
                {
                    const auto& passName = m_PassOrder[i];
                    if (const auto dit = m_PassAccessDeclarations.find(passName);
                        dit != m_PassAccessDeclarations.end())
                    {
                        for (const auto& decl : dit->second)
                        {
                            if (decl.ResourceName == barrier.Resource && decl.IsWrite)
                            {
                                // Keep scanning — we want the LAST writer.
                                t.ProducerPass = passName;
                                t.FromUsage = decl.WriteUsage;
                            }
                        }
                    }
                }
            }

            transitions.push_back(std::move(t));
        }

        // Annotate each transition with cross-lane sync metadata.
        for (auto& tr : transitions)
        {
            // External producers (imported resources) are treated as Graphics lane.
            const QueueLane producerLane = (tr.ProducerPass == "external")
                                               ? QueueLane::Graphics
                                               : passToLane(tr.ProducerPass);
            const QueueLane consumerLane = passToLane(tr.ConsumerPass);
            tr.ProducerLane = producerLane;
            tr.ConsumerLane = consumerLane;
            tr.IsCrossLane = (producerLane != consumerLane);
        }

        return transitions;
    }

    std::vector<RenderGraph::ResourceLifetime> RenderGraph::GetResourceLifetimes() const
    {
        // ----------------------------------------------------------------
        // Unified resource lifetime records.
        //
        // For every registered resource we walk the pass execution order and
        // inspect m_PassAccessDeclarations to find:
        //   • the FIRST write  (producer; "external" when import-only)
        //   • the LAST  read   (last consumer; "" when write-only)
        //
        // Flags (IsImported / IsExtracted / IsHistory / IsTransient) are
        // derived from the internal data structures that the graph already
        // maintains — no new per-resource metadata is needed.
        // ----------------------------------------------------------------

        // --- Build auxiliary lookup sets --------------------------------

        // Imported: any resource registered via ImportTexture/ImportFramebuffer/
        // ImportBuffer (the key is the canonical resource name).
        std::unordered_set<std::string> importedNames;
        importedNames.reserve(m_ImportedResources.size());
        for (const auto& [name, _] : m_ImportedResources)
            importedNames.insert(name);

        // Extracted: resources with a pending TextureExtract or
        // FramebufferExtract callback.  Resolve handles → names.
        std::unordered_set<std::string> extractedNames;
        extractedNames.reserve(m_TextureExtracts.size() + m_FramebufferExtracts.size());
        for (const auto& ex : m_TextureExtracts)
        {
            const auto n = GetResourceName(ex.Handle);
            if (!n.empty())
                extractedNames.emplace(n);
        }
        for (const auto& ex : m_FramebufferExtracts)
        {
            const auto n = GetResourceName(ex.Handle);
            if (!n.empty())
                extractedNames.emplace(n);
        }

        // History: resources listed as HistoryResource in any temporal
        // contract (they are imported from the previous frame).
        std::unordered_set<std::string> historyNames;
        historyNames.reserve(m_TemporalHistoryContracts.size());
        for (const auto& contract : m_TemporalHistoryContracts)
            historyNames.insert(contract.HistoryResource);

        // Transient: resources that appear in the transient plan and will
        // actually be allocated (WillAllocate == true).
        std::unordered_set<std::string> transientNames;
        transientNames.reserve(m_TransientPlan.size());
        for (const auto& entry : m_TransientPlan)
            if (entry.WillAllocate)
                transientNames.insert(entry.Resource);

        // --- Build pass-order index map ---------------------------------
        std::unordered_map<std::string, u32> passOrderIdx;
        passOrderIdx.reserve(m_PassOrder.size());
        for (u32 i = 0; i < static_cast<u32>(m_PassOrder.size()); ++i)
            passOrderIdx.emplace(m_PassOrder[i], i);

        // --- Produce one ResourceLifetime per registered resource -------
        // GetRegisteredResources() ensures the registry is lazily built before
        // we iterate — do NOT access m_RegisteredResources directly here.
        const auto& registeredResources = GetRegisteredResources();
        std::vector<ResourceLifetime> lifetimes;
        lifetimes.reserve(registeredResources.size());

        for (const auto& info : registeredResources)
        {
            ResourceLifetime lt;
            lt.ResourceName = info.Name;
            lt.IsImported = importedNames.contains(info.Name);
            lt.IsExtracted = extractedNames.contains(info.Name);
            lt.IsHistory = historyNames.contains(info.Name);
            lt.IsTransient = transientNames.contains(info.Name);

            // Walk the execution order to find first write and last read.
            for (u32 i = 0; i < static_cast<u32>(m_PassOrder.size()); ++i)
            {
                const auto& passName = m_PassOrder[i];
                const auto dit = m_PassAccessDeclarations.find(passName);
                if (dit == m_PassAccessDeclarations.end())
                    continue;

                for (const auto& decl : dit->second)
                {
                    if (decl.ResourceName != info.Name)
                        continue;

                    if (decl.IsWrite)
                    {
                        // Record the FIRST write only.
                        if (i < lt.FirstWritePassIndex)
                        {
                            lt.FirstWritePassIndex = i;
                            lt.FirstWritePass = passName;
                            lt.FirstWriteUsage = decl.WriteUsage;
                        }
                    }
                    else
                    {
                        // Record the LAST read (keep updating on every new read).
                        if (lt.LastReadPassIndex == std::numeric_limits<u32>::max() || i >= lt.LastReadPassIndex)
                        {
                            lt.LastReadPassIndex = i;
                            lt.LastReadPass = passName;
                            lt.LastReadUsage = decl.ReadUsage;
                        }
                    }
                }
            }

            // If no write was found the resource is import-only.
            if (lt.FirstWritePassIndex == std::numeric_limits<u32>::max())
                lt.FirstWritePass = "external";

            lifetimes.push_back(std::move(lt));
        }

        return lifetimes;
    }

    void RenderGraph::LogSubmissionPlanIfChanged()
    {
        if (!IsRenderGraphDiagnosticsEnabled())
            return;

        std::string digest;
        std::unordered_map<std::string, u32> passIndexByName;
        passIndexByName.reserve(m_CachedSubmissionPlan.size());

        u32 passIndex = 0;
        for (const auto& cmd : m_CachedSubmissionPlan)
        {
            if (cmd.CommandKind != SubmissionCommand::Kind::Pass)
                continue;

            if (!digest.empty())
                digest += " -> ";

            if (IsPassReachable(cmd.PassName))
            {
                digest += cmd.PassName;
            }
            else
            {
                digest += "(";
                digest += cmd.PassName;
                digest += ":culled)";
            }

            passIndexByName.emplace(cmd.PassName, passIndex++);
        }

        if (digest.empty())
            digest = "<empty>";

        if (digest == m_LastLoggedSubmissionPlanDigest)
            return;

        OLO_CORE_TRACE("RenderGraph submission plan: {}", digest);

        const auto passIndexLabel = [&passIndexByName](std::string_view passName) -> std::string
        {
            if (const auto it = passIndexByName.find(std::string(passName)); it != passIndexByName.end())
                return "#" + std::to_string(it->second);
            return "n/a";
        };

        if (passIndexByName.contains("SSAOPass") || passIndexByName.contains("GTAOPass") ||
            passIndexByName.contains("AOApplyPass"))
        {
            OLO_CORE_TRACE("RenderGraph AO/Post order: SSAO={}, GTAO={}, AOApply={}, Bloom={}, ToneMap={}, Vignette={}, FXAA={}, SelectionOutline={}, UIComposite={}, Final={}",
                           passIndexLabel("SSAOPass"),
                           passIndexLabel("GTAOPass"),
                           passIndexLabel("AOApplyPass"),
                           passIndexLabel("BloomPass"),
                           passIndexLabel("ToneMapPass"),
                           passIndexLabel("VignettePass"),
                           passIndexLabel("FXAAPass"),
                           passIndexLabel("SelectionOutlinePass"),
                           passIndexLabel("UICompositePass"),
                           passIndexLabel("FinalPass"));
        }

        m_LastLoggedSubmissionPlanDigest = std::move(digest);
    }

    void RenderGraph::ResolveFinalPass()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FinalPassName.empty())
        {
            // If no final pass was explicitly set, try to find a pass with no dependents
            // Iterate m_PassOrder (deterministic) instead of m_PassLookup (unordered_map)
            // Build set of all passes that are producers (have consumers)
            std::unordered_set<std::string> hasConsumers;
            for (const auto& [consumer, producers] : m_Dependencies)
                for (const auto& prod : producers)
                    hasConsumers.insert(prod);

            for (auto it = m_PassOrder.rbegin(); it != m_PassOrder.rend(); ++it)
            {
                const auto& name = *it;
                if (!hasConsumers.contains(name))
                {
                    m_FinalPassName = name;
                    if (IsRenderGraphDiagnosticsEnabled())
                        OLO_CORE_TRACE("RenderGraph: Auto-selected final pass: {}", name);
                    break;
                }
            }
        }

        if (m_FinalPassName.empty())
        {
            OLO_CORE_WARN("RenderGraph: Could not determine final pass!");
        }
    }

    void RenderGraph::ComputeReachability()
    {
        OLO_PROFILE_FUNCTION();

        m_ReachablePasses.clear();
        m_CulledPasses.clear();

        // Only apply culling when the final output is explicitly requested.
        // If final pass is auto-selected, preserve legacy behavior.
        if (!m_HasExplicitFinalPass)
        {
            for (const auto& [passName, pass] : m_PassLookup)
                m_ReachablePasses.insert(passName);
            return;
        }

        if (m_FinalPassName.empty())
        {
            OLO_CORE_WARN("ComputeReachability: final pass not set; all passes marked reachable");
            for (const auto& [passName, pass] : m_PassLookup)
                m_ReachablePasses.insert(passName);
            return;
        }

        std::unordered_set<std::string> visited;
        std::vector<std::string> stack;

        stack.push_back(m_FinalPassName);
        m_ReachablePasses.insert(m_FinalPassName);

        while (!stack.empty())
        {
            const auto current = stack.back();
            stack.pop_back();

            if (visited.contains(current))
                continue;
            visited.insert(current);

            if (m_Dependencies.contains(current))
            {
                for (const auto& dependency : m_Dependencies[current])
                {
                    if (!m_ReachablePasses.contains(dependency))
                    {
                        m_ReachablePasses.insert(dependency);
                        stack.push_back(dependency);
                    }
                }
            }
        }

        // Also walk backward using declared resource
        // dependencies (DeclareRead / DeclareWrite).  Legacy RenderPasses
        // whose ordering edges were intentionally removed from m_Dependencies
        // (in favour of declaration-derived ordering) must still be reachable
        // from the final pass or they would all be culled.
        //
        // Algorithm:
        //   1. Build a resource-name → writer-pass map from every pass in
        //      m_PassLookup (covers both legacy and graph-native passes).
        //   2. Iteratively expand m_ReachablePasses: for each already-reachable
        //      pass, look up its declared reads and add the writer(s) of each
        //      read resource.  Repeat until stable.
        {
            std::unordered_map<std::string, std::vector<std::string>> resourceWriters;
            resourceWriters.reserve(m_PassLookup.size() * 4u);

            const auto findFrameAccesses = [this](const std::string& passName) -> const std::vector<RGAccessDeclaration>*
            {
                if (const auto accessIt = m_PassAccessDeclarations.find(passName); accessIt != m_PassAccessDeclarations.end())
                {
                    return &accessIt->second;
                }
                return nullptr;
            };

            for (const auto& [passName, pass] : m_PassLookup)
            {
                if (!pass)
                    continue;

                if (const auto* accesses = findFrameAccesses(passName))
                {
                    for (const auto& access : *accesses)
                    {
                        if (access.IsWrite && !access.ResourceName.empty())
                        {
                            resourceWriters[access.ResourceName].push_back(passName);
                        }
                    }
                    continue;
                }

                for (const ResourceHandle& write : pass->GetWrites())
                {
                    if (!write.Name.empty())
                        resourceWriters[write.Name].push_back(passName);
                }
            }

            bool anyNew = true;
            while (anyNew)
            {
                anyNew = false;
                const std::vector<std::string> snapshot(m_ReachablePasses.begin(),
                                                        m_ReachablePasses.end());
                for (const auto& passName : snapshot)
                {
                    auto passIt = m_PassLookup.find(passName);
                    if (passIt == m_PassLookup.end() || !passIt->second)
                        continue;

                    if (const auto* accesses = findFrameAccesses(passName))
                    {
                        for (const auto& access : *accesses)
                        {
                            if (access.IsWrite)
                                continue;

                            auto writerIt = resourceWriters.find(access.ResourceName);
                            if (writerIt == resourceWriters.end())
                                continue;
                            for (const auto& writerName : writerIt->second)
                            {
                                if (m_ReachablePasses.insert(writerName).second)
                                    anyNew = true;
                            }
                        }
                        continue;
                    }

                    for (const ResourceHandle& read : passIt->second->GetReads())
                    {
                        auto writerIt = resourceWriters.find(read.Name);
                        if (writerIt == resourceWriters.end())
                            continue;
                        for (const auto& writerName : writerIt->second)
                        {
                            if (m_ReachablePasses.insert(writerName).second)
                                anyNew = true;
                        }
                    }
                }
            }
        }

        for (const auto& [passName, pass] : m_PassLookup)
        {
            if (m_ReachablePasses.contains(passName))
                continue;

            if (pass && pass->IsSideEffecting())
            {
                m_ReachablePasses.insert(passName);
                if (IsRenderGraphDiagnosticsEnabled())
                    OLO_CORE_TRACE("Pass '{}' is unreachable but has side effects; keeping it", passName);
            }
            else
            {
                m_CulledPasses.push_back(passName);
            }
        }

        std::sort(m_CulledPasses.begin(), m_CulledPasses.end());

        std::string digest;
        for (const auto& passName : m_CulledPasses)
        {
            if (!digest.empty())
                digest += ",";
            digest += passName;
        }
        if (digest.empty())
            digest = "<none>";

        if (digest != m_LastLoggedCulledPassDigest)
        {
            if (IsRenderGraphDiagnosticsEnabled())
                OLO_CORE_TRACE("RenderGraph culled passes changed: {}", digest);
            m_LastLoggedCulledPassDigest = std::move(digest);
        }
    }

    MemoryBarrierFlags RenderGraph::ResolveProducerBarrierFlags(const RGWriteUsage usage)
    {
        switch (usage)
        {
            case RGWriteUsage::ShaderImage:
                return MemoryBarrierFlags::ShaderImageAccess;
            case RGWriteUsage::ShaderStorage:
                return MemoryBarrierFlags::ShaderStorage;
            case RGWriteUsage::TransferDest:
                return MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::BufferUpdate;
            case RGWriteUsage::RenderTarget:
            case RGWriteUsage::DepthStencil:
            case RGWriteUsage::Clear:
                return MemoryBarrierFlags::Framebuffer;
            default:
                return MemoryBarrierFlags::None;
        }
    }

    MemoryBarrierFlags RenderGraph::ResolveConsumerBarrierFlags(const RGReadUsage usage)
    {
        switch (usage)
        {
            case RGReadUsage::ShaderSample:
                return MemoryBarrierFlags::TextureFetch;
            case RGReadUsage::ShaderImage:
                return MemoryBarrierFlags::ShaderImageAccess;
            case RGReadUsage::ShaderStorage:
                return MemoryBarrierFlags::ShaderStorage;
            case RGReadUsage::TransferSource:
                return MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::BufferUpdate;
            case RGReadUsage::RenderTargetRead:
            case RGReadUsage::InputAttachment:
                return MemoryBarrierFlags::Framebuffer;
            case RGReadUsage::ComputeIndirectArgs:
                return MemoryBarrierFlags::Command;
            default:
                return MemoryBarrierFlags::None;
        }
    }

    void RenderGraph::ComputeBarrierPlan()
    {
        OLO_PROFILE_FUNCTION();

        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BarrierDiagnostics.clear();

        struct LastWriterState
        {
            std::string PassName;
            RGWriteUsage Usage = RGWriteUsage::RenderTarget;
        };

        std::unordered_map<std::string, LastWriterState> lastWriterByResource;
        lastWriterByResource.reserve(m_PassOrder.size() * 2u);

        std::unordered_map<std::string, std::unordered_set<std::string>> allWriterPassesByResource;
        allWriterPassesByResource.reserve(m_PassAccessDeclarations.size() * 2u);
        for (const auto& [passName, accessDeclarations] : m_PassAccessDeclarations)
        {
            for (const auto& access : accessDeclarations)
            {
                if (!access.IsWrite || access.ResourceName.empty())
                    continue;
                allWriterPassesByResource[access.ResourceName].insert(passName);
            }
        }

        for (const auto& passName : m_PassOrder)
        {
            if (!IsPassReachable(passName))
                continue;

            const auto declarationIt = m_PassAccessDeclarations.find(passName);
            if (declarationIt == m_PassAccessDeclarations.end())
                continue;

            auto plannedFlags = MemoryBarrierFlags::None;
            for (const auto& access : declarationIt->second)
            {
                if (access.ResourceName.empty())
                    continue;

                if (!access.IsWrite)
                {
                    const auto writerIt = lastWriterByResource.find(access.ResourceName);
                    if (writerIt == lastWriterByResource.end())
                    {
                        const auto allWritersIt = allWriterPassesByResource.find(access.ResourceName);
                        if (allWritersIt == allWriterPassesByResource.end() || allWritersIt->second.empty())
                        {
                            m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                                .Kind = BarrierDiagnosticKind::MissingProducer,
                                .PassName = passName,
                                .Resource = access.ResourceName,
                                .Message = "No producer declared for read resource '" + access.ResourceName + "' before pass '" + passName + "'",
                            });
                        }
                        else
                        {
                            auto hasReachableWriter = false;
                            for (const auto& writerPassName : allWritersIt->second)
                            {
                                if (IsPassReachable(writerPassName))
                                {
                                    hasReachableWriter = true;
                                    break;
                                }
                            }

                            if (!hasReachableWriter)
                            {
                                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                                    .Kind = BarrierDiagnosticKind::CulledProducer,
                                    .PassName = passName,
                                    .Resource = access.ResourceName,
                                    .Message = "Read resource '" + access.ResourceName + "' in pass '" + passName + "' only has unreachable/culled producers",
                                });
                            }
                        }
                        continue;
                    }
                    if (writerIt->second.PassName == passName)
                        continue;

                    const auto flags = ResolveProducerBarrierFlags(writerIt->second.Usage) |
                                       ResolveConsumerBarrierFlags(access.ReadUsage);
                    if (flags == MemoryBarrierFlags::None)
                    {
                        m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                            .Kind = BarrierDiagnosticKind::UnmappedTransition,
                            .PassName = passName,
                            .Resource = access.ResourceName,
                            .Message = "No barrier mapping for transition to pass '" + passName + "' on resource '" + access.ResourceName + "'",
                        });
                        continue;
                    }

                    plannedFlags |= flags;
                    m_PlannedBarriers.push_back(PlannedBarrier{
                        .BeforePass = passName,
                        .Resource = access.ResourceName,
                        .Flags = flags,
                        .Range = access.Range,
                    });
                }
                else
                {
                    const auto writerIt = lastWriterByResource.find(access.ResourceName);
                    if (writerIt != lastWriterByResource.end() && writerIt->second.PassName != passName)
                    {
                        const auto flags = ResolveProducerBarrierFlags(writerIt->second.Usage) |
                                           ResolveProducerBarrierFlags(access.WriteUsage);
                        if (flags == MemoryBarrierFlags::None)
                        {
                            m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                                .Kind = BarrierDiagnosticKind::UnmappedTransition,
                                .PassName = passName,
                                .Resource = access.ResourceName,
                                .Message = "No barrier mapping for write-after-write transition in pass '" + passName + "' on resource '" + access.ResourceName + "'",
                            });
                        }
                        else
                        {
                            plannedFlags |= flags;
                            m_PlannedBarriers.push_back(PlannedBarrier{
                                .BeforePass = passName,
                                .Resource = access.ResourceName,
                                .Flags = flags,
                                .Range = access.Range,
                            });
                        }
                    }

                    lastWriterByResource[access.ResourceName] = LastWriterState{
                        .PassName = passName,
                        .Usage = access.WriteUsage,
                    };
                }
            }

            if (plannedFlags != MemoryBarrierFlags::None)
                m_PassBarrierFlags[passName] = plannedFlags;
        }
    }

    bool RenderGraph::IsPassReachable(const std::string& passName) const
    {
        return m_ReachablePasses.contains(passName);
    }

    bool RenderGraph::IsHistoryTextureResource(std::string_view resourceName) const
    {
        const auto handle = GetTextureHandle(resourceName);
        if (!handle.IsValid() || !IsTextureHandleCurrent(handle))
            return false;
        if (handle.Index >= m_PhysicalTextures.size())
            return false;
        return m_PhysicalTextures[handle.Index].IsHistory;
    }

    bool RenderGraph::IsResourceReachableForExtraction(std::string_view resourceName) const
    {
        if (resourceName.empty())
            return false;

        const auto* info = FindRegisteredResource(resourceName);
        if (!info)
            return true;
        if (info->Producers.empty())
            return true;

        for (const auto& producer : info->Producers)
        {
            if (m_ReachablePasses.contains(producer))
                return true;
        }
        return false;
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

        // Declaration-derived edge synthesis.
        // Augment the closure with implicit RAW (read-after-write) edges that
        // can be derived from pass-level DeclareRead/DeclareWrite pairs.  This
        // allows passes that declare their resource access correctly to omit
        // explicit AddExecutionDependency calls for those resources.
        //
        // Algorithm:
        //   1. For every (producer, consumer) pair where producer.GetWrites()
        //      and consumer.GetReads() share a resource name, add a direct
        //      derived edge: consumer depends on producer.
        //   2. Run a fixed-point iteration so the new edges propagate through
        //      the closure (i.e. passes that depend on the consumer also
        //      transitively depend on the producer).
        {
            for (const auto& producerName : m_PassOrder)
            {
                auto producerIt = m_PassLookup.find(producerName);
                if (producerIt == m_PassLookup.end() || !producerIt->second)
                    continue;

                for (const ResourceHandle& write : producerIt->second->GetWrites())
                {
                    for (const auto& consumerName : m_PassOrder)
                    {
                        if (consumerName == producerName)
                            continue;
                        auto consumerIt = m_PassLookup.find(consumerName);
                        if (consumerIt == m_PassLookup.end() || !consumerIt->second)
                            continue;

                        for (const ResourceHandle& read : consumerIt->second->GetReads())
                        {
                            if (read.Name == write.Name)
                            {
                                closure[consumerName].insert(producerName);
                                break; // one match per write is enough
                            }
                        }
                    }
                }
            }

            // Propagate transitivity of the newly added derived edges until
            // no new entries are inserted.
            bool anyChange = true;
            while (anyChange)
            {
                anyChange = false;
                for (auto& [passName, cls] : closure)
                {
                    std::vector<std::string> toAdd;
                    for (const auto& dep : cls)
                    {
                        auto depIt = closure.find(dep);
                        if (depIt == closure.end())
                            continue;
                        for (const auto& ancestor : depIt->second)
                        {
                            if (!cls.contains(ancestor))
                                toAdd.push_back(ancestor);
                        }
                    }
                    for (const auto& a : toAdd)
                    {
                        if (cls.insert(a).second)
                            anyChange = true;
                    }
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

        // -----------------------------------------------------------------
        // Phase E validation: same-pass overlapping read/write requires an
        // explicit feedback declaration (not yet exposed), so flag for now.
        // -----------------------------------------------------------------
        auto rangesOverlap = [](const RGSubresourceRange& lhs, const RGSubresourceRange& rhs) -> bool
        {
            auto spanOverlap = [](u32 lhsBase, u32 lhsCount, u32 rhsBase, u32 rhsCount) -> bool
            {
                const auto lhsEndExclusive = lhsCount == ~0u
                                                 ? std::numeric_limits<u64>::max()
                                                 : static_cast<u64>(lhsBase) + static_cast<u64>(lhsCount);
                const auto rhsEndExclusive = rhsCount == ~0u
                                                 ? std::numeric_limits<u64>::max()
                                                 : static_cast<u64>(rhsBase) + static_cast<u64>(rhsCount);

                return static_cast<u64>(lhsBase) < rhsEndExclusive &&
                       static_cast<u64>(rhsBase) < lhsEndExclusive;
            };

            return spanOverlap(lhs.BaseMip, lhs.MipCount, rhs.BaseMip, rhs.MipCount) &&
                   spanOverlap(lhs.BaseLayer, lhs.LayerCount, rhs.BaseLayer, rhs.LayerCount) &&
                   spanOverlap(lhs.BaseSlice, lhs.SliceCount, rhs.BaseSlice, rhs.SliceCount);
        };

        for (const auto& [passName, accesses] : m_PassAccessDeclarations)
        {
            for (sizet readIdx = 0; readIdx < accesses.size(); ++readIdx)
            {
                const auto& readAccess = accesses[readIdx];
                if (readAccess.IsWrite)
                    continue;

                for (sizet writeIdx = 0; writeIdx < accesses.size(); ++writeIdx)
                {
                    const auto& writeAccess = accesses[writeIdx];
                    if (!writeAccess.IsWrite)
                        continue;
                    if (readAccess.ResourceName != writeAccess.ResourceName)
                        continue;
                    if (!rangesOverlap(readAccess.Range, writeAccess.Range))
                        continue;

                    Hazard h;
                    h.Kind = HazardKind::FeedbackWithoutDeclaration;
                    h.Resource = readAccess.ResourceName;
                    h.Producer = passName;
                    h.Consumer = passName;
                    h.Message = "Feedback hazard: pass '" + passName +
                                "' reads and writes overlapping subresources of resource '" +
                                readAccess.ResourceName + "' without an explicit feedback declaration";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                    hazards.push_back(std::move(h));
                    break;
                }
            }
        }

        // -----------------------------------------------------------------
        // Phase E validation: imported resource lifetime misuse.
        // If an imported resource is both produced and consumed in-graph,
        // it must have a currently valid backing object.
        // -----------------------------------------------------------------
        for (const auto& resource : m_RegisteredResources)
        {
            if (!resource.Desc.Imported)
                continue;
            if (resource.Producers.empty() || resource.Consumers.empty())
                continue;

            bool hasValidBacking = true;
            if (resource.TextureHandle.IsValid())
                hasValidBacking = ResolveTexture(resource.TextureHandle) != 0;
            else if (resource.BufferHandle.IsValid())
                hasValidBacking = ResolveBuffer(resource.BufferHandle) != 0;
            else if (resource.FramebufferHandle.IsValid())
                hasValidBacking = ResolveFramebuffer(resource.FramebufferHandle) != nullptr;

            if (hasValidBacking)
                continue;

            Hazard h;
            h.Kind = HazardKind::ImportedResourceLifetimeMisuse;
            h.Resource = resource.Name;
            h.Producer = resource.Producers.front();
            h.Consumer = resource.Consumers.front();
            h.Message = "Imported resource lifetime misuse: resource '" + resource.Name +
                        "' is produced and consumed in-graph but has no valid backing object";
            OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
            hazards.push_back(std::move(h));
        }

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

            // Collect read resource names from both legacy (DeclareRead) and
            // graph-native (builder.Read) access declarations, deduplicating.
            std::vector<std::string> readNames;
            for (const ResourceHandle& r : pass.GetReads())
                readNames.push_back(r.Name);
            if (const auto aIt = m_PassAccessDeclarations.find(passName);
                aIt != m_PassAccessDeclarations.end())
            {
                for (const auto& access : aIt->second)
                {
                    if (!access.IsWrite && !access.ResourceName.empty())
                    {
                        if (std::find(readNames.begin(), readNames.end(), access.ResourceName) ==
                            readNames.end())
                            readNames.push_back(access.ResourceName);
                    }
                }
            }

            // Collect write resource names from both sources, deduplicating.
            std::vector<std::string> writeNames;
            for (const ResourceHandle& w : pass.GetWrites())
                writeNames.push_back(w.Name);
            if (const auto aIt = m_PassAccessDeclarations.find(passName);
                aIt != m_PassAccessDeclarations.end())
            {
                for (const auto& access : aIt->second)
                {
                    if (access.IsWrite && !access.ResourceName.empty())
                    {
                        if (std::find(writeNames.begin(), writeNames.end(), access.ResourceName) ==
                            writeNames.end())
                            writeNames.push_back(access.ResourceName);
                    }
                }
            }

            // Record reads first — a same-pass read+write on the same
            // resource isn't a hazard against itself.
            for (const auto& rName : readNames)
            {
                ResourceState& st = state[rName];
                if (!st.LastWriter.empty() && st.LastWriter != passName && !dependsOn(passName, st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::ReadAfterWrite;
                    h.Resource = rName;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "RAW: pass '" + passName + "' reads resource '" + rName +
                                "' written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
                    hazards.push_back(std::move(h));
                }
                st.LiveReaders.insert(passName);
            }

            for (const auto& wName : writeNames)
            {
                ResourceState& st = state[wName];

                if (!st.LastWriter.empty() && st.LastWriter != passName && !dependsOn(passName, st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::WriteAfterWrite;
                    h.Resource = wName;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "WAW: pass '" + passName + "' writes resource '" + wName +
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
                        h.Resource = wName;
                        h.Producer = reader;
                        h.Consumer = passName;
                        h.Message = "WAR: pass '" + passName + "' overwrites resource '" + wName +
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

        for (const auto& [name, desc] : m_TransientResourceDescs)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.empty())
                info.Desc.DebugName = name;
            info.Desc.Imported = false;
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

        // Phase E: include setup-time builder declarations so graph-native
        // passes are represented in producer/consumer tracking and typed
        // handle assignment even when legacy RenderPass declarations are empty.
        for (const auto& [passName, accesses] : m_PassAccessDeclarations)
        {
            for (const auto& access : accesses)
            {
                ResourceHandle syntheticHandle(access.ResourceName, ResourceHandle::Kind::Unknown);
                registerDeclaration(passName, syntheticHandle, access.IsWrite);
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

    std::string RenderGraph::BuildTransientAliasGroup(const RGResourceDesc& desc)
    {
        // Base key covers kind, dimensions, mips, samples and queue.
        std::string key = std::to_string(static_cast<u32>(desc.Kind)) + ":" +
                          std::to_string(static_cast<u32>(desc.Format)) + ":" +
                          std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                          "x" + std::to_string(desc.DepthOrLayers) + ":m" + std::to_string(desc.MipLevels) +
                          ":s" + std::to_string(desc.Samples) + ":q" + std::to_string(static_cast<u32>(desc.Queue));
        // MRT: append each attachment format so MRT layouts don't alias
        // with single-attachment FBs or with each other.
        if (!desc.Attachments.empty())
        {
            key += ":mrt";
            for (const auto fmt : desc.Attachments)
                key += "," + std::to_string(static_cast<u32>(fmt));
        }
        return key;
    }

    u64 RenderGraph::EstimateTransientBytes(const RGResourceDesc& desc)
    {
        auto bytesPerPixelForFormat = [](const RGResourceFormat format) -> u64
        {
            switch (format)
            {
                case RGResourceFormat::R8UNorm:
                    return 1;
                case RGResourceFormat::R32Float:
                case RGResourceFormat::RG16Float:
                case RGResourceFormat::RGBA8UNorm:
                case RGResourceFormat::Depth24Stencil8:
                case RGResourceFormat::Depth32Float:
                    return 4;
                case RGResourceFormat::RGBA16Float:
                    return 8;
                case RGResourceFormat::Unknown:
                default:
                    return 0;
            }
        };

        if (desc.Kind == ResourceHandle::Kind::StorageBuffer || desc.Kind == ResourceHandle::Kind::UniformBuffer)
            return desc.Width;

        if (desc.Width == 0 || desc.Height == 0)
            return 0;

        const auto layerCount = std::max(desc.DepthOrLayers, 1u);
        const auto mipCount = std::max(desc.MipLevels, 1u);
        const auto sampleCount = std::max(desc.Samples, 1u);

        // MRT: sum bytes across all attachment layers.
        if (!desc.Attachments.empty())
        {
            u64 total = 0;
            for (const auto fmt : desc.Attachments)
            {
                const auto bpp = bytesPerPixelForFormat(fmt);
                total += bpp * static_cast<u64>(desc.Width) * static_cast<u64>(desc.Height) *
                         static_cast<u64>(layerCount) * static_cast<u64>(mipCount) * static_cast<u64>(sampleCount);
            }
            return total;
        }

        const auto bpp = bytesPerPixelForFormat(desc.Format);
        if (bpp == 0)
            return 0;

        return bpp * static_cast<u64>(desc.Width) * static_cast<u64>(desc.Height) *
               static_cast<u64>(layerCount) * static_cast<u64>(mipCount) * static_cast<u64>(sampleCount);
    }

    bool RenderGraph::IsTransientDescriptorAllocatable(const RGResourceDesc& desc)
    {
        if (desc.Imported)
            return false;

        switch (desc.Kind)
        {
            case ResourceHandle::Kind::Texture2D:
            case ResourceHandle::Kind::Texture2DArray:
            case ResourceHandle::Kind::TextureCube:
            case ResourceHandle::Kind::TextureCubeArray:
                return desc.Width > 0 &&
                       desc.Height > 0 &&
                       desc.Format != RGResourceFormat::Unknown &&
                       ToImageFormat(desc.Format) != ImageFormat::None;
            case ResourceHandle::Kind::Framebuffer:
                // MRT: at least one valid attachment required; dims must be set.
                if (!desc.Attachments.empty())
                {
                    return desc.Width > 0 &&
                           desc.Height > 0 &&
                           std::any_of(desc.Attachments.begin(), desc.Attachments.end(),
                                       [](const RGResourceFormat fmt)
                                       {
                                           return ToFramebufferFormat(fmt) != FramebufferTextureFormat::None;
                                       });
                }
                return desc.Width > 0 &&
                       desc.Height > 0 &&
                       desc.Format != RGResourceFormat::Unknown &&
                       ToFramebufferFormat(desc.Format) != FramebufferTextureFormat::None;
            case ResourceHandle::Kind::StorageBuffer:
            case ResourceHandle::Kind::UniformBuffer:
                return desc.Width > 0;
            case ResourceHandle::Kind::Unknown:
            default:
                return false;
        }
    }

    std::string_view RenderGraph::GetTransientDescriptorSkipReason(const RGResourceDesc& desc)
    {
        if (desc.Imported)
            return "imported-resource";

        switch (desc.Kind)
        {
            case ResourceHandle::Kind::Texture2D:
            case ResourceHandle::Kind::Texture2DArray:
            case ResourceHandle::Kind::TextureCube:
            case ResourceHandle::Kind::TextureCubeArray:
            {
                if (desc.Width == 0 || desc.Height == 0)
                    return "missing-dimensions";
                if (desc.Format == RGResourceFormat::Unknown)
                    return "unknown-format";
                if (ToImageFormat(desc.Format) == ImageFormat::None)
                    return "unsupported-image-format";
                return "descriptor-incomplete";
            }
            case ResourceHandle::Kind::Framebuffer:
            {
                if (desc.Width == 0 || desc.Height == 0)
                    return "missing-dimensions";
                // MRT path: a non-empty Attachments list replaces Format.
                if (!desc.Attachments.empty())
                {
                    const bool anyValid = std::any_of(desc.Attachments.begin(), desc.Attachments.end(),
                                                      [](const RGResourceFormat fmt)
                                                      {
                                                          return ToFramebufferFormat(fmt) != FramebufferTextureFormat::None;
                                                      });
                    if (!anyValid)
                        return "unsupported-framebuffer-format";
                    return "descriptor-incomplete";
                }
                if (desc.Format == RGResourceFormat::Unknown)
                    return "unknown-format";
                if (ToFramebufferFormat(desc.Format) == FramebufferTextureFormat::None)
                    return "unsupported-framebuffer-format";
                return "descriptor-incomplete";
            }
            case ResourceHandle::Kind::StorageBuffer:
            case ResourceHandle::Kind::UniformBuffer:
            {
                if (desc.Width == 0)
                    return "zero-size-buffer";
                return "descriptor-incomplete";
            }
            case ResourceHandle::Kind::Unknown:
            default:
                return "unknown-kind";
        }
    }

    void RenderGraph::RebuildTransientPlan()
    {
        m_TransientPlan.clear();
        if (m_TransientResourceDescs.empty())
            return;

        struct Lifetime
        {
            bool Reachable = false;
            u32 First = std::numeric_limits<u32>::max();
            u32 Last = 0;
            std::string FirstPass;
            std::string LastPass;
        };

        std::unordered_map<std::string, Lifetime> lifetimes;
        lifetimes.reserve(m_TransientResourceDescs.size());

        for (u32 passIndex = 0; passIndex < static_cast<u32>(m_PassOrder.size()); ++passIndex)
        {
            const auto& passName = m_PassOrder[passIndex];
            const auto accessIt = m_PassAccessDeclarations.find(passName);
            if (accessIt == m_PassAccessDeclarations.end())
                continue;

            for (const auto& access : accessIt->second)
            {
                if (!m_TransientResourceDescs.contains(access.ResourceName))
                    continue;

                auto& lifetime = lifetimes[access.ResourceName];
                if (!IsPassReachable(passName))
                    continue;

                lifetime.Reachable = true;
                if (passIndex < lifetime.First)
                {
                    lifetime.First = passIndex;
                    lifetime.FirstPass = passName;
                }
                if (passIndex >= lifetime.Last)
                {
                    lifetime.Last = passIndex;
                    lifetime.LastPass = passName;
                }
            }
        }

        m_TransientPlan.reserve(m_TransientResourceDescs.size());
        for (const auto& [resourceName, desc] : m_TransientResourceDescs)
        {
            TransientPlanEntry entry;
            entry.Resource = resourceName;
            entry.Kind = desc.Kind;
            entry.AliasGroup = BuildTransientAliasGroup(desc);
            entry.EstimatedBytes = EstimateTransientBytes(desc);

            if (const auto ltIt = lifetimes.find(resourceName); ltIt != lifetimes.end())
            {
                const auto& lt = ltIt->second;
                entry.Reachable = lt.Reachable;
                entry.FirstPassIndex = lt.First;
                entry.LastPassIndex = lt.Last;
                entry.FirstPass = lt.FirstPass;
                entry.LastPass = lt.LastPass;
            }

            if (!entry.Reachable)
            {
                entry.SkipReason = "unreachable-or-disabled";
            }
            else if (!IsTransientDescriptorAllocatable(desc))
            {
                entry.SkipReason = std::string(GetTransientDescriptorSkipReason(desc));
            }
            else
            {
                entry.WillAllocate = true;
            }

            m_TransientPlan.push_back(std::move(entry));
        }

        std::sort(m_TransientPlan.begin(), m_TransientPlan.end(),
                  [](const TransientPlanEntry& lhs, const TransientPlanEntry& rhs)
                  {
                      if (lhs.AliasGroup != rhs.AliasGroup)
                          return lhs.AliasGroup < rhs.AliasGroup;
                      if (lhs.FirstPassIndex != rhs.FirstPassIndex)
                          return lhs.FirstPassIndex < rhs.FirstPassIndex;
                      return lhs.Resource < rhs.Resource;
                  });

        struct ActiveSlot
        {
            u32 Slot = 0;
            u32 LastPassIndex = 0;
        };

        std::unordered_map<std::string, std::vector<ActiveSlot>> activeByGroup;
        activeByGroup.reserve(m_TransientPlan.size());

        std::unordered_map<std::string, u32> nextSlotByGroup;
        nextSlotByGroup.reserve(m_TransientPlan.size());

        for (auto& entry : m_TransientPlan)
        {
            if (!entry.WillAllocate)
                continue;

            auto& active = activeByGroup[entry.AliasGroup];
            auto slotAssigned = std::numeric_limits<u32>::max();

            for (auto& candidate : active)
            {
                if (candidate.LastPassIndex < entry.FirstPassIndex)
                {
                    slotAssigned = candidate.Slot;
                    candidate.LastPassIndex = entry.LastPassIndex;
                    break;
                }
            }

            if (slotAssigned == std::numeric_limits<u32>::max())
            {
                slotAssigned = nextSlotByGroup[entry.AliasGroup]++;
                active.push_back(ActiveSlot{ .Slot = slotAssigned, .LastPassIndex = entry.LastPassIndex });
            }

            entry.AliasSlot = slotAssigned;
        }
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

        const auto queueLaneToString = [](const QueueLane lane) -> const char*
        {
            switch (lane)
            {
                case QueueLane::Compute:
                    return "Compute";
                case QueueLane::Copy:
                    return "Copy";
                case QueueLane::Graphics:
                default:
                    return "Graphics";
            }
        };

        out << "// OloEngine RenderGraph — generated by RenderGraph::DumpToDot\n";
        out << "// Render with:  dot -Tsvg " << filePath << " -o graph.svg\n";
        out << "// Node colours: blue=Graphics, amber=Compute, teal=Copy, green=FinalPass, grey=Culled\n";
        out << "digraph RenderGraph {\n";
        out << "    rankdir=LR;\n";
        out << "    node [shape=box, style=\"rounded,filled\", fillcolor=\"#e8f0fe\", fontname=\"Helvetica\"];\n";
        out << "    edge [fontname=\"Helvetica\", fontsize=10];\n\n";

        // Nodes in insertion order; final pass is double-ringed.
        // Compute passes are amber (#fff3cd), copy passes are teal (#cff4fc),
        //                   async-compute candidates get a "[async]" label prefix.
        for (const auto& name : m_InsertionOrder)
        {
            const bool isFinal = (name == m_FinalPassName);

            const auto passIt = m_PassLookup.find(name);
            const bool isCompute = passIt != m_PassLookup.end() && passIt->second &&
                                   passIt->second->GetPassWorkType() == RenderPass::PassWorkType::Compute;
            const bool isCopy = passIt != m_PassLookup.end() && passIt->second &&
                                passIt->second->GetPassWorkType() == RenderPass::PassWorkType::Copy;
            const bool isAsyncCandidate = passIt != m_PassLookup.end() && passIt->second &&
                                          passIt->second->IsAsyncComputeCandidate();

            const std::string label = isAsyncCandidate ? ("[async] " + name) : name;

            out << "    \"" << name << "\"";

            std::vector<std::string> attributes;
            attributes.push_back(std::string("label=\"") + label + "\"");
            if (isFinal)
                attributes.emplace_back("peripheries=2");

            const bool hasReachabilityData = !m_ReachablePasses.empty();
            const bool isCulled = hasReachabilityData && !m_ReachablePasses.contains(name);
            if (isCulled)
            {
                attributes.emplace_back("fillcolor=\"#f2f2f2\"");
                attributes.emplace_back("style=\"rounded,dashed,filled\"");
            }
            else if (isFinal)
            {
                attributes.emplace_back("fillcolor=\"#d4edda\"");
            }
            else if (isCompute)
            {
                // Amber: compute-dispatch only — no rasterization
                attributes.emplace_back("fillcolor=\"#fff3cd\"");
            }
            else if (isCopy)
            {
                // Teal: transfer/blit only
                attributes.emplace_back("fillcolor=\"#cff4fc\"");
            }

            if (!attributes.empty())
            {
                out << " [";
                for (sizet i = 0; i < attributes.size(); ++i)
                {
                    out << attributes[i];
                    if (i + 1 < attributes.size())
                        out << ", ";
                }
                out << "]";
            }

            out << ";\n";
        }
        out << "\n";

        // Dashed grey edges = ordering-only (AddExecutionDependency / ConnectPass)
        for (const auto& [consumer, producers] : m_Dependencies)
        {
            for (const auto& producer : producers)
            {
                out << "    \"" << producer << "\" -> \"" << consumer
                    << "\" [style=dashed, color=\"#5f6368\", label=\"order\"];\n";
            }
        }

        out << "\n    // Planned barriers\n";
        for (const auto& barrier : m_PlannedBarriers)
        {
            out << "    // before '" << barrier.BeforePass << "': resource='" << barrier.Resource
                << "', flags=0x" << std::hex << static_cast<u32>(barrier.Flags) << std::dec << "\n";
        }

        out << "\n    // Barrier diagnostics\n";
        for (const auto& diagnostic : m_BarrierDiagnostics)
        {
            out << "    // pass='" << diagnostic.PassName << "', resource='" << diagnostic.Resource
                << "': " << diagnostic.Message << "\n";
        }

        out << "\n    // Async batch lane assignments\n";
        const auto batches = GetAsyncComputeBatches();
        for (sizet i = 0; i < batches.size(); ++i)
        {
            out << "    // batch " << i << ": lane=" << queueLaneToString(batches[i].Lane) << ", passes=[";
            for (sizet pi = 0; pi < batches[i].ComputePasses.size(); ++pi)
            {
                out << batches[i].ComputePasses[pi];
                if (pi + 1 < batches[i].ComputePasses.size())
                    out << ",";
            }
            out << "]\n";
        }

        // Cross-lane sync records in DOT comments.
        const auto dotTransitions = GetResourceTransitions();
        const auto crossLaneDot = std::count_if(dotTransitions.begin(), dotTransitions.end(),
                                                [](const ResourceTransition& tr)
                                                { return tr.IsCrossLane; });
        out << "\n    // Cross-lane sync records (" << crossLaneDot << " total)\n";
        for (const auto& tr : dotTransitions)
        {
            if (!tr.IsCrossLane)
                continue;
            out << "    // cross-lane: resource='" << tr.ResourceName
                << "', producer='" << tr.ProducerPass << "' (" << queueLaneToString(tr.ProducerLane) << ")"
                << " -> consumer='" << tr.ConsumerPass << "' (" << queueLaneToString(tr.ConsumerLane) << ")\n";
        }

        out << "}\n";
        out.close();

        OLO_CORE_INFO("RenderGraph::DumpToDot: wrote {} passes, {} dependency groups to '{}'",
                      m_InsertionOrder.size(),
                      m_Dependencies.size(),
                      filePath);
        return true;
    }

    bool RenderGraph::DumpToJson(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();

        EnsureResourceRegistryBuilt();

        std::ofstream out(filePath);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("RenderGraph::DumpToJson: failed to open '{}' for writing", filePath);
            return false;
        }

        const auto jsonEscape = [](const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char c : value)
            {
                switch (c)
                {
                    case '\\':
                        escaped += "\\\\";
                        break;
                    case '"':
                        escaped += "\\\"";
                        break;
                    case '\n':
                        escaped += "\\n";
                        break;
                    case '\r':
                        escaped += "\\r";
                        break;
                    case '\t':
                        escaped += "\\t";
                        break;
                    default:
                        escaped.push_back(c);
                        break;
                }
            }
            return escaped;
        };

        const auto barrierDiagnosticKindToString = [](const BarrierDiagnosticKind kind)
        {
            switch (kind)
            {
                case BarrierDiagnosticKind::MissingProducer:
                    return "MissingProducer";
                case BarrierDiagnosticKind::CulledProducer:
                    return "CulledProducer";
                case BarrierDiagnosticKind::UnmappedTransition:
                    return "UnmappedTransition";
                case BarrierDiagnosticKind::StaleExtractionHandle:
                    return "StaleExtractionHandle";
                case BarrierDiagnosticKind::ExtractionOfCulledResource:
                    return "ExtractionOfCulledResource";
                case BarrierDiagnosticKind::InvalidHistoryContract:
                    return "InvalidHistoryContract";
                default:
                    return "Unknown";
            }
        };

        // Work-type string conversion for JSON/DOT output.
        const auto passWorkTypeToString = [](const RenderPass::PassWorkType type) -> const char*
        {
            switch (type)
            {
                case RenderPass::PassWorkType::Compute:
                    return "Compute";
                case RenderPass::PassWorkType::Copy:
                    return "Copy";
                case RenderPass::PassWorkType::Graphics:
                default:
                    return "Graphics";
            }
        };

        const auto queueLaneToString = [](const QueueLane lane) -> const char*
        {
            switch (lane)
            {
                case QueueLane::Compute:
                    return "Compute";
                case QueueLane::Copy:
                    return "Copy";
                case QueueLane::Graphics:
                default:
                    return "Graphics";
            }
        };

        const auto readUsageToString = [](const RGReadUsage usage)
        {
            switch (usage)
            {
                case RGReadUsage::ShaderSample:
                    return "ShaderSample";
                case RGReadUsage::ShaderImage:
                    return "ShaderImage";
                case RGReadUsage::ShaderStorage:
                    return "ShaderStorage";
                case RGReadUsage::RenderTargetRead:
                    return "RenderTargetRead";
                case RGReadUsage::ComputeIndirectArgs:
                    return "ComputeIndirectArgs";
                case RGReadUsage::TransferSource:
                    return "TransferSource";
                case RGReadUsage::InputAttachment:
                    return "InputAttachment";
                default:
                    return "Unknown";
            }
        };

        const auto writeUsageToString = [](const RGWriteUsage usage)
        {
            switch (usage)
            {
                case RGWriteUsage::RenderTarget:
                    return "RenderTarget";
                case RGWriteUsage::DepthStencil:
                    return "DepthStencil";
                case RGWriteUsage::ShaderImage:
                    return "ShaderImage";
                case RGWriteUsage::ShaderStorage:
                    return "ShaderStorage";
                case RGWriteUsage::TransferDest:
                    return "TransferDest";
                case RGWriteUsage::Clear:
                    return "Clear";
                default:
                    return "Unknown";
            }
        };

        // Helper to serialise RGSubresourceRange as a compact
        // JSON object.  ~0u (= all mips/layers/slices) is written as -1 so that
        // consumers can distinguish "full range" from an explicit 1-element range.
        const auto subresourceRangeToJson = [](const RGSubresourceRange& r) -> std::string
        {
            auto enc = [](u32 v) -> std::string
            {
                return (v == ~0u) ? "-1" : std::to_string(v);
            };
            return "{ \"baseMip\": " + enc(r.BaseMip) +
                   ", \"mipCount\": " + enc(r.MipCount) +
                   ", \"baseLayer\": " + enc(r.BaseLayer) +
                   ", \"layerCount\": " + enc(r.LayerCount) +
                   ", \"baseSlice\": " + enc(r.BaseSlice) +
                   ", \"sliceCount\": " + enc(r.SliceCount) + " }";
        };

        struct LifetimeInfo
        {
            u32 FirstIndex = std::numeric_limits<u32>::max();
            u32 LastIndex = 0;
            std::string FirstPass;
            std::string LastPass;
            u32 ReadCount = 0;
            u32 WriteCount = 0;
        };

        std::unordered_map<std::string, LifetimeInfo> lifetimeByResource;
        std::unordered_map<std::string, std::unordered_set<std::string>> readModesByResource;
        std::unordered_map<std::string, std::unordered_set<std::string>> writeModesByResource;

        auto noteLifetimeAccess = [&lifetimeByResource](const std::string& resource,
                                                        const bool isWrite,
                                                        const std::string& passName,
                                                        const u32 passIndex)
        {
            if (resource.empty())
                return;

            auto& info = lifetimeByResource[resource];
            if (passIndex < info.FirstIndex)
            {
                info.FirstIndex = passIndex;
                info.FirstPass = passName;
            }
            if (passIndex >= info.LastIndex)
            {
                info.LastIndex = passIndex;
                info.LastPass = passName;
            }

            if (isWrite)
                ++info.WriteCount;
            else
                ++info.ReadCount;
        };

        for (u32 passIndex = 0; passIndex < static_cast<u32>(m_PassOrder.size()); ++passIndex)
        {
            const auto& passName = m_PassOrder[passIndex];

            auto passIt = m_PassLookup.find(passName);
            if (passIt != m_PassLookup.end() && passIt->second)
            {
                for (const auto& read : passIt->second->GetReads())
                {
                    noteLifetimeAccess(read.Name, false, passName, passIndex);
                }
                for (const auto& write : passIt->second->GetWrites())
                {
                    noteLifetimeAccess(write.Name, true, passName, passIndex);
                }
            }

            if (const auto declarationIt = m_PassAccessDeclarations.find(passName);
                declarationIt != m_PassAccessDeclarations.end())
            {
                for (const auto& access : declarationIt->second)
                {
                    noteLifetimeAccess(access.ResourceName, access.IsWrite, passName, passIndex);
                    if (access.IsWrite)
                        writeModesByResource[access.ResourceName].insert(writeUsageToString(access.WriteUsage));
                    else
                        readModesByResource[access.ResourceName].insert(readUsageToString(access.ReadUsage));
                }
            }
        }

        f64 totalCpuMs = 0.0;
        f64 maxCpuMs = 0.0;
        std::string maxPassName;
        std::unordered_map<std::string, f64> cpuMsByPass;
        cpuMsByPass.reserve(m_LastPassTimings.size());
        for (const auto& timing : m_LastPassTimings)
        {
            totalCpuMs += timing.CpuMs;
            cpuMsByPass[timing.PassName] = timing.CpuMs;
            if (timing.CpuMs > maxCpuMs)
            {
                maxCpuMs = timing.CpuMs;
                maxPassName = timing.PassName;
            }
        }
        const std::unordered_set<std::string> culledPasses(m_CulledPasses.begin(), m_CulledPasses.end());

        std::unordered_map<std::string, u32> passOrderIndexByName;
        passOrderIndexByName.reserve(m_PassOrder.size());
        for (sizet i = 0; i < m_PassOrder.size(); ++i)
            passOrderIndexByName[m_PassOrder[i]] = static_cast<u32>(i);

        std::vector<std::string> timingDigestEntries;
        timingDigestEntries.reserve(m_PassOrder.size());
        std::string timingDigestConcat;

        std::vector<std::string> resourceDigestEntries;
        std::string resourceDigestConcat;
        u32 resourceDigestAccessCount = 0;

        std::vector<std::string> barrierDigestEntries;
        std::string barrierDigestConcat;
        u32 barrierFlagsOr = 0;
        u32 missingProducerCount = 0;
        u32 culledProducerCount = 0;
        u32 unmappedTransitionCount = 0;
        u32 staleExtractionCount = 0;
        u32 extractionOfCulledCount = 0;

        const auto executedPassCount = m_LastPassTimings.size();
        const auto averageCpuMs = executedPassCount > 0
                                      ? (totalCpuMs / static_cast<f64>(executedPassCount))
                                      : 0.0;

        for (sizet i = 0; i < m_PassOrder.size(); ++i)
        {
            const auto& passName = m_PassOrder[i];
            const auto timingIt = cpuMsByPass.find(passName);
            const auto executed = timingIt != cpuMsByPass.end();
            const auto cpuMs = executed ? timingIt->second : 0.0;
            const auto cpuUs = static_cast<i64>((cpuMs * 1000.0) + 0.5);

            std::string entry = std::string(passName) + "@" + std::to_string(i) + "=" + std::to_string(cpuUs);
            if (!timingDigestConcat.empty())
                timingDigestConcat += "|";
            timingDigestConcat += entry;
            timingDigestEntries.push_back(std::move(entry));
        }

        {
            std::vector<std::string> descriptorEntries;
            descriptorEntries.reserve(m_RegisteredResources.size());
            for (const auto& resource : m_RegisteredResources)
            {
                descriptorEntries.push_back(std::string("res:") + std::string(resource.Name) + ":" + std::string(ToString(resource.Desc.Kind)) +
                                            ":" + (resource.Desc.Imported ? "1" : "0"));
            }
            std::sort(descriptorEntries.begin(), descriptorEntries.end());
            resourceDigestEntries.insert(resourceDigestEntries.end(), descriptorEntries.begin(), descriptorEntries.end());

            std::vector<std::string> lifetimeNames;
            lifetimeNames.reserve(lifetimeByResource.size());
            for (const auto& [resource, info] : lifetimeByResource)
                lifetimeNames.push_back(resource);
            std::sort(lifetimeNames.begin(), lifetimeNames.end());
            for (const auto& resource : lifetimeNames)
            {
                const auto& info = lifetimeByResource.at(resource);
                resourceDigestEntries.push_back(std::string("life:") + resource + "@" + std::to_string(info.FirstIndex) +
                                                "-" + std::to_string(info.LastIndex) + ":r" +
                                                std::to_string(info.ReadCount) + ":w" +
                                                std::to_string(info.WriteCount));
            }

            std::unordered_set<std::string> accessResourcesSet;
            for (const auto& [resource, modes] : readModesByResource)
                accessResourcesSet.insert(resource);
            for (const auto& [resource, modes] : writeModesByResource)
                accessResourcesSet.insert(resource);

            std::vector<std::string> accessResources;
            accessResources.reserve(accessResourcesSet.size());
            for (const auto& resource : accessResourcesSet)
                accessResources.push_back(resource);
            std::sort(accessResources.begin(), accessResources.end());
            resourceDigestAccessCount = static_cast<u32>(accessResources.size());

            for (const auto& resource : accessResources)
            {
                std::vector<std::string> readModes;
                if (const auto readsIt = readModesByResource.find(resource); readsIt != readModesByResource.end())
                {
                    readModes.assign(readsIt->second.begin(), readsIt->second.end());
                    std::sort(readModes.begin(), readModes.end());
                }

                std::vector<std::string> writeModes;
                if (const auto writesIt = writeModesByResource.find(resource); writesIt != writeModesByResource.end())
                {
                    writeModes.assign(writesIt->second.begin(), writesIt->second.end());
                    std::sort(writeModes.begin(), writeModes.end());
                }

                std::string accessEntry = std::string("acc:") + resource + ":r[";
                for (sizet i = 0; i < readModes.size(); ++i)
                {
                    accessEntry += readModes[i];
                    if (i + 1 < readModes.size())
                        accessEntry += ",";
                }
                accessEntry += "]:w[";
                for (sizet i = 0; i < writeModes.size(); ++i)
                {
                    accessEntry += writeModes[i];
                    if (i + 1 < writeModes.size())
                        accessEntry += ",";
                }
                accessEntry += "]";
                resourceDigestEntries.push_back(std::move(accessEntry));
            }

            for (const auto& plan : m_TransientPlan)
            {
                resourceDigestEntries.push_back(std::string("alias:") + plan.Resource + ":" + plan.AliasGroup +
                                                ":slot=" +
                                                (plan.AliasSlot == std::numeric_limits<u32>::max()
                                                     ? std::string("-1")
                                                     : std::to_string(plan.AliasSlot)) +
                                                ":reachable=" + (plan.Reachable ? "1" : "0") +
                                                ":alloc=" + (plan.WillAllocate ? "1" : "0") +
                                                ":skip=" + plan.SkipReason);
            }

            for (const auto& entry : resourceDigestEntries)
            {
                if (!resourceDigestConcat.empty())
                    resourceDigestConcat += "|";
                resourceDigestConcat += entry;
            }
        }

        {
            struct BarrierDigestRow
            {
                std::string BeforePass;
                std::string Resource;
                u32 Flags = 0;
            };

            std::vector<BarrierDigestRow> barrierRows;
            barrierRows.reserve(m_PlannedBarriers.size());
            for (const auto& barrier : m_PlannedBarriers)
            {
                const auto flags = static_cast<u32>(barrier.Flags);
                barrierRows.push_back({ barrier.BeforePass, barrier.Resource, flags });
                barrierFlagsOr |= flags;
            }
            std::sort(barrierRows.begin(), barrierRows.end(),
                      [](const BarrierDigestRow& lhs, const BarrierDigestRow& rhs)
                      {
                          if (lhs.BeforePass != rhs.BeforePass)
                              return lhs.BeforePass < rhs.BeforePass;
                          if (lhs.Resource != rhs.Resource)
                              return lhs.Resource < rhs.Resource;
                          return lhs.Flags < rhs.Flags;
                      });

            barrierDigestEntries.reserve(m_PlannedBarriers.size() + m_BarrierDiagnostics.size());
            for (const auto& row : barrierRows)
            {
                barrierDigestEntries.push_back(std::string("bar:") + row.BeforePass + "/" + row.Resource +
                                               "/" + std::to_string(row.Flags));
            }

            struct DiagnosticDigestRow
            {
                std::string Kind;
                std::string Pass;
                std::string Resource;
            };

            std::vector<DiagnosticDigestRow> diagnosticRows;
            diagnosticRows.reserve(m_BarrierDiagnostics.size());
            for (const auto& diagnostic : m_BarrierDiagnostics)
            {
                const auto kind = std::string(barrierDiagnosticKindToString(diagnostic.Kind));
                diagnosticRows.push_back({ kind, diagnostic.PassName, diagnostic.Resource });
                switch (diagnostic.Kind)
                {
                    case BarrierDiagnosticKind::MissingProducer:
                        ++missingProducerCount;
                        break;
                    case BarrierDiagnosticKind::CulledProducer:
                        ++culledProducerCount;
                        break;
                    case BarrierDiagnosticKind::UnmappedTransition:
                        ++unmappedTransitionCount;
                        break;
                    case BarrierDiagnosticKind::StaleExtractionHandle:
                        ++staleExtractionCount;
                        break;
                    case BarrierDiagnosticKind::ExtractionOfCulledResource:
                        ++extractionOfCulledCount;
                        break;
                    case BarrierDiagnosticKind::InvalidHistoryContract:
                        break;
                    default:
                        break;
                }
            }
            std::sort(diagnosticRows.begin(), diagnosticRows.end(),
                      [](const DiagnosticDigestRow& lhs, const DiagnosticDigestRow& rhs)
                      {
                          if (lhs.Kind != rhs.Kind)
                              return lhs.Kind < rhs.Kind;
                          if (lhs.Pass != rhs.Pass)
                              return lhs.Pass < rhs.Pass;
                          return lhs.Resource < rhs.Resource;
                      });
            for (const auto& row : diagnosticRows)
            {
                barrierDigestEntries.push_back(std::string("diag:") + row.Kind + "/" + row.Pass + "/" + row.Resource);
            }

            for (const auto& entry : barrierDigestEntries)
            {
                if (!barrierDigestConcat.empty())
                    barrierDigestConcat += "|";
                barrierDigestConcat += entry;
            }
        }

        // Count compute/async-compute passes for frameSummary + graphDigest.
        u32 computePassCount = 0;
        u32 asyncComputeCandidateCount = 0;
        u32 historyResourceCount = 0;
        for (const auto& passName : m_PassOrder)
        {
            const auto passIt = m_PassLookup.find(passName);
            if (passIt == m_PassLookup.end() || !passIt->second)
                continue;
            if (passIt->second->GetPassWorkType() == RenderPass::PassWorkType::Compute)
                ++computePassCount;
            if (passIt->second->IsAsyncComputeCandidate())
                ++asyncComputeCandidateCount;
        }
        for (const auto& resource : m_RegisteredResources)
        {
            if (IsHistoryTextureResource(resource.Name))
                ++historyResourceCount;
        }

        // Batch resource dependency counts for frameSummary/graphDigest.
        const auto dumpBatches = GetAsyncComputeBatches();
        const auto submissionPlan = GetSubmissionPlan();
        u32 batchInputResourceCount = 0;
        u32 batchOutputResourceCount = 0;
        for (const auto& batch : dumpBatches)
        {
            batchInputResourceCount += static_cast<u32>(batch.InputResources.size());
            batchOutputResourceCount += static_cast<u32>(batch.OutputResources.size());
        }

        // Resource transition records for frameSummary/graphDigest.
        const auto dumpTransitions = GetResourceTransitions();
        const auto resourceTransitionCount = static_cast<u32>(dumpTransitions.size());
        // Count cross-lane sync transitions.
        const auto crossLaneSyncCount = static_cast<u32>(std::count_if(
            dumpTransitions.begin(), dumpTransitions.end(),
            [](const ResourceTransition& tr)
            { return tr.IsCrossLane; }));

        // Unified resource lifetime records.
        const auto dumpLifetimes = GetResourceLifetimes();
        const auto resourceLifetimeCount = static_cast<u32>(dumpLifetimes.size());
        u32 fallbackActivationCount = 0;
        for (const auto& activation : m_FallbackActivations)
            fallbackActivationCount += activation.Count;

        const auto submissionCommandKindToString = [](const SubmissionCommand::Kind kind)
        {
            switch (kind)
            {
                case SubmissionCommand::Kind::Pass:
                    return "Pass";
                case SubmissionCommand::Kind::MemoryBarrier:
                    return "MemoryBarrier";
                case SubmissionCommand::Kind::BatchBegin:
                    return "BatchBegin";
                case SubmissionCommand::Kind::BatchEnd:
                    return "BatchEnd";
                default:
                    return "Unknown";
            }
        };

        out << "{\n";
        out << "  \"schemaVersion\": 13,\n";
        out << "  \"timingVersion\": 4,\n";
        out << "  \"finalPass\": \"" << jsonEscape(m_FinalPassName) << "\",\n";
        out << "  \"hasExplicitFinalPass\": " << (m_HasExplicitFinalPass ? "true" : "false") << ",\n";
        out << "  \"hasTimings\": " << (m_LastPassTimings.empty() ? "false" : "true") << ",\n";
        out << "  \"frameSummary\": { "
            << "\"passCount\": " << m_PassOrder.size()
            << ", \"resourceCount\": " << m_RegisteredResources.size()
            << ", \"culledPassCount\": " << m_CulledPasses.size()
            << ", \"plannedBarrierCount\": " << m_PlannedBarriers.size()
            << ", \"barrierDiagnosticCount\": " << m_BarrierDiagnostics.size()
            << ", \"transientAliasCount\": " << m_TransientPlan.size()
            << ", \"timingsCount\": " << executedPassCount
            << ", \"computePassCount\": " << computePassCount
            << ", \"asyncComputeCandidateCount\": " << asyncComputeCandidateCount
            << ", \"historyResourceCount\": " << historyResourceCount
            << ", \"temporalHistoryContractCount\": " << m_TemporalHistoryContracts.size()
            << ", \"asyncBatchCount\": " << dumpBatches.size()
            << ", \"batchInputResourceCount\": " << batchInputResourceCount
            << ", \"batchOutputResourceCount\": " << batchOutputResourceCount
            << ", \"submissionCommandCount\": " << submissionPlan.size()
            << ", \"resourceTransitionCount\": " << resourceTransitionCount
            << ", \"crossLaneSyncCount\": " << crossLaneSyncCount
            << ", \"resourceLifetimeCount\": " << resourceLifetimeCount
            << ", \"fallbackActivationCount\": " << fallbackActivationCount
            << " },\n";
        out << "  \"buildStats\": { "
            << "\"passesVisited\": " << m_LastBuildStats.PassesVisited
            << ", \"declaredReads\": " << m_LastBuildStats.DeclaredReads
            << ", \"declaredWrites\": " << m_LastBuildStats.DeclaredWrites
            << ", \"derivedEdges\": " << m_LastBuildStats.DerivedEdges
            << " },\n";

        out << "  \"passOrder\": [";
        for (sizet i = 0; i < m_PassOrder.size(); ++i)
        {
            out << "\"" << jsonEscape(m_PassOrder[i]) << "\"";
            if (i + 1 < m_PassOrder.size())
                out << ", ";
        }
        out << "],\n";

        out << "  \"culledPasses\": [";
        for (sizet i = 0; i < m_CulledPasses.size(); ++i)
        {
            out << "\"" << jsonEscape(m_CulledPasses[i]) << "\"";
            if (i + 1 < m_CulledPasses.size())
                out << ", ";
        }
        out << "],\n";

        // Per-pass work-type and async-compute flags.
        out << "  \"passFlags\": [\n";
        for (sizet i = 0; i < m_PassOrder.size(); ++i)
        {
            const auto& passName = m_PassOrder[i];
            const auto passIt = m_PassLookup.find(passName);
            const auto workType = (passIt != m_PassLookup.end() && passIt->second)
                                      ? passWorkTypeToString(passIt->second->GetPassWorkType())
                                      : "Graphics";
            const auto asyncCandidate = (passIt != m_PassLookup.end() && passIt->second) &&
                                        passIt->second->IsAsyncComputeCandidate();
            out << "    { \"pass\": \"" << jsonEscape(passName)
                << "\", \"workType\": \"" << workType
                << "\", \"asyncComputeCandidate\": " << (asyncCandidate ? "true" : "false") << " }";
            if (i + 1 < m_PassOrder.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"plannedBarriers\": [\n";
        for (sizet i = 0; i < m_PlannedBarriers.size(); ++i)
        {
            const auto& barrier = m_PlannedBarriers[i];
            out << "    { \"beforePass\": \"" << jsonEscape(barrier.BeforePass)
                << "\", \"resource\": \"" << jsonEscape(barrier.Resource)
                << "\", \"flags\": " << static_cast<u32>(barrier.Flags)
                << ", \"range\": " << subresourceRangeToJson(barrier.Range) << " }";
            if (i + 1 < m_PlannedBarriers.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"barrierDiagnostics\": [\n";
        for (sizet i = 0; i < m_BarrierDiagnostics.size(); ++i)
        {
            const auto& diagnostic = m_BarrierDiagnostics[i];
            out << "    { \"kind\": \"" << barrierDiagnosticKindToString(diagnostic.Kind)
                << "\", \"pass\": \"" << jsonEscape(diagnostic.PassName)
                << "\", \"resource\": \"" << jsonEscape(diagnostic.Resource)
                << "\", \"message\": \"" << jsonEscape(diagnostic.Message) << "\" }";
            if (i + 1 < m_BarrierDiagnostics.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"resources\": [\n";
        for (sizet i = 0; i < m_RegisteredResources.size(); ++i)
        {
            const auto& resource = m_RegisteredResources[i];
            u32 textureID = 0;
            u32 bufferID = 0;
            u32 framebufferID = 0;
            u32 framebufferColor0ID = 0;
            if (resource.TextureHandle.IsValid())
                textureID = ResolveTexture(resource.TextureHandle);
            if (resource.BufferHandle.IsValid())
                bufferID = ResolveBuffer(resource.BufferHandle);
            if (resource.FramebufferHandle.IsValid())
            {
                if (auto fb = ResolveFramebuffer(resource.FramebufferHandle))
                {
                    framebufferID = fb->GetRendererID();
                    framebufferColor0ID = fb->GetColorAttachmentRendererID(0);
                }
            }
            out << "    { \"name\": \"" << jsonEscape(resource.Name)
                << "\", \"kind\": \"" << ToString(resource.Desc.Kind)
                << "\", \"imported\": " << (resource.Desc.Imported ? "true" : "false")
                << ", \"isHistory\": " << (IsHistoryTextureResource(resource.Name) ? "true" : "false")
                << ", \"textureID\": " << textureID
                << ", \"bufferID\": " << bufferID
                << ", \"framebufferID\": " << framebufferID
                << ", \"framebufferColor0ID\": " << framebufferColor0ID << " }";
            if (i + 1 < m_RegisteredResources.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"temporalHistoryContracts\": [\n";
        for (sizet i = 0; i < m_TemporalHistoryContracts.size(); ++i)
        {
            const auto& contract = m_TemporalHistoryContracts[i];
            out << "    { \"historyResource\": \"" << jsonEscape(contract.HistoryResource)
                << "\", \"sourceResource\": \"" << jsonEscape(contract.SourceResource)
                << "\", \"historyImported\": " << (contract.HistoryImported ? "true" : "false")
                << ", \"sourceReachable\": " << (contract.SourceReachable ? "true" : "false") << " }";
            if (i + 1 < m_TemporalHistoryContracts.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"lifetimes\": [\n";
        {
            std::vector<std::string> resourceNames;
            resourceNames.reserve(lifetimeByResource.size());
            for (const auto& [resource, info] : lifetimeByResource)
                resourceNames.push_back(resource);
            std::sort(resourceNames.begin(), resourceNames.end());

            for (sizet i = 0; i < resourceNames.size(); ++i)
            {
                const auto& resource = resourceNames[i];
                const auto& info = lifetimeByResource.at(resource);
                out << "    { \"resource\": \"" << jsonEscape(resource)
                    << "\", \"firstPass\": \"" << jsonEscape(info.FirstPass)
                    << "\", \"lastPass\": \"" << jsonEscape(info.LastPass)
                    << "\", \"firstIndex\": " << info.FirstIndex
                    << ", \"lastIndex\": " << info.LastIndex
                    << ", \"readCount\": " << info.ReadCount
                    << ", \"writeCount\": " << info.WriteCount << " }";
                if (i + 1 < resourceNames.size())
                    out << ",";
                out << "\n";
            }
        }
        out << "  ],\n";

        out << "  \"accessModes\": [\n";
        {
            std::unordered_set<std::string> allResources;
            for (const auto& [resource, modes] : readModesByResource)
                allResources.insert(resource);
            for (const auto& [resource, modes] : writeModesByResource)
                allResources.insert(resource);

            std::vector<std::string> resources;
            resources.reserve(allResources.size());
            for (const auto& resource : allResources)
                resources.push_back(resource);
            std::sort(resources.begin(), resources.end());

            for (sizet i = 0; i < resources.size(); ++i)
            {
                const auto& resource = resources[i];
                out << "    { \"resource\": \"" << jsonEscape(resource) << "\", \"reads\": [";

                std::vector<std::string> readModes;
                if (const auto readsIt = readModesByResource.find(resource); readsIt != readModesByResource.end())
                {
                    readModes.assign(readsIt->second.begin(), readsIt->second.end());
                    std::sort(readModes.begin(), readModes.end());
                }
                for (sizet modeIdx = 0; modeIdx < readModes.size(); ++modeIdx)
                {
                    out << "\"" << jsonEscape(readModes[modeIdx]) << "\"";
                    if (modeIdx + 1 < readModes.size())
                        out << ", ";
                }

                out << "], \"writes\": [";
                std::vector<std::string> writeModes;
                if (const auto writesIt = writeModesByResource.find(resource); writesIt != writeModesByResource.end())
                {
                    writeModes.assign(writesIt->second.begin(), writesIt->second.end());
                    std::sort(writeModes.begin(), writeModes.end());
                }
                for (sizet modeIdx = 0; modeIdx < writeModes.size(); ++modeIdx)
                {
                    out << "\"" << jsonEscape(writeModes[modeIdx]) << "\"";
                    if (modeIdx + 1 < writeModes.size())
                        out << ", ";
                }

                out << "] }";
                if (i + 1 < resources.size())
                    out << ",";
                out << "\n";
            }
        }
        out << "  ],\n";

        out << "  \"aliases\": [\n";
        for (sizet i = 0; i < m_TransientPlan.size(); ++i)
        {
            const auto& plan = m_TransientPlan[i];
            out << "    { \"resource\": \"" << jsonEscape(plan.Resource)
                << "\", \"kind\": \"" << ToString(plan.Kind)
                << "\", \"aliasGroup\": \"" << jsonEscape(plan.AliasGroup)
                << "\", \"aliasSlot\": "
                << (plan.AliasSlot == std::numeric_limits<u32>::max() ? -1 : static_cast<i64>(plan.AliasSlot))
                << ", \"reachable\": " << (plan.Reachable ? "true" : "false")
                << ", \"willAllocate\": " << (plan.WillAllocate ? "true" : "false")
                << ", \"skipReason\": \"" << jsonEscape(plan.SkipReason)
                << "\", \"firstPass\": \"" << jsonEscape(plan.FirstPass)
                << "\", \"lastPass\": \"" << jsonEscape(plan.LastPass)
                << "\", \"firstIndex\": "
                << (plan.FirstPassIndex == std::numeric_limits<u32>::max() ? -1 : static_cast<i64>(plan.FirstPassIndex))
                << ", \"lastIndex\": "
                << (plan.FirstPassIndex == std::numeric_limits<u32>::max() ? -1 : static_cast<i64>(plan.LastPassIndex))
                << ", \"estimatedBytes\": " << plan.EstimatedBytes
                << " }";
            if (i + 1 < m_TransientPlan.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"timingSummary\": { "
            << "\"executedPasses\": " << executedPassCount
            << ", \"culledPasses\": " << m_CulledPasses.size()
            << ", \"totalCpuMs\": " << totalCpuMs
            << ", \"averageCpuMs\": " << averageCpuMs
            << ", \"maxCpuMs\": " << maxCpuMs
            << ", \"maxPass\": \"" << jsonEscape(maxPassName)
            << "\" },\n";

        out << "  \"executionTimeline\": [\n";
        for (sizet i = 0; i < m_PassOrder.size(); ++i)
        {
            const auto& passName = m_PassOrder[i];
            const auto isCulled = culledPasses.contains(passName);
            const auto timingIt = cpuMsByPass.find(passName);
            const auto executed = timingIt != cpuMsByPass.end();
            const auto cpuMs = executed ? timingIt->second : 0.0;
            const auto passIt = m_PassLookup.find(passName);
            const auto workType = (passIt != m_PassLookup.end() && passIt->second)
                                      ? passWorkTypeToString(passIt->second->GetPassWorkType())
                                      : "Graphics";
            const auto asyncCandidate = (passIt != m_PassLookup.end() && passIt->second) &&
                                        passIt->second->IsAsyncComputeCandidate();

            out << "    { \"pass\": \"" << jsonEscape(passName)
                << "\", \"orderIndex\": " << i
                << ", \"culled\": " << (isCulled ? "true" : "false")
                << ", \"executed\": " << (executed ? "true" : "false")
                << ", \"cpuMs\": " << cpuMs
                << ", \"workType\": \"" << workType << "\""
                << ", \"asyncComputeCandidate\": " << (asyncCandidate ? "true" : "false") << " }";
            if (i + 1 < m_PassOrder.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"timingStatsByPass\": {\n";
        for (sizet i = 0; i < m_PassOrder.size(); ++i)
        {
            const auto& passName = m_PassOrder[i];
            const auto isCulled = culledPasses.contains(passName);
            const auto timingIt = cpuMsByPass.find(passName);
            const auto executed = timingIt != cpuMsByPass.end();
            const auto cpuMs = executed ? timingIt->second : 0.0;
            const auto passIt = m_PassLookup.find(passName);
            const auto workType = (passIt != m_PassLookup.end() && passIt->second)
                                      ? passWorkTypeToString(passIt->second->GetPassWorkType())
                                      : "Graphics";
            const auto asyncCandidate = (passIt != m_PassLookup.end() && passIt->second) &&
                                        passIt->second->IsAsyncComputeCandidate();

            out << "    \"" << jsonEscape(passName)
                << "\": { \"orderIndex\": " << i
                << ", \"executed\": " << (executed ? "true" : "false")
                << ", \"culled\": " << (isCulled ? "true" : "false")
                << ", \"cpuMs\": " << cpuMs
                << ", \"workType\": \"" << workType << "\""
                << ", \"asyncComputeCandidate\": " << (asyncCandidate ? "true" : "false") << " }";
            if (i + 1 < m_PassOrder.size())
                out << ",";
            out << "\n";
        }
        out << "  },\n";

        out << "  \"timingDigest\": { "
            << "\"version\": 1"
            << ", \"unit\": \"cpuUs\""
            << ", \"entryCount\": " << timingDigestEntries.size()
            << ", \"concat\": \"" << jsonEscape(timingDigestConcat)
            << "\" },\n";

        out << "  \"resourceDigest\": { "
            << "\"version\": 1"
            << ", \"entryCount\": " << resourceDigestEntries.size()
            << ", \"resourceCount\": " << m_RegisteredResources.size()
            << ", \"lifetimeCount\": " << lifetimeByResource.size()
            << ", \"accessCount\": " << resourceDigestAccessCount
            << ", \"aliasCount\": " << m_TransientPlan.size()
            << ", \"concat\": \"" << jsonEscape(resourceDigestConcat)
            << "\" },\n";

        out << "  \"barrierDigest\": { "
            << "\"version\": 1"
            << ", \"plannedCount\": " << m_PlannedBarriers.size()
            << ", \"diagnosticCount\": " << m_BarrierDiagnostics.size()
            << ", \"missingProducerCount\": " << missingProducerCount
            << ", \"culledProducerCount\": " << culledProducerCount
            << ", \"unmappedTransitionCount\": " << unmappedTransitionCount
            << ", \"staleExtractionCount\": " << staleExtractionCount
            << ", \"extractionOfCulledCount\": " << extractionOfCulledCount
            << ", \"flagsOr\": " << barrierFlagsOr
            << ", \"entryCount\": " << barrierDigestEntries.size()
            << ", \"concat\": \"" << jsonEscape(barrierDigestConcat)
            << "\" },\n";

        const auto graphDigestConcat = std::string("passes=") + std::to_string(m_PassOrder.size()) +
                                       ";resources=" + std::to_string(m_RegisteredResources.size()) +
                                       ";culled=" + std::to_string(m_CulledPasses.size()) +
                                       ";barriers=" + std::to_string(m_PlannedBarriers.size()) +
                                       ";diags=" + std::to_string(m_BarrierDiagnostics.size()) +
                                       ";aliases=" + std::to_string(m_TransientPlan.size()) +
                                       ";timings=" + std::to_string(m_LastPassTimings.size()) +
                                       ";compute=" + std::to_string(computePassCount) +
                                       ";asyncCandidates=" + std::to_string(asyncComputeCandidateCount) +
                                       ";histories=" + std::to_string(historyResourceCount) +
                                       ";historyContracts=" + std::to_string(m_TemporalHistoryContracts.size()) +
                                       ";batches=" + std::to_string(dumpBatches.size()) +
                                       ";batchInputResources=" + std::to_string(batchInputResourceCount) +
                                       ";batchOutputResources=" + std::to_string(batchOutputResourceCount) +
                                       ";submissionCommands=" + std::to_string(submissionPlan.size()) +
                                       ";transitions=" + std::to_string(resourceTransitionCount) +
                                       ";crossLaneSync=" + std::to_string(crossLaneSyncCount) +
                                       ";lifetimes=" + std::to_string(resourceLifetimeCount) +
                                       ";subresourceRanges=present" +
                                       ";fallbacks=" + std::to_string(fallbackActivationCount);

        out << "  \"graphDigest\": { "
            << "\"version\": 1"
            << ", \"concat\": \"" << jsonEscape(graphDigestConcat)
            << "\" },\n";

        // Async-compute batches with cross-boundary resource deps.
        out << "  \"asyncBatches\": [\n";
        for (sizet bi = 0; bi < dumpBatches.size(); ++bi)
        {
            const auto& batch = dumpBatches[bi];
            out << "    {\n";
            out << "      \"lane\": \"" << queueLaneToString(batch.Lane) << "\",\n";

            // ComputePasses array
            out << "      \"computePasses\": [";
            for (sizet pi = 0; pi < batch.ComputePasses.size(); ++pi)
            {
                out << "\"" << jsonEscape(batch.ComputePasses[pi]) << "\"";
                if (pi + 1 < batch.ComputePasses.size())
                    out << ", ";
            }
            out << "],\n";

            // WaitPasses array
            out << "      \"waitPasses\": [";
            for (sizet pi = 0; pi < batch.WaitPasses.size(); ++pi)
            {
                out << "\"" << jsonEscape(batch.WaitPasses[pi]) << "\"";
                if (pi + 1 < batch.WaitPasses.size())
                    out << ", ";
            }
            out << "],\n";

            // SignalPasses array
            out << "      \"signalPasses\": [";
            for (sizet pi = 0; pi < batch.SignalPasses.size(); ++pi)
            {
                out << "\"" << jsonEscape(batch.SignalPasses[pi]) << "\"";
                if (pi + 1 < batch.SignalPasses.size())
                    out << ", ";
            }
            out << "],\n";

            // InputResources array
            out << "      \"inputResources\": [\n";
            for (sizet ri = 0; ri < batch.InputResources.size(); ++ri)
            {
                const auto& dep = batch.InputResources[ri];
                out << "        { \"resource\": \"" << jsonEscape(dep.ResourceName)
                    << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalPass) << "\" }";
                if (ri + 1 < batch.InputResources.size())
                    out << ",";
                out << "\n";
            }
            out << "      ],\n";

            // OutputResources array
            out << "      \"outputResources\": [\n";
            for (sizet ri = 0; ri < batch.OutputResources.size(); ++ri)
            {
                const auto& dep = batch.OutputResources[ri];
                out << "        { \"resource\": \"" << jsonEscape(dep.ResourceName)
                    << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalPass) << "\" }";
                if (ri + 1 < batch.OutputResources.size())
                    out << ",";
                out << "\n";
            }
            out << "      ]\n";

            out << "    }";
            if (bi + 1 < dumpBatches.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // Pre-linearized submission command stream with
        // batch sync/resource metadata.
        out << "  \"submissionPlan\": [\n";
        for (sizet ci = 0; ci < submissionPlan.size(); ++ci)
        {
            const auto& cmd = submissionPlan[ci];
            out << "    { \"kind\": \"" << submissionCommandKindToString(cmd.CommandKind)
                << "\", \"lane\": \"" << queueLaneToString(cmd.Lane) << "\"";

            if (cmd.CommandKind == SubmissionCommand::Kind::Pass)
            {
                out << ", \"pass\": \"" << jsonEscape(cmd.PassName)
                    << "\", \"workType\": \"" << passWorkTypeToString(cmd.WorkType) << "\"";
            }
            else if (cmd.CommandKind == SubmissionCommand::Kind::MemoryBarrier)
            {
                out << ", \"flags\": " << static_cast<u32>(cmd.Barriers);
            }
            else if (cmd.CommandKind == SubmissionCommand::Kind::BatchBegin ||
                     cmd.CommandKind == SubmissionCommand::Kind::BatchEnd)
            {
                out << ", \"batchIndex\": " << cmd.BatchIndex;

                out << ", \"waitPasses\": [";
                for (sizet i = 0; i < cmd.WaitPasses.size(); ++i)
                {
                    out << "\"" << jsonEscape(cmd.WaitPasses[i]) << "\"";
                    if (i + 1 < cmd.WaitPasses.size())
                        out << ", ";
                }
                out << "]";

                out << ", \"signalPasses\": [";
                for (sizet i = 0; i < cmd.SignalPasses.size(); ++i)
                {
                    out << "\"" << jsonEscape(cmd.SignalPasses[i]) << "\"";
                    if (i + 1 < cmd.SignalPasses.size())
                        out << ", ";
                }
                out << "]";

                out << ", \"inputResources\": [";
                for (sizet i = 0; i < cmd.InputResources.size(); ++i)
                {
                    const auto& dep = cmd.InputResources[i];
                    out << "{ \"resource\": \"" << jsonEscape(dep.ResourceName)
                        << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalPass) << "\" }";
                    if (i + 1 < cmd.InputResources.size())
                        out << ", ";
                }
                out << "]";

                out << ", \"outputResources\": [";
                for (sizet i = 0; i < cmd.OutputResources.size(); ++i)
                {
                    const auto& dep = cmd.OutputResources[i];
                    out << "{ \"resource\": \"" << jsonEscape(dep.ResourceName)
                        << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalPass) << "\" }";
                    if (i + 1 < cmd.OutputResources.size())
                        out << ", ";
                }
                out << "]";
            }

            out << " }";
            if (ci + 1 < submissionPlan.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // Resource transition records.
        out << "  \"resourceTransitions\": [\n";
        for (sizet ri = 0; ri < dumpTransitions.size(); ++ri)
        {
            const auto& tr = dumpTransitions[ri];
            out << "    { \"resource\": \"" << jsonEscape(tr.ResourceName)
                << "\", \"producerPass\": \"" << jsonEscape(tr.ProducerPass)
                << "\", \"consumerPass\": \"" << jsonEscape(tr.ConsumerPass)
                << "\", \"fromUsage\": \"" << writeUsageToString(tr.FromUsage)
                << "\", \"toUsage\": \"" << readUsageToString(tr.ToUsage)
                << "\", \"flags\": " << static_cast<u32>(tr.Flags)
                << ", \"range\": " << subresourceRangeToJson(tr.Range)
                << ", \"isCrossLane\": " << (tr.IsCrossLane ? "true" : "false")
                << ", \"producerLane\": \"" << queueLaneToString(tr.ProducerLane)
                << "\", \"consumerLane\": \"" << queueLaneToString(tr.ConsumerLane)
                << "\" }";
            if (ri + 1 < dumpTransitions.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // Resource lifetime records.
        out << "  \"resourceLifetimes\": [\n";
        for (sizet li = 0; li < dumpLifetimes.size(); ++li)
        {
            const auto& lt = dumpLifetimes[li];
            out << "    { \"resource\": \"" << jsonEscape(lt.ResourceName)
                << "\", \"isImported\": " << (lt.IsImported ? "true" : "false")
                << ", \"isExtracted\": " << (lt.IsExtracted ? "true" : "false")
                << ", \"isHistory\": " << (lt.IsHistory ? "true" : "false")
                << ", \"isTransient\": " << (lt.IsTransient ? "true" : "false")
                << ", \"firstWritePassIndex\": "
                << (lt.FirstWritePassIndex == std::numeric_limits<u32>::max()
                        ? -1
                        : static_cast<i64>(lt.FirstWritePassIndex))
                << ", \"lastReadPassIndex\": "
                << (lt.LastReadPassIndex == std::numeric_limits<u32>::max()
                        ? -1
                        : static_cast<i64>(lt.LastReadPassIndex))
                << ", \"firstWritePass\": \"" << jsonEscape(lt.FirstWritePass)
                << "\", \"lastReadPass\": \"" << jsonEscape(lt.LastReadPass)
                << "\", \"firstWriteUsage\": \"" << writeUsageToString(lt.FirstWriteUsage)
                << "\", \"lastReadUsage\": \"" << readUsageToString(lt.LastReadUsage)
                << "\" }";
            if (li + 1 < dumpLifetimes.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"fallbackActivations\": [\n";
        for (sizet i = 0; i < m_FallbackActivations.size(); ++i)
        {
            const auto& activation = m_FallbackActivations[i];
            out << "    { \"pass\": \"" << jsonEscape(activation.PassName)
                << "\", \"reason\": \"" << jsonEscape(activation.Reason)
                << "\", \"count\": " << activation.Count << " }";
            if (i + 1 < m_FallbackActivations.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"timings\": [\n";
        for (sizet i = 0; i < m_LastPassTimings.size(); ++i)
        {
            const auto& timing = m_LastPassTimings[i];
            const auto passOrderIt = passOrderIndexByName.find(timing.PassName);
            const auto orderIndex = passOrderIt != passOrderIndexByName.end()
                                        ? static_cast<i64>(passOrderIt->second)
                                        : -1;
            out << "    { \"pass\": \"" << jsonEscape(timing.PassName)
                << "\", \"orderIndex\": " << orderIndex
                << ", \"cpuMs\": " << timing.CpuMs << " }";
            if (i + 1 < m_LastPassTimings.size())
                out << ",";
            out << "\n";
        }
        out << "  ]\n";
        out << "}\n";
        out.close();

        OLO_CORE_INFO("RenderGraph::DumpToJson: wrote {} passes and {} resources to '{}'",
                      m_PassOrder.size(), m_RegisteredResources.size(), filePath);
        return true;
    }
    // -------------------------------------------------------------------
    // Phase C — Graph-native pass registration and per-frame building
    // -------------------------------------------------------------------

    void RenderGraph::RegisterGraphPass(
        std::string_view name,
        PassSetupFn setup,
        PassExecuteFn execute)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(setup && execute, "Pass setup and execute callbacks must not be null");

        const auto duplicate = std::find_if(m_GraphPasses.begin(), m_GraphPasses.end(),
                                            [name](const GraphPass& pass)
                                            {
                                                return pass.Name == name;
                                            });
        OLO_CORE_ASSERT(duplicate == m_GraphPasses.end(), "Duplicate graph pass registration: {}", name);

        m_GraphPasses.push_back({ std::string(name), setup, execute });
        m_DependencyGraphDirty = true;
    }

    void RenderGraph::ClearGraphPasses()
    {
        m_GraphPasses.clear();
        m_PassAccessDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BarrierDiagnostics.clear();
        m_LastPassTimings.clear();
        m_TransientResourceDescs.clear();
        m_TransientPlan.clear();
        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::BuildFrameGraph()
    {
        OLO_PROFILE_FUNCTION();

        // Defensive: if a previous frame aborted after acquiring transient
        // resources, recycle them before compiling the next frame.
        m_TransientPool.ReleaseAll();
        m_TransientPool.Trim(m_TransientPoolMaxBucketSize);

        // Phase C stub: iterate registered graph passes and call their setup callbacks
        // to populate the builder. Full implementation will:
        //   1. Create an RGBuilder
        //   2. Call each pass's setup callback to record declarations
        //   3. Compile the dependency DAG
        //   4. Perform reachability analysis and culling (Phase D+)
        //   5. Allocate transient resources (Phase D)
        //   6. Generate barriers (Phase E)

        m_LastBuildStats = {};
        m_Dependencies = m_ExplicitDependencies;
        m_DependencyGraphDirty = true;
        m_PassAccessDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BarrierDiagnostics.clear();
        m_TemporalHistoryContracts.clear();

        RGBuilder builder(*this, m_Blackboard);
        std::unordered_map<std::string, std::string> lastWriterByResource;
        const auto graphPassCount = m_GraphPasses.size();
        lastWriterByResource.reserve(graphPassCount * 4u);

        std::unordered_map<std::string, GraphPass*> graphPassByName;
        graphPassByName.reserve(graphPassCount);
        for (auto& pass : m_GraphPasses)
            graphPassByName[pass.Name] = &pass;

        auto tryAddDerivedDependency = [this](const std::string& beforePass, const std::string& afterPass) -> bool
        {
            if (beforePass == afterPass)
                return false;
            if (!m_PassLookup.contains(beforePass) || !m_PassLookup.contains(afterPass))
            {
                OLO_CORE_WARN("tryAddDerivedDependency: pass not found in m_PassLookup: {} -> {}",
                              beforePass, afterPass);
                return false;
            }

            auto& deps = m_Dependencies[afterPass];
            if (std::find(deps.begin(), deps.end(), beforePass) != deps.end())
                return false;

            // Avoid introducing a derived edge that would close a cycle.
            // m_Dependencies stores incoming edges (consumer -> producers),
            // so adding beforePass -> afterPass is illegal if beforePass
            // already transitively depends on afterPass.
            std::unordered_set<std::string> visited;
            std::vector<std::string> frontier{ beforePass };
            while (!frontier.empty())
            {
                std::string current = std::move(frontier.back());
                frontier.pop_back();

                if (!visited.insert(current).second)
                    continue;

                if (current == afterPass)
                {
                    OLO_CORE_WARN("RenderGraph::BuildFrameGraph: skipping derived edge {} -> {} to avoid cycle",
                                  beforePass,
                                  afterPass);
                    return false;
                }

                const auto existingIt = m_Dependencies.find(current);
                if (existingIt == m_Dependencies.end())
                    continue;

                for (const auto& producer : existingIt->second)
                {
                    if (!visited.contains(producer))
                        frontier.push_back(producer);
                }
            }

            AddExecutionDependency(beforePass, afterPass, false);
            return true;
        };

        auto processGraphPass = [&](GraphPass& pass)
        {
            if (!pass.Setup)
                return;

            // Defensive: verify pass name is valid before processing
            if (pass.Name.empty())
            {
                OLO_CORE_ERROR("processGraphPass: encountered pass with empty name");
                return;
            }

            ++m_LastBuildStats.PassesVisited;

            builder.BeginPass(pass.Name);

            // Call setup callback to record declarations
            try
            {
                pass.Setup(builder);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("processGraphPass: exception during setup callback for pass '{}': {}",
                               pass.Name, e.what());
                return;
            }

            const auto& reads = builder.GetDeclaredReads();
            m_LastBuildStats.DeclaredReads += static_cast<u32>(reads.size());

            // Log any empty resource names (indicates handle->name mapping failed)
            for (const auto& resourceName : reads)
            {
                if (resourceName.empty())
                {
                    OLO_CORE_WARN("processGraphPass: pass '{}' declared empty-name read (handle mapping failed)",
                                  pass.Name);
                    continue;
                }

                const auto writerIt = lastWriterByResource.find(resourceName);
                if (writerIt == lastWriterByResource.end())
                    continue;

                const auto& writerPass = writerIt->second;
                if (writerPass == pass.Name)
                    continue;

                if (tryAddDerivedDependency(writerPass, pass.Name))
                {
                    ++m_LastBuildStats.DerivedEdges;
                }
            }

            const auto& writes = builder.GetDeclaredWrites();
            m_LastBuildStats.DeclaredWrites += static_cast<u32>(writes.size());

            const auto& accesses = builder.GetDeclaredAccesses();
            m_PassAccessDeclarations[pass.Name] = accesses;

            for (const auto& resourceName : writes)
            {
                if (resourceName.empty())
                {
                    OLO_CORE_WARN("processGraphPass: pass '{}' declared empty-name write (handle mapping failed)",
                                  pass.Name);
                    continue;
                }

                const auto writerIt = lastWriterByResource.find(resourceName);
                if (writerIt != lastWriterByResource.end())
                {
                    const auto& previousWriter = writerIt->second;
                    if (tryAddDerivedDependency(previousWriter, pass.Name))
                    {
                        ++m_LastBuildStats.DerivedEdges;
                    }
                }

                lastWriterByResource[resourceName] = pass.Name;
            }
        };

        // Use AddPass insertion order as the canonical ordering seed for
        // dependency derivation. m_InsertionOrder contains both legacy RenderPasses and
        // graph-native passes. We iterate through it to maintain execution order, but only
        // process graph-native passes that have setup callbacks (found in graphPassByName).
        for (const auto& passName : m_InsertionOrder)
        {
            auto it = graphPassByName.find(passName);
            if (it == graphPassByName.end())
            {
                // This is a legacy RenderPass, skip it silently
                continue;
            }

            processGraphPass(*it->second);
        }

        if (m_DependencyGraphDirty && !UpdateDependencyGraph())
        {
            OLO_CORE_ERROR("RenderGraph::BuildFrameGraph: dependency graph rebuild failed (cycle)");
            return;
        }

        if (m_DependencyGraphDirty)
        {
            ResolveFinalPass();
            RebuildExecutionCache();
            m_DependencyGraphDirty = false;
        }

        ComputeReachability();
        ComputeBarrierPlan();
        RebuildTransientPlan();

        // Cache the submission plan after barrier planning so
        // Execute() can walk the pre-built IR without re-deriving it.
        m_CachedSubmissionPlan = GetSubmissionPlan();
        LogSubmissionPlanIfChanged();
    }

} // namespace OloEngine
