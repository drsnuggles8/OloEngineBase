#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;

    class InventorySystem
    {
      public:
        static void OnUpdate(Scene* scene, f32 dt);
    };

} // namespace OloEngine
