#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphReachability.h"

#include "OloEngine/Debug/Profiler.h"

namespace OloEngine::RenderGraphReachability
{
    auto ComputeReachableSet(const ScanInput& input) -> RGTransparentStringSet
    {
        OLO_PROFILE_FUNCTION();

        RGTransparentStringSet reachable;

        // Without an explicit final pass, the graph keeps every registered
        // entry reachable (preserves ad-hoc / unit-test execution semantics).
        if (!input.HasExplicitFinalPass)
        {
            reachable.reserve(input.InsertionOrder.size());
            for (const auto& passName : input.InsertionOrder)
                reachable.insert(passName.ToStdString());
            return reachable;
        }

        if (input.FinalPassName.empty())
        {
            // Caller is responsible for logging the warning; we fall back to
            // the same "everything reachable" rule the original implementation
            // used so behavior matches.
            reachable.reserve(input.InsertionOrder.size());
            for (const auto& passName : input.InsertionOrder)
                reachable.insert(passName.ToStdString());
            return reachable;
        }

        // Resource name → writer-pass list. Built once from every pass's
        // setup-time write declarations.
        RGTransparentStringMap<TArray64<FString>> resourceWriters;
        resourceWriters.reserve(input.InsertionOrder.size() * 4u);

        for (const auto& passName : input.InsertionOrder)
        {
            const auto accessIt = input.PassAccessDeclarations.find(passName.ToView());
            if (accessIt == input.PassAccessDeclarations.end())
                continue;

            for (const auto& access : accessIt->second)
            {
                if (access.IsWrite && !access.ResourceName.IsEmpty())
                    resourceWriters[access.ResourceName.ToStdString()].Add(passName);
            }
        }

        std::vector<std::string> stack;

        const auto enqueueReachablePass = [&reachable, &stack](std::string_view passName)
        {
            if (passName.empty())
                return;
            if (reachable.insert(std::string(passName)).second)
                stack.push_back(std::string(passName));
        };

        const auto enqueueWritersForResource = [&resourceWriters, &enqueueReachablePass](std::string_view resourceName)
        {
            if (resourceName.empty())
                return;

            if (const auto writerIt = resourceWriters.find(std::string(resourceName));
                writerIt != resourceWriters.end())
            {
                for (const auto& writerName : writerIt->second)
                    enqueueReachablePass(writerName.ToView());
            }
        };

        // Seed: final pass + every named extract / contract root.
        enqueueReachablePass(std::string(input.FinalPassName));
        for (const auto& resourceName : input.ExtractedResourceNames)
            enqueueWritersForResource(resourceName.ToView());

        // Walk explicit dependency edges (BFS).
        RGTransparentStringSet visited;
        while (!stack.empty())
        {
            const auto current = std::move(stack.back());
            stack.pop_back();

            if (!visited.insert(current).second)
                continue;

            if (const auto dependencyIt = input.Dependencies.find(current); dependencyIt != input.Dependencies.end())
            {
                for (const auto& dependency : dependencyIt->second)
                    enqueueReachablePass(dependency.ToView());
            }
        }

        // Iterative Read→Writer expansion. For each already-reachable
        // pass, add the writer of any resource it reads. Repeat until stable —
        // this handles wrapped passes whose ordering edges are derivation-only.
        bool anyNew = true;
        while (anyNew)
        {
            anyNew = false;
            const std::vector<std::string> snapshot(reachable.begin(), reachable.end());
            for (const auto& passName : snapshot)
            {
                const auto accessIt = input.PassAccessDeclarations.find(passName);
                if (accessIt == input.PassAccessDeclarations.end())
                    continue;

                for (const auto& access : accessIt->second)
                {
                    if (access.IsWrite)
                        continue;

                    auto writerIt = resourceWriters.find(access.ResourceName.ToView());
                    if (writerIt == resourceWriters.end())
                        continue;
                    for (const auto& writerName : writerIt->second)
                    {
                        if (reachable.insert(writerName.ToStdString()).second)
                            anyNew = true;
                    }
                }
            }
        }

        return reachable;
    }
} // namespace OloEngine::RenderGraphReachability
