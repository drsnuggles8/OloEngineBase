#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphDiagnostics.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Environment.h"

#include <atomic>

namespace OloEngine
{
    namespace
    {
        // -1 = not yet resolved from the environment, 0 = off, 1 = on.
        std::atomic<i8> s_State{ -1 };
    } // namespace

    bool IsRenderGraphDiagnosticsEnabled()
    {
        i8 state = s_State.load(std::memory_order_relaxed);
        if (state < 0)
        {
            // A concurrent first read computes the same value from the same
            // variable, so there is nothing to lose if two threads race here.
            state = Env::IsTruthy("OLO_RENDERGRAPH_DIAGNOSTICS") ? i8{ 1 } : i8{ 0 };
            s_State.store(state, std::memory_order_relaxed);
        }
        return state != 0;
    }

    void SetRenderGraphDiagnosticsEnabled(bool enabled)
    {
        s_State.store(enabled ? i8{ 1 } : i8{ 0 }, std::memory_order_relaxed);
    }
} // namespace OloEngine
