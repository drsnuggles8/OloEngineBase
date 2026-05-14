#pragma once

#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class RenderGraph;

    class RenderGraphDebugRuntime
    {
      public:
        static void SetActiveGraph(const Ref<RenderGraph>& graph)
        {
            s_ActiveGraph = graph;
        }

        [[nodiscard]] static const Ref<RenderGraph>& GetActiveGraph()
        {
            return s_ActiveGraph;
        }

      private:
        inline static Ref<RenderGraph> s_ActiveGraph = nullptr;
    };
} // namespace OloEngine
