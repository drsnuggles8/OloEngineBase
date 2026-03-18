#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Navigation/CrowdManager.h"

namespace OloEngine
{
    class Scene;

    class NavigationSystem
    {
    public:
        static void OnUpdate(Scene* scene, f32 dt);
    };
} // namespace OloEngine
