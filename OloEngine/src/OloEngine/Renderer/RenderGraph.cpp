#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Renderer/RenderGraphBarrierPlanner.h"
#include "OloEngine/Renderer/RenderGraphHandleAllocator.h"
#include "OloEngine/Renderer/RenderGraphHazardValidator.h"
#include "OloEngine/Renderer/RenderGraphPlanExecutor.h"
#include "OloEngine/Renderer/RenderGraphReachability.h"
#include "OloEngine/Renderer/RenderGraphResourceRegistry.h"
#include "OloEngine/Renderer/RenderGraphSubmissionPlan.h"
#include "OloEngine/Renderer/RenderGraphTransientPlanner.h"
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
        template<typename TEntry>
        void SynchronizeGraphEntryLifecycle(Ref<TEntry> entry, const u32 physicalWidth, const u32 physicalHeight, const f32 renderScale)
        {
            if (!entry || physicalWidth == 0 || physicalHeight == 0)
                return;

            entry->ResizeFramebuffer(physicalWidth, physicalHeight);

            if (renderScale >= 1.0f)
            {
                entry->ApplyRenderViewport(0u, 0u);
                return;
            }

            const auto renderW = static_cast<u32>(glm::floor(static_cast<f32>(physicalWidth) * renderScale));
            const auto renderH = static_cast<u32>(glm::floor(static_cast<f32>(physicalHeight) * renderScale));
            entry->ApplyRenderViewport(renderW, renderH);
        }

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

        [[nodiscard]] auto HasExplicitVersionQualifier(const std::string_view resourceName) -> bool
        {
            return resourceName.find('@') != std::string_view::npos;
        }

        [[nodiscard]] auto GetVersionLookupBaseName(const std::string_view resourceName) -> std::string_view
        {
            if (const auto versionPos = resourceName.find('@'); versionPos != std::string_view::npos)
                return resourceName.substr(0u, versionPos);

            return resourceName;
        }

        Ref<RenderGraphNode> TryGetGraphEntryNode(
            std::string_view name,
            const std::unordered_map<std::string, Ref<RenderGraphNode>>& nodeLookup)
        {
            if (const auto nodeIt = nodeLookup.find(std::string(name)); nodeIt != nodeLookup.end() && nodeIt->second)
                return nodeIt->second;

            return nullptr;
        }

        [[nodiscard]] auto FindPassAccessDeclarations(
            const std::unordered_map<std::string, std::vector<RGAccessDeclaration>>& passAccessDeclarations,
            std::string_view passName) -> const std::vector<RGAccessDeclaration>*
        {
            if (const auto accessIt = passAccessDeclarations.find(std::string(passName));
                accessIt != passAccessDeclarations.end())
            {
                return &accessIt->second;
            }

            return nullptr;
        }

    } // namespace

    void RenderGraph::Init(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Initializing RenderGraph with dimensions: {}x{}", width, height);

        m_PhysicalWidth = width;
        m_PhysicalHeight = height;
        m_RenderScale = 1.0f;

        for (auto& [name, node] : m_NodeLookup)
        {
            node->SetupFramebuffer(width, height);
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Shutting down RenderGraph");

        m_TransientPool.Clear();

        m_NodeLookup.clear();
        m_Dependencies.clear();
        m_ExplicitDependencies.clear();
        m_InsertionOrder.clear();
        m_ExecutionOrder.clear();
        m_FinalPassName.clear();
        m_HasExplicitFinalPass = false;
        m_ReachablePasses.clear();
        m_CulledPasses.clear();
        m_LastBuildStats = {};
        m_PassAccessDeclarations.clear();
        m_PassFeedbackDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BuildDiagnostics.clear();
        m_BarrierDiagnostics.clear();
        m_LastExecutionTimings.clear();
        m_ResolveFailures.clear();
        m_CachedSubmissionPlan.clear();
        m_LastLoggedSubmissionPlanDigest.clear();
        m_LastLoggedCulledPassDigest.clear();
        m_LastLoggedBuildDiagnosticDigest.clear();
        m_ImportedResources.clear();
        m_ResourceRegistry.clear();
        m_RegisteredResources.clear();
        m_ResourceRegistryDiagnostics.clear();
        m_TextureHandlesByName.clear();
        m_LatestTextureHandlesByBaseName.clear();
        m_TextureViewResourceDescs.clear();
        m_TextureViewDefinitions.clear();
        m_BufferHandlesByName.clear();
        m_LatestBufferHandlesByBaseName.clear();
        m_FramebufferHandlesByName.clear();
        m_LatestFramebufferHandlesByBaseName.clear();
        m_TextureHandleSlots.clear();
        m_BufferHandleSlots.clear();
        m_FramebufferHandleSlots.clear();
        m_FreeTextureHandleIndices.clear();
        m_FreeBufferHandleIndices.clear();
        m_FreeFramebufferHandleIndices.clear();
        m_PhysicalTextures.clear();
        m_PhysicalFramebuffers.clear();
        m_PhysicalBuffers.clear();
        m_TextureExtracts.clear();
        m_ExternalTextureSinkContracts.clear();
        m_HistoryTextureExtracts.clear();
        m_FramebufferExtracts.clear();
        m_TemporalHistoryContracts.clear();
        m_ExternalTextureSinks.clear();
        m_HistoryTextureSinks.clear();
        m_ExternallyBackedTransientTextures.clear();
        m_ExternallyBackedTransientFramebuffers.clear();
        m_Blackboard.Reset();
        m_TransientResourceDescs.clear();
        m_TransientPlan.clear();
        m_ExplicitVersionProducers.clear();
        m_LastWriterPassNameByResource.clear();
        m_ResourceNames.Clear();
        m_PassNames.Clear();
        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::ResetTopology()
    {
        OLO_PROFILE_FUNCTION();

        m_TransientPool.Clear();

        // Wipe topology bookkeeping but leave graph-entry framebuffers /
        // internal state alone — the nodes themselves are owned externally
        // and will be re-registered by the caller as part of the new topology.
        m_NodeLookup.clear();
        m_Dependencies.clear();
        m_ExplicitDependencies.clear();
        m_InsertionOrder.clear();
        m_ExecutionOrder.clear();
        m_FinalPassName.clear();
        m_HasExplicitFinalPass = false;
        m_ReachablePasses.clear();
        m_CulledPasses.clear();
        m_LastBuildStats = {};
        m_PassAccessDeclarations.clear();
        m_PassFeedbackDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BuildDiagnostics.clear();
        m_BarrierDiagnostics.clear();
        m_LastExecutionTimings.clear();
        m_ResolveFailures.clear();
        m_CachedSubmissionPlan.clear();
        m_LastLoggedSubmissionPlanDigest.clear();
        m_LastLoggedCulledPassDigest.clear();
        m_LastLoggedBuildDiagnosticDigest.clear();
        m_DependencyGraphDirty = true;
        m_ImportedResources.clear();
        m_ResourceRegistry.clear();
        m_RegisteredResources.clear();
        m_ResourceRegistryDiagnostics.clear();
        m_TextureHandlesByName.clear();
        m_LatestTextureHandlesByBaseName.clear();
        m_TextureViewResourceDescs.clear();
        m_TextureViewDefinitions.clear();
        m_BufferHandlesByName.clear();
        m_LatestBufferHandlesByBaseName.clear();
        m_FramebufferHandlesByName.clear();
        m_LatestFramebufferHandlesByBaseName.clear();

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
        m_TextureExtracts.clear();
        m_ExternalTextureSinkContracts.clear();
        m_HistoryTextureExtracts.clear();
        m_FramebufferExtracts.clear();
        m_TemporalHistoryContracts.clear();
        m_ExternalTextureSinks.clear();
        m_HistoryTextureSinks.clear();
        m_ExternallyBackedTransientTextures.clear();
        m_ExternallyBackedTransientFramebuffers.clear();
        m_Blackboard.Reset();
        m_TransientResourceDescs.clear();
        m_TransientPlan.clear();
        m_ExplicitVersionProducers.clear();
        m_ResourceNames.Clear();
        m_PassNames.Clear();

        m_ResourceRegistryDirty = true;
    }

    void RenderGraph::AddNode(const Ref<RenderGraphNode>& node)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(node, "RenderGraph::AddNode requires a valid node");

        const std::string name(node->GetName());
        OLO_CORE_ASSERT(!name.empty(), "RenderGraph::AddNode requires a named node");
        OLO_CORE_ASSERT(!m_NodeLookup.contains(name),
                        "RenderGraph entries must have unique names");

        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Adding graph node: {}", name);

        m_InsertionOrder.push_back(name);

        m_NodeLookup[name] = node;
        SynchronizeGraphEntryLifecycle(node, m_PhysicalWidth, m_PhysicalHeight, m_RenderScale);
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
        m_TextureViewResourceDescs.clear();
        m_TextureViewDefinitions.clear();
        m_LatestTextureHandlesByBaseName.clear();
        m_LatestBufferHandlesByBaseName.clear();
        m_LatestFramebufferHandlesByBaseName.clear();
        m_TextureBaseNameAliases.clear();
        m_FramebufferBaseNameAliases.clear();
        m_ExternalTextureSinkContracts.clear();
        m_ExternalTextureSinks.clear();
        m_HistoryTextureSinks.clear();
        m_ExternallyBackedTransientTextures.clear();
        m_ExternallyBackedTransientFramebuffers.clear();
        m_ResourceRegistryDirty = true;
    }

    // =========================================================================
    // Typed import / resolve / extract
    // =========================================================================

    RGTextureHandle RenderGraph::AllocateTextureHandle(std::string_view name, u32 textureID, bool isHistory, bool isPlaceholder, std::string_view placeholderReason)
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

            // If this slot was previously marked free, remove it from the free
            // list before reusing it to avoid duplicate allocations of the same index.
            // Tracked separately so we can decide whether to bump the generation
            // (free-list reuse always invalidates prior handles).
            const auto freeIt = std::ranges::find(m_FreeTextureHandleIndices,
                                                  handle.Index);
            const bool wasOnFreeList = (freeIt != m_FreeTextureHandleIndices.end());
            if (wasOnFreeList)
                m_FreeTextureHandleIndices.erase(freeIt);

            if (handle.Index >= m_PhysicalTextures.size())
                m_PhysicalTextures.resize(static_cast<sizet>(handle.Index) + 1u);

            auto& phys = m_PhysicalTextures[handle.Index];

            // Compare prior backing state BEFORE we overwrite it. We only bump
            // the generation when something the handle actually identifies has
            // changed; otherwise re-importing the same resource keeps prior
            // handle copies valid (avoids invalidating every consumer each frame).
            const bool resourceChanged = (phys.TextureID != textureID) || (phys.IsHistory != isHistory);
            const bool placeholderChanged = (slot.IsPlaceholder != isPlaceholder) ||
                                            (slot.PlaceholderReason != placeholderReason);
            const bool needsGenBump = wasOnFreeList || !slot.Alive || resourceChanged || placeholderChanged;

            if (slot.Generation == 0)
                slot.Generation = 1;
            if (needsGenBump)
                ++slot.Generation;

            slot.Alive = true;
            slot.Name = std::string(name);
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
            handle.Generation = slot.Generation;

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
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
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
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
            m_TextureHandleSlots.push_back(std::move(slot));

            PhysicalTexture phys;
            phys.TextureID = textureID;
            phys.IsHistory = isHistory;
            m_PhysicalTextures.push_back(std::move(phys));
        }

        m_TextureHandlesByName[std::string(name)] = handle;
        return handle;
    }

    RGTextureHandle RenderGraph::CreateVersionedTextureHandle(const RGTextureHandle sourceHandle,
                                                              std::string_view versionedName,
                                                              std::string_view ownerPassName)
    {
        if (!sourceHandle.IsValid() || versionedName.empty())
            return {};

        const auto sourceResource = GetResourceName(sourceHandle);
        if (sourceResource.empty())
            return {};

        auto desc = BuildVersionedResourceDesc(sourceResource,
                                               ResourceHandle::Kind::Texture2D,
                                               versionedName);
        auto versionHandle = AllocateTransientTextureHandle(versionedName, desc);
        if (!ownerPassName.empty())
            m_ExplicitVersionProducers[std::string(versionedName)] = std::string(ownerPassName);
        if (versionHandle.IsValid())
        {
            m_LatestTextureHandlesByBaseName[std::string(GetVersionLookupBaseName(sourceResource))] = versionHandle;
        }

        return versionHandle;
    }

    RGFramebufferHandle RenderGraph::AllocateFramebufferHandle(std::string_view name, const Ref<Framebuffer>& fb, bool isPlaceholder, std::string_view placeholderReason)
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

            // If this slot was previously marked free, remove it from the free
            // list before reusing it. Free-list reuse always counts as a
            // backing-state change for generation purposes.
            const auto freeIt = std::ranges::find(m_FreeFramebufferHandleIndices,
                                                  handle.Index);
            const bool wasOnFreeList = (freeIt != m_FreeFramebufferHandleIndices.end());
            if (wasOnFreeList)
                m_FreeFramebufferHandleIndices.erase(freeIt);

            if (handle.Index >= m_PhysicalFramebuffers.size())
                m_PhysicalFramebuffers.resize(static_cast<sizet>(handle.Index) + 1u);

            auto& phys = m_PhysicalFramebuffers[handle.Index];

            // Bump generation only when the backing resource or placeholder
            // state actually changes. A no-op re-import keeps prior handle
            // copies valid, so consumers caching handles across frames don't
            // get spuriously invalidated.
            const bool resourceChanged = (phys.FB.get() != fb.get());
            const bool placeholderChanged = (slot.IsPlaceholder != isPlaceholder) ||
                                            (slot.PlaceholderReason != placeholderReason);
            const bool needsGenBump = wasOnFreeList || !slot.Alive || resourceChanged || placeholderChanged;

            if (slot.Generation == 0)
                slot.Generation = 1;
            if (needsGenBump)
                ++slot.Generation;

            slot.Alive = true;
            slot.Name = std::string(name);
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
            handle.Generation = slot.Generation;

            phys.FB = fb;
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
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
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
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
            m_FramebufferHandleSlots.push_back(std::move(slot));

            PhysicalFramebuffer phys;
            phys.FB = fb;
            m_PhysicalFramebuffers.push_back(std::move(phys));
        }

        m_FramebufferHandlesByName[std::string(name)] = handle;
        return handle;
    }

    RGFramebufferHandle RenderGraph::CreateVersionedFramebufferHandle(const RGFramebufferHandle sourceHandle,
                                                                      std::string_view versionedName,
                                                                      std::string_view ownerPassName)
    {
        if (!sourceHandle.IsValid() || versionedName.empty())
            return {};

        const auto sourceResource = GetResourceName(sourceHandle);
        if (sourceResource.empty())
            return {};

        auto desc = BuildVersionedResourceDesc(sourceResource,
                                               ResourceHandle::Kind::Framebuffer,
                                               versionedName);
        auto versionHandle = AllocateTransientFramebufferHandle(versionedName, desc);
        if (!ownerPassName.empty())
            m_ExplicitVersionProducers[std::string(versionedName)] = std::string(ownerPassName);
        if (versionHandle.IsValid())
        {
            m_LatestFramebufferHandlesByBaseName[std::string(GetVersionLookupBaseName(sourceResource))] = versionHandle;

            // Auto-publish versioned attachment views: every colour/depth
            // attachment view that was registered against the *base*
            // framebuffer needs a corresponding versioned sibling, otherwise
            // `GetTextureHandle(baseViewName)` (which is what the post-process
            // chain's `ReadFirstValidVersionedInputForPass` ultimately calls)
            // falls back to the base view. The base view's dependency points
            // at the base framebuffer's writer (e.g. ScenePass /
            // DeferredLightingPass), so the pass that produced *this* new
            // version is orphaned and reachability culls it with "No
            // downstream reader". Republishing the views forces the
            // latest-version map for each view name to point at this
            // version's sibling, which traces back to this pass instead.
            //
            // Without this, every RMW pass that writes a new SceneColor
            // version has to remember to publish its own versioned
            // SceneColorTexture (and any other attachment views) — a
            // boilerplate trap that bit us through ForwardOverlayPass +
            // 5 siblings before this fix landed.
            const auto versionTag = versionedName.find('@') != std::string_view::npos
                                        ? versionedName.substr(versionedName.find('@') + 1u)
                                        : std::string_view{};
            if (!versionTag.empty())
            {
                // Snapshot the view names first so we don't mutate the map
                // while iterating it.
                struct PendingVersionedView
                {
                    std::string BaseViewName;
                    u32 AttachmentIndex = 0u;
                    TextureViewKind Kind = TextureViewKind::FramebufferColorAttachment;
                };
                std::vector<PendingVersionedView> pending;
                pending.reserve(8u);
                for (const auto& [viewName, def] : m_TextureViewDefinitions)
                {
                    if (def.ParentResource != sourceResource)
                        continue;
                    if (def.Kind != TextureViewKind::FramebufferColorAttachment &&
                        def.Kind != TextureViewKind::FramebufferDepthAttachment)
                        continue;
                    // Skip views that are already a versioned variant —
                    // only the canonical base views should seed siblings.
                    if (viewName.find('@') != std::string::npos)
                        continue;
                    pending.push_back({ viewName, def.AttachmentIndex, def.Kind });
                }
                for (const auto& entry : pending)
                {
                    const auto versionedViewName = entry.BaseViewName + "@" + std::string(versionTag);
                    if (entry.Kind == TextureViewKind::FramebufferColorAttachment)
                    {
                        [[maybe_unused]] const auto created =
                            CreateFramebufferAttachmentView(versionedViewName, versionHandle, entry.AttachmentIndex);
                    }
                    else // FramebufferDepthAttachment
                    {
                        [[maybe_unused]] const auto created =
                            CreateFramebufferDepthAttachmentView(versionedViewName, versionHandle);
                    }
                }
            }
        }

        return versionHandle;
    }

    RGBufferHandle RenderGraph::AllocateBufferHandle(std::string_view name, u32 bufferID, bool isPlaceholder, std::string_view placeholderReason)
    {
        RGBufferHandle handle;

        // Stable handle reuse by name, mirroring textures/framebuffers.
        if (const auto existingIt = m_BufferHandlesByName.find(std::string(name));
            existingIt != m_BufferHandlesByName.end() &&
            existingIt->second.Index < m_BufferHandleSlots.size())
        {
            handle.Index = existingIt->second.Index;
            auto& slot = m_BufferHandleSlots[handle.Index];

            // If this slot was previously marked free, remove it from the free
            // list before reusing it. Free-list reuse always counts as a
            // backing-state change for generation purposes.
            const auto freeIt = std::ranges::find(m_FreeBufferHandleIndices,
                                                  handle.Index);
            const bool wasOnFreeList = (freeIt != m_FreeBufferHandleIndices.end());
            if (wasOnFreeList)
                m_FreeBufferHandleIndices.erase(freeIt);

            if (handle.Index >= m_PhysicalBuffers.size())
                m_PhysicalBuffers.resize(static_cast<sizet>(handle.Index) + 1u);

            auto& phys = m_PhysicalBuffers[handle.Index];

            // Bump generation only when the backing resource or placeholder
            // state actually changes. A no-op re-import keeps prior handle
            // copies valid for callers caching handles across frames.
            const bool resourceChanged = (phys.BufferID != bufferID);
            const bool placeholderChanged = (slot.IsPlaceholder != isPlaceholder) ||
                                            (slot.PlaceholderReason != placeholderReason);
            const bool needsGenBump = wasOnFreeList || !slot.Alive || resourceChanged || placeholderChanged;

            if (slot.Generation == 0)
                slot.Generation = 1;
            if (needsGenBump)
                ++slot.Generation;

            slot.Alive = true;
            slot.Name = std::string(name);
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
            handle.Generation = slot.Generation;

            phys.BufferID = bufferID;
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
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
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
            slot.IsPlaceholder = isPlaceholder;
            slot.PlaceholderReason = std::string(placeholderReason);
            slot.PlaceholderWarnedThisFrame = false;
            m_BufferHandleSlots.push_back(std::move(slot));

            PhysicalBuffer phys;
            phys.BufferID = bufferID;
            m_PhysicalBuffers.push_back(std::move(phys));
        }

        m_BufferHandlesByName[std::string(name)] = handle;
        return handle;
    }

    RGBufferHandle RenderGraph::CreateVersionedBufferHandle(const RGBufferHandle sourceHandle,
                                                            std::string_view versionedName,
                                                            std::string_view ownerPassName)
    {
        if (!sourceHandle.IsValid() || versionedName.empty())
            return {};

        const auto sourceResource = GetResourceName(sourceHandle);
        if (sourceResource.empty())
            return {};

        auto desc = BuildVersionedResourceDesc(sourceResource,
                                               ResourceHandle::Kind::StorageBuffer,
                                               versionedName);
        auto versionHandle = AllocateTransientBufferHandle(versionedName, desc);
        if (!ownerPassName.empty())
            m_ExplicitVersionProducers[std::string(versionedName)] = std::string(ownerPassName);
        if (versionHandle.IsValid())
        {
            m_LatestBufferHandlesByBaseName[std::string(GetVersionLookupBaseName(sourceResource))] = versionHandle;
        }

        return versionHandle;
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

        return AllocateTextureHandle(name, textureID, false, importDesc.IsPlaceholder, importDesc.PlaceholderReason);
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

        return AllocateFramebufferHandle(name, fb, importDesc.IsPlaceholder, importDesc.PlaceholderReason);
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

        return AllocateBufferHandle(name, bufferID, importDesc.IsPlaceholder, importDesc.PlaceholderReason);
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

        return AllocateTextureHandle(name, textureID, /*isHistory=*/true, importDesc.IsPlaceholder, importDesc.PlaceholderReason);
    }

    RGTextureHandle RenderGraph::CreateFramebufferAttachmentView(std::string_view name,
                                                                 const RGFramebufferHandle framebufferHandle,
                                                                 const u32 colorAttachmentIndex)
    {
        if (name.empty() || !framebufferHandle.IsValid())
            return {};

        const auto parentResource = std::string(GetResourceName(framebufferHandle));
        if (parentResource.empty())
            return {};

        const auto* parentInfo = FindRegisteredResource(parentResource);
        if (!parentInfo || parentInfo->Desc.Kind != ResourceHandle::Kind::Framebuffer)
        {
            OLO_CORE_ERROR("RenderGraph::CreateFramebufferAttachmentView: parent '{}' is not a registered framebuffer resource",
                           parentResource);
            return {};
        }

        const auto isDepthFormat = [](const RGResourceFormat format)
        {
            return format == RGResourceFormat::Depth24Stencil8 ||
                   format == RGResourceFormat::Depth32Float ||
                   format == RGResourceFormat::Unknown;
        };

        RGResourceDesc viewDesc = parentInfo->Desc;
        viewDesc.Kind = ResourceHandle::Kind::Texture2D;
        viewDesc.DebugName = std::string(name);
        viewDesc.Attachments.clear();
        viewDesc.DepthOrLayers = 1u;

        if (!parentInfo->Desc.Attachments.empty())
        {
            if (colorAttachmentIndex >= parentInfo->Desc.Attachments.size())
            {
                OLO_CORE_ERROR("RenderGraph::CreateFramebufferAttachmentView: attachment index {} is out of range for framebuffer '{}'",
                               colorAttachmentIndex,
                               parentResource);
                return {};
            }

            const auto attachmentFormat = parentInfo->Desc.Attachments[colorAttachmentIndex];
            if (isDepthFormat(attachmentFormat))
            {
                OLO_CORE_ERROR("RenderGraph::CreateFramebufferAttachmentView: attachment index {} of '{}' is not a colour attachment",
                               colorAttachmentIndex,
                               parentResource);
                return {};
            }

            viewDesc.Format = attachmentFormat;
        }
        else
        {
            if (colorAttachmentIndex != 0u || isDepthFormat(parentInfo->Desc.Format))
            {
                OLO_CORE_ERROR("RenderGraph::CreateFramebufferAttachmentView: framebuffer '{}' has no matching colour attachment {}",
                               parentResource,
                               colorAttachmentIndex);
                return {};
            }

            viewDesc.Format = parentInfo->Desc.Format;
        }

        const auto stableName = std::string(name);
        m_TextureViewDefinitions[stableName] = TextureViewDefinition{
            .ParentResource = parentResource,
            .Kind = TextureViewKind::FramebufferColorAttachment,
            .AttachmentIndex = colorAttachmentIndex,
        };
        m_TextureViewResourceDescs[stableName] = viewDesc;
        m_ResourceRegistryDirty = true;

        const auto handle = AllocateTextureHandle(name, 0u, /*isHistory=*/false, viewDesc.IsPlaceholder, viewDesc.PlaceholderReason);
        if (handle.IsValid() && HasExplicitVersionQualifier(stableName))
            m_LatestTextureHandlesByBaseName[std::string(GetVersionLookupBaseName(stableName))] = handle;
        return handle;
    }

    RGTextureHandle RenderGraph::CreateFramebufferDepthAttachmentView(std::string_view name,
                                                                      const RGFramebufferHandle framebufferHandle)
    {
        if (name.empty() || !framebufferHandle.IsValid())
            return {};

        const auto parentResource = std::string(GetResourceName(framebufferHandle));
        if (parentResource.empty())
            return {};

        const auto* parentInfo = FindRegisteredResource(parentResource);
        if (!parentInfo || parentInfo->Desc.Kind != ResourceHandle::Kind::Framebuffer)
        {
            OLO_CORE_ERROR("RenderGraph::CreateFramebufferDepthAttachmentView: parent '{}' is not a registered framebuffer resource",
                           parentResource);
            return {};
        }

        const auto isDepthFormat = [](const RGResourceFormat format)
        {
            return format == RGResourceFormat::Depth24Stencil8 ||
                   format == RGResourceFormat::Depth32Float;
        };

        RGResourceDesc viewDesc = parentInfo->Desc;
        viewDesc.Kind = ResourceHandle::Kind::Texture2D;
        viewDesc.DebugName = std::string(name);
        viewDesc.Attachments.clear();
        viewDesc.DepthOrLayers = 1u;

        if (!parentInfo->Desc.Attachments.empty())
        {
            if (const auto depthIt = std::ranges::find_if(parentInfo->Desc.Attachments,
                                                          [&isDepthFormat](const RGResourceFormat format)
                                                          {
                                                              return isDepthFormat(format);
                                                          });
                depthIt != parentInfo->Desc.Attachments.end())
            {
                viewDesc.Format = *depthIt;
            }
            else
            {
                OLO_CORE_ERROR("RenderGraph::CreateFramebufferDepthAttachmentView: framebuffer '{}' has no depth attachment",
                               parentResource);
                return {};
            }
        }
        else
        {
            if (!isDepthFormat(parentInfo->Desc.Format))
            {
                OLO_CORE_ERROR("RenderGraph::CreateFramebufferDepthAttachmentView: framebuffer '{}' has no depth attachment",
                               parentResource);
                return {};
            }

            viewDesc.Format = parentInfo->Desc.Format;
        }

        const auto stableName = std::string(name);
        m_TextureViewDefinitions[stableName] = TextureViewDefinition{
            .ParentResource = parentResource,
            .Kind = TextureViewKind::FramebufferDepthAttachment,
            .AttachmentIndex = 0u,
        };
        m_TextureViewResourceDescs[stableName] = viewDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, 0u, /*isHistory=*/false, viewDesc.IsPlaceholder, viewDesc.PlaceholderReason);
    }

    RGTextureHandle RenderGraph::CreateTextureMipView(std::string_view name,
                                                      const RGTextureHandle textureHandle,
                                                      const u32 mipLevel)
    {
        if (name.empty() || !textureHandle.IsValid())
            return {};

        const auto parentResource = std::string(GetResourceName(textureHandle));
        if (parentResource.empty())
            return {};

        if (m_TextureViewDefinitions.find(parentResource) != m_TextureViewDefinitions.end())
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMipView: nested texture views are not supported for '{}'",
                           parentResource);
            return {};
        }

        const auto* parentInfo = FindRegisteredResource(parentResource);
        if (!parentInfo)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMipView: parent '{}' is not a registered texture resource",
                           parentResource);
            return {};
        }

        const auto isTextureKind = [](const ResourceHandle::Kind kind)
        {
            return kind == ResourceHandle::Kind::Texture2D ||
                   kind == ResourceHandle::Kind::Texture2DArray ||
                   kind == ResourceHandle::Kind::TextureCube ||
                   kind == ResourceHandle::Kind::TextureCubeArray;
        };

        if (!isTextureKind(parentInfo->Desc.Kind))
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMipView: parent '{}' is not a registered texture resource",
                           parentResource);
            return {};
        }

        if (const auto parentMipCount = std::max(parentInfo->Desc.MipLevels, 1u); mipLevel >= parentMipCount)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMipView: mip level {} is out of range for '{}' (mipCount={})",
                           mipLevel,
                           parentResource,
                           parentMipCount);
            return {};
        }

        const auto mipDimension = [](u32 dimension, const u32 targetMip)
        {
            if (dimension == 0u)
                return 0u;

            for (u32 mip = 0u; mip < targetMip; ++mip)
                dimension = std::max(dimension / 2u, 1u);

            return dimension;
        };

        RGResourceDesc viewDesc = parentInfo->Desc;
        viewDesc.DebugName = std::string(name);
        viewDesc.Attachments.clear();
        viewDesc.Width = mipDimension(parentInfo->Desc.Width, mipLevel);
        viewDesc.Height = mipDimension(parentInfo->Desc.Height, mipLevel);
        viewDesc.MipLevels = 1u;

        const auto stableName = std::string(name);
        m_TextureViewDefinitions[stableName] = TextureViewDefinition{
            .ParentResource = parentResource,
            .Kind = TextureViewKind::TextureMip,
            .AttachmentIndex = 0u,
            .ParentRange = RGSubresourceRange::Mip(mipLevel),
        };
        m_TextureViewResourceDescs[stableName] = viewDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, 0u, /*isHistory=*/false, viewDesc.IsPlaceholder, viewDesc.PlaceholderReason);
    }

    RGTextureHandle RenderGraph::CreateTextureArrayLayerView(std::string_view name,
                                                             const RGTextureHandle textureHandle,
                                                             const u32 layerIndex)
    {
        if (name.empty() || !textureHandle.IsValid())
            return {};

        const auto parentResource = std::string(GetResourceName(textureHandle));
        if (parentResource.empty())
            return {};

        if (m_TextureViewDefinitions.find(parentResource) != m_TextureViewDefinitions.end())
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureArrayLayerView: nested texture views are not supported for '{}'",
                           parentResource);
            return {};
        }

        const auto* parentInfo = FindRegisteredResource(parentResource);
        if (!parentInfo || parentInfo->Desc.Kind != ResourceHandle::Kind::Texture2DArray)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureArrayLayerView: parent '{}' is not a registered texture-array resource",
                           parentResource);
            return {};
        }

        if (const auto parentLayerCount = std::max(parentInfo->Desc.DepthOrLayers, 1u); layerIndex >= parentLayerCount)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureArrayLayerView: layer index {} is out of range for '{}' (layerCount={})",
                           layerIndex,
                           parentResource,
                           parentLayerCount);
            return {};
        }

        RGResourceDesc viewDesc = parentInfo->Desc;
        viewDesc.DebugName = std::string(name);
        viewDesc.Attachments.clear();
        viewDesc.DepthOrLayers = 1u;

        const auto stableName = std::string(name);
        m_TextureViewDefinitions[stableName] = TextureViewDefinition{
            .ParentResource = parentResource,
            .Kind = TextureViewKind::TextureArrayLayer,
            .AttachmentIndex = 0u,
            .ParentRange = RGSubresourceRange::Layer(layerIndex),
        };
        m_TextureViewResourceDescs[stableName] = viewDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, 0u, /*isHistory=*/false, viewDesc.IsPlaceholder, viewDesc.PlaceholderReason);
    }

    RGTextureHandle RenderGraph::CreateTextureCubeFaceView(std::string_view name,
                                                           const RGTextureHandle textureHandle,
                                                           const u32 faceIndex)
    {
        if (name.empty() || !textureHandle.IsValid())
            return {};

        const auto parentResource = std::string(GetResourceName(textureHandle));
        if (parentResource.empty())
            return {};

        if (m_TextureViewDefinitions.find(parentResource) != m_TextureViewDefinitions.end())
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureCubeFaceView: nested texture views are not supported for '{}'",
                           parentResource);
            return {};
        }

        const auto* parentInfo = FindRegisteredResource(parentResource);
        if (!parentInfo || parentInfo->Desc.Kind != ResourceHandle::Kind::TextureCube)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureCubeFaceView: parent '{}' is not a registered cubemap resource",
                           parentResource);
            return {};
        }

        if (faceIndex >= 6u)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureCubeFaceView: face index {} is out of range for '{}'",
                           faceIndex,
                           parentResource);
            return {};
        }

        RGResourceDesc viewDesc = parentInfo->Desc;
        viewDesc.DebugName = std::string(name);
        viewDesc.Attachments.clear();
        viewDesc.DepthOrLayers = 1u;

        RGSubresourceRange faceRange{};
        faceRange.BaseSlice = faceIndex;
        faceRange.SliceCount = 1u;

        const auto stableName = std::string(name);
        m_TextureViewDefinitions[stableName] = TextureViewDefinition{
            .ParentResource = parentResource,
            .Kind = TextureViewKind::TextureCubeFace,
            .AttachmentIndex = 0u,
            .ParentRange = faceRange,
        };
        m_TextureViewResourceDescs[stableName] = viewDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, 0u, /*isHistory=*/false, viewDesc.IsPlaceholder, viewDesc.PlaceholderReason);
    }

    RGTextureHandle RenderGraph::CreateTextureMultisampleResolveView(std::string_view name,
                                                                     const RGTextureHandle multisampleTextureHandle,
                                                                     const RGTextureHandle resolvedTextureHandle)
    {
        if (name.empty() || !multisampleTextureHandle.IsValid() || !resolvedTextureHandle.IsValid())
            return {};

        const auto parentResource = std::string(GetResourceName(multisampleTextureHandle));
        const auto backingResource = std::string(GetResourceName(resolvedTextureHandle));
        if (parentResource.empty() || backingResource.empty())
            return {};

        const auto* parentInfo = FindRegisteredResource(parentResource);
        const auto* backingInfo = FindRegisteredResource(backingResource);
        if (!parentInfo || !backingInfo)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMultisampleResolveView: parent '{}' or backing '{}' is not a registered texture resource",
                           parentResource,
                           backingResource);
            return {};
        }

        const auto isTextureKind = [](const ResourceHandle::Kind kind)
        {
            return kind == ResourceHandle::Kind::Texture2D ||
                   kind == ResourceHandle::Kind::Texture2DArray ||
                   kind == ResourceHandle::Kind::TextureCube ||
                   kind == ResourceHandle::Kind::TextureCubeArray;
        };

        if (!isTextureKind(parentInfo->Desc.Kind) || !isTextureKind(backingInfo->Desc.Kind))
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMultisampleResolveView: parent '{}' or backing '{}' is not a registered texture resource",
                           parentResource,
                           backingResource);
            return {};
        }

        if (parentInfo->Desc.Samples <= 1u)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMultisampleResolveView: parent '{}' is not multisampled (samples={})",
                           parentResource,
                           parentInfo->Desc.Samples);
            return {};
        }

        if (std::max(backingInfo->Desc.Samples, 1u) != 1u)
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMultisampleResolveView: backing '{}' must be single-sample (samples={})",
                           backingResource,
                           backingInfo->Desc.Samples);
            return {};
        }

        if (parentInfo->Desc.Kind != backingInfo->Desc.Kind ||
            parentInfo->Desc.Format != backingInfo->Desc.Format ||
            parentInfo->Desc.Width != backingInfo->Desc.Width ||
            parentInfo->Desc.Height != backingInfo->Desc.Height ||
            std::max(parentInfo->Desc.DepthOrLayers, 1u) != std::max(backingInfo->Desc.DepthOrLayers, 1u))
        {
            OLO_CORE_ERROR("RenderGraph::CreateTextureMultisampleResolveView: parent '{}' and backing '{}' descriptors do not match",
                           parentResource,
                           backingResource);
            return {};
        }

        auto viewDesc = backingInfo->Desc;
        viewDesc.DebugName = std::string(name);
        viewDesc.Attachments.clear();
        viewDesc.Samples = 1u;

        const auto stableName = std::string(name);
        m_TextureViewDefinitions[stableName] = TextureViewDefinition{
            .ParentResource = parentResource,
            .BackingResource = backingResource,
            .Kind = TextureViewKind::TextureMultisampleResolve,
            .AttachmentIndex = 0u,
            .ParentRange = RGSubresourceRange::Full(),
        };
        m_TextureViewResourceDescs[stableName] = viewDesc;
        m_ResourceRegistryDirty = true;

        return AllocateTextureHandle(name, 0u, /*isHistory=*/false, viewDesc.IsPlaceholder, viewDesc.PlaceholderReason);
    }

    u32 RenderGraph::ResolveTexture(RGTextureHandle handle) const
    {
        EnsureResourceRegistryBuilt();

        if (!handle.IsValid() || handle.Index >= m_PhysicalTextures.size())
            return 0;
        if (handle.Index >= m_TextureHandleSlots.size())
            return 0;
        const auto& slot = m_TextureHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return 0;

        if (slot.IsPlaceholder && !slot.PlaceholderWarnedThisFrame)
        {
            OLO_CORE_WARN("RenderGraph: resolving placeholder texture resource '{}' (reason: {})",
                          slot.Name,
                          slot.PlaceholderReason.empty() ? "unspecified" : slot.PlaceholderReason);
            slot.PlaceholderWarnedThisFrame = true;
        }

        if (const auto viewIt = m_TextureViewDefinitions.find(slot.Name);
            viewIt != m_TextureViewDefinitions.end())
        {
            const auto isTextureSubresourceView = [](const TextureViewKind kind)
            {
                return kind == TextureViewKind::TextureMip ||
                       kind == TextureViewKind::TextureArrayLayer ||
                       kind == TextureViewKind::TextureCubeFace;
            };

            if (viewIt->second.Kind == TextureViewKind::TextureMultisampleResolve)
            {
                if (const auto backingIt = m_TextureHandlesByName.find(viewIt->second.BackingResource);
                    backingIt != m_TextureHandlesByName.end() && IsTextureHandleCurrent(backingIt->second))
                {
                    return ResolveTexture(backingIt->second);
                }

                return 0;
            }

            if (isTextureSubresourceView(viewIt->second.Kind))
            {
                if (const auto parentIt = m_TextureHandlesByName.find(viewIt->second.ParentResource);
                    parentIt != m_TextureHandlesByName.end() && IsTextureHandleCurrent(parentIt->second))
                {
                    return ResolveTexture(parentIt->second);
                }

                return 0;
            }

            if (const auto parentIt = m_FramebufferHandlesByName.find(viewIt->second.ParentResource);
                parentIt != m_FramebufferHandlesByName.end() && IsFramebufferHandleCurrent(parentIt->second))
            {
                if (auto framebuffer = ResolveFramebuffer(parentIt->second))
                {
                    if (viewIt->second.Kind == TextureViewKind::FramebufferDepthAttachment)
                        return framebuffer->GetDepthAttachmentRendererID();

                    return framebuffer->GetColorAttachmentRendererID(viewIt->second.AttachmentIndex);
                }
            }

            return 0;
        }

        return m_PhysicalTextures[handle.Index].TextureID;
    }

    Ref<Framebuffer> RenderGraph::ResolveFramebuffer(RGFramebufferHandle handle) const
    {
        EnsureResourceRegistryBuilt();

        if (!handle.IsValid() || handle.Index >= m_PhysicalFramebuffers.size())
            return nullptr;
        if (handle.Index >= m_FramebufferHandleSlots.size())
            return nullptr;
        const auto& slot = m_FramebufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return nullptr;

        if (slot.IsPlaceholder && !slot.PlaceholderWarnedThisFrame)
        {
            OLO_CORE_WARN("RenderGraph: resolving placeholder framebuffer resource '{}' (reason: {})",
                          slot.Name,
                          slot.PlaceholderReason.empty() ? "unspecified" : slot.PlaceholderReason);
            slot.PlaceholderWarnedThisFrame = true;
        }

        return m_PhysicalFramebuffers[handle.Index].FB;
    }

    u32 RenderGraph::ResolveBuffer(RGBufferHandle handle) const
    {
        EnsureResourceRegistryBuilt();

        if (!handle.IsValid() || handle.Index >= m_PhysicalBuffers.size())
            return 0;
        if (handle.Index >= m_BufferHandleSlots.size())
            return 0;
        const auto& slot = m_BufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return 0;

        if (slot.IsPlaceholder && !slot.PlaceholderWarnedThisFrame)
        {
            OLO_CORE_WARN("RenderGraph: resolving placeholder buffer resource '{}' (reason: {})",
                          slot.Name,
                          slot.PlaceholderReason.empty() ? "unspecified" : slot.PlaceholderReason);
            slot.PlaceholderWarnedThisFrame = true;
        }

        return m_PhysicalBuffers[handle.Index].BufferID;
    }

    std::string RenderGraph::GetResourceName(RGTextureHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_TextureHandleSlots.size())
            return {};

        const auto& slot = m_TextureHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return {};

        return slot.Name;
    }

    std::string RenderGraph::GetResourceName(RGFramebufferHandle handle) const
    {
        if (!handle.IsValid() || handle.Index >= m_FramebufferHandleSlots.size())
            return {};

        const auto& slot = m_FramebufferHandleSlots[handle.Index];
        if (!slot.Alive || slot.Generation != handle.Generation)
            return {};

        return slot.Name;
    }

    std::string RenderGraph::GetResourceName(RGBufferHandle handle) const
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

    void RenderGraph::RegisterExternalTextureSink(RGTextureHandle sourceHandle,
                                                  const u32 textureID,
                                                  const u32 width,
                                                  const u32 height,
                                                  bool* const validFlag)
    {
        if (!sourceHandle.IsValid())
            return;

        const auto sourceResource = GetResourceName(sourceHandle);
        if (sourceResource.empty())
            return;

        RegisterExternalTextureSink(sourceResource, textureID, width, height, 0u, validFlag);
    }

    void RenderGraph::RegisterExternalTextureSink(RGFramebufferHandle sourceHandle,
                                                  const u32 textureID,
                                                  const u32 width,
                                                  const u32 height,
                                                  const u32 colorAttachmentIndex,
                                                  bool* const validFlag)
    {
        if (!sourceHandle.IsValid())
            return;

        const auto sourceResource = GetResourceName(sourceHandle);
        if (sourceResource.empty())
            return;

        RegisterExternalTextureSink(sourceResource, textureID, width, height, colorAttachmentIndex, validFlag);
    }

    void RenderGraph::RegisterExternalTextureSink(std::string_view sourceResource,
                                                  const u32 textureID,
                                                  const u32 width,
                                                  const u32 height,
                                                  const u32 colorAttachmentIndex,
                                                  bool* const validFlag)
    {
        if (sourceResource.empty())
            return;

        const ExternalTextureSinkKey key{ .SourceResource = std::string(sourceResource), .ColorAttachmentIndex = colorAttachmentIndex };
        m_ExternalTextureSinks[key] = ExternalTextureSink{
            .TextureID = textureID,
            .Width = width,
            .Height = height,
            .ValidFlag = validFlag,
        };
        DeclareExternalTextureSink(sourceResource, colorAttachmentIndex);
        RefreshExternalTextureSinkContracts();
    }

    void RenderGraph::DeclareExternalTextureSink(std::string_view sourceResource,
                                                 const u32 colorAttachmentIndex)
    {
        if (sourceResource.empty())
            return;

        const auto existing = std::ranges::find_if(m_ExternalTextureSinkContracts,
                                                   [sourceResource, colorAttachmentIndex](const ExternalTextureSinkContract& contract)
                                                   {
                                                       return contract.SourceResource == sourceResource &&
                                                              contract.ColorAttachmentIndex == colorAttachmentIndex;
                                                   });
        if (existing != m_ExternalTextureSinkContracts.end())
            return;

        m_ExternalTextureSinkContracts.push_back(ExternalTextureSinkContract{
            .SourceResource = std::string(sourceResource),
            .ColorAttachmentIndex = colorAttachmentIndex,
        });
    }

    void RenderGraph::RefreshExternalTextureSinkContracts()
    {
        if (m_ExternalTextureSinkContracts.empty())
            return;

        EnsureResourceRegistryBuilt();

        for (auto& contract : m_ExternalTextureSinkContracts)
        {
            contract.SourceKind = ResourceHandle::Kind::Unknown;
            contract.SourceReachable = IsResourceReachableForExtraction(contract.SourceResource);

            if (const auto resourceIt = m_ResourceRegistry.find(contract.SourceResource);
                resourceIt != m_ResourceRegistry.end())
            {
                contract.SourceKind = resourceIt->second.Desc.Kind;
            }
        }
    }

    void RenderGraph::RegisterHistoryTextureSink(std::string_view historyResource,
                                                 const u32 textureID,
                                                 const u32 width,
                                                 const u32 height,
                                                 bool* const validFlag)
    {
        if (historyResource.empty())
            return;

        m_HistoryTextureSinks[std::string(historyResource)] = HistoryTextureSink{
            .TextureID = textureID,
            .Width = width,
            .Height = height,
            .ValidFlag = validFlag,
        };
    }

    void RenderGraph::DeclareHistoryTextureExtraction(std::string_view historyResource,
                                                      std::string_view sourceResource,
                                                      const TemporalHistoryContract::SourceKind kind,
                                                      const u32 colorAttachmentIndex)
    {
        if (historyResource.empty() || sourceResource.empty())
            return;

        const auto existing = std::ranges::find_if(m_TemporalHistoryContracts,
                                                   [historyResource, sourceResource, kind, colorAttachmentIndex](const TemporalHistoryContract& contract)
                                                   {
                                                       return contract.HistoryResource == historyResource &&
                                                              contract.SourceResource == sourceResource &&
                                                              contract.Kind == kind &&
                                                              contract.ColorAttachmentIndex == colorAttachmentIndex;
                                                   });
        if (existing != m_TemporalHistoryContracts.end())
            return;

        m_TemporalHistoryContracts.push_back(TemporalHistoryContract{
            .HistoryResource = std::string(historyResource),
            .SourceResource = std::string(sourceResource),
            .Kind = kind,
            .ColorAttachmentIndex = colorAttachmentIndex,
        });
    }

    void RenderGraph::RefreshTemporalHistoryContracts()
    {
        for (auto& contract : m_TemporalHistoryContracts)
        {
            contract.HistoryImported = HasHistoryTextureSink(contract.HistoryResource) ||
                                       IsHistoryTextureResource(contract.HistoryResource);
            contract.SourceReachable = IsResourceReachableForExtraction(contract.SourceResource);
        }
    }

    void RenderGraph::ExtractHistoryTexture(std::string_view historyResource,
                                            RGTextureHandle sourceHandle)
    {
        if (!sourceHandle.IsValid() || historyResource.empty())
            return;

        const auto sourceResource = std::string(GetResourceName(sourceHandle));
        DeclareHistoryTextureExtraction(historyResource,
                                        sourceResource,
                                        TemporalHistoryContract::SourceKind::Texture);
        RefreshTemporalHistoryContracts();
    }

    void RenderGraph::ExtractHistoryTexture(std::string_view historyResource,
                                            RGTextureHandle sourceHandle,
                                            std::function<void(u32)> callback)
    {
        if (!sourceHandle.IsValid() || !callback || historyResource.empty())
            return;

        const auto sourceResource = std::string(GetResourceName(sourceHandle));
        DeclareHistoryTextureExtraction(historyResource,
                                        sourceResource,
                                        TemporalHistoryContract::SourceKind::Texture);
        RefreshTemporalHistoryContracts();
        m_HistoryTextureExtracts.push_back(HistoryTextureExtract{
            .HistoryResource = std::string(historyResource),
            .Kind = HistoryTextureExtract::SourceKind::Texture,
            .SourceTextureHandle = sourceHandle,
            .Callback = std::move(callback),
        });
    }

    void RenderGraph::ExtractHistoryTexture(std::string_view historyResource,
                                            RGFramebufferHandle sourceHandle,
                                            const u32 colorAttachmentIndex)
    {
        if (!sourceHandle.IsValid() || historyResource.empty())
            return;

        const auto sourceResource = std::string(GetResourceName(sourceHandle));
        DeclareHistoryTextureExtraction(historyResource,
                                        sourceResource,
                                        TemporalHistoryContract::SourceKind::Framebuffer,
                                        colorAttachmentIndex);
        RefreshTemporalHistoryContracts();
    }

    void RenderGraph::ExtractHistoryTexture(std::string_view historyResource,
                                            RGFramebufferHandle sourceHandle,
                                            std::function<void(u32)> callback,
                                            const u32 colorAttachmentIndex)
    {
        if (!sourceHandle.IsValid() || !callback || historyResource.empty())
            return;

        const auto sourceResource = std::string(GetResourceName(sourceHandle));
        DeclareHistoryTextureExtraction(historyResource,
                                        sourceResource,
                                        TemporalHistoryContract::SourceKind::Framebuffer,
                                        colorAttachmentIndex);
        RefreshTemporalHistoryContracts();
        m_HistoryTextureExtracts.push_back(HistoryTextureExtract{
            .HistoryResource = std::string(historyResource),
            .Kind = HistoryTextureExtract::SourceKind::Framebuffer,
            .SourceFramebufferHandle = sourceHandle,
            .ColorAttachmentIndex = colorAttachmentIndex,
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

        for (auto& [historyResource, sink] : m_HistoryTextureSinks)
        {
            (void)historyResource;
            if (sink.ValidFlag)
                *sink.ValidFlag = false;
        }

        for (const auto& contract : m_TemporalHistoryContracts)
        {
            const auto sinkIt = m_HistoryTextureSinks.find(contract.HistoryResource);
            if (sinkIt == m_HistoryTextureSinks.end())
                continue;

            auto& sink = sinkIt->second;
            if (sink.TextureID == 0 || sink.Width == 0 || sink.Height == 0)
                continue;

            if (!diagnoseExtractionResource(contract.SourceResource, "<history-sink>"))
                continue;

            u32 sourceTextureID = 0;
            if (contract.Kind == TemporalHistoryContract::SourceKind::Texture)
            {
                const auto sourceHandle = GetTextureHandle(contract.SourceResource);
                if (!sourceHandle.IsValid() || !IsTextureHandleCurrent(sourceHandle))
                {
                    m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                        .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                        .PassName = "<history-sink>",
                        .Resource = std::string(contract.SourceResource),
                        .Message = "History sink requested with stale or invalid source texture handle",
                    });
                    continue;
                }

                sourceTextureID = ResolveTexture(sourceHandle);
            }
            else
            {
                const auto sourceHandle = GetFramebufferHandle(contract.SourceResource);
                if (!sourceHandle.IsValid() || !IsFramebufferHandleCurrent(sourceHandle))
                {
                    m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                        .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                        .PassName = "<history-sink>",
                        .Resource = std::string(contract.SourceResource),
                        .Message = "History sink requested with stale or invalid source framebuffer handle",
                    });
                    continue;
                }

                if (auto sourceFramebuffer = ResolveFramebuffer(sourceHandle))
                    sourceTextureID = sourceFramebuffer->GetColorAttachmentRendererID(contract.ColorAttachmentIndex);
            }

            if (sourceTextureID == 0)
                continue;

            glCopyImageSubData(sourceTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               sink.TextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               static_cast<GLsizei>(sink.Width),
                               static_cast<GLsizei>(sink.Height),
                               1);
            if (sink.ValidFlag)
                *sink.ValidFlag = true;
        }

        for (const auto& [sinkKey, sink] : m_ExternalTextureSinks)
        {
            if (sink.ValidFlag)
                *sink.ValidFlag = false;

            if (sink.TextureID == 0 || sink.Width == 0 || sink.Height == 0)
                continue;

            if (!diagnoseExtractionResource(sinkKey.SourceResource, "<external-sink>"))
                continue;

            u32 sourceTextureID = 0;
            if (const auto textureHandle = GetTextureHandle(sinkKey.SourceResource);
                textureHandle.IsValid() && IsTextureHandleCurrent(textureHandle))
            {
                sourceTextureID = ResolveTexture(textureHandle);
            }
            else if (const auto framebufferHandle = GetFramebufferHandle(sinkKey.SourceResource);
                     framebufferHandle.IsValid() && IsFramebufferHandleCurrent(framebufferHandle))
            {
                if (auto sourceFramebuffer = ResolveFramebuffer(framebufferHandle))
                    sourceTextureID = sourceFramebuffer->GetColorAttachmentRendererID(sinkKey.ColorAttachmentIndex);
            }
            else
            {
                m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                    .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                    .PassName = "<external-sink>",
                    .Resource = sinkKey.SourceResource,
                    .Message = "External texture sink requested with stale or invalid source handle",
                });
                continue;
            }

            if (sourceTextureID == 0)
                continue;

            glCopyImageSubData(sourceTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               sink.TextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               static_cast<GLsizei>(sink.Width),
                               static_cast<GLsizei>(sink.Height),
                               1);
            if (sink.ValidFlag)
                *sink.ValidFlag = true;
        }

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

            if (const auto resourceName = GetResourceName(extract.Handle); !diagnoseExtractionResource(resourceName, "<extract-texture>"))
                continue;
            extract.Callback(ResolveTexture(extract.Handle));
        }

        for (const auto& extract : m_HistoryTextureExtracts)
        {
            std::string sourceResource;
            u32 sourceTextureID = 0;

            if (extract.Kind == HistoryTextureExtract::SourceKind::Texture)
            {
                if (!IsTextureHandleCurrent(extract.SourceTextureHandle))
                {
                    m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                        .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                        .PassName = "<extract-history>",
                        .Resource = {},
                        .Message = "History extraction requested with stale or invalid source handle",
                    });
                    continue;
                }

                sourceResource = GetResourceName(extract.SourceTextureHandle);
                sourceTextureID = ResolveTexture(extract.SourceTextureHandle);
            }
            else
            {
                if (!IsFramebufferHandleCurrent(extract.SourceFramebufferHandle))
                {
                    m_BarrierDiagnostics.push_back(BarrierDiagnostic{
                        .Kind = BarrierDiagnosticKind::StaleExtractionHandle,
                        .PassName = "<extract-history>",
                        .Resource = {},
                        .Message = "History extraction requested with stale or invalid source handle",
                    });
                    continue;
                }

                sourceResource = GetResourceName(extract.SourceFramebufferHandle);
                if (auto sourceFramebuffer = ResolveFramebuffer(extract.SourceFramebufferHandle))
                    sourceTextureID = sourceFramebuffer->GetColorAttachmentRendererID(extract.ColorAttachmentIndex);
            }

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

            extract.Callback(sourceTextureID);
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

            if (const auto resourceName = GetResourceName(extract.Handle); !diagnoseExtractionResource(resourceName, "<extract-framebuffer>"))
                continue;
            extract.Callback(ResolveFramebuffer(extract.Handle));
        }

        m_TextureExtracts.clear();
        m_HistoryTextureExtracts.clear();
        m_FramebufferExtracts.clear();
    }

    // -------------------------------------------------------------------
    // Transient resource allocation (builder support)
    // -------------------------------------------------------------------

    RGTextureHandle RenderGraph::AllocateTransientTextureHandle(std::string_view name, const RGResourceDesc& desc)
    {
        const auto stableName = std::string(name);
        const auto refreshLatestVersionLookup = [this, &stableName](const RGTextureHandle handle)
        {
            if (HasExplicitVersionQualifier(stableName))
                m_LatestTextureHandlesByBaseName[std::string(GetVersionLookupBaseName(stableName))] = handle;
        };

        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::Texture2D;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = stableName;

        m_TransientResourceDescs[stableName] = transientDesc;
        m_ResourceRegistryDirty = true;
        if (const u32 nameId = m_ResourceNames.Find(stableName); nameId != 0u)
            m_ExternallyBackedTransientTextures.erase(nameId);

        if (const auto existingIt = m_TextureHandlesByName.find(stableName);
            existingIt != m_TextureHandlesByName.end() &&
            existingIt->second.Index < m_TextureHandleSlots.size())
        {
            auto& slot = m_TextureHandleSlots[existingIt->second.Index];
            if (slot.Generation == 0)
                slot.Generation = 1;

            slot.Alive = true;
            slot.Name = stableName;
            slot.IsPlaceholder = false;
            slot.PlaceholderReason.clear();
            slot.PlaceholderWarnedThisFrame = false;

            const auto freeIt = std::ranges::find(m_FreeTextureHandleIndices,
                                                  existingIt->second.Index);
            if (freeIt != m_FreeTextureHandleIndices.end())
                m_FreeTextureHandleIndices.erase(freeIt);

            if (existingIt->second.Index >= m_PhysicalTextures.size())
                m_PhysicalTextures.resize(static_cast<sizet>(existingIt->second.Index) + 1u);

            const RGTextureHandle handle{ existingIt->second.Index, slot.Generation };
            existingIt->second = handle;
            refreshLatestVersionLookup(handle);
            return handle;
        }

        if (m_FreeTextureHandleIndices.empty())
        {
            u32 index = static_cast<u32>(m_TextureHandleSlots.size());
            m_TextureHandleSlots.emplace_back();
            m_TextureHandleSlots[index].Alive = true;
            m_TextureHandleSlots[index].Name = stableName;
            m_TextureHandleSlots[index].IsPlaceholder = false;
            m_TextureHandleSlots[index].PlaceholderReason.clear();
            m_TextureHandleSlots[index].PlaceholderWarnedThisFrame = false;
            m_PhysicalTextures.emplace_back();
            const RGTextureHandle handle{ index, static_cast<u32>(m_TextureHandleSlots[index].Generation) };
            m_TextureHandlesByName[stableName] = handle;
            refreshLatestVersionLookup(handle);
            return handle;
        }

        u32 index = m_FreeTextureHandleIndices.back();
        m_FreeTextureHandleIndices.pop_back();
        auto& slot = m_TextureHandleSlots[index];
        if (slot.Generation == 0)
            slot.Generation = 1;
        slot.Alive = true;
        slot.Name = stableName;
        slot.IsPlaceholder = false;
        slot.PlaceholderReason.clear();
        slot.PlaceholderWarnedThisFrame = false;
        if (index >= m_PhysicalTextures.size())
            m_PhysicalTextures.resize(static_cast<sizet>(index) + 1u);

        const RGTextureHandle handle{ index, slot.Generation };
        m_TextureHandlesByName[stableName] = handle;
        refreshLatestVersionLookup(handle);
        return handle;
    }

    RGTextureHandle RenderGraph::DeclareTransientTexture(std::string_view name, const RGResourceDesc& desc)
    {
        return AllocateTransientTextureHandle(name, desc);
    }

    RGTextureHandle RenderGraph::DeclareTransientTexture(std::string_view name,
                                                         const RGResourceDesc& desc,
                                                         const u32 backingTextureID)
    {
        if (backingTextureID == 0)
            return DeclareTransientTexture(name, desc);

        auto handle = AllocateTransientTextureHandle(name, desc);
        if (!handle.IsValid())
            return handle;

        const auto stableName = std::string(name);
        m_ExternallyBackedTransientTextures.insert(m_ResourceNames.Intern(stableName));

        if (handle.Index >= m_PhysicalTextures.size())
            m_PhysicalTextures.resize(static_cast<sizet>(handle.Index) + 1u);

        auto& physicalTexture = m_PhysicalTextures[handle.Index];
        physicalTexture.TextureID = backingTextureID;
        physicalTexture.IsHistory = false;
        return handle;
    }

    RGFramebufferHandle RenderGraph::AllocateTransientFramebufferHandle(std::string_view name, const RGResourceDesc& desc)
    {
        const auto stableName = std::string(name);
        const auto refreshLatestVersionLookup = [this, &stableName](const RGFramebufferHandle handle)
        {
            if (HasExplicitVersionQualifier(stableName))
                m_LatestFramebufferHandlesByBaseName[std::string(GetVersionLookupBaseName(stableName))] = handle;
        };

        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::Framebuffer;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = stableName;

        m_TransientResourceDescs[stableName] = transientDesc;
        m_ResourceRegistryDirty = true;
        if (const u32 nameId = m_ResourceNames.Find(stableName); nameId != 0u)
            m_ExternallyBackedTransientFramebuffers.erase(nameId);

        if (const auto existingIt = m_FramebufferHandlesByName.find(stableName);
            existingIt != m_FramebufferHandlesByName.end() &&
            existingIt->second.Index < m_FramebufferHandleSlots.size())
        {
            auto& slot = m_FramebufferHandleSlots[existingIt->second.Index];
            if (slot.Generation == 0)
                slot.Generation = 1;

            slot.Alive = true;
            slot.Name = stableName;
            slot.IsPlaceholder = false;
            slot.PlaceholderReason.clear();
            slot.PlaceholderWarnedThisFrame = false;

            const auto freeIt = std::ranges::find(m_FreeFramebufferHandleIndices,
                                                  existingIt->second.Index);
            if (freeIt != m_FreeFramebufferHandleIndices.end())
                m_FreeFramebufferHandleIndices.erase(freeIt);

            if (existingIt->second.Index >= m_PhysicalFramebuffers.size())
                m_PhysicalFramebuffers.resize(static_cast<sizet>(existingIt->second.Index) + 1u);

            const RGFramebufferHandle handle{ existingIt->second.Index, slot.Generation };
            existingIt->second = handle;
            refreshLatestVersionLookup(handle);
            return handle;
        }

        if (m_FreeFramebufferHandleIndices.empty())
        {
            u32 index = static_cast<u32>(m_FramebufferHandleSlots.size());
            m_FramebufferHandleSlots.emplace_back();
            m_FramebufferHandleSlots[index].Alive = true;
            m_FramebufferHandleSlots[index].Name = stableName;
            m_FramebufferHandleSlots[index].IsPlaceholder = false;
            m_FramebufferHandleSlots[index].PlaceholderReason.clear();
            m_FramebufferHandleSlots[index].PlaceholderWarnedThisFrame = false;
            m_PhysicalFramebuffers.emplace_back();
            const RGFramebufferHandle handle{ index, static_cast<u32>(m_FramebufferHandleSlots[index].Generation) };
            m_FramebufferHandlesByName[stableName] = handle;
            refreshLatestVersionLookup(handle);
            return handle;
        }

        u32 index = m_FreeFramebufferHandleIndices.back();
        m_FreeFramebufferHandleIndices.pop_back();
        auto& slot = m_FramebufferHandleSlots[index];
        if (slot.Generation == 0)
            slot.Generation = 1;
        slot.Alive = true;
        slot.Name = stableName;
        slot.IsPlaceholder = false;
        slot.PlaceholderReason.clear();
        slot.PlaceholderWarnedThisFrame = false;
        if (index >= m_PhysicalFramebuffers.size())
            m_PhysicalFramebuffers.resize(static_cast<sizet>(index) + 1u);

        const RGFramebufferHandle handle{ index, slot.Generation };
        m_FramebufferHandlesByName[stableName] = handle;
        refreshLatestVersionLookup(handle);
        return handle;
    }

    RGFramebufferHandle RenderGraph::DeclareTransientFramebuffer(std::string_view name, const RGResourceDesc& desc)
    {
        return AllocateTransientFramebufferHandle(name, desc);
    }

    RGFramebufferHandle RenderGraph::DeclareTransientFramebuffer(std::string_view name,
                                                                 const RGResourceDesc& desc,
                                                                 const Ref<Framebuffer>& backingFramebuffer)
    {
        if (!backingFramebuffer)
            return DeclareTransientFramebuffer(name, desc);

        auto handle = AllocateTransientFramebufferHandle(name, desc);
        if (!handle.IsValid())
            return handle;

        const auto stableName = std::string(name);
        m_ExternallyBackedTransientFramebuffers.insert(m_ResourceNames.Intern(stableName));

        if (handle.Index >= m_PhysicalFramebuffers.size())
            m_PhysicalFramebuffers.resize(static_cast<sizet>(handle.Index) + 1u);

        m_PhysicalFramebuffers[handle.Index].FB = backingFramebuffer;
        return handle;
    }

    RGBufferHandle RenderGraph::AllocateTransientBufferHandle(std::string_view name, const RGResourceDesc& desc)
    {
        const auto stableName = std::string(name);
        const auto refreshLatestVersionLookup = [this, &stableName](const RGBufferHandle handle)
        {
            if (HasExplicitVersionQualifier(stableName))
                m_LatestBufferHandlesByBaseName[std::string(GetVersionLookupBaseName(stableName))] = handle;
        };

        auto transientDesc = desc;
        transientDesc.Imported = false;
        if (transientDesc.Kind == ResourceHandle::Kind::Unknown)
            transientDesc.Kind = ResourceHandle::Kind::StorageBuffer;
        if (transientDesc.DebugName.empty())
            transientDesc.DebugName = stableName;

        m_TransientResourceDescs[stableName] = transientDesc;
        m_ResourceRegistryDirty = true;

        if (const auto existingIt = m_BufferHandlesByName.find(stableName);
            existingIt != m_BufferHandlesByName.end() &&
            existingIt->second.Index < m_BufferHandleSlots.size())
        {
            auto& slot = m_BufferHandleSlots[existingIt->second.Index];
            if (slot.Generation == 0)
                slot.Generation = 1;

            slot.Alive = true;
            slot.Name = stableName;
            slot.IsPlaceholder = false;
            slot.PlaceholderReason.clear();
            slot.PlaceholderWarnedThisFrame = false;

            const auto freeIt = std::ranges::find(m_FreeBufferHandleIndices,
                                                  existingIt->second.Index);
            if (freeIt != m_FreeBufferHandleIndices.end())
                m_FreeBufferHandleIndices.erase(freeIt);

            if (existingIt->second.Index >= m_PhysicalBuffers.size())
                m_PhysicalBuffers.resize(static_cast<sizet>(existingIt->second.Index) + 1u);

            const RGBufferHandle handle{ existingIt->second.Index, slot.Generation };
            existingIt->second = handle;
            refreshLatestVersionLookup(handle);
            return handle;
        }

        if (m_FreeBufferHandleIndices.empty())
        {
            u32 index = static_cast<u32>(m_BufferHandleSlots.size());
            m_BufferHandleSlots.emplace_back();
            m_BufferHandleSlots[index].Alive = true;
            m_BufferHandleSlots[index].Name = stableName;
            m_BufferHandleSlots[index].IsPlaceholder = false;
            m_BufferHandleSlots[index].PlaceholderReason.clear();
            m_BufferHandleSlots[index].PlaceholderWarnedThisFrame = false;
            m_PhysicalBuffers.emplace_back();
            const RGBufferHandle handle{ index, static_cast<u32>(m_BufferHandleSlots[index].Generation) };
            m_BufferHandlesByName[stableName] = handle;
            refreshLatestVersionLookup(handle);
            return handle;
        }

        u32 index = m_FreeBufferHandleIndices.back();
        m_FreeBufferHandleIndices.pop_back();
        auto& slot = m_BufferHandleSlots[index];
        if (slot.Generation == 0)
            slot.Generation = 1;
        slot.Alive = true;
        slot.Name = stableName;
        slot.IsPlaceholder = false;
        slot.PlaceholderReason.clear();
        slot.PlaceholderWarnedThisFrame = false;
        if (index >= m_PhysicalBuffers.size())
            m_PhysicalBuffers.resize(static_cast<sizet>(index) + 1u);

        const RGBufferHandle handle{ index, slot.Generation };
        m_BufferHandlesByName[stableName] = handle;
        refreshLatestVersionLookup(handle);
        return handle;
    }

    RGResourceDesc RenderGraph::BuildVersionedResourceDesc(std::string_view sourceResource,
                                                           const ResourceHandle::Kind fallbackKind,
                                                           std::string_view versionedName) const
    {
        const auto sourceName = std::string(sourceResource);
        const auto debugName = std::string(versionedName);

        const auto cloneDesc = [&debugName, fallbackKind](const RGResourceDesc& sourceDesc)
        {
            auto clonedDesc = sourceDesc;
            clonedDesc.Imported = false;
            clonedDesc.IsPlaceholder = false;
            clonedDesc.PlaceholderReason.clear();
            clonedDesc.DebugName = debugName;
            if (clonedDesc.Kind == ResourceHandle::Kind::Unknown)
                clonedDesc.Kind = fallbackKind;
            return clonedDesc;
        };

        if (const auto transientIt = m_TransientResourceDescs.find(sourceName);
            transientIt != m_TransientResourceDescs.end())
        {
            return cloneDesc(transientIt->second);
        }

        if (const auto importedIt = m_ImportedResources.find(sourceName);
            importedIt != m_ImportedResources.end())
        {
            return cloneDesc(importedIt->second);
        }

        EnsureResourceRegistryBuilt();
        if (const auto registeredIt = m_ResourceRegistry.find(sourceName);
            registeredIt != m_ResourceRegistry.end())
        {
            return cloneDesc(registeredIt->second.Desc);
        }

        auto desc = RGResourceDesc::FromHandleKind(fallbackKind, versionedName);
        desc.Imported = false;
        return desc;
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

        // Transparent lookups: pass `name` (string_view) directly into the
        // transparent map — no per-call std::string construction.
        if (!HasExplicitVersionQualifier(name))
        {
            if (const auto versionIt = m_LatestTextureHandlesByBaseName.find(name);
                versionIt != m_LatestTextureHandlesByBaseName.end() && IsTextureHandleCurrent(versionIt->second))
            {
                return versionIt->second;
            }
        }

        if (auto it = m_TextureHandlesByName.find(name); it != m_TextureHandlesByName.end())
            return it->second;

        // Base-name alias fallback: re-resolve through the target name so
        // the caller picks up the latest version of whichever upstream is
        // active this frame. One-hop only (alias targets are stored as
        // canonical base names; chained aliasing isn't required).
        if (const u32 nameId = m_ResourceNames.Find(name); nameId != 0u)
        {
            if (const auto aliasIt = m_TextureBaseNameAliases.find(nameId);
                aliasIt != m_TextureBaseNameAliases.end() && aliasIt->second != nameId)
            {
                return GetTextureHandle(m_ResourceNames.NameOf(aliasIt->second));
            }
        }

        return {};
    }

    RGBufferHandle RenderGraph::GetBufferHandle(std::string_view name) const
    {
        EnsureResourceRegistryBuilt();

        if (!HasExplicitVersionQualifier(name))
        {
            if (const auto versionIt = m_LatestBufferHandlesByBaseName.find(name);
                versionIt != m_LatestBufferHandlesByBaseName.end() && IsBufferHandleCurrent(versionIt->second))
            {
                return versionIt->second;
            }
        }

        if (auto it = m_BufferHandlesByName.find(name); it != m_BufferHandlesByName.end())
            return it->second;

        return {};
    }

    RGFramebufferHandle RenderGraph::GetFramebufferHandle(std::string_view name) const
    {
        EnsureResourceRegistryBuilt();

        if (!HasExplicitVersionQualifier(name))
        {
            if (const auto versionIt = m_LatestFramebufferHandlesByBaseName.find(name);
                versionIt != m_LatestFramebufferHandlesByBaseName.end() && IsFramebufferHandleCurrent(versionIt->second))
            {
                return versionIt->second;
            }
        }

        if (auto it = m_FramebufferHandlesByName.find(name); it != m_FramebufferHandlesByName.end())
            return it->second;

        // Base-name alias fallback (see GetTextureHandle for rationale).
        if (const u32 nameId = m_ResourceNames.Find(name); nameId != 0u)
        {
            if (const auto aliasIt = m_FramebufferBaseNameAliases.find(nameId);
                aliasIt != m_FramebufferBaseNameAliases.end() && aliasIt->second != nameId)
            {
                return GetFramebufferHandle(m_ResourceNames.NameOf(aliasIt->second));
            }
        }

        return {};
    }

    void RenderGraph::RegisterTextureAlias(std::string_view aliasName, std::string_view targetBaseName)
    {
        if (aliasName.empty() || targetBaseName.empty() || aliasName == targetBaseName)
            return;
        const u32 aliasId = m_ResourceNames.Intern(aliasName);
        const u32 targetId = m_ResourceNames.Intern(targetBaseName);
        m_TextureBaseNameAliases[aliasId] = targetId;
    }

    void RenderGraph::RegisterFramebufferAlias(std::string_view aliasName, std::string_view targetBaseName)
    {
        if (aliasName.empty() || targetBaseName.empty() || aliasName == targetBaseName)
            return;
        const u32 aliasId = m_ResourceNames.Intern(aliasName);
        const u32 targetId = m_ResourceNames.Intern(targetBaseName);
        m_FramebufferBaseNameAliases[aliasId] = targetId;
    }

    std::map<std::string, std::string> RenderGraph::GetTextureBaseNameAliases() const
    {
        std::map<std::string, std::string> result;
        for (const auto& [aliasId, targetId] : m_TextureBaseNameAliases)
            result.emplace(std::string(m_ResourceNames.NameOf(aliasId)),
                           std::string(m_ResourceNames.NameOf(targetId)));
        return result;
    }

    std::map<std::string, std::string> RenderGraph::GetFramebufferBaseNameAliases() const
    {
        std::map<std::string, std::string> result;
        for (const auto& [aliasId, targetId] : m_FramebufferBaseNameAliases)
            result.emplace(std::string(m_ResourceNames.NameOf(aliasId)),
                           std::string(m_ResourceNames.NameOf(targetId)));
        return result;
    }

    std::string RenderGraph::ReverseResolveTextureName(RGTextureHandle handle) const
    {
        if (!handle.IsValid())
            return {};
        EnsureResourceRegistryBuilt();
        for (const auto& info : m_RegisteredResources)
        {
            if (info.TextureHandle == handle)
                return info.Name;
        }
        return {};
    }

    std::string RenderGraph::ReverseResolveFramebufferName(RGFramebufferHandle handle) const
    {
        if (!handle.IsValid())
            return {};
        EnsureResourceRegistryBuilt();
        for (const auto& info : m_RegisteredResources)
        {
            if (info.FramebufferHandle == handle)
                return info.Name;
        }
        return {};
    }

    std::string RenderGraph::FindAttachmentViewParent(std::string_view name) const
    {
        if (name.empty())
            return {};
        const auto it = m_TextureViewDefinitions.find(std::string(name));
        if (it == m_TextureViewDefinitions.end())
            return {};
        // Only color-attachment views forward to a framebuffer parent.
        // Other view kinds (mip, layer, multisample resolve) parent to a
        // texture resource that has its own lifetime tracking already.
        if (it->second.Kind != TextureViewKind::FramebufferColorAttachment &&
            it->second.Kind != TextureViewKind::FramebufferDepthAttachment)
            return {};
        return it->second.ParentResource;
    }

    auto RenderGraph::GetLastWriterPassName(std::string_view resourceName) const -> const std::string&
    {
        static const std::string emptyName;
        if (resourceName.empty())
            return emptyName;
        if (const auto it = m_LastWriterPassNameByResource.find(std::string(resourceName));
            it != m_LastWriterPassNameByResource.end())
        {
            return it->second;
        }
        return emptyName;
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

        if (!ContainsGraphEntry(outputPass))
        {
            OLO_CORE_ERROR("RenderGraph::ConnectPass: Output pass '{}' not found!", outputPass);
            return;
        }

        if (!ContainsGraphEntry(inputPass))
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

        if (!ContainsGraphEntry(beforePass))
        {
            OLO_CORE_ERROR("RenderGraph::AddExecutionDependency: Pass '{}' not found!", beforePass);
            return;
        }

        if (!ContainsGraphEntry(afterPass))
        {
            OLO_CORE_ERROR("RenderGraph::AddExecutionDependency: Pass '{}' not found!", afterPass);
            return;
        }

        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Adding execution dependency (ordering only): {} -> {}", beforePass, afterPass);

        // Only add dependency for execution ordering, no framebuffer piping (avoid duplicates)
        if (auto& deps = m_Dependencies[afterPass]; std::ranges::find(deps, beforePass) == deps.end())
        {
            deps.push_back(beforePass);
        }

        if (persistent)
        {
            auto& explicitDeps = m_ExplicitDependencies[afterPass];
            if (std::ranges::find(explicitDeps, beforePass) == explicitDeps.end())
            {
                explicitDeps.push_back(beforePass);
            }
        }

        m_DependencyGraphDirty = true;
    }

    void RenderGraph::Execute()
    {
        OLO_PROFILE_FUNCTION();
        OLO_PERF_SCOPE_AUTO("RG::Execute");

        m_LastExecutionTimings.clear();
        m_LastExecutionTimings.reserve(m_ExecutionOrder.size());
        m_ResolveFailures.clear();

        for (auto& slot : m_TextureHandleSlots)
            slot.PlaceholderWarnedThisFrame = false;
        for (auto& slot : m_FramebufferHandleSlots)
            slot.PlaceholderWarnedThisFrame = false;
        for (auto& slot : m_BufferHandleSlots)
            slot.PlaceholderWarnedThisFrame = false;

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Cycle detected — m_ExecutionOrder is empty. Abort execution;
                // running with a partial topo order would execute the wrong
                // subset of passes in the wrong order. Keep the dirty flag
                // set so a corrected graph can retry.
                OLO_CORE_ERROR("RenderGraph::Execute: aborting because dependency graph rebuild failed");
                m_TransientPool.ReleaseAll();
                m_TransientPool.Trim(m_TransientPoolMaxBucketSize);
                return;
            }
            ResolveFinalPass();
            m_DependencyGraphDirty = false;

            // Compute backward reachability from the final output.
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

        // Run the pre-built submission-plan IR through the extracted plan
        // executor module (Phase 7 slice 7). The executor is a thin loop
        // over the cached IR that dispatches each command to the abstract
        // RGCommandContext — backend bindings stay one level deeper in
        // OpenGLRendererAPI.
        RGCommandContext commandContext;
        commandContext.SetRenderGraph(this);
        m_LastExecutionTimings = RenderGraphPlanExecutor::ExecutePlan({
            .SubmissionPlan = m_CachedSubmissionPlan,
            .Context = commandContext,
            .RuntimeBarrierExecutionEnabled = m_RuntimeBarrierExecutionEnabled,
            .IsPassReachable = [this](const std::string& passName)
            { return IsPassReachable(passName); },
            .BatchEventHook = m_BatchEventHook,
            .PostPassHook = m_PostPassHook,
            .GraphForPostPassHook = this,
        });
        commandContext.SetRenderGraph(nullptr);

        // Third pass: fire extraction callbacks queued by passes during Execute().
        FlushExtractions();
        m_TransientPool.ReleaseAll();
        m_TransientPool.Trim(m_TransientPoolMaxBucketSize);
    }

    void RenderGraph::RecordResolveFailure(const std::string_view passName, const std::string_view reason) const
    {
        if (passName.empty() || reason.empty())
            return;

        auto existing = std::ranges::find_if(m_ResolveFailures,
                                             [passName, reason](const ResolveFailure& failure)
                                             {
                                                 return failure.PassName == passName &&
                                                        failure.Reason == reason;
                                             });

        if (existing != m_ResolveFailures.end())
        {
            ++existing->Count;
            return;
        }

        m_ResolveFailures.push_back(ResolveFailure{
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
            case RGResourceFormat::R32Int:
                return ImageFormat::R32I;
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
            case RGResourceFormat::R32Int:
                return FramebufferTextureFormat::RED_INTEGER;
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

        // Per (alias-group + alias-slot) — used during pass 1 to acquire a
        // GPU object once and reuse it for every WillAllocate=true entry that
        // shares the same group+slot. The transient planner already proved
        // these entries have non-overlapping lifetimes, so a single physical
        // resource backs them all.
        std::unordered_map<std::string, Ref<Texture>> textureAliasesBySlot;
        std::unordered_map<std::string, Ref<Framebuffer>> framebufferAliasesBySlot;
        std::unordered_map<std::string, Ref<StorageBuffer>> bufferAliasesBySlot;

        // Per alias-group only (any slot) — used during pass 2 to wire a
        // WillAllocate=false handle to *some* live sibling in the same group.
        // These handles previously got their physical pointer nulled even
        // when a sibling alias still held the live GPU object, breaking the
        // invariant that "the GPU object is shared by everyone in the group."
        // External consumers querying these handles (editor picking, frame
        // capture thumbnails) saw null where the actual resource was sitting
        // one alias-step away.
        std::unordered_map<std::string, Ref<Texture>> textureAliasesByGroup;
        std::unordered_map<std::string, Ref<Framebuffer>> framebufferAliasesByGroup;
        std::unordered_map<std::string, Ref<StorageBuffer>> bufferAliasesByGroup;

        textureAliasesBySlot.reserve(m_TransientPlan.size());
        framebufferAliasesBySlot.reserve(m_TransientPlan.size());
        bufferAliasesBySlot.reserve(m_TransientPlan.size());
        textureAliasesByGroup.reserve(m_TransientPlan.size());
        framebufferAliasesByGroup.reserve(m_TransientPlan.size());
        bufferAliasesByGroup.reserve(m_TransientPlan.size());

        const auto entryIsManaged = [this](const RenderGraph::TransientPlanEntry& entry) -> bool
        {
            // Skip resources whose physical backing is owned outside the
            // transient pool — imported resources keep the importer's slot,
            // externally-backed transients resolve to caller-supplied backing.
            // Either case must preserve the existing physical pointer.
            return !m_ImportedResources.contains(entry.Resource) &&
                   !IsExternallyBackedTransientResource(entry.Resource);
        };

        // -------------------------------------------------------------------
        // Pass 1: acquire GPU objects for WillAllocate=true entries and wire
        // each entry's handle to its slot's physical pointer. Populate the
        // by-group map alongside so pass 2 can find sibling aliases.
        // -------------------------------------------------------------------
        for (const auto& entry : m_TransientPlan)
        {
            if (!entryIsManaged(entry))
                continue;

            const auto descriptorIt = m_TransientResourceDescs.find(entry.Resource);
            if (descriptorIt == m_TransientResourceDescs.end())
                continue;

            if (!entry.WillAllocate)
                continue; // pass 2

            const auto& desc = descriptorIt->second;
            const auto aliasKey = entry.AliasGroup + "#" + std::to_string(entry.AliasSlot);

            switch (desc.Kind)
            {
                case ResourceHandle::Kind::Texture2D:
                case ResourceHandle::Kind::Texture2DArray:
                case ResourceHandle::Kind::TextureCube:
                case ResourceHandle::Kind::TextureCubeArray:
                {
                    auto textureIt = textureAliasesBySlot.find(aliasKey);
                    if (textureIt == textureAliasesBySlot.end())
                    {
                        TextureSpecification spec;
                        spec.Width = desc.Width;
                        spec.Height = desc.Height;
                        spec.Format = ToImageFormat(desc.Format);
                        spec.GenerateMips = desc.MipLevels > 1;
                        spec.MipLevels = desc.MipLevels;
                        spec.Samples = std::max(desc.Samples, 1u);

                        auto transientTexture = m_TransientPool.AcquireTexture(spec);
                        textureIt = textureAliasesBySlot.emplace(aliasKey, transientTexture).first;
                        // First time we've seen this alias group with a live
                        // physical — record it so pass 2's siblings can share.
                        textureAliasesByGroup.try_emplace(entry.AliasGroup, transientTexture);
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
                    auto framebufferIt = framebufferAliasesBySlot.find(aliasKey);
                    if (framebufferIt == framebufferAliasesBySlot.end())
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
                        framebufferIt = framebufferAliasesBySlot.emplace(aliasKey, transientFramebuffer).first;
                        framebufferAliasesByGroup.try_emplace(entry.AliasGroup, transientFramebuffer);
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
                    auto bufferIt = bufferAliasesBySlot.find(aliasKey);
                    if (bufferIt == bufferAliasesBySlot.end())
                    {
                        auto transientBuffer = m_TransientPool.AcquireBuffer(desc.Width);
                        bufferIt = bufferAliasesBySlot.emplace(aliasKey, transientBuffer).first;
                        bufferAliasesByGroup.try_emplace(entry.AliasGroup, transientBuffer);
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

        // -------------------------------------------------------------------
        // Pass 2: WillAllocate=false entries inherit the physical resource of
        // any sibling in the same alias group. If no sibling exists (i.e. the
        // entire group is unallocated this frame), the handle's physical is
        // explicitly cleared so a stale pointer from a previous frame can't
        // resolve to a now-recycled GPU object.
        // -------------------------------------------------------------------
        for (const auto& entry : m_TransientPlan)
        {
            if (!entryIsManaged(entry))
                continue;

            const auto descriptorIt = m_TransientResourceDescs.find(entry.Resource);
            if (descriptorIt == m_TransientResourceDescs.end())
                continue;

            if (entry.WillAllocate)
                continue; // already handled in pass 1

            const auto& desc = descriptorIt->second;

            switch (desc.Kind)
            {
                case ResourceHandle::Kind::Texture2D:
                case ResourceHandle::Kind::Texture2DArray:
                case ResourceHandle::Kind::TextureCube:
                case ResourceHandle::Kind::TextureCubeArray:
                {
                    Ref<Texture> sibling;
                    if (const auto it = textureAliasesByGroup.find(entry.AliasGroup); it != textureAliasesByGroup.end())
                        sibling = it->second;

                    if (const auto texHandleIt = m_TextureHandlesByName.find(entry.Resource);
                        texHandleIt != m_TextureHandlesByName.end() &&
                        texHandleIt->second.Index < m_PhysicalTextures.size())
                    {
                        m_PhysicalTextures[texHandleIt->second.Index].TextureID = sibling ? sibling->GetRendererID() : 0;
                    }
                    break;
                }
                case ResourceHandle::Kind::Framebuffer:
                {
                    Ref<Framebuffer> sibling;
                    if (const auto it = framebufferAliasesByGroup.find(entry.AliasGroup); it != framebufferAliasesByGroup.end())
                        sibling = it->second;

                    if (const auto fbHandleIt = m_FramebufferHandlesByName.find(entry.Resource);
                        fbHandleIt != m_FramebufferHandlesByName.end() &&
                        fbHandleIt->second.Index < m_PhysicalFramebuffers.size())
                    {
                        m_PhysicalFramebuffers[fbHandleIt->second.Index].FB = sibling;
                    }
                    break;
                }
                case ResourceHandle::Kind::UniformBuffer:
                case ResourceHandle::Kind::StorageBuffer:
                {
                    Ref<StorageBuffer> sibling;
                    if (const auto it = bufferAliasesByGroup.find(entry.AliasGroup); it != bufferAliasesByGroup.end())
                        sibling = it->second;

                    if (const auto bufferHandleIt = m_BufferHandlesByName.find(entry.Resource);
                        bufferHandleIt != m_BufferHandlesByName.end() &&
                        bufferHandleIt->second.Index < m_PhysicalBuffers.size())
                    {
                        m_PhysicalBuffers[bufferHandleIt->second.Index].BufferID = sibling ? sibling->GetRendererID() : 0;
                    }
                    break;
                }
                case ResourceHandle::Kind::Unknown:
                default:
                    break;
            }
        }
    }

    void RenderGraph::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        const bool dimensionsChanged = (width != m_PhysicalWidth) || (height != m_PhysicalHeight);
        m_PhysicalWidth = width;
        m_PhysicalHeight = height;

        for (auto& [name, node] : m_NodeLookup)
        {
            node->ResizeFramebuffer(width, height);
        }

        // Evict cached transient framebuffers / textures whose dimensions are
        // now stale. The pool keys by full spec (incl. width/height), so on a
        // viewport resize the post-resize Acquire calls all miss into fresh
        // buckets and the old buckets sit around forever — both leaking GPU
        // memory (each orphaned FB carries its color+depth attachments) AND
        // letting the alias-group resolver hand a stale-size sibling to a
        // downstream pass, which then blits old-size content into the new-size
        // target and produces visible "duplicated, offset" ghost geometry.
        //
        // Clear() also wipes any acquired-but-not-yet-released items. That is
        // safe here because Resize() runs in OnUpdate before the frame's
        // rendering begins; the previous frame's ReleaseAll has already
        // returned everything to the pool.
        if (dimensionsChanged)
        {
            m_TransientPool.Clear();
        }

        // Physical resize resets render viewport overrides on all FBOs.
        // Re-apply the current DRS scale so the render viewport is correct
        // for the new physical dimensions (scale 1.0 means no-op).
        if (m_RenderScale < 1.0f)
        {
            const auto renderW = static_cast<u32>(glm::floor(static_cast<f32>(m_PhysicalWidth) * m_RenderScale));
            const auto renderH = static_cast<u32>(glm::floor(static_cast<f32>(m_PhysicalHeight) * m_RenderScale));
            for (auto& [name, node] : m_NodeLookup)
            {
                node->ApplyRenderViewport(renderW, renderH);
            }
        }
    }

    void RenderGraph::SetRenderScale(const f32 scale)
    {
        OLO_PROFILE_FUNCTION();
        m_RenderScale = glm::clamp(scale, 0.25f, 1.0f);

        if (m_PhysicalWidth == 0 || m_PhysicalHeight == 0)
            return;

        const auto renderW = static_cast<u32>(glm::floor(static_cast<f32>(m_PhysicalWidth) * m_RenderScale));
        const auto renderH = static_cast<u32>(glm::floor(static_cast<f32>(m_PhysicalHeight) * m_RenderScale));

        for (auto& [name, node] : m_NodeLookup)
        {
            if (m_RenderScale >= 1.0f)
            {
                node->ApplyRenderViewport(0u, 0u);
            }
            else
            {
                node->ApplyRenderViewport(renderW, renderH);
            }
        }
    }

    u32 RenderGraph::GetRenderWidth() const
    {
        if (m_PhysicalWidth == 0)
            return 0u;
        return static_cast<u32>(glm::floor(static_cast<f32>(m_PhysicalWidth) * m_RenderScale));
    }

    u32 RenderGraph::GetRenderHeight() const
    {
        if (m_PhysicalHeight == 0)
            return 0u;
        return static_cast<u32>(glm::floor(static_cast<f32>(m_PhysicalHeight) * m_RenderScale));
    }

    glm::vec2 RenderGraph::GetRenderScaleBounds() const
    {
        if (m_PhysicalWidth == 0 || m_PhysicalHeight == 0)
            return { 1.0f, 1.0f };
        const auto rw = static_cast<f32>(GetRenderWidth());
        const auto rh = static_cast<f32>(GetRenderHeight());
        return { rw / static_cast<f32>(m_PhysicalWidth), rh / static_cast<f32>(m_PhysicalHeight) };
    }

    void RenderGraph::SetFinalPass(const std::string& passName)
    {
        m_FinalPassName = passName;
        m_HasExplicitFinalPass = true;
        m_DependencyGraphDirty = true;
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

    std::vector<RenderGraph::NodeSubmissionInfo> RenderGraph::GetNodeSubmissionInfo() const
    {
        std::vector<NodeSubmissionInfo> result;
        result.reserve(m_NodeLookup.size());

        const auto appendEntryInfo = [this, &result](const std::string& name)
        {
            if (const auto nodeIt = m_NodeLookup.find(name); nodeIt != m_NodeLookup.end() && nodeIt->second)
            {
                const auto declarationsIt = m_PassAccessDeclarations.find(name);
                result.push_back(NodeSubmissionInfo{
                    .NodeName = name,
                    .DeclaresResources = declarationsIt != m_PassAccessDeclarations.end() &&
                                         !declarationsIt->second.empty(),
                    .WorkType = nodeIt->second->GetPassWorkType(),
                    .AsyncComputeCandidate = nodeIt->second->IsAsyncComputeCandidate(),
                });
            }
        };

        std::unordered_set<std::string> visited;
        visited.reserve(m_InsertionOrder.size());

        for (const auto& name : m_InsertionOrder)
        {
            appendEntryInfo(name);
            visited.insert(name);
        }

        for (const auto& [name, node] : m_NodeLookup)
        {
            if (!visited.contains(name))
                appendEntryInfo(name);
        }

        return result;
    }

    bool RenderGraph::UpdateDependencyGraph()
    {
        OLO_PROFILE_FUNCTION();

        m_ExecutionOrder.clear();

        // Iterative DFS-based topological sort. Previously this used a
        // recursive std::function<bool(const std::string&)>, which heap-
        // allocates per call and was the slower of the two topo sorts that
        // run back-to-back here (UpdateDependencyGraph + HoistComputePasses).
        // The explicit stack avoids the allocation and call-frame overhead.
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> inProgress;

        struct Frame
        {
            const std::string* Node;
            std::size_t NextDepIdx;
        };
        std::vector<Frame> stack;
        stack.reserve(m_InsertionOrder.size());

        const auto visit = [this, &visited, &inProgress, &stack](const std::string& root) -> bool
        {
            if (visited.contains(root))
                return true;

            stack.push_back(Frame{ &root, 0u });
            inProgress.insert(root);

            while (!stack.empty())
            {
                auto& frame = stack.back();
                const auto& nodeName = *frame.Node;

                const auto depIt = m_Dependencies.find(nodeName);
                const bool hasDeps = depIt != m_Dependencies.end();
                const auto* deps = hasDeps ? &depIt->second : nullptr;

                if (deps && frame.NextDepIdx < deps->size())
                {
                    const auto& dep = (*deps)[frame.NextDepIdx++];
                    if (inProgress.contains(dep))
                    {
                        OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Cycle detected in graph!");
                        return false;
                    }
                    if (visited.contains(dep))
                        continue;

                    inProgress.insert(dep);
                    stack.push_back(Frame{ &dep, 0u });
                    continue;
                }

                // All dependencies processed — emit this node in post-order.
                visited.insert(nodeName);
                inProgress.erase(nodeName);
                m_ExecutionOrder.push_back(nodeName);
                stack.pop_back();
            }

            return true;
        };

        // Visit all nodes in insertion order so ties are broken
        // deterministically (independent of std::unordered_map hashing).
        for (const auto& name : m_InsertionOrder)
        {
            if (!ContainsGraphEntry(name))
                continue;
            if (!visited.contains(name))
            {
                if (!visit(name))
                {
                    OLO_CORE_ERROR("RenderGraph::UpdateDependencyGraph: Failed to build execution order!");
                    m_ExecutionOrder.clear();
                    return false;
                }
            }
        }

        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("RenderGraph execution order updated with {} nodes", m_ExecutionOrder.size());

        // Hoist independent AsyncComputeCandidate passes before graphics.
        HoistComputePasses();

        return true;
    }

    void RenderGraph::HoistComputePasses()
    {
        OLO_PROFILE_FUNCTION();

        // Fast-path: skip the reorder when no async-compute candidate exists.
        bool hasCandidate = false;
        for (const auto& name : m_ExecutionOrder)
        {
            if (IsGraphEntryAsyncComputeCandidate(name))
            {
                hasCandidate = true;
                break;
            }
        }
        if (!hasCandidate)
            return;

        // Modified Kahn's algorithm over m_ExecutionOrder:
        //   when multiple passes are simultaneously ready (in-degree == 0),
        //   all AsyncComputeCandidate passes are drained before any graphics
        //   pass advances.  The result is still a valid topological order.
        std::unordered_set<std::string_view> passSet;
        passSet.reserve(m_ExecutionOrder.size());
        for (const auto& name : m_ExecutionOrder)
            passSet.insert(name);

        std::unordered_map<std::string, u32> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> successors;
        inDegree.reserve(m_ExecutionOrder.size());

        for (const auto& name : m_ExecutionOrder)
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
            // Build lastWriter from the authoritative declaration source for
            // each pass. After BuildFrameGraph(), passes that participated in
            // setup own a compiled access entry (possibly empty), so their
            // legacy static declarations must not leak back into scheduler
            // ordering. Fall back to static declarations only for pre-build /
            // legacy-only paths that have no compiled access entry at all.
            std::unordered_map<std::string, std::string> lastWriter;
            lastWriter.reserve(m_ExecutionOrder.size() * 4u);

            for (const auto& prodName : m_ExecutionOrder)
            {
                if (const auto* accesses = FindPassAccessDeclarations(m_PassAccessDeclarations, prodName))
                {
                    for (const auto& access : *accesses)
                    {
                        if (access.IsWrite && !access.ResourceName.empty())
                            lastWriter[access.ResourceName] = prodName;
                    }
                }
            }

            // For each pass, add an implicit producer→consumer edge for every
            // resource it reads whose last writer is a different pass.
            for (const auto& consName : m_ExecutionOrder)
            {
                auto addImplicitEdge = [&consName, &passSet, &successors, &inDegree](const std::string& producer)
                {
                    if (producer == consName || !passSet.contains(producer))
                        return;
                    // Skip if the edge is already present to avoid double-counting.
                    auto& succVec = successors[producer];
                    if (std::ranges::find(succVec, consName) != succVec.end())
                        return;
                    succVec.push_back(consName);
                    ++inDegree[consName];
                };

                if (const auto* accesses = FindPassAccessDeclarations(m_PassAccessDeclarations, consName))
                {
                    for (const auto& access : *accesses)
                    {
                        if (!access.IsWrite && !access.ResourceName.empty())
                        {
                            if (const auto wIt = lastWriter.find(access.ResourceName);
                                wIt != lastWriter.end())
                            {
                                addImplicitEdge(wIt->second);
                            }
                        }
                    }
                }
            }
        }

        std::deque<std::string> computeReady;
        std::deque<std::string> graphicsReady;

        auto classify = [this, &computeReady, &graphicsReady](const std::string& passName)
        {
            if (IsGraphEntryAsyncComputeCandidate(passName))
                computeReady.push_back(passName);
            else
                graphicsReady.push_back(passName);
        };

        for (const auto& name : m_ExecutionOrder)
        {
            if (inDegree[name] == 0)
                classify(name);
        }

        std::vector<std::string> reordered;
        reordered.reserve(m_ExecutionOrder.size());

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
        if (reordered.size() == m_ExecutionOrder.size())
        {
            m_ExecutionOrder = std::move(reordered);
            if (IsRenderGraphDiagnosticsEnabled())
                OLO_CORE_TRACE("RenderGraph: Compute-hoist applied to execution order");
        }
    }

    // Async-compute batch query.
    // Partitions the hoisted execution order into contiguous runs of
    // AsyncComputeCandidate passes.  Each batch records the non-batch passes
    // it must wait for (WaitNodes) and the non-batch passes that must wait
    // for it (SignalNodes) — the fence-sync metadata needed by explicit-
    // barrier backends (Vulkan semaphores, DX12 fence Signal/Wait).
    std::vector<RenderGraph::AsyncComputeBatch> RenderGraph::GetAsyncComputeBatches() const
    {
        return RenderGraphSubmissionPlan::ComputeBatches({
            .ExecutionOrder = m_ExecutionOrder,
            .Dependencies = m_Dependencies,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .IsGraphEntryAsyncComputeCandidate = [this](std::string_view name)
            { return IsGraphEntryAsyncComputeCandidate(name); },
        });
    }

    // Submission-plan IR.
    // Merges the hoisted execution order, the barrier plan, and the
    // async-compute batch boundaries into a single linearised
    // command stream that a backend can replay without touching the graph.
    std::vector<RenderGraph::SubmissionCommand> RenderGraph::GetSubmissionPlan() const
    {
        // Delegate to the extracted submission-plan module (Phase 7 slice 6).
        // GetAsyncComputeBatches itself delegates; here we just plumb the
        // results into the IR builder.
        const auto batches = GetAsyncComputeBatches();
        return RenderGraphSubmissionPlan::BuildPlan({
            .ExecutionOrder = m_ExecutionOrder,
            .PlannedBarriers = m_PlannedBarriers,
            .Batches = batches,
            .GetPassWorkType = [this](const std::string& passName)
            { return GetGraphEntryWorkType(passName); },
            .ResolveNodePointer = [this](const std::string& passName) -> RenderGraphNode*
            {
                if (auto nodeIt = m_NodeLookup.find(passName); nodeIt != m_NodeLookup.end() && nodeIt->second)
                    return const_cast<RenderGraphNode*>(nodeIt->second.Raw());
                return nullptr;
            },
        });
    }

    // -------------------------------------------------------------------
    // Explicit resource transition records
    // -------------------------------------------------------------------
    std::vector<RenderGraph::ResourceTransition> RenderGraph::GetResourceTransitions() const
    {
        // Delegate to the extracted barrier-planner module (Phase 7 split).
        return RenderGraphBarrierPlanner::BuildResourceTransitions({
            .PlannedBarriers = m_PlannedBarriers,
            .ExecutionOrder = m_ExecutionOrder,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .GetPassWorkType = [this](const std::string& passName)
            { return GetGraphEntryWorkType(passName); },
        });
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
        // Flags (IsImported / IsExtracted / IsHistory / IsTransient /
        // HasExternalBacking) are derived from the internal data structures
        // that the graph already maintains.
        // ----------------------------------------------------------------

        // --- Build auxiliary lookup sets --------------------------------

        // Extracted: resources with a pending TextureExtract or
        // FramebufferExtract callback.  Resolve handles → names.
        std::unordered_set<std::string> extractedNames;
        extractedNames.reserve(m_TextureExtracts.size() +
                               m_FramebufferExtracts.size() +
                               m_TemporalHistoryContracts.size() +
                               m_ExternalTextureSinkContracts.size());
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
        for (const auto& contract : m_TemporalHistoryContracts)
        {
            if (!contract.SourceResource.empty())
                extractedNames.insert(contract.SourceResource);
        }
        for (const auto& contract : m_ExternalTextureSinkContracts)
        {
            if (!contract.SourceResource.empty())
                extractedNames.insert(contract.SourceResource);
        }

        // History: resources listed as HistoryResource in any temporal
        // contract (they are imported from the previous frame).
        std::unordered_set<std::string> historyNames;
        historyNames.reserve(m_TemporalHistoryContracts.size());
        for (const auto& contract : m_TemporalHistoryContracts)
            historyNames.insert(contract.HistoryResource);

        // --- Build pass-order index map ---------------------------------
        std::unordered_map<std::string, u32> passOrderIdx;
        passOrderIdx.reserve(m_ExecutionOrder.size());
        for (u32 i = 0; i < static_cast<u32>(m_ExecutionOrder.size()); ++i)
            passOrderIdx.emplace(m_ExecutionOrder[i], i);

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
            lt.IsImported = IsImportedResource(info.Name);
            lt.IsExtracted = extractedNames.contains(info.Name);
            lt.IsHistory = historyNames.contains(info.Name);
            lt.IsTransient = IsTransientResource(info.Name);
            lt.HasExternalBacking = IsExternallyBackedTransientResource(info.Name);

            // Walk the execution order to find first write and last read.
            for (u32 i = 0; i < static_cast<u32>(m_ExecutionOrder.size()); ++i)
            {
                const auto& passName = m_ExecutionOrder[i];
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

            if (IsPassReachable(cmd.NodeName))
            {
                digest += cmd.NodeName;
            }
            else
            {
                digest += "(";
                digest += cmd.NodeName;
                digest += ":culled)";
            }

            passIndexByName.emplace(cmd.NodeName, passIndex++);
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

        if (!m_HasExplicitFinalPass)
        {
            m_FinalPassName.clear();
            return;
        }

        if (m_FinalPassName.empty())
        {
            OLO_CORE_WARN("RenderGraph: explicit final pass not set");
        }
    }

    void RenderGraph::ComputeReachability()
    {
        OLO_PROFILE_FUNCTION();

        m_ReachablePasses.clear();
        m_CulledPasses.clear();

        if (m_HasExplicitFinalPass && m_FinalPassName.empty())
        {
            OLO_CORE_WARN("ComputeReachability: final pass not set; all passes marked reachable");
        }

        // Gather extraction / temporal-history / external-sink resource roots
        // for the BFS. The reachability module needs only resource names; we
        // filter stale handles and skip empty contract entries here.
        std::vector<std::string> extractedResourceNames;
        extractedResourceNames.reserve(m_TextureExtracts.size() + m_FramebufferExtracts.size() +
                                       m_TemporalHistoryContracts.size() + m_ExternalTextureSinkContracts.size());
        for (const auto& extract : m_TextureExtracts)
        {
            if (!IsTextureHandleCurrent(extract.Handle))
                continue;
            auto name = std::string(GetResourceName(extract.Handle));
            if (!name.empty())
                extractedResourceNames.push_back(std::move(name));
        }
        for (const auto& extract : m_FramebufferExtracts)
        {
            if (!IsFramebufferHandleCurrent(extract.Handle))
                continue;
            auto name = std::string(GetResourceName(extract.Handle));
            if (!name.empty())
                extractedResourceNames.push_back(std::move(name));
        }
        for (const auto& contract : m_TemporalHistoryContracts)
        {
            if (!contract.SourceResource.empty())
                extractedResourceNames.push_back(contract.SourceResource);
        }
        for (const auto& contract : m_ExternalTextureSinkContracts)
        {
            if (!contract.SourceResource.empty())
                extractedResourceNames.push_back(contract.SourceResource);
        }

        // Delegate the BFS / iterative read→writer expansion to the
        // RenderGraphReachability module (Phase 7 split).
        m_ReachablePasses = RenderGraphReachability::ComputeReachableSet({
            .HasExplicitFinalPass = m_HasExplicitFinalPass,
            .FinalPassName = m_FinalPassName,
            .InsertionOrder = m_InsertionOrder,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .Dependencies = m_Dependencies,
            .ExtractedResourceNames = extractedResourceNames,
        });

        // Refresh contract metadata (depends on m_ReachablePasses) before the
        // culling sweep so any contract that lost its producer is reported.
        RefreshTemporalHistoryContracts();
        RefreshExternalTextureSinkContracts();

        // Sweep: passes not reachable but side-effecting (Present / Readback /
        // NeverCull / ExternalSideEffect) come back into the reachable set so
        // their side effects still run; everything else lands in m_CulledPasses.
        for (const auto& passName : m_InsertionOrder)
        {
            if (m_ReachablePasses.contains(passName))
                continue;

            if (IsGraphEntrySideEffecting(passName))
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

        std::ranges::sort(m_CulledPasses);

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
        return RenderGraphBarrierPlanner::ResolveProducerBarrierFlags(usage);
    }

    MemoryBarrierFlags RenderGraph::ResolveConsumerBarrierFlags(const RGReadUsage usage)
    {
        return RenderGraphBarrierPlanner::ResolveConsumerBarrierFlags(usage);
    }

    void RenderGraph::ComputeBarrierPlan()
    {
        OLO_PROFILE_FUNCTION();

        // Delegate to the extracted barrier-planner module (Phase 7 split).
        // The module is backend-agnostic: it consumes the compiled-frame access
        // declarations + execution order, produces a `PlannedBarrier` list and
        // a per-pass barrier-flag map. The OpenGL backend then translates the
        // flags to `glMemoryBarrier` bits via the abstract
        // `RGCommandContext::MemoryBarrier(flags)` entry point.
        auto plan = RenderGraphBarrierPlanner::ComputePlan({
            .ExecutionOrder = m_ExecutionOrder,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .IsPassReachable = [this](const std::string& passName)
            { return IsPassReachable(passName); },
        });

        m_PlannedBarriers = std::move(plan.PlannedBarriers);
        m_PassBarrierFlags = std::move(plan.PassBarrierFlags);
        m_BarrierDiagnostics = std::move(plan.Diagnostics);
    }

    bool RenderGraph::IsPassReachable(const std::string& passName) const
    {
        return m_ReachablePasses.contains(passName);
    }

    bool RenderGraph::ContainsGraphEntry(std::string_view name) const
    {
        const auto key = std::string(name);
        return m_NodeLookup.contains(key);
    }

    bool RenderGraph::IsGraphEntryAsyncComputeCandidate(std::string_view name) const
    {
        if (const auto node = TryGetGraphEntryNode(name, m_NodeLookup))
            return node->IsAsyncComputeCandidate();

        return false;
    }

    bool RenderGraph::IsGraphEntrySideEffecting(std::string_view name) const
    {
        if (const auto node = TryGetGraphEntryNode(name, m_NodeLookup))
            return node->IsSideEffecting();

        return false;
    }

    RenderGraphPassWorkType RenderGraph::GetGraphEntryWorkType(std::string_view name) const
    {
        if (const auto node = TryGetGraphEntryNode(name, m_NodeLookup))
            return node->GetPassWorkType();

        return RenderGraphPassWorkType::Graphics;
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

    bool RenderGraph::IsImportedResource(std::string_view resourceName) const
    {
        if (resourceName.empty())
            return false;

        const auto stableName = std::string(resourceName);
        if (m_ImportedResources.contains(stableName))
            return true;

        if (const auto viewIt = m_TextureViewDefinitions.find(stableName);
            viewIt != m_TextureViewDefinitions.end() &&
            viewIt->second.ParentResource != stableName)
        {
            return IsImportedResource(viewIt->second.ParentResource);
        }

        return false;
    }

    bool RenderGraph::IsTransientResource(std::string_view resourceName) const
    {
        if (resourceName.empty())
            return false;

        const auto stableName = std::string(resourceName);
        if (m_TransientResourceDescs.contains(stableName))
            return true;

        if (const auto viewIt = m_TextureViewDefinitions.find(stableName);
            viewIt != m_TextureViewDefinitions.end() &&
            viewIt->second.ParentResource != stableName)
        {
            return IsTransientResource(viewIt->second.ParentResource);
        }

        return false;
    }

    bool RenderGraph::IsExternallyBackedTransientResource(std::string_view resourceName) const
    {
        if (resourceName.empty())
            return false;

        const auto stableName = std::string(resourceName);
        // Read-only lookup via Find — if the name was never interned, it
        // can't be in either set, so we can short-circuit to false.
        if (const u32 nameId = m_ResourceNames.Find(stableName); nameId != 0u)
        {
            if (m_ExternallyBackedTransientTextures.contains(nameId))
                return true;
            if (m_ExternallyBackedTransientFramebuffers.contains(nameId))
                return true;
        }

        if (const auto viewIt = m_TextureViewDefinitions.find(stableName);
            viewIt != m_TextureViewDefinitions.end())
        {
            if (!viewIt->second.BackingResource.empty() &&
                viewIt->second.BackingResource != stableName &&
                IsExternallyBackedTransientResource(viewIt->second.BackingResource))
            {
                return true;
            }

            if (viewIt->second.ParentResource != stableName)
                return IsExternallyBackedTransientResource(viewIt->second.ParentResource);
        }

        return false;
    }

    bool RenderGraph::HasHistoryTextureSink(std::string_view historyResource) const
    {
        if (historyResource.empty())
            return false;

        return m_HistoryTextureSinks.contains(std::string(historyResource));
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
    bool RenderGraph::ValidateExecutionTopology()
    {
        if (!m_DependencyGraphDirty)
            return true;

        return UpdateDependencyGraph();
    }

    std::vector<RenderGraph::Hazard> RenderGraph::ValidateResourceHazards()
    {
        // Setup-time declarations only land in m_PassAccessDeclarations after
        // BuildFrameGraph runs each pass's Setup callback. Auto-build only
        // when the dependency graph is actually dirty so back-to-back calls
        // (or a caller that already ran BuildFrameGraph this frame) don't
        // pay for a redundant per-pass Setup rebuild — that double-build
        // was the cause of the 2026-05-12 Debug-mode FPS regression
        // (Renderer3DFrameExecution validates before Execute()).
        if (m_DependencyGraphDirty)
            BuildFrameGraph();
        return ValidateResourceHazardsInternal();
    }

    std::vector<RenderGraph::Hazard> RenderGraph::ValidateCompiledResourceHazards()
    {
        if (m_DependencyGraphDirty)
            BuildFrameGraph();
        return ValidateResourceHazardsInternal();
    }

    std::vector<RenderGraph::Hazard> RenderGraph::ValidateResourceHazardsInternal()
    {
        OLO_PROFILE_FUNCTION();

        // Ensure the registry and topology are up to date before delegating
        // to the pure validator module.
        EnsureResourceRegistryBuilt();

        if (m_DependencyGraphDirty)
        {
            if (!UpdateDependencyGraph())
            {
                // Partial topo order is useless — running the remaining
                // validator over an incomplete m_ExecutionOrder would produce
                // misleading "missing dependency" reports for passes the
                // cycle excluded. Surface a synthetic Cycle hazard so
                // callers can distinguish "no hazards" from "could not
                // validate" (the empty-vector overload used to conflate
                // both).
                OLO_CORE_ERROR("RenderGraph::ValidateResourceHazards: aborting (graph has a cycle)");
                std::vector<Hazard> hazards;
                Hazard h;
                h.Kind = HazardKind::Cycle;
                h.Message = "RenderGraph contains a cycle; resource hazard validation aborted";
                hazards.push_back(std::move(h));
                return hazards;
            }
            // Note: we deliberately leave m_DependencyGraphDirty set so the
            // first Execute() after validation still runs ResolveFinalPass +
            // submission-plan rebuild. The validator only needs topo order.
        }

        // Delegate to the extracted hazard-validator module (Phase 7 split).
        return RenderGraphHazardValidator::Validate({
            .IsPassReachable = [this](const std::string& passName)
            { return IsPassReachable(passName); },
            .ResolveTexture = [this](RGTextureHandle handle)
            { return ResolveTexture(handle); },
            .ResolveBuffer = [this](RGBufferHandle handle)
            { return ResolveBuffer(handle); },
            .ResolveFramebuffer = [this](RGFramebufferHandle handle)
            { return ResolveFramebuffer(handle); },
            .ExecutionOrder = m_ExecutionOrder,
            .Dependencies = m_Dependencies,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .PassFeedbackDeclarations = m_PassFeedbackDeclarations,
            .RegistryDiagnostics = m_ResourceRegistryDiagnostics,
            .RegisteredResources = m_RegisteredResources,
        });
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

        // Phase A — pure registry build (descriptor merging + access walking +
        // kind-mismatch diagnostics + canonical sort). Delegated to the
        // RenderGraphResourceRegistry module (Phase 7 split).
        auto built = RenderGraphResourceRegistry::Build({
            .ImportedResources = m_ImportedResources,
            .TransientResourceDescs = m_TransientResourceDescs,
            .TextureViewResourceDescs = m_TextureViewResourceDescs,
            .InsertionOrder = m_InsertionOrder,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .IsExternallyBackedTransientResource = [this](std::string_view name)
            { return IsExternallyBackedTransientResource(name); },
        });

        m_ResourceRegistry = std::move(built.Registry);
        m_RegisteredResources = std::move(built.Sorted);
        m_ResourceRegistryDiagnostics = std::move(built.Diagnostics);

        // Phase B — reconcile per-type handle slot tables + allocate handles
        // for live resources. The generic slot-allocator pattern is shared
        // across the three handle families and lives in the
        // RenderGraphHandleAllocator module (Phase 7 slice 8).
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

        RenderGraphHandleAllocator::Reconcile(m_TextureHandlesByName, m_TextureHandleSlots, m_FreeTextureHandleIndices, activeTextureNames);
        RenderGraphHandleAllocator::Reconcile(m_BufferHandlesByName, m_BufferHandleSlots, m_FreeBufferHandleIndices, activeBufferNames);
        RenderGraphHandleAllocator::Reconcile(m_FramebufferHandlesByName, m_FramebufferHandleSlots, m_FreeFramebufferHandleIndices, activeFramebufferNames);

        for (auto& info : m_RegisteredResources)
        {
            if (isTextureKind(info.Desc.Kind))
            {
                info.TextureHandle = RenderGraphHandleAllocator::Allocate(info.Name,
                                                                          m_TextureHandlesByName,
                                                                          m_TextureHandleSlots,
                                                                          m_PhysicalTextures,
                                                                          m_FreeTextureHandleIndices,
                                                                          [](u32 index, u32 generation)
                                                                          {
                                                                              return RGTextureHandle{ index, generation };
                                                                          });
            }
            else if (isBufferKind(info.Desc.Kind))
            {
                info.BufferHandle = RenderGraphHandleAllocator::Allocate(info.Name,
                                                                         m_BufferHandlesByName,
                                                                         m_BufferHandleSlots,
                                                                         m_PhysicalBuffers,
                                                                         m_FreeBufferHandleIndices,
                                                                         [](u32 index, u32 generation)
                                                                         {
                                                                             return RGBufferHandle{ index, generation };
                                                                         });
            }
            else if (info.Desc.Kind == ResourceHandle::Kind::Framebuffer)
            {
                info.FramebufferHandle = RenderGraphHandleAllocator::Allocate(info.Name,
                                                                              m_FramebufferHandlesByName,
                                                                              m_FramebufferHandleSlots,
                                                                              m_PhysicalFramebuffers,
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
        return RenderGraphTransientPlanner::BuildAliasGroup(desc);
    }

    u64 RenderGraph::EstimateTransientBytes(const RGResourceDesc& desc)
    {
        return RenderGraphTransientPlanner::EstimateBytes(desc);
    }

    bool RenderGraph::IsTransientDescriptorAllocatable(const RGResourceDesc& desc)
    {
        return RenderGraphTransientPlanner::IsAllocatable(desc);
    }

    std::string_view RenderGraph::GetTransientDescriptorSkipReason(const RGResourceDesc& desc)
    {
        return RenderGraphTransientPlanner::GetSkipReason(desc);
    }

    void RenderGraph::RebuildTransientPlan()
    {
        // Delegate the lifetime scan + alias-slot assignment to the
        // RenderGraphTransientPlanner module (Phase 7 slice 5).
        m_TransientPlan = RenderGraphTransientPlanner::ComputePlan({
            .TransientResourceDescs = m_TransientResourceDescs,
            .ExecutionOrder = m_ExecutionOrder,
            .PassAccessDeclarations = m_PassAccessDeclarations,
            .IsPassReachable = [this](const std::string& passName)
            { return IsPassReachable(passName); },
            .IsExternallyBackedTransientResource = [this](std::string_view name)
            { return IsExternallyBackedTransientResource(name); },
        });
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

            const auto workType = GetGraphEntryWorkType(name);
            const bool isCompute = workType == RenderGraphPassWorkType::Compute;
            const bool isCopy = workType == RenderGraphPassWorkType::Copy;
            const bool isAsyncCandidate = IsGraphEntryAsyncComputeCandidate(name);

            const std::string label = isAsyncCandidate ? ("[async] " + name) : name;

            out << "    \"" << name << "\"";

            std::vector<std::string> attributes;
            attributes.push_back(std::string("label=\"") + label + "\"");
            if (isFinal)
                attributes.emplace_back("peripheries=2");

            const bool hasReachabilityData = !m_ReachablePasses.empty();
            if (const bool isCulled = hasReachabilityData && !m_ReachablePasses.contains(name); isCulled)
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
                << "', flags=0x" << std::hex << std::to_underlying(barrier.Flags) << std::dec << "\n";
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
            for (sizet pi = 0; pi < batches[i].ComputeNodes.size(); ++pi)
            {
                out << batches[i].ComputeNodes[pi];
                if (pi + 1 < batches[i].ComputeNodes.size())
                    out << ",";
            }
            out << "]\n";
        }

        // Cross-lane sync records in DOT comments.
        const auto dotTransitions = GetResourceTransitions();
        const auto crossLaneDot = std::ranges::count_if(dotTransitions,
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
        const auto passWorkTypeToString = [](const RenderGraphPassWorkType type) -> const char*
        {
            switch (type)
            {
                case RenderGraphPassWorkType::Compute:
                    return "Compute";
                case RenderGraphPassWorkType::Copy:
                    return "Copy";
                case RenderGraphPassWorkType::Graphics:
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

        for (u32 passIndex = 0; passIndex < static_cast<u32>(m_ExecutionOrder.size()); ++passIndex)
        {
            const auto& passName = m_ExecutionOrder[passIndex];

            if (const auto* accesses = FindPassAccessDeclarations(m_PassAccessDeclarations, passName))
            {
                for (const auto& access : *accesses)
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
        cpuMsByPass.reserve(m_LastExecutionTimings.size());
        for (const auto& timing : m_LastExecutionTimings)
        {
            totalCpuMs += timing.CpuMs;
            cpuMsByPass[timing.NodeName] = timing.CpuMs;
            if (timing.CpuMs > maxCpuMs)
            {
                maxCpuMs = timing.CpuMs;
                maxPassName = timing.NodeName;
            }
        }
        const std::unordered_set<std::string> culledPasses(m_CulledPasses.begin(), m_CulledPasses.end());

        std::unordered_map<std::string, u32> passOrderIndexByName;
        passOrderIndexByName.reserve(m_ExecutionOrder.size());
        for (sizet i = 0; i < m_ExecutionOrder.size(); ++i)
            passOrderIndexByName[m_ExecutionOrder[i]] = static_cast<u32>(i);

        std::vector<std::string> timingDigestEntries;
        timingDigestEntries.reserve(m_ExecutionOrder.size());
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

        const auto executedPassCount = m_LastExecutionTimings.size();
        const auto averageCpuMs = executedPassCount > 0
                                      ? (totalCpuMs / static_cast<f64>(executedPassCount))
                                      : 0.0;

        for (sizet i = 0; i < m_ExecutionOrder.size(); ++i)
        {
            const auto& passName = m_ExecutionOrder[i];
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
                                            ":" + (resource.Desc.Imported ? "1" : "0") +
                                            ":xb" + (resource.HasExternalBacking ? "1" : "0"));
            }
            std::ranges::sort(descriptorEntries);
            resourceDigestEntries.insert(resourceDigestEntries.end(), descriptorEntries.begin(), descriptorEntries.end());

            std::vector<std::string> lifetimeNames;
            lifetimeNames.reserve(lifetimeByResource.size());
            for (const auto& [resource, info] : lifetimeByResource)
                lifetimeNames.push_back(resource);
            std::ranges::sort(lifetimeNames);
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
            std::ranges::sort(accessResources);
            resourceDigestAccessCount = static_cast<u32>(accessResources.size());

            for (const auto& resource : accessResources)
            {
                std::vector<std::string> readModes;
                if (const auto readsIt = readModesByResource.find(resource); readsIt != readModesByResource.end())
                {
                    readModes.assign(readsIt->second.begin(), readsIt->second.end());
                    std::ranges::sort(readModes);
                }

                std::vector<std::string> writeModes;
                if (const auto writesIt = writeModesByResource.find(resource); writesIt != writeModesByResource.end())
                {
                    writeModes.assign(writesIt->second.begin(), writesIt->second.end());
                    std::ranges::sort(writeModes);
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
                const auto flags = std::to_underlying(barrier.Flags);
                barrierRows.push_back({ barrier.BeforePass, barrier.Resource, flags });
                barrierFlagsOr |= flags;
            }
            std::ranges::sort(barrierRows,
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
            std::ranges::sort(diagnosticRows,
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
        const auto externalTextureSinkContractCount = static_cast<u32>(m_ExternalTextureSinkContracts.size());
        const auto externallyBackedTransientRootCount =
            static_cast<u32>(m_ExternallyBackedTransientTextures.size() + m_ExternallyBackedTransientFramebuffers.size());
        u32 externallyBackedResourceCount = 0;
        struct PassAuthoringRow
        {
            std::string PassName;
            bool Reachable = false;
        };

        std::vector<PassAuthoringRow> passAuthoringRows;
        passAuthoringRows.reserve(m_InsertionOrder.size());
        for (const auto& passName : m_ExecutionOrder)
        {
            if (GetGraphEntryWorkType(passName) == RenderGraphPassWorkType::Compute)
                ++computePassCount;
            if (IsGraphEntryAsyncComputeCandidate(passName))
                ++asyncComputeCandidateCount;
        }
        for (const auto& resource : m_RegisteredResources)
        {
            if (IsHistoryTextureResource(resource.Name))
                ++historyResourceCount;
            if (resource.HasExternalBacking)
                ++externallyBackedResourceCount;
        }

        for (const auto& passName : m_InsertionOrder)
        {
            if (const auto node = TryGetGraphEntryNode(passName, m_NodeLookup); !node)
                continue;

            passAuthoringRows.push_back(PassAuthoringRow{
                .PassName = passName,
                .Reachable = m_ReachablePasses.contains(passName),
            });
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
        const auto crossLaneSyncCount = static_cast<u32>(std::ranges::count_if(
            dumpTransitions,
            [](const ResourceTransition& tr)
            { return tr.IsCrossLane; }));

        // Unified resource lifetime records.
        const auto dumpLifetimes = GetResourceLifetimes();
        const auto resourceLifetimeCount = static_cast<u32>(dumpLifetimes.size());
        u32 resolveFailureCount = 0;
        for (const auto& failure : m_ResolveFailures)
            resolveFailureCount += failure.Count;

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
        out << "  \"schemaVersion\": 16,\n";
        out << "  \"timingVersion\": 4,\n";
        out << "  \"finalPass\": \"" << jsonEscape(m_FinalPassName) << "\",\n";
        out << "  \"hasExplicitFinalPass\": " << (m_HasExplicitFinalPass ? "true" : "false") << ",\n";
        out << "  \"hasTimings\": " << (m_LastExecutionTimings.empty() ? "false" : "true") << ",\n";
        out << "  \"frameSummary\": { "
            << "\"passCount\": " << m_ExecutionOrder.size()
            << ", \"resourceCount\": " << m_RegisteredResources.size()
            << ", \"culledPassCount\": " << m_CulledPasses.size()
            << ", \"plannedBarrierCount\": " << m_PlannedBarriers.size()
            << ", \"buildDiagnosticCount\": " << m_BuildDiagnostics.size()
            << ", \"barrierDiagnosticCount\": " << m_BarrierDiagnostics.size()
            << ", \"transientAliasCount\": " << m_TransientPlan.size()
            << ", \"timingsCount\": " << executedPassCount
            << ", \"computePassCount\": " << computePassCount
            << ", \"asyncComputeCandidateCount\": " << asyncComputeCandidateCount
            << ", \"historyResourceCount\": " << historyResourceCount
            << ", \"externallyBackedTransientRootCount\": " << externallyBackedTransientRootCount
            << ", \"externallyBackedResourceCount\": " << externallyBackedResourceCount
            << ", \"temporalHistoryContractCount\": " << m_TemporalHistoryContracts.size()
            << ", \"externalTextureSinkContractCount\": " << externalTextureSinkContractCount
            << ", \"asyncBatchCount\": " << dumpBatches.size()
            << ", \"batchInputResourceCount\": " << batchInputResourceCount
            << ", \"batchOutputResourceCount\": " << batchOutputResourceCount
            << ", \"submissionCommandCount\": " << submissionPlan.size()
            << ", \"resourceTransitionCount\": " << resourceTransitionCount
            << ", \"crossLaneSyncCount\": " << crossLaneSyncCount
            << ", \"resourceLifetimeCount\": " << resourceLifetimeCount
            << ", \"resolveFailureCount\": " << resolveFailureCount
            << " },\n";
        out << "  \"buildStats\": { "
            << "\"passesVisited\": " << m_LastBuildStats.PassesVisited
            << ", \"declaredReads\": " << m_LastBuildStats.DeclaredReads
            << ", \"declaredWrites\": " << m_LastBuildStats.DeclaredWrites
            << ", \"derivedEdges\": " << m_LastBuildStats.DerivedEdges
            << ", \"orderSensitiveResults\": " << m_LastBuildStats.OrderSensitiveResults
            << " },\n";

        const auto buildDiagnosticKindToString = [](const BuildDiagnosticKind kind)
        {
            switch (kind)
            {
                case BuildDiagnosticKind::RegistrationOrderSensitivity:
                    return "RegistrationOrderSensitivity";
            }

            return "Unknown";
        };

        out << "  \"buildDiagnostics\": [\n";
        for (sizet i = 0; i < m_BuildDiagnostics.size(); ++i)
        {
            const auto& diagnostic = m_BuildDiagnostics[i];
            out << "    { \"kind\": \"" << buildDiagnosticKindToString(diagnostic.Kind)
                << "\", \"resource\": \"" << jsonEscape(diagnostic.Resource)
                << "\", \"currentBeforePass\": \"" << jsonEscape(diagnostic.CurrentBeforePass)
                << "\", \"currentAfterPass\": \"" << jsonEscape(diagnostic.CurrentAfterPass)
                << "\", \"alternateBeforePass\": \"" << jsonEscape(diagnostic.AlternateBeforePass)
                << "\", \"alternateAfterPass\": \"" << jsonEscape(diagnostic.AlternateAfterPass)
                << "\", \"message\": \"" << jsonEscape(diagnostic.Message) << "\" }";
            if (i + 1 < m_BuildDiagnostics.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"passOrder\": [";
        for (sizet i = 0; i < m_ExecutionOrder.size(); ++i)
        {
            out << "\"" << jsonEscape(m_ExecutionOrder[i]) << "\"";
            if (i + 1 < m_ExecutionOrder.size())
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
        for (sizet i = 0; i < m_ExecutionOrder.size(); ++i)
        {
            const auto& passName = m_ExecutionOrder[i];
            const auto workType = passWorkTypeToString(GetGraphEntryWorkType(passName));
            const auto asyncCandidate = IsGraphEntryAsyncComputeCandidate(passName);
            out << "    { \"pass\": \"" << jsonEscape(passName)
                << "\", \"workType\": \"" << workType
                << "\", \"asyncComputeCandidate\": " << (asyncCandidate ? "true" : "false") << " }";
            if (i + 1 < m_ExecutionOrder.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"passAuthoring\": [\n";
        for (sizet i = 0; i < passAuthoringRows.size(); ++i)
        {
            const auto& row = passAuthoringRows[i];
            out << "    { \"pass\": \"" << jsonEscape(row.PassName)
                << "\", \"reachable\": " << (row.Reachable ? "true" : "false") << " }";
            if (i + 1 < passAuthoringRows.size())
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
                << "\", \"flags\": " << std::to_underlying(barrier.Flags)
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
                << ", \"hasExternalBacking\": " << (resource.HasExternalBacking ? "true" : "false")
                << ", \"textureID\": " << textureID
                << ", \"bufferID\": " << bufferID
                << ", \"framebufferID\": " << framebufferID
                << ", \"framebufferColor0ID\": " << framebufferColor0ID << " }";
            if (i + 1 < m_RegisteredResources.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"externalTextureSinkContracts\": [\n";
        for (sizet i = 0; i < m_ExternalTextureSinkContracts.size(); ++i)
        {
            const auto& contract = m_ExternalTextureSinkContracts[i];
            out << "    { \"sourceResource\": \"" << jsonEscape(contract.SourceResource)
                << "\", \"sourceKind\": \"" << ToString(contract.SourceKind)
                << "\", \"colorAttachmentIndex\": " << contract.ColorAttachmentIndex
                << ", \"sourceReachable\": " << (contract.SourceReachable ? "true" : "false") << " }";
            if (i + 1 < m_ExternalTextureSinkContracts.size())
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
                << "\", \"sourceKind\": \"" << (contract.Kind == TemporalHistoryContract::SourceKind::Texture ? "Texture" : "Framebuffer")
                << "\", \"colorAttachmentIndex\": " << contract.ColorAttachmentIndex
                << ", \"historyImported\": " << (contract.HistoryImported ? "true" : "false")
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
            std::ranges::sort(resourceNames);

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
            std::ranges::sort(resources);

            for (sizet i = 0; i < resources.size(); ++i)
            {
                const auto& resource = resources[i];
                out << "    { \"resource\": \"" << jsonEscape(resource) << "\", \"reads\": [";

                std::vector<std::string> readModes;
                if (const auto readsIt = readModesByResource.find(resource); readsIt != readModesByResource.end())
                {
                    readModes.assign(readsIt->second.begin(), readsIt->second.end());
                    std::ranges::sort(readModes);
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
                    std::ranges::sort(writeModes);
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
        for (sizet i = 0; i < m_ExecutionOrder.size(); ++i)
        {
            const auto& passName = m_ExecutionOrder[i];
            const auto isCulled = culledPasses.contains(passName);
            const auto timingIt = cpuMsByPass.find(passName);
            const auto executed = timingIt != cpuMsByPass.end();
            const auto cpuMs = executed ? timingIt->second : 0.0;
            const auto workType = passWorkTypeToString(GetGraphEntryWorkType(passName));
            const auto asyncCandidate = IsGraphEntryAsyncComputeCandidate(passName);

            out << "    { \"pass\": \"" << jsonEscape(passName)
                << "\", \"orderIndex\": " << i
                << ", \"culled\": " << (isCulled ? "true" : "false")
                << ", \"executed\": " << (executed ? "true" : "false")
                << ", \"cpuMs\": " << cpuMs
                << ", \"workType\": \"" << workType << "\""
                << ", \"asyncComputeCandidate\": " << (asyncCandidate ? "true" : "false") << " }";
            if (i + 1 < m_ExecutionOrder.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        out << "  \"timingStatsByPass\": {\n";
        for (sizet i = 0; i < m_ExecutionOrder.size(); ++i)
        {
            const auto& passName = m_ExecutionOrder[i];
            const auto isCulled = culledPasses.contains(passName);
            const auto timingIt = cpuMsByPass.find(passName);
            const auto executed = timingIt != cpuMsByPass.end();
            const auto cpuMs = executed ? timingIt->second : 0.0;
            const auto workType = passWorkTypeToString(GetGraphEntryWorkType(passName));
            const auto asyncCandidate = IsGraphEntryAsyncComputeCandidate(passName);

            out << "    \"" << jsonEscape(passName)
                << "\": { \"orderIndex\": " << i
                << ", \"executed\": " << (executed ? "true" : "false")
                << ", \"culled\": " << (isCulled ? "true" : "false")
                << ", \"cpuMs\": " << cpuMs
                << ", \"workType\": \"" << workType << "\""
                << ", \"asyncComputeCandidate\": " << (asyncCandidate ? "true" : "false") << " }";
            if (i + 1 < m_ExecutionOrder.size())
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
            << ", \"externallyBackedTransientRootCount\": " << externallyBackedTransientRootCount
            << ", \"externallyBackedResourceCount\": " << externallyBackedResourceCount
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

        const auto graphDigestConcat = std::string("passes=") + std::to_string(m_ExecutionOrder.size()) +
                                       ";resources=" + std::to_string(m_RegisteredResources.size()) +
                                       ";culled=" + std::to_string(m_CulledPasses.size()) +
                                       ";barriers=" + std::to_string(m_PlannedBarriers.size()) +
                                       ";diags=" + std::to_string(m_BarrierDiagnostics.size()) +
                                       ";aliases=" + std::to_string(m_TransientPlan.size()) +
                                       ";timings=" + std::to_string(m_LastExecutionTimings.size()) +
                                       ";compute=" + std::to_string(computePassCount) +
                                       ";asyncCandidates=" + std::to_string(asyncComputeCandidateCount) +
                                       ";histories=" + std::to_string(historyResourceCount) +
                                       ";externalBackingRoots=" + std::to_string(externallyBackedTransientRootCount) +
                                       ";externalBackingResources=" + std::to_string(externallyBackedResourceCount) +
                                       ";externalTextureSinks=" + std::to_string(externalTextureSinkContractCount) +
                                       ";historyContracts=" + std::to_string(m_TemporalHistoryContracts.size()) +
                                       ";batches=" + std::to_string(dumpBatches.size()) +
                                       ";batchInputResources=" + std::to_string(batchInputResourceCount) +
                                       ";batchOutputResources=" + std::to_string(batchOutputResourceCount) +
                                       ";submissionCommands=" + std::to_string(submissionPlan.size()) +
                                       ";transitions=" + std::to_string(resourceTransitionCount) +
                                       ";crossLaneSync=" + std::to_string(crossLaneSyncCount) +
                                       ";lifetimes=" + std::to_string(resourceLifetimeCount) +
                                       ";subresourceRanges=present" +
                                       ";resolveFailures=" + std::to_string(resolveFailureCount);

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
            for (sizet pi = 0; pi < batch.ComputeNodes.size(); ++pi)
            {
                out << "\"" << jsonEscape(batch.ComputeNodes[pi]) << "\"";
                if (pi + 1 < batch.ComputeNodes.size())
                    out << ", ";
            }
            out << "],\n";

            // WaitPasses array
            out << "      \"waitPasses\": [";
            for (sizet pi = 0; pi < batch.WaitNodes.size(); ++pi)
            {
                out << "\"" << jsonEscape(batch.WaitNodes[pi]) << "\"";
                if (pi + 1 < batch.WaitNodes.size())
                    out << ", ";
            }
            out << "],\n";

            // SignalPasses array
            out << "      \"signalPasses\": [";
            for (sizet pi = 0; pi < batch.SignalNodes.size(); ++pi)
            {
                out << "\"" << jsonEscape(batch.SignalNodes[pi]) << "\"";
                if (pi + 1 < batch.SignalNodes.size())
                    out << ", ";
            }
            out << "],\n";

            // InputResources array
            out << "      \"inputResources\": [\n";
            for (sizet ri = 0; ri < batch.InputResources.size(); ++ri)
            {
                const auto& dep = batch.InputResources[ri];
                out << "        { \"resource\": \"" << jsonEscape(dep.ResourceName)
                    << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalNode) << "\" }";
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
                    << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalNode) << "\" }";
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
                out << ", \"pass\": \"" << jsonEscape(cmd.NodeName)
                    << "\", \"workType\": \"" << passWorkTypeToString(cmd.WorkType) << "\"";
            }
            else if (cmd.CommandKind == SubmissionCommand::Kind::MemoryBarrier)
            {
                out << ", \"flags\": " << std::to_underlying(cmd.Barriers);
            }
            else if (cmd.CommandKind == SubmissionCommand::Kind::BatchBegin ||
                     cmd.CommandKind == SubmissionCommand::Kind::BatchEnd)
            {
                out << ", \"batchIndex\": " << cmd.BatchIndex;

                out << ", \"waitPasses\": [";
                for (sizet i = 0; i < cmd.WaitNodes.size(); ++i)
                {
                    out << "\"" << jsonEscape(cmd.WaitNodes[i]) << "\"";
                    if (i + 1 < cmd.WaitNodes.size())
                        out << ", ";
                }
                out << "]";

                out << ", \"signalPasses\": [";
                for (sizet i = 0; i < cmd.SignalNodes.size(); ++i)
                {
                    out << "\"" << jsonEscape(cmd.SignalNodes[i]) << "\"";
                    if (i + 1 < cmd.SignalNodes.size())
                        out << ", ";
                }
                out << "]";

                out << ", \"inputResources\": [";
                for (sizet i = 0; i < cmd.InputResources.size(); ++i)
                {
                    const auto& dep = cmd.InputResources[i];
                    out << "{ \"resource\": \"" << jsonEscape(dep.ResourceName)
                        << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalNode) << "\" }";
                    if (i + 1 < cmd.InputResources.size())
                        out << ", ";
                }
                out << "]";

                out << ", \"outputResources\": [";
                for (sizet i = 0; i < cmd.OutputResources.size(); ++i)
                {
                    const auto& dep = cmd.OutputResources[i];
                    out << "{ \"resource\": \"" << jsonEscape(dep.ResourceName)
                        << "\", \"externalPass\": \"" << jsonEscape(dep.ExternalNode) << "\" }";
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
                << "\", \"flags\": " << std::to_underlying(tr.Flags)
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
                << ", \"hasExternalBacking\": " << (lt.HasExternalBacking ? "true" : "false")
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

        out << "  \"resolveFailures\": [\n";
        for (sizet i = 0; i < m_ResolveFailures.size(); ++i)
        {
            const auto& failure = m_ResolveFailures[i];
            out << "    { \"pass\": \"" << jsonEscape(failure.PassName)
                << "\", \"reason\": \"" << jsonEscape(failure.Reason)
                << "\", \"count\": " << failure.Count << " }";
            if (i + 1 < m_ResolveFailures.size())
                out << ",";
            out << "\n";
        }
        out << "  ],\n";

        // Debug block — LLM-friendly diagnostic snapshot answering the
        // single most common question: "why isn't the rendered output what
        // I expect?". Contains FinalRenderPass's selected primary input,
        // active base-name aliases, and per-pass enabled/ready/culled
        // status with derived cull reasons.
        out << "  \"debug\": {\n";
        {
            // FinalPass selected input — the resource that actually reaches
            // the swap chain this frame. (Class is FinalRenderPass but the
            // pipeline registers it under "FinalPass".)
            std::string finalFb;
            std::string finalTex;
            if (!m_FinalPassName.empty())
            {
                if (const auto finalIt = m_NodeLookup.find(m_FinalPassName);
                    finalIt != m_NodeLookup.end() && finalIt->second)
                {
                    finalFb = ReverseResolveFramebufferName(finalIt->second->GetPrimaryInputFramebufferHandle());
                    finalTex = ReverseResolveTextureName(finalIt->second->GetPrimaryInputTextureHandle());
                }
            }
            out << "    \"finalPassInput\": { "
                << "\"framebuffer\": \"" << jsonEscape(finalFb)
                << "\", \"texture\": \"" << jsonEscape(finalTex) << "\" },\n";

            // Alias map snapshot. Empty objects when no aliases active.
            // Maps are u32->u32 internally; resolve through the interner once
            // here for emission. std::map for deterministic JSON ordering.
            const auto writeAliasMap = [this, &out, &jsonEscape](const char* label,
                                                                 const std::unordered_map<u32, u32>& aliases,
                                                                 bool trailingComma)
            {
                std::map<std::string, std::string> sorted;
                for (const auto& [aliasId, targetId] : aliases)
                    sorted.emplace(std::string(m_ResourceNames.NameOf(aliasId)),
                                   std::string(m_ResourceNames.NameOf(targetId)));
                out << "    \"" << label << "\": {";
                sizet idx = 0;
                for (const auto& [aliasName, targetName] : sorted)
                {
                    out << " \"" << jsonEscape(aliasName) << "\": \"" << jsonEscape(targetName) << "\"";
                    if (++idx < sorted.size())
                        out << ",";
                }
                out << " }";
                if (trailingComma)
                    out << ",";
                out << "\n";
            };
            writeAliasMap("framebufferAliases", m_FramebufferBaseNameAliases, true);
            writeAliasMap("textureAliases", m_TextureBaseNameAliases, true);

            // Per-pass diagnostics: enabled / ready / culled + primary I/O
            // handle resolutions + derived cull reason. Stable order =
            // execution order, then any registered passes that didn't make
            // it into the execution plan (culled with no scheduled slot).
            out << "    \"passDiagnostics\": [\n";
            std::vector<std::string> diagOrder;
            diagOrder.reserve(m_NodeLookup.size());
            std::unordered_set<std::string> emittedInDiag;
            for (const auto& passName : m_ExecutionOrder)
            {
                diagOrder.push_back(passName);
                emittedInDiag.insert(passName);
            }
            for (const auto& [passName, _node] : m_NodeLookup)
            {
                if (!emittedInDiag.contains(passName))
                {
                    diagOrder.push_back(passName);
                    emittedInDiag.insert(passName);
                }
            }
            std::sort(diagOrder.begin() + static_cast<std::ptrdiff_t>(m_ExecutionOrder.size()),
                      diagOrder.end());

            const auto deriveCullReason = [this, &culledPasses](const std::string& passName) -> std::string
            {
                std::vector<const ResourceInfo*> writtenResources;
                for (const auto& info : m_RegisteredResources)
                {
                    if (std::ranges::find(info.Producers, passName) != info.Producers.end())
                        writtenResources.push_back(&info);
                }
                if (writtenResources.empty())
                    return "no declared outputs";

                for (const auto* info : writtenResources)
                {
                    for (const auto& consumer : info->Consumers)
                    {
                        if (!culledPasses.contains(consumer))
                            return "indirectly unreachable";
                    }
                }
                return "no downstream reader";
            };

            for (sizet pi = 0; pi < diagOrder.size(); ++pi)
            {
                const auto& passName = diagOrder[pi];
                const auto nodeIt = m_NodeLookup.find(passName);
                const auto node = nodeIt != m_NodeLookup.end() ? nodeIt->second : Ref<RenderGraphNode>{};
                const bool culled = culledPasses.contains(passName);
                const bool enabled = node ? node->IsEnabled() : false;
                const bool ready = node ? node->IsReadyForExecution() : false;

                const auto inFb = node ? node->GetPrimaryInputFramebufferHandle() : RGFramebufferHandle{};
                const auto inTex = node ? node->GetPrimaryInputTextureHandle() : RGTextureHandle{};
                const auto outFb = node ? node->GetPrimaryOutputFramebufferHandle() : RGFramebufferHandle{};
                const auto outTex = node ? node->GetPrimaryOutputTextureHandle() : RGTextureHandle{};

                out << "      { \"pass\": \"" << jsonEscape(passName)
                    << "\", \"isEnabled\": " << (enabled ? "true" : "false")
                    << ", \"isReady\": " << (ready ? "true" : "false")
                    << ", \"isCulled\": " << (culled ? "true" : "false")
                    << ", \"cullReason\": \"" << jsonEscape(culled ? deriveCullReason(passName) : std::string{})
                    << "\", \"primaryInputFramebuffer\": \"" << jsonEscape(ReverseResolveFramebufferName(inFb))
                    << "\", \"primaryInputTexture\": \"" << jsonEscape(ReverseResolveTextureName(inTex))
                    << "\", \"primaryOutputFramebuffer\": \"" << jsonEscape(ReverseResolveFramebufferName(outFb))
                    << "\", \"primaryOutputTexture\": \"" << jsonEscape(ReverseResolveTextureName(outTex))
                    << "\" }";
                if (pi + 1 < diagOrder.size())
                    out << ",";
                out << "\n";
            }
            out << "    ]\n";
        }
        out << "  },\n";

        out << "  \"timings\": [\n";
        for (sizet i = 0; i < m_LastExecutionTimings.size(); ++i)
        {
            const auto& timing = m_LastExecutionTimings[i];
            const auto passOrderIt = passOrderIndexByName.find(timing.NodeName);
            const auto orderIndex = passOrderIt != passOrderIndexByName.end()
                                        ? static_cast<i64>(passOrderIt->second)
                                        : -1;
            out << "    { \"pass\": \"" << jsonEscape(timing.NodeName)
                << "\", \"orderIndex\": " << orderIndex
                << ", \"cpuMs\": " << timing.CpuMs << " }";
            if (i + 1 < m_LastExecutionTimings.size())
                out << ",";
            out << "\n";
        }
        out << "  ]\n";
        out << "}\n";
        out.close();

        OLO_CORE_INFO("RenderGraph::DumpToJson: wrote {} passes and {} resources to '{}'",
                      m_ExecutionOrder.size(), m_RegisteredResources.size(), filePath);
        return true;
    }
    // -------------------------------------------------------------------
    // Per-frame graph building
    // -------------------------------------------------------------------

    void RenderGraph::BuildFrameGraph(u64 cacheFingerprint)
    {
        OLO_PROFILE_FUNCTION();
        OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph");

        // Defensive: if a previous frame aborted after acquiring transient
        // resources, recycle them before compiling the next frame.
        m_TransientPool.ReleaseAll();
        m_TransientPool.Trim(m_TransientPoolMaxBucketSize);

        // Caller-driven cache: when the supplied fingerprint matches the last
        // successful build and no external change has marked the graph dirty,
        // the cached m_PassAccessDeclarations / m_Dependencies / reachability
        // / barrier plan / transient plan / submission plan from the previous
        // frame are still valid. Execute() will walk the cached submission
        // plan unchanged.
        if (cacheFingerprint != 0u &&
            !m_DependencyGraphDirty &&
            m_HasValidBuildFrameGraphCache &&
            cacheFingerprint == m_LastBuildFrameGraphFingerprint)
        {
            return;
        }

        // Cache invariant: while we're rebuilding, the cached fingerprint is
        // not yet valid. We clear m_PassAccessDeclarations / m_Dependencies /
        // etc. below, so if the rebuild fails partway (e.g. cycle detection
        // bails out at UpdateDependencyGraph), the previous frame's cached
        // state is no longer consistent with what's in memory.
        m_HasValidBuildFrameGraphCache = false;

        // Build per-frame declarations and derived dependencies from
        // graph-native node setup callbacks before compiling reachability,
        // barriers, and transient planning.

        m_LastBuildStats = {};
        m_Dependencies = m_ExplicitDependencies;
        m_DependencyGraphDirty = true;
        m_PassAccessDeclarations.clear();
        m_PassFeedbackDeclarations.clear();
        m_PassBarrierFlags.clear();
        m_PlannedBarriers.clear();
        m_BuildDiagnostics.clear();
        m_BarrierDiagnostics.clear();
        m_TemporalHistoryContracts.clear();
        m_ExplicitVersionProducers.clear();
        m_LastWriterPassNameByResource.clear();
        m_LatestTextureHandlesByBaseName.clear();
        m_LatestBufferHandlesByBaseName.clear();
        m_LatestFramebufferHandlesByBaseName.clear();

        RGBuilder builder(*this, m_Blackboard);

        // Per-subresource dependency tracker: resource name → list of (pass, range) slots.
        // Used to derive ordering edges only between passes that actually touch the same
        // subresource (e.g. two passes writing different mips do NOT need an edge).
        struct DepWriterSlot
        {
            std::string PassName;
            RGSubresourceRange Range;
        };
        std::unordered_map<std::string, std::vector<DepWriterSlot>> lastWriterByResource;
        const auto graphEntryCount = m_InsertionOrder.size();
        lastWriterByResource.reserve(graphEntryCount * 4u);

        struct EdgeKey
        {
            std::string BeforePass;
            std::string AfterPass;

            auto operator==(const EdgeKey&) const -> bool = default;
        };

        struct EdgeKeyHasher
        {
            [[nodiscard]] auto operator()(const EdgeKey& key) const -> sizet
            {
                auto beforeHash = std::hash<std::string>{}(key.BeforePass);
                auto afterHash = std::hash<std::string>{}(key.AfterPass);
                return beforeHash ^ (afterHash + 0x9e3779b9u + (beforeHash << 6u) + (beforeHash >> 2u));
            }
        };

        struct DerivedEdgeOrigin
        {
            std::string ResourceName;
            bool FromPassDependency = false;
        };

        using DerivedEdgeMap = std::unordered_map<EdgeKey, DerivedEdgeOrigin, EdgeKeyHasher>;

        struct SimulatedDependencyResult
        {
            std::unordered_map<std::string, std::vector<std::string>> Dependencies;
            DerivedEdgeMap DerivedEdges;
        };

        std::unordered_map<std::string, std::vector<std::string>> declaredPassDependenciesByPass;
        declaredPassDependenciesByPass.reserve(graphEntryCount);
        std::vector<std::string> processedNodeNames;
        processedNodeNames.reserve(graphEntryCount);

        // Subresource overlap helper (same semantics as in ComputeBarrierPlan).
        auto depRangeOverlaps1D = [](u32 baseA, u32 countA, u32 baseB, u32 countB) -> bool
        {
            if (countA == ~0u || countB == ~0u)
                return true;
            return baseA < baseB + countB && baseB < baseA + countA;
        };
        auto depSubresourceRangesOverlap = [&depRangeOverlaps1D](const RGSubresourceRange& a,
                                                                 const RGSubresourceRange& b) -> bool
        {
            return depRangeOverlaps1D(a.BaseMip, a.MipCount, b.BaseMip, b.MipCount) &&
                   depRangeOverlaps1D(a.BaseLayer, a.LayerCount, b.BaseLayer, b.LayerCount);
        };

        auto accessDeclarationsEqual = [](const RGAccessDeclaration& lhs,
                                          const RGAccessDeclaration& rhs) -> bool
        {
            return lhs.ResourceName == rhs.ResourceName &&
                   lhs.IsWrite == rhs.IsWrite &&
                   lhs.ReadUsage == rhs.ReadUsage &&
                   lhs.WriteUsage == rhs.WriteUsage &&
                   lhs.Range.BaseMip == rhs.Range.BaseMip &&
                   lhs.Range.MipCount == rhs.Range.MipCount &&
                   lhs.Range.BaseLayer == rhs.Range.BaseLayer &&
                   lhs.Range.LayerCount == rhs.Range.LayerCount &&
                   lhs.Range.BaseSlice == rhs.Range.BaseSlice &&
                   lhs.Range.SliceCount == rhs.Range.SliceCount;
        };

        auto feedbackDeclarationsEqual = [](const RGFeedbackDeclaration& lhs,
                                            const RGFeedbackDeclaration& rhs) -> bool
        {
            return lhs.ResourceName == rhs.ResourceName &&
                   lhs.Range.BaseMip == rhs.Range.BaseMip &&
                   lhs.Range.MipCount == rhs.Range.MipCount &&
                   lhs.Range.BaseLayer == rhs.Range.BaseLayer &&
                   lhs.Range.LayerCount == rhs.Range.LayerCount &&
                   lhs.Range.BaseSlice == rhs.Range.BaseSlice &&
                   lhs.Range.SliceCount == rhs.Range.SliceCount;
        };

        auto appendUniqueAccessDeclaration = [&accessDeclarationsEqual](std::vector<RGAccessDeclaration>& declarations,
                                                                        const RGAccessDeclaration& declaration)
        {
            if (declaration.ResourceName.empty())
                return;

            if (std::ranges::find_if(declarations,
                                     [&declaration, &accessDeclarationsEqual](const RGAccessDeclaration& existing)
                                     {
                                         return accessDeclarationsEqual(existing, declaration);
                                     }) == declarations.end())
            {
                declarations.push_back(declaration);
            }
        };

        auto appendUniqueFeedbackDeclaration = [&feedbackDeclarationsEqual](std::vector<RGFeedbackDeclaration>& declarations,
                                                                            const RGFeedbackDeclaration& declaration)
        {
            if (declaration.ResourceName.empty())
                return;

            if (std::ranges::find_if(declarations,
                                     [&declaration, &feedbackDeclarationsEqual](const RGFeedbackDeclaration& existing)
                                     {
                                         return feedbackDeclarationsEqual(existing, declaration);
                                     }) == declarations.end())
            {
                declarations.push_back(declaration);
            }
        };

        auto expandTextureViewAccesses =
            [this, &appendUniqueAccessDeclaration, &depSubresourceRangesOverlap](const std::vector<RGAccessDeclaration>& accesses)
        {
            std::vector<RGAccessDeclaration> expandedAccesses;
            expandedAccesses.reserve(accesses.size() * 3u);

            const auto isTextureSubresourceView = [](const TextureViewKind kind)
            {
                return kind == TextureViewKind::TextureMip ||
                       kind == TextureViewKind::TextureArrayLayer ||
                       kind == TextureViewKind::TextureCubeFace;
            };

            for (const auto& access : accesses)
            {
                appendUniqueAccessDeclaration(expandedAccesses, access);
                if (access.ResourceName.empty())
                    continue;

                const auto resourceKey = std::string(access.ResourceName);
                if (const auto viewIt = m_TextureViewDefinitions.find(resourceKey);
                    viewIt != m_TextureViewDefinitions.end())
                {
                    if (isTextureSubresourceView(viewIt->second.Kind))
                    {
                        auto expandedAccess = access;
                        expandedAccess.ResourceName = viewIt->second.ParentResource;
                        expandedAccess.Range = viewIt->second.ParentRange;
                        appendUniqueAccessDeclaration(expandedAccesses, expandedAccess);
                    }
                    else if (viewIt->second.Kind == TextureViewKind::TextureMultisampleResolve &&
                             !viewIt->second.BackingResource.empty())
                    {
                        auto expandedAccess = access;
                        expandedAccess.ResourceName = viewIt->second.BackingResource;
                        expandedAccess.Range = RGSubresourceRange::Full();
                        appendUniqueAccessDeclaration(expandedAccesses, expandedAccess);
                    }

                    continue;
                }

                for (const auto& [viewName, viewDef] : m_TextureViewDefinitions)
                {
                    if (viewDef.ParentResource != access.ResourceName)
                        continue;

                    if (isTextureSubresourceView(viewDef.Kind) &&
                        !depSubresourceRangesOverlap(access.Range, viewDef.ParentRange))
                    {
                        continue;
                    }

                    auto expandedAccess = access;
                    expandedAccess.ResourceName = viewName;
                    if (isTextureSubresourceView(viewDef.Kind) ||
                        viewDef.Kind == TextureViewKind::TextureMultisampleResolve)
                        expandedAccess.Range = RGSubresourceRange::Full();
                    appendUniqueAccessDeclaration(expandedAccesses, expandedAccess);
                }
            }

            return expandedAccesses;
        };

        auto expandTextureViewFeedbacks =
            [this, &appendUniqueFeedbackDeclaration, &depSubresourceRangesOverlap](const std::vector<RGFeedbackDeclaration>& feedbacks)
        {
            std::vector<RGFeedbackDeclaration> expandedFeedbacks;
            expandedFeedbacks.reserve(feedbacks.size() * 3u);

            const auto isTextureSubresourceView = [](const TextureViewKind kind)
            {
                return kind == TextureViewKind::TextureMip ||
                       kind == TextureViewKind::TextureArrayLayer ||
                       kind == TextureViewKind::TextureCubeFace;
            };

            for (const auto& feedback : feedbacks)
            {
                appendUniqueFeedbackDeclaration(expandedFeedbacks, feedback);
                if (feedback.ResourceName.empty())
                    continue;

                const auto resourceKey = std::string(feedback.ResourceName);
                if (const auto viewIt = m_TextureViewDefinitions.find(resourceKey);
                    viewIt != m_TextureViewDefinitions.end())
                {
                    if (isTextureSubresourceView(viewIt->second.Kind))
                    {
                        auto expandedFeedback = feedback;
                        expandedFeedback.ResourceName = viewIt->second.ParentResource;
                        expandedFeedback.Range = viewIt->second.ParentRange;
                        appendUniqueFeedbackDeclaration(expandedFeedbacks, expandedFeedback);
                    }
                    else if (viewIt->second.Kind == TextureViewKind::TextureMultisampleResolve &&
                             !viewIt->second.BackingResource.empty())
                    {
                        auto expandedFeedback = feedback;
                        expandedFeedback.ResourceName = viewIt->second.BackingResource;
                        expandedFeedback.Range = RGSubresourceRange::Full();
                        appendUniqueFeedbackDeclaration(expandedFeedbacks, expandedFeedback);
                    }

                    continue;
                }

                for (const auto& [viewName, viewDef] : m_TextureViewDefinitions)
                {
                    if (viewDef.ParentResource != feedback.ResourceName)
                        continue;

                    if (isTextureSubresourceView(viewDef.Kind) &&
                        !depSubresourceRangesOverlap(feedback.Range, viewDef.ParentRange))
                    {
                        continue;
                    }

                    auto expandedFeedback = feedback;
                    expandedFeedback.ResourceName = viewName;
                    if (isTextureSubresourceView(viewDef.Kind) ||
                        viewDef.Kind == TextureViewKind::TextureMultisampleResolve)
                        expandedFeedback.Range = RGSubresourceRange::Full();
                    appendUniqueFeedbackDeclaration(expandedFeedbacks, expandedFeedback);
                }
            }

            return expandedFeedbacks;
        };

        auto tryAddDerivedDependency = [this](const std::string& beforePass, const std::string& afterPass) -> bool
        {
            if (beforePass == afterPass)
                return false;
            if (!ContainsGraphEntry(beforePass) || !ContainsGraphEntry(afterPass))
            {
                // Intentional miss: passes name future producers (e.g. DLP
                // declares DependsOnPass("SSAOPass") + DependsOnPass("GTAOPass")
                // — only one is active per frame). Only surface this when
                // diagnostics are explicitly enabled so it's available for
                // catching typos during development without spamming
                // production logs.
                if (IsRenderGraphDiagnosticsEnabled())
                {
                    OLO_CORE_WARN("tryAddDerivedDependency: graph entry not found: {} -> {}",
                                  beforePass, afterPass);
                }
                return false;
            }

            if (auto& deps = m_Dependencies[afterPass]; std::ranges::find(deps, beforePass) != deps.end())
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

        auto processGraphNode = [this, &builder, &expandTextureViewAccesses, &expandTextureViewFeedbacks,
                                 &declaredPassDependenciesByPass, &tryAddDerivedDependency, &lastWriterByResource,
                                 &depSubresourceRangesOverlap, &processedNodeNames](RenderGraphNode& node)
        {
            const std::string nodeName(node.GetName());
            if (nodeName.empty())
            {
                OLO_CORE_ERROR("processGraphNode: encountered node with empty name");
                return;
            }

            ++m_LastBuildStats.PassesVisited;

            builder.BeginPass(nodeName);

            // Clear the per-pass primary I/O handles before Setup so each
            // frame starts fresh — previously this lived inside the default
            // Setup() impl, which silently reset state and was a footgun for
            // any derived pass that forgot to chain to base. Doing it here,
            // outside the pass, makes the contract enforced by the graph.
            node.ResetPrimaryHandlesForFrame();

            try
            {
                node.Setup(builder, m_Blackboard);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("processGraphNode: exception during setup callback for node '{}': {}",
                               nodeName, e.what());
                return;
            }

            const auto& reads = builder.GetDeclaredReads();
            m_LastBuildStats.DeclaredReads += static_cast<u32>(reads.size());

            const auto& accesses = builder.GetDeclaredAccesses();
            const auto expandedAccesses = expandTextureViewAccesses(accesses);
            m_PassAccessDeclarations[nodeName] = expandedAccesses;

            const auto& feedbacks = builder.GetDeclaredFeedbacks();
            m_PassFeedbackDeclarations[nodeName] = expandTextureViewFeedbacks(feedbacks);

            const auto& passDependencies = builder.GetDeclaredPassDependencies();
            declaredPassDependenciesByPass[nodeName] = passDependencies;
            for (const auto& beforePass : passDependencies)
            {
                if (beforePass.empty())
                    continue;

                if (tryAddDerivedDependency(beforePass, nodeName))
                    ++m_LastBuildStats.DerivedEdges;
            }

            for (const auto& access : expandedAccesses)
            {
                if (access.ResourceName.empty())
                    continue;

                if (!access.IsWrite)
                {
                    if (const auto explicitVersionIt = m_ExplicitVersionProducers.find(access.ResourceName);
                        explicitVersionIt != m_ExplicitVersionProducers.end())
                    {
                        if (explicitVersionIt->second != nodeName &&
                            tryAddDerivedDependency(explicitVersionIt->second, nodeName))
                        {
                            ++m_LastBuildStats.DerivedEdges;
                        }
                        continue;
                    }

                    const auto writerIt = lastWriterByResource.find(access.ResourceName);
                    if (writerIt == lastWriterByResource.end())
                        continue;
                    for (const auto& slot : writerIt->second)
                    {
                        if (slot.PassName == nodeName)
                            continue;
                        if (!depSubresourceRangesOverlap(slot.Range, access.Range))
                            continue;
                        if (tryAddDerivedDependency(slot.PassName, nodeName))
                            ++m_LastBuildStats.DerivedEdges;
                    }
                }
                else
                {
                    auto& writerVec = lastWriterByResource[access.ResourceName];
                    for (const auto& slot : writerVec)
                    {
                        if (slot.PassName == nodeName)
                            continue;
                        if (!depSubresourceRangesOverlap(slot.Range, access.Range))
                            continue;
                        if (tryAddDerivedDependency(slot.PassName, nodeName))
                            ++m_LastBuildStats.DerivedEdges;
                    }

                    bool slotUpdated = false;
                    for (auto& slot : writerVec)
                    {
                        if (slot.PassName == nodeName && depSubresourceRangesOverlap(slot.Range, access.Range))
                        {
                            slot.Range = access.Range;
                            slotUpdated = true;
                            break;
                        }
                    }
                    if (!slotUpdated)
                        writerVec.push_back(DepWriterSlot{ nodeName, access.Range });

                    // Track the most-recent writer by resource base name so a
                    // subsequent RMW pass's Setup can call
                    // RenderGraph::GetLastWriterPassName(resource) and emit an
                    // explicit DependsOnPass edge without a typed setter.
                    //
                    // When the write is on a versioned variant (`X@tag` from
                    // WriteNewVersion), also update the BASE name's entry so
                    // a downstream RMW pass that calls
                    // DependsOnPreviousWriter("X") picks up the latest
                    // versioned writer — not just the original base-name
                    // writer. Without this, a chain like:
                    //   DeferredLightingPass    writes "SceneColor"
                    //   ForwardOverlayPass      WriteNewVersion("SceneColor")
                    //   ParticlePass            DependsOnPreviousWriter("SceneColor")
                    // resolves ParticlePass's DependsOnPreviousWriter to
                    // DeferredLightingPass (skipping ForwardOverlay), and the
                    // reachability sweep culls ForwardOverlay as "no
                    // downstream reader" whenever the optional read paths in
                    // between are absent.
                    m_LastWriterPassNameByResource[access.ResourceName] = nodeName;
                    if (const auto baseName = GetVersionLookupBaseName(access.ResourceName);
                        baseName != access.ResourceName)
                    {
                        m_LastWriterPassNameByResource[std::string(baseName)] = nodeName;

                        // Same fix at the LOCAL Read→Writer derivation level:
                        // when a versioned write happens, also publish the
                        // current pass as a writer of the BASE resource name
                        // so subsequent Reads on the base handle (e.g.
                        // OITResolvePass reading `blackboard.OIT.OITAccum`)
                        // derive a dependency on the versioned writer, not
                        // just the original base writer.
                        //
                        // Without this, the OIT chain in WB-OIT mode is
                        // silently broken: OITPreparePass clears OITAccum
                        // (base), Decal / Particle write versioned siblings,
                        // and OITResolve reads the base — so only
                        // OITPreparePass is in OITResolve's dependency set,
                        // and Decal / Particle are culled as orphans even
                        // though their writes feed the resolve.
                        const auto baseNameStr = std::string(baseName);
                        auto& baseWriterVec = lastWriterByResource[baseNameStr];
                        bool baseSlotPresent = false;
                        for (auto& slot : baseWriterVec)
                        {
                            if (slot.PassName == nodeName)
                            {
                                slot.Range = RGSubresourceRange::Full();
                                baseSlotPresent = true;
                                break;
                            }
                        }
                        if (!baseSlotPresent)
                            baseWriterVec.push_back(DepWriterSlot{ nodeName, RGSubresourceRange::Full() });
                    }
                }
            }

            for (const auto& resourceName : reads)
            {
                if (resourceName.empty())
                    OLO_CORE_WARN("processGraphNode: node '{}' declared empty-name read (handle mapping failed)", nodeName);
            }

            const auto& writes = builder.GetDeclaredWrites();
            m_LastBuildStats.DeclaredWrites += static_cast<u32>(writes.size());

            for (const auto& resourceName : writes)
            {
                if (resourceName.empty())
                    OLO_CORE_WARN("processGraphNode: node '{}' declared empty-name write (handle mapping failed)", nodeName);
            }

            processedNodeNames.push_back(nodeName);
        };

        // Use insertion order as the canonical ordering seed for dependency
        // derivation. Only graph-native nodes contribute per-frame builder
        // declarations here; pass-only entries still participate through their
        // static RenderPass declarations in later reachability/hazard passes.
        {
            OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph/SetupLoop");
            for (const auto& passName : m_InsertionOrder)
            {
                const auto nodeIt = m_NodeLookup.find(passName);
                if (nodeIt != m_NodeLookup.end() && nodeIt->second)
                    processGraphNode(*nodeIt->second);
            }
        }

        auto simulateDerivedDependencies =
            [this, &declaredPassDependenciesByPass, &depSubresourceRangesOverlap](const std::vector<std::string>& visitOrder) -> SimulatedDependencyResult
        {
            SimulatedDependencyResult result{};
            result.Dependencies = m_ExplicitDependencies;
            std::unordered_map<std::string, std::vector<DepWriterSlot>> simulatedLastWriterByResource;
            simulatedLastWriterByResource.reserve(visitOrder.size() * 4u);

            result.DerivedEdges.reserve(visitOrder.size() * 4u);

            auto tryAddSimulatedDerivedDependency =
                [this, &result](const std::string& beforePass,
                                const std::string& afterPass,
                                const DerivedEdgeOrigin& origin) -> bool
            {
                if (beforePass == afterPass)
                    return false;
                if (!ContainsGraphEntry(beforePass) || !ContainsGraphEntry(afterPass))
                    return false;

                auto& deps = result.Dependencies[afterPass];
                if (std::ranges::find(deps, beforePass) != deps.end())
                    return false;

                std::unordered_set<std::string> visited;
                std::vector<std::string> frontier{ beforePass };
                while (!frontier.empty())
                {
                    auto current = std::move(frontier.back());
                    frontier.pop_back();

                    if (!visited.insert(current).second)
                        continue;

                    if (current == afterPass)
                        return false;

                    const auto existingIt = result.Dependencies.find(current);
                    if (existingIt == result.Dependencies.end())
                        continue;

                    for (const auto& producer : existingIt->second)
                    {
                        if (!visited.contains(producer))
                            frontier.push_back(producer);
                    }
                }

                deps.push_back(beforePass);
                result.DerivedEdges.emplace(EdgeKey{ beforePass, afterPass }, origin);
                return true;
            };

            for (const auto& nodeName : visitOrder)
            {
                if (const auto passDependencyIt = declaredPassDependenciesByPass.find(nodeName);
                    passDependencyIt != declaredPassDependenciesByPass.end())
                {
                    for (const auto& beforePass : passDependencyIt->second)
                    {
                        if (beforePass.empty())
                            continue;

                        tryAddSimulatedDerivedDependency(beforePass, nodeName, DerivedEdgeOrigin{ "", true });
                    }
                }

                const auto accessIt = m_PassAccessDeclarations.find(nodeName);
                if (accessIt == m_PassAccessDeclarations.end())
                    continue;

                for (const auto& access : accessIt->second)
                {
                    if (access.ResourceName.empty())
                        continue;

                    if (!access.IsWrite)
                    {
                        if (const auto explicitVersionIt = m_ExplicitVersionProducers.find(access.ResourceName);
                            explicitVersionIt != m_ExplicitVersionProducers.end())
                        {
                            if (explicitVersionIt->second != nodeName)
                            {
                                tryAddSimulatedDerivedDependency(explicitVersionIt->second,
                                                                 nodeName,
                                                                 DerivedEdgeOrigin{ access.ResourceName, false });
                            }
                            continue;
                        }

                        const auto writerIt = simulatedLastWriterByResource.find(access.ResourceName);
                        if (writerIt == simulatedLastWriterByResource.end())
                            continue;

                        for (const auto& slot : writerIt->second)
                        {
                            if (slot.PassName == nodeName)
                                continue;
                            if (!depSubresourceRangesOverlap(slot.Range, access.Range))
                                continue;

                            tryAddSimulatedDerivedDependency(slot.PassName,
                                                             nodeName,
                                                             DerivedEdgeOrigin{ access.ResourceName, false });
                        }
                    }
                    else
                    {
                        auto& writerVec = simulatedLastWriterByResource[access.ResourceName];
                        for (const auto& slot : writerVec)
                        {
                            if (slot.PassName == nodeName)
                                continue;
                            if (!depSubresourceRangesOverlap(slot.Range, access.Range))
                                continue;

                            tryAddSimulatedDerivedDependency(slot.PassName,
                                                             nodeName,
                                                             DerivedEdgeOrigin{ access.ResourceName, false });
                        }

                        bool slotUpdated = false;
                        for (auto& slot : writerVec)
                        {
                            if (slot.PassName == nodeName && depSubresourceRangesOverlap(slot.Range, access.Range))
                            {
                                slot.Range = access.Range;
                                slotUpdated = true;
                                break;
                            }
                        }

                        if (!slotUpdated)
                            writerVec.push_back(DepWriterSlot{ nodeName, access.Range });
                    }
                }
            }

            return result;
        };

        // Registration-order-sensitivity diagnostic. Pure analysis: compares forward vs.
        // reversed-iteration derived dependency results and reports edges that differ.
        // Stripped in Dist; gated behind OLO_RENDERGRAPH_DIAGNOSTICS otherwise so it
        // does not run on the per-frame hot path unless explicitly enabled.
#if !defined(OLO_DIST)
        if (IsRenderGraphDiagnosticsEnabled())
        {
            const auto currentSimulation = simulateDerivedDependencies(processedNodeNames);
            auto reversedProcessedNodeNames = processedNodeNames;
            std::ranges::reverse(reversedProcessedNodeNames);
            const auto reversedSimulation = simulateDerivedDependencies(reversedProcessedNodeNames);

            enum class SimulatedOrderingRelation
            {
                Unordered,
                BeforeAfter,
                AfterBefore,
            };

            const auto hasSimulatedOrdering = [](const std::unordered_map<std::string, std::vector<std::string>>& dependencies,
                                                 const std::string& beforePass,
                                                 const std::string& afterPass) -> bool
            {
                if (beforePass.empty() || afterPass.empty() || beforePass == afterPass)
                    return false;

                std::unordered_set<std::string> visited;
                std::vector<std::string> frontier{ afterPass };
                while (!frontier.empty())
                {
                    auto current = std::move(frontier.back());
                    frontier.pop_back();

                    if (!visited.insert(current).second)
                        continue;

                    if (current == beforePass)
                        return true;

                    const auto it = dependencies.find(current);
                    if (it == dependencies.end())
                        continue;

                    for (const auto& producer : it->second)
                    {
                        if (!visited.contains(producer))
                            frontier.push_back(producer);
                    }
                }

                return false;
            };

            const auto getSimulatedOrderingRelation = [&hasSimulatedOrdering](const std::unordered_map<std::string, std::vector<std::string>>& dependencies,
                                                                              const std::string& passA,
                                                                              const std::string& passB) -> SimulatedOrderingRelation
            {
                const bool aBeforeB = hasSimulatedOrdering(dependencies, passA, passB);
                const bool bBeforeA = hasSimulatedOrdering(dependencies, passB, passA);

                if (aBeforeB && !bBeforeA)
                    return SimulatedOrderingRelation::BeforeAfter;
                if (!aBeforeB && bBeforeA)
                    return SimulatedOrderingRelation::AfterBefore;
                return SimulatedOrderingRelation::Unordered;
            };

            const auto shouldReportBuildDiagnostic =
                [&currentSimulation, &reversedSimulation, &getSimulatedOrderingRelation](const EdgeKey* currentEdge,
                                                                                         const EdgeKey* alternateEdge) -> bool
            {
                const auto* comparisonEdge = currentEdge ? currentEdge : alternateEdge;
                if (!comparisonEdge)
                    return false;

                const auto currentRelation = getSimulatedOrderingRelation(currentSimulation.Dependencies,
                                                                          comparisonEdge->BeforePass,
                                                                          comparisonEdge->AfterPass);
                const auto alternateRelation = getSimulatedOrderingRelation(reversedSimulation.Dependencies,
                                                                            comparisonEdge->BeforePass,
                                                                            comparisonEdge->AfterPass);
                return currentRelation != alternateRelation;
            };

            auto appendBuildDiagnostic =
                [this](const EdgeKey* currentEdge,
                       const DerivedEdgeOrigin* currentOrigin,
                       const EdgeKey* alternateEdge,
                       const DerivedEdgeOrigin* alternateOrigin)
            {
                BuildDiagnostic diagnostic{};
                diagnostic.Kind = BuildDiagnosticKind::RegistrationOrderSensitivity;

                if (currentOrigin && !currentOrigin->ResourceName.empty())
                    diagnostic.Resource = currentOrigin->ResourceName;
                else if (alternateOrigin && !alternateOrigin->ResourceName.empty())
                    diagnostic.Resource = alternateOrigin->ResourceName;

                if (currentEdge)
                {
                    diagnostic.CurrentBeforePass = currentEdge->BeforePass;
                    diagnostic.CurrentAfterPass = currentEdge->AfterPass;
                }

                if (alternateEdge)
                {
                    diagnostic.AlternateBeforePass = alternateEdge->BeforePass;
                    diagnostic.AlternateAfterPass = alternateEdge->AfterPass;
                }

                std::string message = "registration order changed derived dependency result";
                if (!diagnostic.Resource.empty())
                    message += " for resource '" + diagnostic.Resource + "'";

                if (currentEdge && alternateEdge)
                {
                    message += ": current build derives '" + diagnostic.CurrentBeforePass + "' -> '" + diagnostic.CurrentAfterPass +
                               "', reversed visitation derives '" + diagnostic.AlternateBeforePass + "' -> '" + diagnostic.AlternateAfterPass + "'";
                }
                else if (currentEdge)
                {
                    message += ": current build derives '" + diagnostic.CurrentBeforePass + "' -> '" + diagnostic.CurrentAfterPass +
                               "', reversed visitation derives no matching edge";
                }
                else if (alternateEdge)
                {
                    message += ": current build derives no matching edge, reversed visitation derives '" + diagnostic.AlternateBeforePass +
                               "' -> '" + diagnostic.AlternateAfterPass + "'";
                }

                diagnostic.Message = std::move(message);
                m_BuildDiagnostics.push_back(std::move(diagnostic));
            };

            std::unordered_set<EdgeKey, EdgeKeyHasher> consumedReversedEdges;
            for (const auto& [edge, origin] : currentSimulation.DerivedEdges)
            {
                if (reversedSimulation.DerivedEdges.contains(edge))
                    continue;

                const EdgeKey oppositeEdge{ edge.AfterPass, edge.BeforePass };
                if (const auto oppositeIt = reversedSimulation.DerivedEdges.find(oppositeEdge);
                    oppositeIt != reversedSimulation.DerivedEdges.end())
                {
                    if (!shouldReportBuildDiagnostic(&edge, &oppositeEdge))
                        continue;

                    appendBuildDiagnostic(&edge, &origin, &oppositeEdge, &oppositeIt->second);
                    consumedReversedEdges.insert(oppositeEdge);
                    continue;
                }

                if (!shouldReportBuildDiagnostic(&edge, nullptr))
                    continue;

                appendBuildDiagnostic(&edge, &origin, nullptr, nullptr);
            }

            for (const auto& [edge, origin] : reversedSimulation.DerivedEdges)
            {
                if (currentSimulation.DerivedEdges.contains(edge) || consumedReversedEdges.contains(edge))
                    continue;

                if (!shouldReportBuildDiagnostic(nullptr, &edge))
                    continue;

                appendBuildDiagnostic(nullptr, nullptr, &edge, &origin);
            }

            std::ranges::sort(m_BuildDiagnostics,
                              [](const BuildDiagnostic& lhs, const BuildDiagnostic& rhs)
                              {
                                  if (lhs.Resource != rhs.Resource)
                                      return lhs.Resource < rhs.Resource;
                                  if (lhs.CurrentBeforePass != rhs.CurrentBeforePass)
                                      return lhs.CurrentBeforePass < rhs.CurrentBeforePass;
                                  if (lhs.CurrentAfterPass != rhs.CurrentAfterPass)
                                      return lhs.CurrentAfterPass < rhs.CurrentAfterPass;
                                  if (lhs.AlternateBeforePass != rhs.AlternateBeforePass)
                                      return lhs.AlternateBeforePass < rhs.AlternateBeforePass;
                                  if (lhs.AlternateAfterPass != rhs.AlternateAfterPass)
                                      return lhs.AlternateAfterPass < rhs.AlternateAfterPass;
                                  return lhs.Message < rhs.Message;
                              });

            m_LastBuildStats.OrderSensitiveResults = static_cast<u32>(m_BuildDiagnostics.size());

            if (m_BuildDiagnostics.empty())
            {
                m_LastLoggedBuildDiagnosticDigest.clear();
            }
            else
            {
                std::string buildDiagnosticDigest;
                for (const auto& diagnostic : m_BuildDiagnostics)
                {
                    if (!buildDiagnosticDigest.empty())
                        buildDiagnosticDigest += ';';
                    buildDiagnosticDigest += diagnostic.Message;
                }

                if (buildDiagnosticDigest != m_LastLoggedBuildDiagnosticDigest)
                {
                    OLO_CORE_WARN("RenderGraph::BuildFrameGraph: registration order changed the derived dependency result ({} diagnostics)",
                                  m_BuildDiagnostics.size());
                    for (const auto& diagnostic : m_BuildDiagnostics)
                        OLO_CORE_WARN("RenderGraph::BuildFrameGraph: {}", diagnostic.Message);

                    m_LastLoggedBuildDiagnosticDigest = std::move(buildDiagnosticDigest);
                }
            }
        } // end if (IsRenderGraphDiagnosticsEnabled())
#endif // !defined(OLO_DIST)

        {
            OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph/UpdateDependencyGraph");
            if (m_DependencyGraphDirty && !UpdateDependencyGraph())
            {
                OLO_CORE_ERROR("RenderGraph::BuildFrameGraph: dependency graph rebuild failed (cycle)");
                return;
            }

            if (m_DependencyGraphDirty)
            {
                ResolveFinalPass();
                m_DependencyGraphDirty = false;
            }
        }

        {
            OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph/ComputeReachability");
            ComputeReachability();
        }
        RefreshTemporalHistoryContracts();
        {
            OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph/ComputeBarrierPlan");
            ComputeBarrierPlan();
        }
        {
            OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph/RebuildTransientPlan");
            RebuildTransientPlan();
        }

        // Cache the submission plan after barrier planning so
        // Execute() can walk the pre-built IR without re-deriving it.
        {
            OLO_PERF_SCOPE_AUTO("RG::BuildFrameGraph/GetSubmissionPlan");
            m_CachedSubmissionPlan = GetSubmissionPlan();
        }
        LogSubmissionPlanIfChanged();

        // Cache the fingerprint of this successful build so subsequent calls
        // with the same caller-supplied fingerprint can short-circuit the
        // whole function.
        if (cacheFingerprint != 0u)
        {
            m_LastBuildFrameGraphFingerprint = cacheFingerprint;
            m_HasValidBuildFrameGraphCache = true;
        }
        else
        {
            m_HasValidBuildFrameGraphCache = false;
        }
    }

} // namespace OloEngine
