#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::RenderGraphHandleAllocator
{
    // Generic handle-allocator templates extracted from
    // `RenderGraph::EnsureResourceRegistryBuilt` (Phase B) as the final
    // Phase 7 module-split slice (2026-05-12). The graph keeps three
    // identical handle families (texture / buffer / framebuffer) — each
    // with a `Name → Handle` map, a slot table (alive flag + generation
    // counter + name), a free-index list, and a parallel physical-resource
    // array. The two operations below are family-agnostic so they live as
    // templated free functions instead of three near-identical lambdas
    // baked into the registry rebuild.
    //
    //   - `Reconcile`: kills handle entries whose names no longer appear in
    //     the active set (i.e. the resource was removed from the registry).
    //     Bumps the slot's generation counter so any stale handles cached
    //     by callers fail their generation check, and returns the slot
    //     index to the free list.
    //   - `Allocate`: returns the handle for an existing name, or allocates
    //     a new slot — reusing a free index when one is available, growing
    //     the slot table otherwise. Keeps the parallel physical-resource
    //     array sized to the slot table.

    template <typename HandleT, typename SlotT>
    void Reconcile(std::unordered_map<std::string, HandleT>& handlesByName,
                   std::vector<SlotT>& slots,
                   std::vector<u32>& freeIndices,
                   const std::unordered_set<std::string>& activeNames)
    {
        for (auto it = handlesByName.begin(); it != handlesByName.end();)
        {
            if (activeNames.contains(it->first))
            {
                ++it;
                continue;
            }

            const auto staleHandle = it->second;
            if (staleHandle.IsValid() && staleHandle.Index < slots.size())
            {
                auto& slot = slots[staleHandle.Index];
                slot.Alive = false;
                slot.Name.clear();
                if (slot.Generation == 0)
                    slot.Generation = 1;
                ++slot.Generation;
                freeIndices.push_back(staleHandle.Index);
            }

            it = handlesByName.erase(it);
        }
    }

    template <typename HandleT, typename SlotT, typename PhysicalT, typename MakeHandleFn>
    HandleT Allocate(const std::string& name,
                     std::unordered_map<std::string, HandleT>& handlesByName,
                     std::vector<SlotT>& slots,
                     std::vector<PhysicalT>& physicals,
                     std::vector<u32>& freeIndices,
                     MakeHandleFn makeHandle)
    {
        const auto ensurePhysicalCapacity = [&physicals](const u32 index)
        {
            if (index >= physicals.size())
                physicals.resize(static_cast<sizet>(index) + 1u);
        };

        if (auto it = handlesByName.find(name); it != handlesByName.end())
        {
            ensurePhysicalCapacity(it->second.Index);
            return it->second;
        }

        u32 index = 0;
        if (!freeIndices.empty())
        {
            index = freeIndices.back();
            freeIndices.pop_back();

            auto& slot = slots[index];
            slot.Alive = true;
            slot.Name = name;
            if (slot.Generation == 0)
                slot.Generation = 1;

            ensurePhysicalCapacity(index);
            auto handle = makeHandle(index, slot.Generation);
            handlesByName[name] = handle;
            return handle;
        }

        index = static_cast<u32>(slots.size());
        slots.push_back({ 1, true, name });
        physicals.resize(slots.size());
        auto handle = makeHandle(index, 1);
        handlesByName[name] = handle;
        return handle;
    }
} // namespace OloEngine::RenderGraphHandleAllocator
