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
        void AppendUnique(std::vector<std::string>& names, const std::string& value)
        {
            if (std::find(names.begin(), names.end(), value) == names.end())
                names.push_back(value);
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
            if (info.Desc.DebugName.empty())
                info.Desc.DebugName = name;
            info.Desc.Imported = true;
            result.Registry[name] = std::move(info);
        }

        for (const auto& [name, desc] : input.TransientResourceDescs)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.empty())
                info.Desc.DebugName = name;
            info.Desc.Imported = false;
            result.Registry[name] = std::move(info);
        }

        for (const auto& [name, desc] : input.TextureViewResourceDescs)
        {
            ResourceInfo info;
            info.Name = name;
            info.Desc = desc;
            if (info.Desc.DebugName.empty())
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
        const auto registerDeclaration = [&result](const std::string& passName,
                                                   const ResourceHandle& handle,
                                                   const bool isWrite)
        {
            auto [it, inserted] = result.Registry.try_emplace(handle.Name);
            auto& info = it->second;
            if (inserted)
            {
                info.Name = handle.Name;
                info.Desc = RGResourceDesc::FromHandleKind(handle.Type, handle.Name);
            }
            else if (info.Desc.DebugName.empty())
            {
                info.Desc.DebugName = handle.Name;
            }

            const auto declaredKind = handle.Type;
            if (const auto existingKind = info.Desc.Kind; existingKind == ResourceHandle::Kind::Unknown && declaredKind != ResourceHandle::Kind::Unknown)
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
                result.Diagnostics.push_back(std::move(h));
            }

            if (isWrite)
                AppendUnique(info.Producers, passName);
            else
                AppendUnique(info.Consumers, passName);
        };

        for (const auto& passName : input.InsertionOrder)
        {
            const auto accessIt = input.PassAccessDeclarations.find(passName);
            if (accessIt == input.PassAccessDeclarations.end())
                continue;

            for (const auto& access : accessIt->second)
            {
                ResourceHandle syntheticHandle(access.ResourceName, ResourceHandle::Kind::Unknown);
                registerDeclaration(passName, syntheticHandle, access.IsWrite);
            }
        }

        // 4. Produce the canonical sorted view downstream stages consume.
        result.Sorted.reserve(result.Registry.size());
        for (const auto& [name, info] : result.Registry)
            result.Sorted.push_back(info);
        std::sort(result.Sorted.begin(), result.Sorted.end(),
                  [](const ResourceInfo& lhs, const ResourceInfo& rhs)
                  {
                      return lhs.Name < rhs.Name;
                  });

        return result;
    }
} // namespace OloEngine::RenderGraphResourceRegistry
