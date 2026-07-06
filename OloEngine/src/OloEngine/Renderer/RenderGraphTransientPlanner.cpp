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
        std::string key = std::to_string(static_cast<u32>(std::to_underlying(desc.Kind))) + ":" +
                          std::to_string(static_cast<u32>(std::to_underlying(desc.Format))) + ":" +
                          std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                          "x" + std::to_string(desc.DepthOrLayers) + ":m" + std::to_string(desc.MipLevels) +
                          ":s" + std::to_string(desc.Samples) + ":q" + std::to_string(static_cast<u32>(std::to_underlying(desc.Queue)));
        // MRT: append each attachment format so MRT layouts don't alias with
        // single-attachment FBs or with each other.
        if (!desc.Attachments.empty())
        {
            key += ":mrt";
            for (const auto fmt : desc.Attachments)
                key += "," + std::to_string(static_cast<u32>(std::to_underlying(fmt)));
        }
        return key;
    }

    auto HashAliasGroup(const RGResourceDesc& desc) -> u64
    {
        // 64-bit FNV-1a over the descriptor fields. Same identity contract as
        // BuildAliasGroup — anything that affects backing compatibility goes in.
        constexpr u64 fnvOffset = 14695981039346656037ULL;
        constexpr u64 fnvPrime = 1099511628211ULL;
        u64 h = fnvOffset;
        const auto mix = [&h, &fnvPrime](u64 v)
        {
            for (u32 i = 0; i < 8; ++i)
            {
                h ^= (v >> (i * 8u)) & 0xFFu;
                h *= fnvPrime;
            }
        };
        mix(static_cast<u64>(std::to_underlying(desc.Kind)));
        mix(static_cast<u64>(std::to_underlying(desc.Format)));
        mix(static_cast<u64>(desc.Width));
        mix(static_cast<u64>(desc.Height));
        mix(static_cast<u64>(desc.DepthOrLayers));
        mix(static_cast<u64>(desc.MipLevels));
        mix(static_cast<u64>(desc.Samples));
        mix(static_cast<u64>(std::to_underlying(desc.Queue)));
        // MRT formats — order matters and a sentinel separates from a non-MRT
        // descriptor that happens to have one trailing attachment.
        if (!desc.Attachments.empty())
        {
            mix(0xDEADBEEFCAFEBABEULL); // MRT marker
            for (const auto fmt : desc.Attachments)
                mix(static_cast<u64>(std::to_underlying(fmt)));
        }
        return h;
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
                           std::ranges::any_of(desc.Attachments,
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
                    const bool anyValid = std::ranges::any_of(desc.Attachments,
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

        const auto touchResource = [&input, &lifetimes](const std::string& resourceName,
                                                        u32 passIndex,
                                                        const std::string& passName)
        {
            if (!input.TransientResourceDescs.contains(resourceName))
                return;

            auto& lifetime = lifetimes[resourceName];
            if (!input.IsPassReachable(passName))
                return;

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
        };

        for (u32 passIndex = 0; passIndex < static_cast<u32>(input.ExecutionOrder.size()); ++passIndex)
        {
            const auto& passName = input.ExecutionOrder[passIndex];
            if (const auto accessIt = input.PassAccessDeclarations.find(passName);
                accessIt != input.PassAccessDeclarations.end())
            {
                for (const auto& access : accessIt->second)
                    touchResource(access.ResourceName, passIndex, passName);
            }

            // Attachment-view writes extend their parent framebuffer's
            // lifetime without a hazard-tracked access declaration (see
            // RGBuilder::Write) — fold those touches in too so a pass that
            // seeds an MRT purely through attachment views (e.g.
            // OITPreparePass) isn't excluded from the parent's FirstPassIndex.
            if (const auto lifetimeIt = input.PassLifetimeExtensions.find(passName);
                lifetimeIt != input.PassLifetimeExtensions.end())
            {
                for (const auto& resourceName : lifetimeIt->second)
                    touchResource(resourceName, passIndex, passName);
            }
        }

        // 2. Compose one plan entry per transient descriptor; classify into
        //    allocatable vs skip-with-reason. We compute the hashed alias
        //    group key once per entry and use it for the sort comparator
        //    and the slot-assignment lookups below — string compares would
        //    cost O(L) per touch otherwise. The string form survives only
        //    for JSON output via `TransientPlanEntry::AliasGroup`.
        plan.reserve(input.TransientResourceDescs.size());
        // Sidecar map: resourceName → hashed alias group. The key uses the
        // owned std::string in the input map (stable through this function),
        // not a string_view into entry.Resource which gets moved-from when we
        // push_back below.
        std::unordered_map<std::string, u64> aliasGroupHashByResource;
        aliasGroupHashByResource.reserve(input.TransientResourceDescs.size());
        for (const auto& [resourceName, desc] : input.TransientResourceDescs)
        {
            RenderGraph::TransientPlanEntry entry;
            entry.Resource = resourceName;
            entry.Kind = desc.Kind;
            entry.AliasGroup = BuildAliasGroup(desc);
            entry.EstimatedBytes = EstimateBytes(desc);
            aliasGroupHashByResource.emplace(resourceName, HashAliasGroup(desc));

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

        // 3. Canonical sort: by alias-group hash, then by first-use pass
        //    index, then by resource name (deterministic across rebuilds).
        //    The hash lookup replaces an O(L) string compare per probe.
        std::ranges::sort(plan,
                          [&aliasGroupHashByResource](const RenderGraph::TransientPlanEntry& lhs, const RenderGraph::TransientPlanEntry& rhs)
                          {
                              const auto lhsHash = aliasGroupHashByResource.at(lhs.Resource);
                              if (const auto rhsHash = aliasGroupHashByResource.at(rhs.Resource); lhsHash != rhsHash)
                                  return lhsHash < rhsHash;
                              if (lhs.FirstPassIndex != rhs.FirstPassIndex)
                                  return lhs.FirstPassIndex < rhs.FirstPassIndex;
                              return lhs.Resource < rhs.Resource;
                          });

        // 4. Alias-slot assignment per alias group: non-overlapping lifetimes
        //    share a slot, otherwise allocate a new one. Keyed by hash, not
        //    string, so lookups are O(1) hash compare instead of O(L) string.
        struct ActiveSlot
        {
            u32 Slot = 0;
            u32 LastPassIndex = 0;
        };

        std::unordered_map<u64, std::vector<ActiveSlot>> activeByGroup;
        activeByGroup.reserve(plan.size());

        std::unordered_map<u64, u32> nextSlotByGroup;
        nextSlotByGroup.reserve(plan.size());

        for (auto& entry : plan)
        {
            if (!entry.WillAllocate)
                continue;

            const auto groupHash = aliasGroupHashByResource.at(entry.Resource);
            auto& active = activeByGroup[groupHash];
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
                slotAssigned = nextSlotByGroup[groupHash]++;
                active.push_back(ActiveSlot{ .Slot = slotAssigned, .LastPassIndex = entry.LastPassIndex });
            }

            entry.AliasSlot = slotAssigned;
        }

        return plan;
    }
} // namespace OloEngine::RenderGraphTransientPlanner
