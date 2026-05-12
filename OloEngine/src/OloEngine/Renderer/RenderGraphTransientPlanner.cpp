#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphTransientPlanner.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Texture.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace OloEngine::RenderGraphTransientPlanner
{
    auto BuildAliasGroup(const RGResourceDesc& desc) -> std::string
    {
        // Base key covers kind, dimensions, mips, samples and queue.
        std::string key = std::to_string(static_cast<u32>(desc.Kind)) + ":" +
                          std::to_string(static_cast<u32>(desc.Format)) + ":" +
                          std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                          "x" + std::to_string(desc.DepthOrLayers) + ":m" + std::to_string(desc.MipLevels) +
                          ":s" + std::to_string(desc.Samples) + ":q" + std::to_string(static_cast<u32>(desc.Queue));
        // MRT: append each attachment format so MRT layouts don't alias with
        // single-attachment FBs or with each other.
        if (!desc.Attachments.empty())
        {
            key += ":mrt";
            for (const auto fmt : desc.Attachments)
                key += "," + std::to_string(static_cast<u32>(fmt));
        }
        return key;
    }

    auto EstimateBytes(const RGResourceDesc& desc) -> u64
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
                case RGResourceFormat::R32Int:
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

    auto IsAllocatable(const RGResourceDesc& desc) -> bool
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
                       RenderGraph::ToImageFormat(desc.Format) != ImageFormat::None;
            case ResourceHandle::Kind::Framebuffer:
                // MRT: at least one valid attachment required; dims must be set.
                if (!desc.Attachments.empty())
                {
                    return desc.Width > 0 &&
                           desc.Height > 0 &&
                           std::any_of(desc.Attachments.begin(), desc.Attachments.end(),
                                       [](const RGResourceFormat fmt)
                                       {
                                           return RenderGraph::ToFramebufferFormat(fmt) != FramebufferTextureFormat::None;
                                       });
                }
                return desc.Width > 0 &&
                       desc.Height > 0 &&
                       desc.Format != RGResourceFormat::Unknown &&
                       RenderGraph::ToFramebufferFormat(desc.Format) != FramebufferTextureFormat::None;
            case ResourceHandle::Kind::StorageBuffer:
            case ResourceHandle::Kind::UniformBuffer:
                return desc.Width > 0;
            case ResourceHandle::Kind::Unknown:
            default:
                return false;
        }
    }

    auto GetSkipReason(const RGResourceDesc& desc) -> std::string_view
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
                if (RenderGraph::ToImageFormat(desc.Format) == ImageFormat::None)
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
                                                          return RenderGraph::ToFramebufferFormat(fmt) != FramebufferTextureFormat::None;
                                                      });
                    if (!anyValid)
                        return "unsupported-framebuffer-format";
                    return "descriptor-incomplete";
                }
                if (desc.Format == RGResourceFormat::Unknown)
                    return "unknown-format";
                if (RenderGraph::ToFramebufferFormat(desc.Format) == FramebufferTextureFormat::None)
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

    auto ComputePlan(const PlanInput& input) -> std::vector<RenderGraph::TransientPlanEntry>
    {
        std::vector<RenderGraph::TransientPlanEntry> plan;
        if (input.TransientResourceDescs.empty())
            return plan;

        // 1. Walk execution order to derive per-resource lifetimes (first /
        //    last access pass + reachability flag).
        struct Lifetime
        {
            bool Reachable = false;
            u32 First = std::numeric_limits<u32>::max();
            u32 Last = 0;
            std::string FirstPass;
            std::string LastPass;
        };

        std::unordered_map<std::string, Lifetime> lifetimes;
        lifetimes.reserve(input.TransientResourceDescs.size());

        for (u32 passIndex = 0; passIndex < static_cast<u32>(input.ExecutionOrder.size()); ++passIndex)
        {
            const auto& passName = input.ExecutionOrder[passIndex];
            const auto accessIt = input.PassAccessDeclarations.find(passName);
            if (accessIt == input.PassAccessDeclarations.end())
                continue;

            for (const auto& access : accessIt->second)
            {
                if (!input.TransientResourceDescs.contains(access.ResourceName))
                    continue;

                auto& lifetime = lifetimes[access.ResourceName];
                if (!input.IsPassReachable(passName))
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

        // 2. Compose one plan entry per transient descriptor; classify into
        //    allocatable vs skip-with-reason.
        plan.reserve(input.TransientResourceDescs.size());
        for (const auto& [resourceName, desc] : input.TransientResourceDescs)
        {
            RenderGraph::TransientPlanEntry entry;
            entry.Resource = resourceName;
            entry.Kind = desc.Kind;
            entry.AliasGroup = BuildAliasGroup(desc);
            entry.EstimatedBytes = EstimateBytes(desc);

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
            else if (input.IsExternallyBackedTransientResource(resourceName))
            {
                entry.SkipReason = "external-backing";
            }
            else if (!IsAllocatable(desc))
            {
                entry.SkipReason = std::string(GetSkipReason(desc));
            }
            else
            {
                entry.WillAllocate = true;
            }

            plan.push_back(std::move(entry));
        }

        // 3. Canonical sort: by alias group, then by first-use pass index,
        //    then by resource name (deterministic across rebuilds).
        std::sort(plan.begin(), plan.end(),
                  [](const RenderGraph::TransientPlanEntry& lhs, const RenderGraph::TransientPlanEntry& rhs)
                  {
                      if (lhs.AliasGroup != rhs.AliasGroup)
                          return lhs.AliasGroup < rhs.AliasGroup;
                      if (lhs.FirstPassIndex != rhs.FirstPassIndex)
                          return lhs.FirstPassIndex < rhs.FirstPassIndex;
                      return lhs.Resource < rhs.Resource;
                  });

        // 4. Alias-slot assignment per alias group: non-overlapping lifetimes
        //    share a slot, otherwise allocate a new one.
        struct ActiveSlot
        {
            u32 Slot = 0;
            u32 LastPassIndex = 0;
        };

        std::unordered_map<std::string, std::vector<ActiveSlot>> activeByGroup;
        activeByGroup.reserve(plan.size());

        std::unordered_map<std::string, u32> nextSlotByGroup;
        nextSlotByGroup.reserve(plan.size());

        for (auto& entry : plan)
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

        return plan;
    }
} // namespace OloEngine::RenderGraphTransientPlanner
