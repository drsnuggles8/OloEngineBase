#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphHazardValidator.h"

#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace OloEngine::RenderGraphHazardValidator
{
    namespace
    {
        [[nodiscard]] auto RangesOverlap(const RGSubresourceRange& lhs, const RGSubresourceRange& rhs) -> bool
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
        }
    } // namespace

    auto Validate(const ValidatorInput& input) -> std::vector<RenderGraph::Hazard>
    {
        OLO_PROFILE_FUNCTION();

        using Hazard = RenderGraph::Hazard;
        using HazardKind = RenderGraph::HazardKind;

        const auto shouldInspectPass = [&input](const std::string& passName)
        {
            return input.IsPassReachable(passName);
        };

        std::vector<Hazard> hazards;
        hazards.reserve(input.RegistryDiagnostics.size());

        // 1. Forward filtered registry-stage diagnostics (e.g. kind mismatches)
        //    for reachable passes only.
        for (const auto& diagnostic : input.RegistryDiagnostics)
        {
            const bool producerRelevant = diagnostic.Producer.empty() || shouldInspectPass(diagnostic.Producer);
            const bool consumerRelevant = diagnostic.Consumer.empty() || shouldInspectPass(diagnostic.Consumer);
            if (!producerRelevant || !consumerRelevant)
                continue;
            hazards.push_back(diagnostic);
        }

        // 2. Build transitive dependency closure: for each pass P, closure[P]
        //    is the set of all passes that must execute before P.
        std::unordered_map<std::string, std::unordered_set<std::string>> closure;
        closure.reserve(input.ExecutionOrder.size());
        for (const auto& passName : input.ExecutionOrder)
        {
            std::unordered_set<std::string>& cls = closure[passName];
            std::vector<std::string> frontier;
            auto depsIt = input.Dependencies.find(passName);
            if (depsIt != input.Dependencies.end())
            {
                frontier.insert(frontier.end(), depsIt->second.begin(), depsIt->second.end());
            }
            while (!frontier.empty())
            {
                const std::string parent = std::move(frontier.back());
                frontier.pop_back();
                if (!cls.insert(parent).second)
                    continue;
                auto parentDeps = input.Dependencies.find(parent);
                if (parentDeps == input.Dependencies.end())
                    continue;
                for (const auto& grand : parentDeps->second)
                {
                    if (!cls.contains(grand))
                        frontier.push_back(grand);
                }
            }
        }

        const auto dependsOn = [&closure](const std::string& later, const std::string& earlier) -> bool
        {
            auto it = closure.find(later);
            if (it == closure.end())
                return false;
            return it->second.contains(earlier);
        };

        // 3. Same-pass feedback validation. A pass that reads and writes
        //    overlapping subresources of the same resource must declare it
        //    via builder.AllowSamePassReadWrite(); otherwise emit a Feedback
        //    hazard. The declaration is only correct for genuine intra-pass
        //    ping-pong / iteration patterns; inter-pass RMW must rename via
        //    WriteNewVersion instead.
        const auto feedbackCoversOverlap = [&input](const std::string& passName,
                                                    const RGAccessDeclaration& readAccess,
                                                    const RGAccessDeclaration& writeAccess)
        {
            if (const auto feedbackIt = input.PassFeedbackDeclarations.find(passName);
                feedbackIt != input.PassFeedbackDeclarations.end())
            {
                for (const auto& feedback : feedbackIt->second)
                {
                    if (feedback.ResourceName != readAccess.ResourceName)
                        continue;
                    if (!RangesOverlap(feedback.Range, readAccess.Range))
                        continue;
                    if (!RangesOverlap(feedback.Range, writeAccess.Range))
                        continue;
                    return true;
                }
            }
            return false;
        };

        const auto validateFeedbackHazards = [&hazards, &feedbackCoversOverlap, &shouldInspectPass](const std::string& passName,
                                                                                                    const std::vector<RGAccessDeclaration>& accesses)
        {
            if (!shouldInspectPass(passName))
                return;

            const auto accessCount = accesses.size();
            for (sizet readIdx = 0; readIdx < accessCount; ++readIdx)
            {
                const auto& readAccess = accesses[readIdx];
                if (readAccess.IsWrite)
                    continue;

                for (sizet writeIdx = 0; writeIdx < accessCount; ++writeIdx)
                {
                    const auto& writeAccess = accesses[writeIdx];
                    if (!writeAccess.IsWrite)
                        continue;
                    if (readAccess.ResourceName != writeAccess.ResourceName)
                        continue;
                    if (!RangesOverlap(readAccess.Range, writeAccess.Range))
                        continue;
                    if (feedbackCoversOverlap(passName, readAccess, writeAccess))
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
        };

        for (const auto& passName : input.ExecutionOrder)
        {
            if (const auto accessIt = input.PassAccessDeclarations.find(passName);
                accessIt != input.PassAccessDeclarations.end())
            {
                validateFeedbackHazards(passName, accessIt->second);
            }
        }

        // 4. Imported-resource lifetime misuse. If an imported resource is
        //    produced and consumed in-graph (by reachable passes) it must
        //    have a valid backing object.
        const auto findRelevantPass = [&shouldInspectPass](const std::vector<std::string>& passNames) -> std::string
        {
            for (const auto& passName : passNames)
            {
                if (shouldInspectPass(passName))
                    return passName;
            }
            return {};
        };

        for (const auto& resource : input.RegisteredResources)
        {
            if (!resource.Desc.Imported)
                continue;

            const auto relevantProducer = findRelevantPass(resource.Producers);
            const auto relevantConsumer = findRelevantPass(resource.Consumers);
            if (relevantProducer.empty() || relevantConsumer.empty())
                continue;

            bool hasValidBacking = true;
            if (resource.TextureHandle.IsValid())
                hasValidBacking = input.ResolveTexture(resource.TextureHandle) != 0;
            else if (resource.BufferHandle.IsValid())
                hasValidBacking = input.ResolveBuffer(resource.BufferHandle) != 0;
            else if (resource.FramebufferHandle.IsValid())
                hasValidBacking = input.ResolveFramebuffer(resource.FramebufferHandle) != nullptr;

            if (hasValidBacking)
                continue;

            Hazard h;
            h.Kind = HazardKind::ImportedResourceLifetimeMisuse;
            h.Resource = resource.Name;
            h.Producer = relevantProducer;
            h.Consumer = relevantConsumer;
            h.Message = "Imported resource lifetime misuse: resource '" + resource.Name +
                        "' is produced and consumed in-graph but has no valid backing object";
            OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message);
            hazards.push_back(std::move(h));
        }

        // 5. Cross-pass RAW / WAW / WAR validation. Walk execution order and
        //    track last-writer + live-reader state per resource; emit hazards
        //    whenever a later pass doesn't transitively depend on the prior
        //    writer / reader.
        struct ResourceState
        {
            std::string LastWriter;
            std::unordered_set<std::string> LiveReaders;
        };
        std::unordered_map<std::string, ResourceState> state;

        const auto appendUniqueName = [](std::vector<std::string>& names, std::string_view resourceName)
        {
            if (resourceName.empty())
                return;
            if (std::find(names.begin(), names.end(), resourceName) == names.end())
                names.emplace_back(resourceName);
        };

        for (const auto& passName : input.ExecutionOrder)
        {
            if (!shouldInspectPass(passName))
                continue;

            std::vector<std::string> readNames;
            std::vector<std::string> writeNames;

            if (const auto accessIt = input.PassAccessDeclarations.find(passName);
                accessIt != input.PassAccessDeclarations.end())
            {
                for (const auto& access : accessIt->second)
                {
                    if (access.IsWrite)
                        appendUniqueName(writeNames, access.ResourceName);
                    else
                        appendUniqueName(readNames, access.ResourceName);
                }
            }

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
} // namespace OloEngine::RenderGraphHazardValidator
