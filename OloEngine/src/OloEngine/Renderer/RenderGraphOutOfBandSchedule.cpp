#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <nlohmann/json.hpp>

#include <algorithm>

// RenderGraph members for out-of-band boundaries (issue #1331). Declared in
// RenderGraph.h; kept out of RenderGraph.cpp so the contract reads in one
// place: registration, frame-epilogue reads, declaration rules, the runtime
// ledger check and the exposed schedule.

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool DeclarationCovers(const std::span<const RGOutOfBandDeclaration> declarations,
                                             const std::string_view boundary, const RGOutOfBandAccess access)
        {
            return std::ranges::any_of(declarations, [&](const RGOutOfBandDeclaration& declaration)
                                       {
                                           if (declaration.Boundary.ToView() != boundary)
                                               return false;
                                           switch (access)
                                           {
                                               case RGOutOfBandAccess::Write:
                                                   return declaration.Access == RGOutOfBandAccess::Write;
                                               case RGOutOfBandAccess::Read:
                                                   // A writer may read back what it wrote.
                                                   return declaration.Access == RGOutOfBandAccess::Read ||
                                                          declaration.Access == RGOutOfBandAccess::Write;
                                               case RGOutOfBandAccess::ReadPreviousFrame:
                                                   return declaration.Access == RGOutOfBandAccess::ReadPreviousFrame;
                                           }
                                           return false; });
        }

        [[nodiscard]] RenderGraph::Hazard MakeHazard(const RenderGraph::HazardKind kind, std::string_view resource,
                                                     std::string_view producer, std::string_view consumer,
                                                     std::string message)
        {
            RenderGraph::Hazard hazard;
            hazard.Kind = kind;
            hazard.Resource = FString(resource);
            hazard.Producer = FString(producer);
            hazard.Consumer = FString(consumer);
            hazard.Message = FString(message);
            return hazard;
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Registry
    // ------------------------------------------------------------------------
    void RenderGraph::RegisterOutOfBandBoundary(const RGOutOfBandBoundary& boundary)
    {
        if (boundary.Name.empty())
            return;
        m_OutOfBandBoundaries[std::string(boundary.Name)] = boundary;
    }

    const RGOutOfBandBoundary* RenderGraph::FindOutOfBandBoundary(const std::string_view name) const
    {
        if (const auto it = m_OutOfBandBoundaries.find(name); it != m_OutOfBandBoundaries.end())
            return &it->second;
        for (const RGOutOfBandBoundary& boundary : GetProductionOutOfBandBoundaries())
        {
            if (boundary.Name == name)
                return &boundary;
        }
        return nullptr;
    }

    TArray64<RGOutOfBandBoundary> RenderGraph::GetOutOfBandBoundaries() const
    {
        TArray64<RGOutOfBandBoundary> boundaries;
        for (const RGOutOfBandBoundary& boundary : GetProductionOutOfBandBoundaries())
        {
            if (!m_OutOfBandBoundaries.contains(boundary.Name))
                boundaries.Add(boundary);
        }
        for (const auto& [name, boundary] : m_OutOfBandBoundaries)
            boundaries.Add(boundary);
        std::ranges::sort(boundaries, {}, &RGOutOfBandBoundary::Name);
        return boundaries;
    }

    std::span<const RGOutOfBandDeclaration> RenderGraph::GetPassOutOfBandDeclarations(const std::string_view passName) const
    {
        if (const auto it = m_PassOutOfBandDeclarations.find(passName); it != m_PassOutOfBandDeclarations.end())
            return { it->second.GetData(), static_cast<sizet>(it->second.Num()) };
        return {};
    }

    // ------------------------------------------------------------------------
    // Frame-epilogue reads
    // ------------------------------------------------------------------------
    void RenderGraph::DeclareFrameEpilogueRead(const std::string_view resourceName, const std::string_view consumer)
    {
        if (resourceName.empty())
            return;
        if (const auto existing = std::ranges::find_if(m_FrameEpilogueReads, [&](const FrameEpilogueRead& read)
                                                       { return read.Resource.ToView() == resourceName; });
            existing != m_FrameEpilogueReads.end())
        {
            existing->Consumer = FString(consumer);
            return;
        }
        m_FrameEpilogueReads.Add(FrameEpilogueRead{ .Resource = FString(resourceName), .Consumer = FString(consumer) });
        // A new root changes reachability and lifetimes: a cached plan is stale.
        InvalidateBuildFrameGraphCache();
    }

    void RenderGraph::ClearFrameEpilogueReads()
    {
        if (m_FrameEpilogueReads.IsEmpty())
            return;
        m_FrameEpilogueReads.Reset();
        m_FrameEpilogueFramebuffers.clear();
        m_FrameEpilogueTextures.clear();
        InvalidateBuildFrameGraphCache();
    }

    void RenderGraph::RemoveFrameEpilogueRead(const std::string_view resourceName)
    {
        const auto removed = m_FrameEpilogueReads.RemoveAll([&](const FrameEpilogueRead& read)
                                                            { return read.Resource.ToView() == resourceName; });
        if (removed == 0)
            return;
        m_FrameEpilogueFramebuffers.erase(std::string(resourceName));
        m_FrameEpilogueTextures.erase(std::string(resourceName));
        InvalidateBuildFrameGraphCache();
    }

    bool RenderGraph::HasFrameEpilogueRead(const std::string_view resourceName) const
    {
        return std::ranges::any_of(m_FrameEpilogueReads, [&](const FrameEpilogueRead& read)
                                   { return read.Resource.ToView() == resourceName; });
    }

    Ref<Framebuffer> RenderGraph::GetFrameEpilogueFramebuffer(const std::string_view resourceName) const
    {
        if (const auto it = m_FrameEpilogueFramebuffers.find(resourceName); it != m_FrameEpilogueFramebuffers.end())
            return it->second;
        return nullptr;
    }

    RHI::ResourceHandle RenderGraph::GetFrameEpilogueTexture(const std::string_view resourceName) const
    {
        if (const auto it = m_FrameEpilogueTextures.find(resourceName); it != m_FrameEpilogueTextures.end())
            return it->second;
        return {};
    }

    // ------------------------------------------------------------------------
    // Declaration rules
    // ------------------------------------------------------------------------
    bool RenderGraph::DependsOnTransitively(const std::string_view after, const std::string_view before) const
    {
        if (after == before)
            return true;
        RGTransparentStringSet visited;
        std::vector<std::string> frontier{ std::string(after) };
        while (!frontier.empty())
        {
            std::string current = std::move(frontier.back());
            frontier.pop_back();
            if (!visited.insert(current).second)
                continue;
            const auto it = m_Dependencies.find(current);
            if (it == m_Dependencies.end())
                continue;
            for (const FString& dependency : it->second)
            {
                if (dependency.ToView() == before)
                    return true;
                if (!visited.contains(dependency.ToView()))
                    frontier.push_back(dependency.ToStdString());
            }
        }
        return false;
    }

    void RenderGraph::AppendOutOfBandDeclarationHazards(TArray64<Hazard>& hazards) const
    {
        struct Use
        {
            std::string Pass;
            RGOutOfBandAccess Access;
        };
        RGTransparentStringMap<std::vector<Use>> usesByBoundary;

        for (const FString& passName : m_ExecutionOrder)
        {
            if (!IsPassReachable(passName.ToView()))
                continue;
            for (const RGOutOfBandDeclaration& declaration : GetPassOutOfBandDeclarations(passName.ToView()))
            {
                const RGOutOfBandBoundary* boundary = FindOutOfBandBoundary(declaration.Boundary.ToView());
                if (!boundary)
                {
                    hazards.Add(MakeHazard(HazardKind::UnregisteredOutOfBandBoundary, declaration.Boundary.ToView(), passName.ToView(),
                                           passName.ToView(),
                                           "pass '" + passName.ToStdString() + "' declares unregistered out-of-band boundary '" +
                                               declaration.Boundary.ToStdString() + "'"));
                    continue;
                }
                if (boundary->Kind != declaration.Kind)
                {
                    hazards.Add(MakeHazard(HazardKind::UnregisteredOutOfBandBoundary, declaration.Boundary.ToView(), passName.ToView(),
                                           passName.ToView(),
                                           "pass '" + passName.ToStdString() + "' declares '" + declaration.Boundary.ToStdString() +
                                               "' as a " + std::string(ToString(declaration.Kind)) + " but it is registered as a " +
                                               std::string(ToString(boundary->Kind))));
                }
                usesByBoundary[declaration.Boundary.ToStdString()].push_back(Use{ passName.ToStdString(), declaration.Access });
            }
        }

        for (const auto& [boundary, uses] : usesByBoundary)
        {
            // `uses` is in execution order.
            for (sizet i = 0; i < uses.size(); ++i)
            {
                for (sizet j = i + 1; j < uses.size(); ++j)
                {
                    const Use& earlier = uses[i];
                    const Use& later = uses[j];
                    if (earlier.Pass == later.Pass)
                        continue;

                    const bool earlierWrites = earlier.Access == RGOutOfBandAccess::Write;
                    const bool laterWrites = later.Access == RGOutOfBandAccess::Write;

                    // A previous-frame read after this frame's writer reads
                    // the rebuilt value, not the retained one.
                    if (earlierWrites && later.Access == RGOutOfBandAccess::ReadPreviousFrame)
                    {
                        hazards.Add(MakeHazard(HazardKind::OutOfBandOrdering, boundary, earlier.Pass, later.Pass,
                                               "'" + later.Pass + "' reads the previous frame's '" + boundary + "' after '" +
                                                   earlier.Pass + "' rebuilt it this frame"));
                        continue;
                    }
                    // A current-frame read before a writer misses that write.
                    if (earlier.Access == RGOutOfBandAccess::Read && laterWrites)
                    {
                        const bool alsoWrites = std::ranges::any_of(uses, [&](const Use& use)
                                                                    { return use.Pass == earlier.Pass && use.Access == RGOutOfBandAccess::Write; });
                        if (!alsoWrites)
                        {
                            hazards.Add(MakeHazard(HazardKind::OutOfBandOrdering, boundary, later.Pass, earlier.Pass,
                                                   "'" + earlier.Pass + "' reads this frame's '" + boundary +
                                                       "' before its writer '" + later.Pass + "' runs"));
                        }
                        continue;
                    }
                    // Any other pair that involves a write must be ordered by
                    // an edge, not by where the scheduler happened to put it.
                    if ((earlierWrites || laterWrites) && !DependsOnTransitively(later.Pass, earlier.Pass))
                    {
                        hazards.Add(MakeHazard(HazardKind::OutOfBandOrdering, boundary, earlier.Pass, later.Pass,
                                               "'" + later.Pass + "' and '" + earlier.Pass + "' both touch '" + boundary +
                                                   "' (one writes) with no edge between them"));
                    }
                }
            }
        }

        for (const Hazard& hazard : hazards)
        {
            if (hazard.Kind == HazardKind::OutOfBandOrdering || hazard.Kind == HazardKind::UnregisteredOutOfBandBoundary)
                OLO_CORE_ERROR("RenderGraph hazard: {}", hazard.Message.ToView());
        }
    }

    // ------------------------------------------------------------------------
    // Runtime ledger
    // ------------------------------------------------------------------------
    TArray64<RenderGraph::Hazard> RenderGraph::ValidateOutOfBandLedger()
    {
        TArray64<Hazard> hazards;
        const TArray64<RGOutOfBandLedger::Entry> entries = m_OutOfBandLedger.GetEntries();

        for (sizet index = 0; index < static_cast<sizet>(entries.Num()); ++index)
        {
            const RGOutOfBandLedger::Entry& entry = entries[index];
            const std::string boundaryName = entry.Boundary.ToStdString();
            const std::string pass = entry.Pass.ToStdString();
            const RGOutOfBandBoundary* boundary = FindOutOfBandBoundary(boundaryName);
            if (!boundary)
            {
                hazards.Add(MakeHazard(HazardKind::UnregisteredOutOfBandBoundary, boundaryName, pass, pass,
                                       "an access to unregistered out-of-band boundary '" + boundaryName + "' in the " +
                                           std::string(ToString(entry.Phase))));
                continue;
            }

            if (entry.Phase == RGLedgerPhase::Prologue || entry.Phase == RGLedgerPhase::Epilogue)
            {
                const RGFramePhaseUse allowed = entry.Phase == RGLedgerPhase::Prologue ? boundary->Prologue : boundary->Epilogue;
                if (!FramePhaseUseAllows(allowed, entry.Access))
                {
                    hazards.Add(MakeHazard(HazardKind::UndeclaredOutOfBandAccess, boundaryName, std::string(ToString(entry.Phase)), pass,
                                           "the frame " + std::string(ToString(entry.Phase)) + " performs an undeclared " +
                                               std::string(ToString(entry.Access)) + " of '" + boundaryName +
                                               "' (the boundary allows " + std::string(ToString(allowed)) + ")"));
                }
                continue;
            }

            // Graph phase.
            if (pass.empty())
            {
                hazards.Add(MakeHazard(HazardKind::UndeclaredOutOfBandAccess, boundaryName, "", "",
                                       "a " + std::string(ToString(entry.Access)) + " of '" + boundaryName +
                                           "' inside RenderGraph::Execute outside any pass"));
                continue;
            }
            if (!DeclarationCovers(GetPassOutOfBandDeclarations(pass), boundaryName, entry.Access))
            {
                hazards.Add(MakeHazard(HazardKind::UndeclaredOutOfBandAccess, boundaryName, pass, pass,
                                       "pass '" + pass + "' performs an undeclared " + std::string(ToString(entry.Access)) +
                                           " of out-of-band boundary '" + boundaryName + "'"));
            }

            // What actually happened, in call order, independent of what was
            // declared: a read that ran before, or a previous-frame read that
            // ran after, another pass's write in the same frame.
            if (entry.Access == RGOutOfBandAccess::Write)
                continue;
            for (sizet later = index + 1; later < static_cast<sizet>(entries.Num()) && entry.Access == RGOutOfBandAccess::Read; ++later)
            {
                const auto& other = entries[later];
                if (other.Phase == RGLedgerPhase::Graph && other.Boundary == entry.Boundary &&
                    other.Access == RGOutOfBandAccess::Write && other.Pass != entry.Pass)
                {
                    hazards.Add(MakeHazard(HazardKind::OutOfBandOrdering, boundaryName, other.Pass.ToView(), pass,
                                           "pass '" + pass + "' read this frame's '" + boundaryName + "' before '" +
                                               other.Pass.ToStdString() + "' wrote it"));
                    break;
                }
            }
            for (sizet earlier = 0; earlier < index && entry.Access == RGOutOfBandAccess::ReadPreviousFrame; ++earlier)
            {
                const auto& other = entries[earlier];
                if (other.Phase == RGLedgerPhase::Graph && other.Boundary == entry.Boundary &&
                    other.Access == RGOutOfBandAccess::Write && other.Pass != entry.Pass)
                {
                    hazards.Add(MakeHazard(HazardKind::OutOfBandOrdering, boundaryName, other.Pass.ToView(), pass,
                                           "pass '" + pass + "' read the previous frame's '" + boundaryName + "' after '" +
                                               other.Pass.ToStdString() + "' rebuilt it this frame"));
                    break;
                }
            }
        }

        // Log once per distinct result so a persistent fault does not flood.
        std::string digest;
        for (const Hazard& hazard : hazards)
            digest.append(hazard.Message.ToView()).push_back(';');
        if (digest != m_LastLoggedOutOfBandLedgerDigest)
        {
            for (const Hazard& hazard : hazards)
                OLO_CORE_ERROR("RenderGraph out-of-band hazard: {}", hazard.Message.ToView());
            m_LastLoggedOutOfBandLedgerDigest = std::move(digest);
        }
        m_LastOutOfBandLedgerHazards = hazards;
        return hazards;
    }

    // ------------------------------------------------------------------------
    // Exposed schedule
    // ------------------------------------------------------------------------
    std::string RenderGraph::ExportOutOfBandScheduleJson() const
    {
        using Json = nlohmann::ordered_json;
        Json root = Json::object();
        root["schema"] = "olo-render-graph-out-of-band-schedule";
        root["version"] = 1;
        root["finalPass"] = m_FinalPassName.ToStdString();

        const auto sideEffectNames = [](const RenderGraphNode::SideEffect effects)
        {
            Json names = Json::array();
            const auto bits = std::to_underlying(effects);
            const auto has = [bits](RenderGraphNode::SideEffect effect)
            { return (bits & std::to_underlying(effect)) != 0u; };
            if (has(RenderGraphNode::SideEffect::Readback))
                names.push_back("readback");
            if (has(RenderGraphNode::SideEffect::Present))
                names.push_back("present");
            if (has(RenderGraphNode::SideEffect::DebugCapture))
                names.push_back("debug-capture");
            if (has(RenderGraphNode::SideEffect::Timestamp))
                names.push_back("timestamp");
            if (has(RenderGraphNode::SideEffect::NeverCull))
                names.push_back("never-cull");
            return names;
        };

        // Passes: execution order first, then anything culled or unscheduled
        // in registration order, so every registered pass appears once.
        Json passes = Json::array();
        RGTransparentStringSet listed;
        const auto appendPass = [&](const FString& passName, const i64 position)
        {
            if (!listed.insert(passName.ToStdString()).second)
                return;
            Json pass = Json::object();
            pass["name"] = passName.ToStdString();
            pass["position"] = position;
            pass["culled"] = !IsPassReachable(passName.ToView());
            const auto nodeIt = m_NodeLookup.find(passName);
            const RenderGraphNode* node = nodeIt != m_NodeLookup.end() ? nodeIt->second.Raw() : nullptr;
            Json effects = node ? sideEffectNames(node->GetSideEffects()) : Json::array();
            pass["sideEffects"] = effects;
            if (!effects.empty())
            {
                const std::string_view reason = FindSideEffectReason(passName.ToView());
                pass["sideEffectReason"] = reason.empty() ? std::string("UNDOCUMENTED") : std::string(reason);
            }
            Json declarations = Json::array();
            for (const RGOutOfBandDeclaration& declaration : GetPassOutOfBandDeclarations(passName.ToView()))
            {
                declarations.push_back({ { "boundary", declaration.Boundary.ToStdString() },
                                         { "kind", std::string(ToString(declaration.Kind)) },
                                         { "access", std::string(ToString(declaration.Access)) } });
            }
            pass["outOfBand"] = std::move(declarations);
            Json dependsOn = Json::array();
            if (const auto depIt = m_Dependencies.find(passName); depIt != m_Dependencies.end())
            {
                for (const FString& dependency : depIt->second)
                {
                    const bool orderingOnly = m_OrderingOnlyEdges.contains({ dependency.ToStdString(), passName.ToStdString() });
                    dependsOn.push_back({ { "pass", dependency.ToStdString() }, { "orderingOnly", orderingOnly } });
                }
            }
            pass["dependsOn"] = std::move(dependsOn);
            passes.push_back(std::move(pass));
        };
        for (i64 i = 0; i < m_ExecutionOrder.Num(); ++i)
            appendPass(m_ExecutionOrder[i], i);
        for (const FString& passName : m_InsertionOrder)
            appendPass(passName, -1);
        root["passes"] = std::move(passes);

        // Boundaries with their in-graph producers and consumers.
        Json boundaries = Json::array();
        for (const RGOutOfBandBoundary& boundary : GetOutOfBandBoundaries())
        {
            Json producers = Json::array();
            Json consumers = Json::array();
            Json previousFrameConsumers = Json::array();
            for (const FString& passName : m_ExecutionOrder)
            {
                for (const RGOutOfBandDeclaration& declaration : GetPassOutOfBandDeclarations(passName.ToView()))
                {
                    if (declaration.Boundary.ToView() != boundary.Name)
                        continue;
                    const std::string name = passName.ToStdString();
                    switch (declaration.Access)
                    {
                        case RGOutOfBandAccess::Write:
                            producers.push_back(name);
                            break;
                        case RGOutOfBandAccess::Read:
                            consumers.push_back(name);
                            break;
                        case RGOutOfBandAccess::ReadPreviousFrame:
                            previousFrameConsumers.push_back(name);
                            break;
                    }
                }
            }
            boundaries.push_back({ { "name", std::string(boundary.Name) },
                                   { "kind", std::string(ToString(boundary.Kind)) },
                                   { "producers", std::move(producers) },
                                   { "consumers", std::move(consumers) },
                                   { "previousFrameConsumers", std::move(previousFrameConsumers) },
                                   { "prologue", std::string(ToString(boundary.Prologue)) },
                                   { "epilogue", std::string(ToString(boundary.Epilogue)) },
                                   { "owner", std::string(boundary.Owner) },
                                   { "reason", std::string(boundary.Reason) } });
        }
        root["boundaries"] = std::move(boundaries);

        Json epilogueReads = Json::array();
        for (const FrameEpilogueRead& read : m_FrameEpilogueReads)
            epilogueReads.push_back({ { "resource", read.Resource.ToStdString() }, { "consumer", read.Consumer.ToStdString() } });
        root["frameEpilogueReads"] = std::move(epilogueReads);

        Json phaseWork = Json::array();
        for (const RGFramePhaseWork& work : GetFramePhaseWork())
        {
            phaseWork.push_back({ { "name", std::string(work.Name) },
                                  { "phase", std::string(ToString(work.Phase)) },
                                  { "owner", std::string(work.Owner) },
                                  { "consumers", std::string(work.Consumers) },
                                  { "reason", std::string(work.Reason) } });
        }
        root["framePhaseWork"] = std::move(phaseWork);

        Json ledger = Json::array();
        for (const RGOutOfBandLedger::Entry& entry : m_OutOfBandLedger.GetEntries())
        {
            ledger.push_back({ { "boundary", entry.Boundary.ToStdString() },
                               { "pass", entry.Pass.ToStdString() },
                               { "phase", std::string(ToString(entry.Phase)) },
                               { "access", std::string(ToString(entry.Access)) } });
        }
        root["ledger"] = std::move(ledger);
        Json ledgerHazards = Json::array();
        for (const Hazard& hazard : m_LastOutOfBandLedgerHazards)
            ledgerHazards.push_back(hazard.Message.ToStdString());
        root["ledgerHazards"] = std::move(ledgerHazards);

        return root.dump(2);
    }
} // namespace OloEngine
