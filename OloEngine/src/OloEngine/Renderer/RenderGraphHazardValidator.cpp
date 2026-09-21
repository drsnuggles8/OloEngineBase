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

    auto Validate(const ValidatorInput& input) -> TArray64<RenderGraph::Hazard>
    {
        OLO_PROFILE_FUNCTION();

        using Hazard = RenderGraph::Hazard;
        using HazardKind = RenderGraph::HazardKind;

        const auto shouldInspectPass = [&input](std::string_view passName)
        {
            return input.IsPassReachable(passName);
        };

        TArray64<Hazard> hazards;
        hazards.Reserve(input.RegistryDiagnostics.size());

        // 1. Forward filtered registry-stage diagnostics (e.g. kind mismatches)
        //    for reachable passes only.
        for (const auto& diagnostic : input.RegistryDiagnostics)
        {
            const bool producerRelevant = diagnostic.Producer.IsEmpty() || shouldInspectPass(diagnostic.Producer.ToStdString());
            const bool consumerRelevant = diagnostic.Consumer.IsEmpty() || shouldInspectPass(diagnostic.Consumer.ToStdString());
            if (!producerRelevant || !consumerRelevant)
                continue;
            hazards.Add(diagnostic);
        }

        // 2. Build transitive dependency closure: for each pass P, closure[P]
        //    is the set of all passes that must execute before P.
        RGTransparentStringMap<RGTransparentStringSet> closure;
        closure.reserve(input.ExecutionOrder.size());
        for (const auto& passName : input.ExecutionOrder)
        {
            RGTransparentStringSet& cls = closure[passName.ToStdString()];
            std::vector<std::string> frontier;
            if (auto depsIt = input.Dependencies.find(passName.ToView()); depsIt != input.Dependencies.end())
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
                    if (!cls.contains(grand.ToView()))
                        frontier.push_back(std::string(grand));
                }
            }
        }

        const auto dependsOn = [&closure](std::string_view later, std::string_view earlier) -> bool
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
        const auto feedbackCoversOverlap = [&input](std::string_view passName,
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

        const auto validateFeedbackHazards = [&hazards, &feedbackCoversOverlap, &shouldInspectPass](std::string_view passName,
                                                                                                    const TArray64<RGAccessDeclaration>& accesses)
        {
            if (!shouldInspectPass(passName))
                return;

            const auto accessCount = accesses.Num();
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
                    h.Message = "Feedback hazard: pass '" + std::string(passName) +
                                "' reads and writes overlapping subresources of resource '" +
                                readAccess.ResourceName + "' without an explicit feedback declaration";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
                    hazards.Add(std::move(h));
                    break;
                }
            }
        };

        for (const auto& passName : input.ExecutionOrder)
        {
            if (const auto accessIt = input.PassAccessDeclarations.find(passName.ToView());
                accessIt != input.PassAccessDeclarations.end())
            {
                validateFeedbackHazards(passName.ToView(), accessIt->second);
            }
        }

        // 4. Imported-resource lifetime misuse. If an imported resource is
        //    produced and consumed in-graph (by reachable passes) it must
        //    have a valid backing object.
        const auto findRelevantPass = [&shouldInspectPass](const TArray64<FString>& passNames) -> std::string
        {
            for (const auto& passName : passNames)
            {
                if (shouldInspectPass(passName.ToStdString()))
                    return passName.ToStdString();
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
                hasValidBacking = input.HasBufferBacking(resource.BufferHandle);
            else if (resource.FramebufferHandle.IsValid())
                hasValidBacking = input.ResolveFramebuffer(resource.FramebufferHandle) != nullptr;
            else
            {
                // No additional handling required.
            }

            if (hasValidBacking)
                continue;

            Hazard h;
            h.Kind = HazardKind::ImportedResourceLifetimeMisuse;
            h.Resource = resource.Name;
            h.Producer = relevantProducer;
            h.Consumer = relevantConsumer;
            h.Message = "Imported resource lifetime misuse: resource '" + resource.Name.ToStdString() +
                        "' is produced and consumed in-graph but has no valid backing object";
            OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
            hazards.Add(std::move(h));
        }

        // 5. Cross-pass RAW / WAW / WAR validation. Walk execution order and
        //    track last-writer + live-reader state per resource; emit hazards
        //    whenever a later pass doesn't transitively depend on the prior
        //    writer / reader.
        struct ResourceState
        {
            std::string LastWriter;
            RGTransparentStringSet LiveReaders;
        };
        RGTransparentStringMap<ResourceState> state;

        const auto appendUniqueName = [](std::vector<std::string>& names, std::string_view resourceName)
        {
            if (resourceName.empty())
                return;
            if (std::ranges::find(names, resourceName) == names.end())
                names.emplace_back(resourceName);
        };

        for (const auto& passName : input.ExecutionOrder)
        {
            if (!shouldInspectPass(passName.ToView()))
                continue;

            std::vector<std::string> readNames;
            std::vector<std::string> writeNames;

            if (const auto accessIt = input.PassAccessDeclarations.find(passName.ToView());
                accessIt != input.PassAccessDeclarations.end())
            {
                for (const auto& access : accessIt->second)
                {
                    if (access.IsWrite)
                        appendUniqueName(writeNames, access.ResourceName.ToView());
                    else
                        appendUniqueName(readNames, access.ResourceName.ToView());
                }
            }

            for (const auto& rName : readNames)
            {
                ResourceState& st = state[rName];
                if (!st.LastWriter.empty() && st.LastWriter != passName.ToView() && !dependsOn(passName.ToView(), st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::ReadAfterWrite;
                    h.Resource = rName;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "RAW: pass '" + passName + "' reads resource '" + rName +
                                "' written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
                    hazards.Add(std::move(h));
                }
                st.LiveReaders.insert(passName.ToStdString());
            }

            for (const auto& wName : writeNames)
            {
                ResourceState& st = state[wName];

                if (!st.LastWriter.empty() && st.LastWriter != passName.ToView() && !dependsOn(passName.ToView(), st.LastWriter))
                {
                    Hazard h;
                    h.Kind = HazardKind::WriteAfterWrite;
                    h.Resource = wName;
                    h.Producer = st.LastWriter;
                    h.Consumer = passName;
                    h.Message = "WAW: pass '" + passName + "' writes resource '" + wName +
                                "' previously written by '" + st.LastWriter +
                                "' without declaring a dependency";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
                    hazards.Add(std::move(h));
                }

                for (const auto& reader : st.LiveReaders)
                {
                    if (reader == passName)
                        continue;
                    if (!dependsOn(passName.ToView(), reader))
                    {
                        Hazard h;
                        h.Kind = HazardKind::WriteAfterRead;
                        h.Resource = wName;
                        h.Producer = reader;
                        h.Consumer = passName;
                        h.Message = "WAR: pass '" + passName + "' overwrites resource '" + wName +
                                    "' still live for reader '" + reader +
                                    "' without declaring a dependency";
                        OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
                        hazards.Add(std::move(h));
                    }
                }

                st.LastWriter = std::string(passName);
                st.LiveReaders.clear();
            }
        }

        return hazards;
    }
} // namespace OloEngine::RenderGraphHazardValidator
