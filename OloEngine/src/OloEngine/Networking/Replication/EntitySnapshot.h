#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    class Scene;

    class EntitySnapshot
    {
    public:
        static std::vector<u8> Capture(Scene& scene);
        static void Apply(Scene& scene, const std::vector<u8>& data);
    };
}
