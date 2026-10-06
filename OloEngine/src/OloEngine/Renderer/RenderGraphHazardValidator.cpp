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
                if (lhsCount == 0u || rhsCount == 0u)
                    return false;
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

        [[nodiscard]] bool AttachmentsOverlap(const PhysicalAccess& lhs, const PhysicalAccess& rhs)
        {
            if (lhs.Resource != rhs.Resource)
                return false;
            if (lhs.AttachmentAspect == PhysicalAccess::Aspect::WholeResource ||
                rhs.AttachmentAspect == PhysicalAccess::Aspect::WholeResource)
                return true;
            return lhs.AttachmentAspect == rhs.AttachmentAspect &&
                   (lhs.AttachmentAspect != PhysicalAccess::Aspect::Color || lhs.AttachmentIndex == rhs.AttachmentIndex);
        }

        [[nodiscard]] bool CoversOverlap(const RGSubresourceRange& declaration,
                                         const RGSubresourceRange& lhs, const RGSubresourceRange& rhs)
        {
            const auto covers = [](u32 base, u32 count, u32 lhsBase, u32 lhsCount, u32 rhsBase, u32 rhsCount)
            {
                const auto end = [](u32 start, u32 size)
                { return size == ~0u ? std::numeric_limits<u64>::max() : static_cast<u64>(start) + size; };
                return base <= std::max(lhsBase, rhsBase) &&
                       end(base, count) >= std::min(end(lhsBase, lhsCount), end(rhsBase, rhsCount));
            };
            return covers(declaration.BaseMip, declaration.MipCount, lhs.BaseMip, lhs.MipCount, rhs.BaseMip, rhs.MipCount) &&
                   covers(declaration.BaseLayer, declaration.LayerCount, lhs.BaseLayer, lhs.LayerCount, rhs.BaseLayer, rhs.LayerCount) &&
                   covers(declaration.BaseSlice, declaration.SliceCount, lhs.BaseSlice, lhs.SliceCount, rhs.BaseSlice, rhs.SliceCount);
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
                for (const auto& dep : depsIt->second)
                    frontier.emplace_back(dep.ToView());
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
        //    ping-pong / iteration patterns. Inter-pass publication uses
        //    WriteNewVersion, which retains storage and cannot legalize sampling
        //    an attachment that the pass also writes.
        const auto resolvePhysical = [&input](const FString& name, const RGSubresourceRange& range)
        {
            return input.ResolvePhysicalAccess ? input.ResolvePhysicalAccess(name.ToView(), range)
                                               : PhysicalAccess{ .Resource = name, .Range = range };
        };
        const auto feedbackCoversOverlap = [&input, &resolvePhysical](std::string_view passName,
                                                                      const PhysicalAccess& readAccess,
                                                                      const PhysicalAccess& writeAccess, const RGReadUsage readUsage)
        {
            if (const auto feedbackIt = input.PassFeedbackDeclarations.find(passName);
                feedbackIt != input.PassFeedbackDeclarations.end())
            {
                for (const auto& feedback : feedbackIt->second)
                {
                    if (feedback.ReadUsage && *feedback.ReadUsage != readUsage)
                        continue;
                    const auto physical = resolvePhysical(feedback.ResourceName, feedback.Range);
                    if (!AttachmentsOverlap(physical, readAccess) || !AttachmentsOverlap(physical, writeAccess))
                        continue;
                    // A declaration for colour 0 cannot authorize a whole-FBO
                    // overlap that also includes colour 1 or depth.
                    if (physical.AttachmentAspect != PhysicalAccess::Aspect::WholeResource &&
                        readAccess.AttachmentAspect == PhysicalAccess::Aspect::WholeResource &&
                        writeAccess.AttachmentAspect == PhysicalAccess::Aspect::WholeResource)
                        continue;
                    if (!CoversOverlap(physical.Range, readAccess.Range, writeAccess.Range))
                        continue;
                    return true;
                }
            }
            return false;
        };

        const auto validateFeedbackHazards = [&hazards, &feedbackCoversOverlap, &shouldInspectPass, &resolvePhysical](std::string_view passName,
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

                // Attachment load/blend/depth-test reads are the normal RMW
                // contract. They are not shader/image sampling feedback.
                const auto physicalRead = resolvePhysical(readAccess.ResourceName, readAccess.Range);

                for (sizet writeIdx = 0; writeIdx < accessCount; ++writeIdx)
                {
                    const auto& writeAccess = accesses[writeIdx];
                    if (!writeAccess.IsWrite)
                        continue;
                    if (readAccess.ReadUsage == RGReadUsage::RenderTargetRead &&
                        (writeAccess.WriteUsage == RGWriteUsage::RenderTarget || writeAccess.WriteUsage == RGWriteUsage::DepthStencil))
                        continue;
                    const auto physicalWrite = resolvePhysical(writeAccess.ResourceName, writeAccess.Range);
                    if (!AttachmentsOverlap(physicalRead, physicalWrite))
                        continue;
                    if (!RangesOverlap(physicalRead.Range, physicalWrite.Range))
                        continue;
                    if (feedbackCoversOverlap(passName, physicalRead, physicalWrite, readAccess.ReadUsage))
                        continue;

                    Hazard h;
                    h.Kind = HazardKind::FeedbackWithoutDeclaration;
                    h.Resource = readAccess.ResourceName;
                    h.Producer = passName;
                    h.Consumer = passName;
                    h.Message = "Feedback hazard: pass '" + std::string(passName) +
                                "' reads and writes overlapping subresources of resource '" +
                                readAccess.ResourceName + "' (physical target '" + physicalRead.Resource +
                                "', write '" + writeAccess.ResourceName + "') without an explicit feedback declaration";
                    OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
                    hazards.Add(std::move(h));
                    break;
                }
            }
        };

        for (const auto& passName : input.ExecutionOrder)
        {
            if (input.PassSetupAccessDeclarations)
            {
                const auto setup = input.PassSetupAccessDeclarations->find(passName.ToView());
                if (setup != input.PassSetupAccessDeclarations->end())
                {
                    validateFeedbackHazards(passName.ToView(), setup->second);
                    continue;
                }
            }
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
