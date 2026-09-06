// =============================================================================
// RendererStateCheck.h
//
// Cross-test guard for the process-wide RENDERER CONFIGURATION (issue #1074).
//
// The rule this enforces: a test must leave the process-global renderer
// configuration exactly as it found it. Everything else here is the story of
// why that needs a guard rather than a convention.
//
// `Renderer3D::s_Data` is a single static shared by every test in this binary,
// and roughly a dozen of its members are *configuration* rather than per-frame
// scratch: the rendering path, the culling toggles, and eleven settings structs
// (fog, wind, snow, precipitation, the water fields, post-process). A test that
// writes one and does not put it back changes what the renderer DOES for every
// later test in the same process — silently, and with no GL-level symptom for
// `GLErrorStateCheck` to catch.
//
// The failure this produces is uniquely misleading. The victim is a visual
// evidence test whose feature-on and feature-off captures come out identical,
// or whose differential reads ~0 where it expected a large one — the effect
// under test simply is not rendering, because the path or the toggle it needs
// was left switched off by an unrelated test that ran earlier. The victim
// passes 10/10 under its own `--gtest_filter` and fails only in the monolithic
// run, so the evidence points everywhere except the cause. Bisecting one such
// poisoner to its source took ~5 full-suite runs at 17 minutes each.
//
// CI structurally cannot see any of this: `gtest_discover_tests` registers
// every case as its own ctest entry, so CI runs one process per test and no two
// of these tests ever share an address space.
//
// What the listener does, after EVERY test:
//
//   1. **Restores** the configuration to what it was when the test started.
//      This is the structural fix, and it is why the guard restores rather than
//      only reporting: a fixture that saves and restores "the two settings I
//      remembered" leaves the other nine free to leak, and every new settings
//      struct added to the renderer silently joins the hazard. Restoring
//      centrally makes a partial restore unexpressible.
//   2. **Counts** the leak and attributes it to the test that caused it. A
//      summary at the end of the run names the polluting tests and the state
//      each one leaked, so a leak is never invisible even when the restore
//      has already made it harmless.
//   3. Under `--olo-strict-renderer-state`, additionally **fails** the
//      polluting test. Off by default because a leak is now repaired rather
//      than propagated: turning ~100 mystery failures in victim tests into
//      several hundred failures in polluting tests would trade one unreadable
//      signal for another. The flag is what a fixture author runs to see their
//      own leak as a named failure.
//
// Why a listener and not a fixture `TearDown()`: most GPU tests are plain
// `TEST()` bodies gated on `OLO_ENSURE_GPU_OR_SKIP()`, not `TEST_F` subclasses
// of `RendererAttachedTest`, and a poisoner can be any test in the binary — a
// fixture teardown would miss exactly the ones nobody thought to look at. This
// mirrors `GLErrorStateCheck`, which solved the same problem for GL state.
//
// Headless-safe: when `Renderer3D` was never initialized (CI without a GPU,
// where every GL test SKIPs) the check is a clean no-op and never fabricates a
// failure. It is also a no-op across the boundary where a test initializes or
// shuts down the renderer, since there is no before/after pair to compare.
// =============================================================================
#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterFoam.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"

#include <string>

namespace OloEngine::Tests::RendererState
{
    // A copy of every process-global renderer configuration value a test can
    // reach. Deliberately NOT a copy of `Renderer3D::s_Data`: that struct is
    // mostly per-frame scratch (matrices, statistics, GPU resource handles)
    // which every frame overwrites and no test can leak. Only values that
    // PERSIST across a `BeginScene`/`EndScene` pair belong here — those are the
    // ones a later test inherits.
    //
    // Adding a settings struct to `Renderer3D`? Add it here too. The cost of
    // forgetting is a new class of cross-test failure that looks like a broken
    // renderer; `RendererStateCheckTest` pins the count so the omission shows
    // up as a failing test rather than as folklore.
    struct Snapshot
    {
        bool Valid = false;

        // --- The settings structs, in `Renderer3D` declaration order ---
        RendererSettings Renderer{};
        PostProcessSettings PostProcess{};
        SnowSettings Snow{};
        FogSettings Fog{};
        WindSettings Wind{};
        SnowAccumulationSettings SnowAccumulation{};
        WaterDisturbanceSettings WaterDisturbance{};
        WaterWakeSettings WaterWakeShape{};
        WaterFoam::WaterFoamSettings WaterFoamAdvection{};
        SnowEjectaSettings SnowEjecta{};
        PrecipitationSettings Precipitation{};

        // --- Scene-published render state ---
        CloudscapeRenderState Cloudscape{};
        UnderwaterFogState UnderwaterFog{};

        // --- Scalar toggles that live outside any settings struct ---
        // Deliberately ABSENT: `DepthPrepassEnabled`. It is not independent
        // state — `ApplyRendererSettings` recomputes it from `RendererSettings`
        // (`ComputeSettingsDerivedDepthPrepass`: Forward+ and Deferred force it
        // on), and `RendererSettings` is snapshotted above. Snapshotting the
        // derived value as well reports a leak the first time anything calls
        // `ApplyRendererSettings` after `Renderer::Init`, because Init leaves
        // the raw flag at its default without ever running the derivation.
        bool FrustumCulling = true;
        bool DynamicCulling = true;
        bool DepthAwareClusterCulling = true;
        bool OcclusionCulling = false;
        bool HZBOcclusionCulling = false;
        bool CullingCameraFrozen = false;
        bool CameraRelative = true;
        bool SelectionOutline = false;
        bool ForceDisableCulling = false;
        f32 RenderScale = 1.0f;
    };

    /// Capture the current process-global renderer configuration. Returns false
    /// (leaving `out.Valid == false`) when `Renderer3D` is not initialized —
    /// there is no configuration to read, and no leak is possible.
    bool Capture(Snapshot& out);

    /// Append a human-readable description of every entry that differs between
    /// `before` and `after` to `outDetail`, and return how many differed.
    /// Returns 0 when either snapshot is invalid: a test that brought the
    /// renderer up or took it down has no before/after pair to compare.
    u32 Describe(const Snapshot& before, const Snapshot& after, std::string& outDetail);

    /// Write `snapshot` back over the live renderer configuration. Re-applies
    /// the renderer settings afterwards so the render graph is reconfigured to
    /// match — a restored `RendererSettings::Path` that the graph was never told
    /// about leaves the pipeline built for the wrong path, which is the same
    /// bug with an extra step. No-op when `snapshot.Valid` is false.
    void Restore(const Snapshot& snapshot);

    /// Installs the renderer-state listener on the global gtest listener list.
    /// Call once from main() after InitGoogleTest. Idempotent.
    void RegisterListener();

    /// Number of tests that leaked renderer configuration so far this run.
    /// Exposed for the end-of-run summary and for self-tests.
    [[nodiscard]] u32 LeakCount();
} // namespace OloEngine::Tests::RendererState
