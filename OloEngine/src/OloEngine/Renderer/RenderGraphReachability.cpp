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

        // Seed: final pass, side-effecting roots and every named extract /
        // contract root.
        enqueueReachablePass(std::string(input.FinalPassName));
        for (const auto& passName : input.SeedPasses)
            enqueueReachablePass(passName.ToView());
        for (const auto& resourceName : input.ExtractedResourceNames)
            enqueueWritersForResource(resourceName.ToView());

        // One fixpoint over both expansions: each newly reachable pass pulls
        // in its dependency edges AND the writers of everything it reads. A
        // pass reached through a read keeps its own dependencies (a
        // DependsOnPass, an out-of-band consumer's producers), which a
        // two-stage scan -- edges first, reads after -- used to drop.
        while (!stack.empty())
        {
            const auto current = std::move(stack.back());
            stack.pop_back();

            if (const auto dependencyIt = input.Dependencies.find(current); dependencyIt != input.Dependencies.end())
            {
                for (const auto& dependency : dependencyIt->second)
                {
                    if (input.OrderingOnlyEdges &&
                        input.OrderingOnlyEdges->contains({ dependency.ToStdString(), current }))
                    {
                        continue;
                    }
                    enqueueReachablePass(dependency.ToView());
                }
            }

            const auto accessIt = input.PassAccessDeclarations.find(current);
            if (accessIt == input.PassAccessDeclarations.end())
                continue;
            for (const auto& access : accessIt->second)
            {
                if (!access.IsWrite)
                    enqueueWritersForResource(access.ResourceName.ToView());
            }
        }

        return reachable;
    }
} // namespace OloEngine::RenderGraphReachability
