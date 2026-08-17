#pragma once

namespace OloEngine
{
    // @brief The render-graph's verbose build/execute diagnostics switch.
    //
    // Five renderer TUs each carried a byte-identical private copy of this —
    // a function-local `static const bool` seeded from an environment read.
    // That is one switch with five independent latches, and the latch is the
    // problem: whichever TU asked first froze the answer for the process, so
    // the test binary had to WRITE the environment in `main` before anything
    // could read it. That `_putenv_s`/`setenv` pair was the only place the
    // engine mutated its own environment, and it existed purely to reach a
    // value there was no other way to set.
    //
    // With one accessor the harness calls `SetRenderGraphDiagnosticsEnabled`
    // and is done. The environment variable stays as the *operator's* way in
    // (it is genuinely a "turn this on for one run of an already-built binary"
    // lever), resolved lazily on first read if nobody set it explicitly.
    [[nodiscard]] bool IsRenderGraphDiagnosticsEnabled();

    // Overrides the switch, whether or not it has already been resolved from
    // OLO_RENDERGRAPH_DIAGNOSTICS. Intended for the test harness and for a
    // future editor toggle; production code should just read.
    void SetRenderGraphDiagnosticsEnabled(bool enabled);
} // namespace OloEngine
