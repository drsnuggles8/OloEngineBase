#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphResourceRegistry.h"

#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <utility>

namespace OloEngine::RenderGraphResourceRegistry
{
    namespace
    {
        void AppendUnique(TArray64<FString>& names, const std::string& value)
        {
            if (!names.ContainsByPredicate([&](const FString& name)
                                           { return name.ToView() == value; }))
                names.Emplace(value);
        }
    } // namespace

    auto Build(const BuildInput& input) -> BuildResult
    {
        OLO_PROFILE_FUNCTION();

        using Hazard = RenderGraph::Hazard;
        using HazardKind = RenderGraph::HazardKind;
        using ResourceInfo = RenderGraph::ResourceInfo;

        BuildResult result;

        // 1. Seed the registry from descriptor maps: imports, transient
        //    descs, and texture views.
        for (const auto& [name, desc] : input.ImportedResources)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.IsEmpty())
                info.Desc.DebugName = name;
            info.Desc.Imported = true;
            result.Registry[name] = std::move(info);
        }

        for (const auto& [name, desc] : input.TransientResourceDescs)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.IsEmpty())
                info.Desc.DebugName = name;
            info.Desc.Imported = false;
            result.Registry[name] = std::move(info);
        }

        for (const auto& [name, desc] : input.TextureViewResourceDescs)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.IsEmpty())
                info.Desc.DebugName = name;
            result.Registry[name] = std::move(info);
        }

        // 2. Annotate external-backing for transient resources whose backing
        //    object is caller-supplied (e.g. histories) rather than pool-
        //    allocated.
        for (auto& [name, info] : result.Registry)
        {
            info.HasExternalBacking = input.IsExternallyBackedTransientResource(name);
        }

        // 3. Walk per-pass access declarations and record each resource's
        //    producer/consumer pass list. Emit a kind-mismatch diagnostic if
        //    two passes declare the same resource as incompatible kinds.
        const auto registerDeclaration = [&result](std::string_view passName,
                                                   const RGResourceHandle& handle,
                                                   const bool isWrite)
        {
            auto [it, inserted] = result.Registry.try_emplace(handle.Name.ToStdString());
            auto& info = it->second;
            if (inserted)
            {
                info.Name = handle.Name;
                info.Desc = RGResourceDesc::FromHandleKind(handle.Type, handle.Name.ToView());
            }
            else if (info.Desc.DebugName.IsEmpty())
            {
                info.Desc.DebugName = handle.Name;
            }
            else
            {
                // No additional handling required.
            }

            const auto declaredKind = handle.Type;
            if (const auto existingKind = info.Desc.Kind; existingKind == RGResourceHandle::Kind::Unknown && declaredKind != RGResourceHandle::Kind::Unknown)
            {
                info.Desc.Kind = declaredKind;
            }
            else if (existingKind != RGResourceHandle::Kind::Unknown &&
                     declaredKind != RGResourceHandle::Kind::Unknown &&
                     existingKind != declaredKind)
            {
                const auto priorPass = !info.Producers.IsEmpty()
                                           ? info.Producers[0]
                                           : (!info.Consumers.IsEmpty() ? info.Consumers[0] : FString{});

                Hazard h;
                h.Kind = HazardKind::ResourceKindMismatch;
                h.Resource = handle.Name;
                h.Producer = priorPass;
                h.Consumer = passName;
                h.Message = "Kind mismatch: resource '" + handle.Name +
                            "' was previously declared as '" + std::string(ToString(existingKind)) +
                            "' but pass '" + passName + "' declares it as '" +
                            std::string(ToString(declaredKind)) + "'";
                OLO_CORE_ERROR("RenderGraph hazard: {}", h.Message.ToView());
                result.Diagnostics.Add(std::move(h));
            }
            else
            {
                // No additional handling required.
            }

            if (isWrite)
                AppendUnique(info.Producers, std::string(passName));
            else
                AppendUnique(info.Consumers, std::string(passName));
        };

        for (const auto& passName : input.InsertionOrder)
        {
            const auto accessIt = input.PassAccessDeclarations.find(passName.ToStdString());
            if (accessIt == input.PassAccessDeclarations.end())
                continue;

            for (const auto& access : accessIt->second)
            {
                RGResourceHandle syntheticHandle(access.ResourceName.ToView(), RGResourceHandle::Kind::Unknown);
                registerDeclaration(passName.ToStdString(), syntheticHandle, access.IsWrite);
            }
        }

        // 4. Produce the canonical sorted view downstream stages consume.
        result.Sorted.Reserve(result.Registry.size());
        for (const auto& [name, info] : result.Registry)
            result.Sorted.Add(info);
        result.Sorted.Sort(
            [](const ResourceInfo& lhs, const ResourceInfo& rhs)
            {
                return lhs.Name < rhs.Name;
            });

        return result;
    }
} // namespace OloEngine::RenderGraphResourceRegistry
