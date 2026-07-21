#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"

namespace OloEngine
{
    class RenderGraph;

    class RenderGraphDebugRuntime
    {
      public:
        static void SetActiveGraph(const Ref<RenderGraph>& graph)
        {
            s_ActiveGraph = graph;
            // A graph teardown invalidates the snapshot's installed-hook
            // bookkeeping; disarm so a stale graph pointer is never touched.
            // Renderer shutdown (graph == null) is also the last moment the
            // GL context is guaranteed alive, so free the scratch clone here —
            // the snapshot object itself is a process-static whose destructor
            // must never call GL.
            if (!graph)
            {
                s_PassSnapshot.Disarm();
                s_PassSnapshot.ReleaseScratch();
            }
        }

        [[nodiscard]] static const Ref<RenderGraph>& GetActiveGraph()
        {
            return s_ActiveGraph;
        }

        // The process-wide afterPass snapshot the MCP capture/probe/stats
        // tools share (issue #607). Main-thread only, like the graph itself.
        [[nodiscard]] static RenderGraphPassSnapshot& GetPassSnapshot()
        {
            return s_PassSnapshot;
        }

      private:
        inline static Ref<RenderGraph> s_ActiveGraph = nullptr;
        inline static RenderGraphPassSnapshot s_PassSnapshot;
    };
} // namespace OloEngine
