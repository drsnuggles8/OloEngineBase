#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphBarrierPlanner.h"

#include "OloEngine/Debug/Profiler.h"

#include <cstddef>
#include <utility>

namespace OloEngine::RenderGraphBarrierPlanner
{
    namespace
    {
        // Per-subresource writer state: tracks the last pass that wrote each
        // unique subresource of a resource (mip / layer granularity).
        struct LastWriterState
        {
            std::string PassName;
            RGWriteUsage Usage = RGWriteUsage::RenderTarget;
            RGSubresourceRange Range = RGSubresourceRange::Full();
        };

        // Returns true when two 1-D intervals [baseA, baseA+countA) and
        // [baseB, baseB+countB) overlap. ~0u means "unbounded" (all).
        [[nodiscard]] auto RangeOverlaps1D(u32 baseA, u32 countA, u32 baseB, u32 countB) -> bool
        {
            if (countA == ~0u || countB == ~0u)
                return true;
            return baseA < baseB + countB && baseB < baseA + countA;
        }

        [[nodiscard]] auto SubresourceRangesOverlap(const RGSubresourceRange& a, const RGSubresourceRange& b) -> bool
        {
            return RangeOverlaps1D(a.BaseMip, a.MipCount, b.BaseMip, b.MipCount) &&
                   RangeOverlaps1D(a.BaseLayer, a.LayerCount, b.BaseLayer, b.LayerCount);
        }
    } // namespace

    auto ResolveProducerBarrierFlags(const RGWriteUsage usage) -> MemoryBarrierFlags
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

    auto ResolveConsumerBarrierFlags(const RGReadUsage usage) -> MemoryBarrierFlags
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

    auto ComputePlan(const PlanInput& input) -> PlanResult
    {
        OLO_PROFILE_FUNCTION();

        PlanResult result;

        // resource name → per-subresource writer slots (one entry per (pass, range) pair)
        std::unordered_map<std::string, std::vector<LastWriterState>> lastWriterByResource;
        lastWriterByResource.reserve(input.ExecutionOrder.size() * 2u);

        std::unordered_map<std::string, std::unordered_set<std::string>> allWriterPassesByResource;
        allWriterPassesByResource.reserve(input.PassAccessDeclarations.size() * 2u);
        for (const auto& [passName, accessDeclarations] : input.PassAccessDeclarations)
        {
            for (const auto& access : accessDeclarations)
            {
                if (!access.IsWrite || access.ResourceName.empty())
                    continue;
                allWriterPassesByResource[access.ResourceName].insert(passName);
            }
        }

        for (const auto& passName : input.ExecutionOrder)
        {
            if (!input.IsPassReachable(passName))
                continue;

            const auto declarationIt = input.PassAccessDeclarations.find(passName);
            if (declarationIt == input.PassAccessDeclarations.end())
                continue;

            auto plannedFlags = MemoryBarrierFlags::None;
            for (const auto& access : declarationIt->second)
            {
                if (access.ResourceName.empty())
                    continue;

                if (!access.IsWrite)
                {
                    // Find every writer whose subresource range overlaps this read.
                    const auto writerIt = lastWriterByResource.find(access.ResourceName);
                    if (writerIt == lastWriterByResource.end() || writerIt->second.empty())
                    {
                        const auto allWritersIt = allWriterPassesByResource.find(access.ResourceName);
                        if (allWritersIt == allWriterPassesByResource.end() || allWritersIt->second.empty())
                        {
                            result.Diagnostics.push_back(RenderGraph::BarrierDiagnostic{
                                .Kind = RenderGraph::BarrierDiagnosticKind::MissingProducer,
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
                                if (input.IsPassReachable(writerPassName))
                                {
                                    hasReachableWriter = true;
                                    break;
                                }
                            }

                            if (!hasReachableWriter)
                            {
                                result.Diagnostics.push_back(RenderGraph::BarrierDiagnostic{
                                    .Kind = RenderGraph::BarrierDiagnosticKind::CulledProducer,
                                    .PassName = passName,
                                    .Resource = access.ResourceName,
                                    .Message = "Read resource '" + access.ResourceName + "' in pass '" + passName + "' only has unreachable/culled producers",
                                });
                            }
                        }
                        continue;
                    }

                    // Emit one barrier per overlapping writer — different mip writes
                    // can have different producer flags and must be tracked separately.
                    for (const auto& writer : writerIt->second)
                    {
                        if (writer.PassName == passName)
                            continue;
                        if (!SubresourceRangesOverlap(writer.Range, access.Range))
                            continue;

                        const auto flags = ResolveProducerBarrierFlags(writer.Usage) |
                                           ResolveConsumerBarrierFlags(access.ReadUsage);
                        if (flags == MemoryBarrierFlags::None)
                        {
                            result.Diagnostics.push_back(RenderGraph::BarrierDiagnostic{
                                .Kind = RenderGraph::BarrierDiagnosticKind::UnmappedTransition,
                                .PassName = passName,
                                .Resource = access.ResourceName,
                                .Message = "No barrier mapping for transition to pass '" + passName + "' on resource '" + access.ResourceName + "'",
                            });
                            continue;
                        }

                        plannedFlags |= flags;
                        result.PlannedBarriers.push_back(RenderGraph::PlannedBarrier{
                            .BeforePass = passName,
                            .Resource = access.ResourceName,
                            .Flags = flags,
                            .Range = access.Range,
                        });
                    }
                }
                else
                {
                    // WAW: emit a barrier for every prior writer whose range overlaps.
                    auto& writerVec = lastWriterByResource[access.ResourceName];
                    for (const auto& writer : writerVec)
                    {
                        if (writer.PassName == passName)
                            continue;
                        if (!SubresourceRangesOverlap(writer.Range, access.Range))
                            continue;

                        const auto flags = ResolveProducerBarrierFlags(writer.Usage) |
                                           ResolveProducerBarrierFlags(access.WriteUsage);
                        if (flags == MemoryBarrierFlags::None)
                        {
                            result.Diagnostics.push_back(RenderGraph::BarrierDiagnostic{
                                .Kind = RenderGraph::BarrierDiagnosticKind::UnmappedTransition,
                                .PassName = passName,
                                .Resource = access.ResourceName,
                                .Message = "No barrier mapping for write-after-write transition in pass '" + passName + "' on resource '" + access.ResourceName + "'",
                            });
                        }
                        else
                        {
                            plannedFlags |= flags;
                            result.PlannedBarriers.push_back(RenderGraph::PlannedBarrier{
                                .BeforePass = passName,
                                .Resource = access.ResourceName,
                                .Flags = flags,
                                .Range = access.Range,
                            });
                        }
                    }

                    // Upsert the writer slot for this pass + range.
                    // If the same pass already owns an overlapping slot, update it
                    // (avoids growing the vector unboundedly for multi-write passes).
                    bool slotUpdated = false;
                    for (auto& writer : writerVec)
                    {
                        if (writer.PassName == passName &&
                            SubresourceRangesOverlap(writer.Range, access.Range))
                        {
                            writer.Usage = access.WriteUsage;
                            writer.Range = access.Range;
                            slotUpdated = true;
                            break;
                        }
                    }
                    if (!slotUpdated)
                    {
                        writerVec.push_back(LastWriterState{
                            .PassName = passName,
                            .Usage = access.WriteUsage,
                            .Range = access.Range,
                        });
                    }
                }
            }

            if (plannedFlags != MemoryBarrierFlags::None)
                result.PassBarrierFlags[passName] = plannedFlags;
        }

        return result;
    }

    auto BuildResourceTransitions(const TransitionInput& input) -> std::vector<RenderGraph::ResourceTransition>
    {
        if (input.PlannedBarriers.empty())
            return {};

        // Build a pass → execution-order-index map for the backward producer scan.
        std::unordered_map<std::string, std::size_t> passOrderIdx;
        passOrderIdx.reserve(input.ExecutionOrder.size());
        for (std::size_t i = 0; i < input.ExecutionOrder.size(); ++i)
            passOrderIdx.emplace(input.ExecutionOrder[i], i);

        const auto passToLane = [&](const std::string& passName) -> RenderGraph::QueueLane
        {
            switch (input.GetPassWorkType(passName))
            {
                case RenderGraphPassWorkType::Compute:
                    return RenderGraph::QueueLane::Compute;
                case RenderGraphPassWorkType::Copy:
                    return RenderGraph::QueueLane::Copy;
                case RenderGraphPassWorkType::Graphics:
                default:
                    return RenderGraph::QueueLane::Graphics;
            }
        };

        std::vector<RenderGraph::ResourceTransition> transitions;
        transitions.reserve(input.PlannedBarriers.size());

        for (const auto& barrier : input.PlannedBarriers)
        {
            RenderGraph::ResourceTransition t;
            t.ResourceName = barrier.Resource;
            t.ConsumerPass = barrier.BeforePass;
            t.Flags = barrier.Flags;
            t.Range = barrier.Range;

            // Determine the consumer's read usage from its declared accesses.
            // Default to ShaderSample when no frame-setup declaration is present
            // (for example nodes that expose only static declaration metadata
            // instead of per-frame builder reads).
            t.ToUsage = RGReadUsage::ShaderSample;
            if (const auto dit = input.PassAccessDeclarations.find(barrier.BeforePass);
                dit != input.PassAccessDeclarations.end())
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
            // this transition. When no writer is found the resource is imported
            // externally (initial state) so ProducerPass stays "external".
            t.ProducerPass = "external";
            t.FromUsage = RGWriteUsage::RenderTarget;

            const auto consumerIdxIt = passOrderIdx.find(barrier.BeforePass);
            if (consumerIdxIt != passOrderIdx.end())
            {
                const std::size_t consumerIdx = consumerIdxIt->second;
                for (std::size_t i = 0; i < consumerIdx; ++i)
                {
                    const auto& passName = input.ExecutionOrder[i];
                    if (const auto dit = input.PassAccessDeclarations.find(passName);
                        dit != input.PassAccessDeclarations.end())
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
            const auto producerLane = (tr.ProducerPass == "external")
                                          ? RenderGraph::QueueLane::Graphics
                                          : passToLane(tr.ProducerPass);
            const auto consumerLane = passToLane(tr.ConsumerPass);
            tr.ProducerLane = producerLane;
            tr.ConsumerLane = consumerLane;
            tr.IsCrossLane = (producerLane != consumerLane);
        }

        return transitions;
    }
} // namespace OloEngine::RenderGraphBarrierPlanner
