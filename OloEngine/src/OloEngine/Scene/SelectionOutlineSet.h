#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Entity.h"

#include <span>
#include <vector>

namespace OloEngine
{
    class Scene;

    /**
     * @brief The entity IDs the editor's selection outline covers (#1533).
     *
     * Each selected entity, every descendant of one, and every enabled groom
     * bound to any of those: what the selection looks like on screen. The
     * outline pass draws its band wherever a covered pixel meets an uncovered
     * one, so a furred animal whose body was covered and whose coat was not
     * showed a band round every gap in the fur -- the body shows through
     * nowhere else -- and the dog lit up with orange speckle.
     *
     * Each ID appears once, in selection order and then discovery order, as the
     * entity-ID attachment holds them (the entt handle).
     */
    [[nodiscard]] std::vector<i32> CollectSelectionOutlineIds(Scene& scene, std::span<const Entity> selected);
} // namespace OloEngine
