#pragma once

#include "OloEngine/Containers/Array.h"

#include "OloEngine/Core/Base.h"

#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine::RenderGraphHandleAllocator
{
    // Generic handle-allocator templates extracted from
    // `RenderGraph::EnsureResourceRegistryBuilt` as the final
    // module-split step (2026-05-12). The graph keeps three
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

    // The HandlesByNameT / ActiveNamesT parameters are template-deducible so
    // callers can pass either the plain `unordered_map<string, ...>` form or
    // the transparent-hash variants used by `RenderGraph` without forcing two
    // overloads. Same applies to `activeNames` (set vs transparent set).
    template<typename HandlesByNameT, typename SlotT, typename ActiveNamesT>
    void Reconcile(HandlesByNameT& handlesByName,
                   TArray64<SlotT>& slots,
                   TArray64<u32>& freeIndices,
                   const ActiveNamesT& activeNames)
    {
        for (auto it = handlesByName.begin(); it != handlesByName.end();)
        {
            if (activeNames.contains(it->first))
            {
                ++it;
                continue;
            }

            if (const auto staleHandle = it->second; staleHandle.IsValid() && staleHandle.Index < static_cast<sizet>(slots.Num()))
            {
                auto& slot = slots[staleHandle.Index];
                slot.Alive = false;
                slot.Name.Reset();
                // Increment first, then repair a wrap to 0: Generation 0 is
                // the "never allocated" sentinel (IsValid() requires > 0), so
                // an overflow must land on 1, not 0.
                ++slot.Generation;
                if (slot.Generation == 0)
                    slot.Generation = 1;
                freeIndices.Add(staleHandle.Index);
            }

            it = handlesByName.erase(it);
        }
    }

    template<typename HandlesByNameT, typename SlotT, typename PhysicalT, typename MakeHandleFn>
    auto Allocate(const std::string& name,
                  HandlesByNameT& handlesByName,
                  TArray64<SlotT>& slots,
                  TArray64<PhysicalT>& physicals,
                  TArray64<u32>& freeIndices,
                  MakeHandleFn makeHandle)
    {
        const auto ensurePhysicalCapacity = [&physicals](const u32 index)
        {
            if (index >= static_cast<sizet>(physicals.Num()))
                physicals.SetNum(static_cast<i64>(static_cast<sizet>(index) + 1u), EAllowShrinking::No);
        };

        if (auto it = handlesByName.find(name); it != handlesByName.end())
        {
            ensurePhysicalCapacity(it->second.Index);
            return it->second;
        }

        u32 index = 0;
        if (!freeIndices.IsEmpty())
        {
            index = freeIndices.Last();
            freeIndices.Pop(EAllowShrinking::No);

            auto& slot = slots[index];
            slot.Alive = true;
            slot.Name = name;
            // A slot leaving the free list is being handed to a DIFFERENT
            // resource than whatever it last named, so its generation must be
            // fresh here, locally — not merely trusted to have been bumped by
            // whichever free path retired it. Without this, a single free
            // site that forgets to bump lets a stale cached handle resolve
            // silently to the new occupant's physical resource instead of
            // failing its generation check.
            ++slot.Generation;
            if (slot.Generation == 0)
                slot.Generation = 1;

            ensurePhysicalCapacity(index);
            auto handle = makeHandle(index, slot.Generation);
            handlesByName[name] = handle;
            return handle;
        }

        index = static_cast<u32>(static_cast<sizet>(slots.Num()));
        slots.Add({ 1, true, name });
        physicals.SetNum(static_cast<i64>(static_cast<sizet>(slots.Num())), EAllowShrinking::No);
        auto handle = makeHandle(index, 1);
        handlesByName[name] = handle;
        return handle;
    }
} // namespace OloEngine::RenderGraphHandleAllocator
