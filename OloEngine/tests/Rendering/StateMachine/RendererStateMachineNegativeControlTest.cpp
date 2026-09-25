// OLO_TEST_LAYER: integration

// =============================================================================
// Negative controls for the renderer state-machine harness (issue #1349).
//
// Each test re-creates a known class of renderer defect through a fault lever
// (DebugLevers.inl, "Fault injection") and runs a trace through the harness
// twice: fault off, where every pair must hold, and fault on, where the check
// the manifest names for that class must report it. A control that survives is
// a finding about the check -- it means that class of bug would ship green --
// so it fails the test rather than passing it.
//
//   fault                           | must be caught by
//   --------------------------------+--------------------------------------------
//   FaultStaleDeclarationKey        | cached-vs-rebuild.plan, fresh-vs-sequence.gl
//   FaultSkipDispatchBindingReset   | binding-cache-cold.gl
//   FaultShortenTransientLifetimes  | alias-vs-noalias.gl
//
// Every test SKIPs without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererStateMachineHarness.h"
#include "RendererStateMachineManifest.h"
#include "StateMachineCoverage.h"

#include "OloEngine/Core/DebugLevers.h"

#include <gtest/gtest.h>

#include <iostream>
#include <set>

namespace OloEngine::Tests::StateMachine
{
    namespace
    {
        class ScopedFault
        {
          public:
            ScopedFault(bool (*get)(), void (*set)(bool)) : m_Set(set), m_Previous(get())
            {
                m_Set(true);
            }
            ~ScopedFault()
            {
                m_Set(m_Previous);
            }
            ScopedFault(const ScopedFault&) = delete;
            ScopedFault& operator=(const ScopedFault&) = delete;

          private:
            void (*m_Set)(bool);
            bool m_Previous;
        };

        [[nodiscard]] Trace ParseOrDie(std::string_view text)
        {
            std::string error;
            std::optional<Trace> trace = ParseTrace(text, &error);
            EXPECT_TRUE(trace.has_value()) << error;
            return trace.value_or(Trace{});
        }

        [[nodiscard]] std::set<std::string> FailedPairs(const TraceResult& result)
        {
            std::set<std::string> pairs;
            for (const PairFailure& failure : result.Failures)
                pairs.insert(failure.PairId);
            return pairs;
        }

        [[nodiscard]] std::string Describe(const TraceResult& result)
        {
            std::string out;
            for (const PairFailure& failure : result.Failures)
                out += "  " + failure.PairId + " " + failure.Where + "\n" + failure.Detail;
            return out.empty() ? "  (no failures)\n" : out;
        }
    } // namespace

    class RendererStateMachineNegativeControl : public RendererStateMachineFixture
    {
      protected:
        // Runs `trace` clean with every pair (it must hold), then once per
        // named detector with the fault on and ONLY that detector enabled.
        // One run per detector, because the detectors interfere: the verify
        // frame of cached-vs-rebuild recompiles the graph at every checkpoint,
        // which cures a stale graph before fresh-vs-sequence could see it. A
        // detector the manifest names has to catch the fault on its own.
        // Returns the detectors that did.
        std::set<std::string> RunControl(const Trace& trace, bool (*get)(), void (*set)(bool),
                                         std::initializer_list<std::string_view> detectors)
        {
            RunOptions options;
            options.StopAtFirstFailure = false;
            const TraceResult clean = RunTrace(trace, options);
            EXPECT_TRUE(clean.Passed()) << "the control trace must hold every pair with the fault OFF, or a catch "
                                           "below proves nothing:\n"
                                        << Describe(clean);
            std::set<std::string> caught;
            for (const std::string_view detector : detectors)
            {
                RunOptions only = options;
                only.OnlyPair = std::string(detector);
                TraceResult faulty;
                {
                    const ScopedFault fault(get, set);
                    faulty = RunTrace(trace, only);
                }
                const std::set<std::string> failed = FailedPairs(faulty);
                const bool hit = failed.contains(std::string(detector));
                std::cout << "[StateMachine] fault " << (hit ? "caught" : "MISSED") << " by " << detector << "\n";
                if (hit)
                    caught.insert(std::string(detector));
            }
            return caught;
        }
    };

    // Bad cache invalidation. SSR's enable and the forward overlay's bucket
    // gate reach the declaration key only through PassStates, which the fault
    // leaves out, so toggling either keeps the cached graph.
    TEST_F(RendererStateMachineNegativeControl, StaleDeclarationKeyIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Trace trace = ParseOrDie("olo-state-machine-trace 1\n"
                                       "initial path=deferred size=320x180 msaa=1 upscale=off features= blended=0 buffering=2 pose=0\n"
                                       "op feature ssr on\n"
                                       "op entity-churn add\n");
        // Ends with SSR on and the blended mesh present: a trace that ends
        // back where the graph was cached would be "correct" by accident.
        const auto failed = RunControl(trace, &Levers::FaultStaleDeclarationKey, &Levers::SetFaultStaleDeclarationKey,
                                       { "cached-vs-rebuild.plan", "fresh-vs-sequence.gl" });
        EXPECT_TRUE(failed.contains("cached-vs-rebuild.plan"))
            << "the declaration-cache verifier did not report a stale plan under a key that ignores every pass input";
        EXPECT_TRUE(failed.contains("fresh-vs-sequence.gl"))
            << "a stale graph rendered the same as a directly configured one: fresh-vs-sequence is blind to it";
        if (failed.contains("cached-vs-rebuild.plan") && failed.contains("fresh-vs-sequence.gl"))
            Coverage::RecordComparison("stale-declaration-key");
    }

    // Missing binding reset. The dispatcher's texture, UBO, material and
    // render-state caches survive the frame-start reset, while the fixture's
    // GLStateGuard (and, in the editor, ImGui) changes bindings between frames.
    TEST_F(RendererStateMachineNegativeControl, MissingBindingResetIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Trace trace = ParseOrDie("olo-state-machine-trace 1\n"
                                       "initial path=forward size=320x180 msaa=1 upscale=off features= blended=0 buffering=2 pose=0\n"
                                       "op feature bloom on\n"
                                       "op path deferred\n"
                                       "op entity-churn add\n");
        const auto failed =
            RunControl(trace, &Levers::FaultSkipDispatchBindingReset, &Levers::SetFaultSkipDispatchBindingReset,
                       { "binding-cache-cold.gl" });
        EXPECT_TRUE(failed.contains("binding-cache-cold.gl"))
            << "a frame that relied on bindings nobody reset rendered the same as a frame with every binding cache "
               "forgotten: binding-cache-cold is blind to a missing reset";
        if (failed.contains("binding-cache-cold.gl"))
            Coverage::RecordComparison("missing-binding-reset");
    }

    // Alias-lifetime error. Every transient's planned lifetime ends one pass
    // early, so a backing still to be read is handed to the next transient of
    // the same descriptor. Deferred with a long post chain gives the planner
    // plenty of same-descriptor RGBA16F transients to share.
    TEST_F(RendererStateMachineNegativeControl, ShortenedAliasLifetimeIsCaught)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const Trace trace = ParseOrDie("olo-state-machine-trace 1\n"
                                       "initial path=deferred size=320x180 msaa=1 upscale=off features=bloom,fxaa,gtao,vignette-grading blended=0 buffering=2 pose=0\n"
                                       "op feature ssr on\n"
                                       "op path forward\n");
        const auto failed =
            RunControl(trace, &Levers::FaultShortenTransientLifetimes, &Levers::SetFaultShortenTransientLifetimes,
                       { "alias-vs-noalias.gl" });
        EXPECT_TRUE(failed.contains("alias-vs-noalias.gl"))
            << "transients sharing a backing while still live rendered the same as unaliased ones: alias-vs-noalias "
               "is blind to an alias-lifetime error";
        if (failed.contains("alias-vs-noalias.gl"))
            Coverage::RecordComparison("alias-lifetime");
    }
} // namespace OloEngine::Tests::StateMachine
