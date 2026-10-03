#include "OloEnginePCH.h"
#include "OloEngine/Scene/SelectionOutlineSet.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <optional>
#include <unordered_set>

namespace OloEngine
{
    std::vector<i32> CollectSelectionOutlineIds(Scene& scene, std::span<const Entity> selected)
    {
        std::vector<i32> ids;
        std::unordered_set<u64> covered;
        std::vector<Entity> pending;

        // An entity and everything under it, depth first, children in their
        // authored order.
        const auto cover = [&](Entity root)
        {
            pending.push_back(root);
            while (!pending.empty())
            {
                const Entity entity = pending.back();
                pending.pop_back();
                if (!entity || !covered.insert(static_cast<u64>(entity.GetUUID())).second)
                {
                    continue;
                }
                ids.push_back(static_cast<i32>(static_cast<u32>(entity)));
                const TArray<UUID>& children = entity.Children();
                for (i32 i = children.Num() - 1; i >= 0; --i)
                {
                    if (const std::optional<Entity> child = scene.TryGetEntityWithUUID(children[i]))
                    {
                        pending.push_back(*child);
                    }
                }
            }
        };

        for (const Entity& entity : selected)
        {
            cover(entity);
        }

        // A coat bound to anything covered is part of what was selected. Until
        // nothing more joins: a coat can have children of its own, and one of
        // them can be another coat's body.
        for (bool grew = true; grew;)
        {
            grew = false;
            const auto view = scene.GetAllEntitiesWith<GroomBindingComponent>();
            for (const auto handle : view)
            {
                const auto& binding = view.get<GroomBindingComponent>(handle);
                const Entity coat{ handle, &scene };
                if (!binding.m_Enabled || static_cast<u64>(binding.m_TargetEntity) == 0u ||
                    !covered.contains(static_cast<u64>(binding.m_TargetEntity)) ||
                    covered.contains(static_cast<u64>(coat.GetUUID())))
                {
                    continue;
                }
                cover(coat);
                grew = true;
            }
        }
        return ids;
    }
} // namespace OloEngine
