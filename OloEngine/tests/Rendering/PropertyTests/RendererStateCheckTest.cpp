// OLO_TEST_LAYER: L4
//
// Self-test for the cross-test renderer-configuration guard (issue #1074).
// Proves the three helpers behind the global listener:
//   - Describe() stays silent across a renderer bring-up/shut-down boundary,
//     where there is no before/after pair to compare,
//   - Describe() names the entry that changed, per settings struct and per
//     scalar toggle,
//   - Capture() then Restore() round-trips the live renderer configuration,
//     including the render graph's "configured-for" path.
//
// The pure-logic cases build Snapshots by hand and need no GL context, so they
// run on headless CI where every GPU test skips — which matters, because the
// guard's whole job is to hold in the monolithic run that only ever happens on
// a machine with a GPU. The round-trip case is GPU-gated.
//
// Each case leaves the renderer configuration as it found it, so the global
// RendererStateListener does not count it as a leak — the restore half of the
// contract, verified in place.
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererStateCheck.h"

#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <gtest/gtest.h>

#include <string>

namespace OloEngine::Tests
{
    // A test that brought the renderer up (or took it down) has no before/after
    // pair. Reporting one would make the first GPU test in every run
    // permanently guilty of a leak it did not cause.
    TEST(RendererStateCheckTest, InvalidSnapshotPairIsNotALeak)
    {
        RendererState::Snapshot invalid; // Valid == false
        RendererState::Snapshot valid;   //
        valid.Valid = true;              //
        valid.Fog.Enabled = !valid.Fog.Enabled;

        std::string detail;
        EXPECT_EQ(RendererState::Describe(invalid, valid, detail), 0u);
        EXPECT_EQ(RendererState::Describe(valid, invalid, detail), 0u);
        EXPECT_EQ(RendererState::Describe(invalid, invalid, detail), 0u);
        EXPECT_TRUE(detail.empty());
    }

    // Two identical valid snapshots are the normal, well-behaved case.
    TEST(RendererStateCheckTest, IdenticalSnapshotsReportNoLeak)
    {
        RendererState::Snapshot before;
        before.Valid = true;
        const RendererState::Snapshot after = before;

        std::string detail;
        EXPECT_EQ(RendererState::Describe(before, after, detail), 0u);
        EXPECT_TRUE(detail.empty());
    }

    // A changed settings struct is reported by its type name, so the summary
    // line says WHAT leaked rather than only that something did.
    TEST(RendererStateCheckTest, NamesTheChangedSettingsStruct)
    {
        RendererState::Snapshot before;
        before.Valid = true;
        RendererState::Snapshot after = before;
        after.Renderer.Path = (before.Renderer.Path == RenderingPath::Deferred)
                                  ? RenderingPath::Forward
                                  : RenderingPath::Deferred;

        std::string detail;
        EXPECT_EQ(RendererState::Describe(before, after, detail), 1u);
        EXPECT_NE(detail.find("RendererSettings"), std::string::npos) << detail;
    }

    // A changed scalar toggle is reported with both values, because "culling is
    // off" is only actionable once you know it was on when the test started.
    TEST(RendererStateCheckTest, NamesTheChangedScalarToggleWithBothValues)
    {
        RendererState::Snapshot before;
        before.Valid = true;
        before.FrustumCulling = true;
        RendererState::Snapshot after = before;
        after.FrustumCulling = false;

        std::string detail;
        EXPECT_EQ(RendererState::Describe(before, after, detail), 1u);
        EXPECT_NE(detail.find("FrustumCullingEnabled"), std::string::npos) << detail;
        EXPECT_NE(detail.find("true"), std::string::npos) << detail;
        EXPECT_NE(detail.find("false"), std::string::npos) << detail;
    }

    // Several leaks in one test are all reported, not just the first. A fixture
    // that switches the path usually drags a couple of toggles with it, and
    // fixing one of three is how this bug survived two previous rounds.
    TEST(RendererStateCheckTest, ReportsEveryChangedEntry)
    {
        RendererState::Snapshot before;
        before.Valid = true;
        before.FrustumCulling = true;
        RendererState::Snapshot after = before;
        after.FrustumCulling = false;
        after.PostProcess.BloomEnabled = !before.PostProcess.BloomEnabled;
        after.Fog.Enabled = !before.Fog.Enabled;

        std::string detail;
        EXPECT_EQ(RendererState::Describe(before, after, detail), 3u);
        EXPECT_NE(detail.find("PostProcessSettings"), std::string::npos) << detail;
        EXPECT_NE(detail.find("FogSettings"), std::string::npos) << detail;
        EXPECT_NE(detail.find("FrustumCullingEnabled"), std::string::npos) << detail;
    }

    // The round trip against the LIVE renderer: capture, mutate the way a
    // polluting test does, restore, and confirm the configuration is back —
    // including the render graph, which `ApplyRendererSettings` reconfigures.
    // A restore that put the structs back but left the graph built for the
    // other path would pass a struct comparison and still render every later
    // test through the wrong pipeline.
    TEST(RendererStateCheckTest, CaptureRestoreRoundTripsLiveConfiguration)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        if (!Renderer3D::IsInitialized())
            Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
        ASSERT_TRUE(Renderer3D::IsInitialized());

        RendererState::Snapshot before;
        ASSERT_TRUE(RendererState::Capture(before));
        EXPECT_TRUE(before.Valid);

        // Mutate exactly as the leak this issue was opened for did: switch the
        // rendering path and flip a toggle, and apply it so the graph follows.
        const RenderingPath originalPath = Renderer3D::GetRendererSettings().Path;
        Renderer3D::GetRendererSettings().Path =
            (originalPath == RenderingPath::Deferred) ? RenderingPath::Forward
                                                      : RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        Renderer3D::EnableFrustumCulling(!before.FrustumCulling);
        Renderer3D::GetFogSettings().Enabled = !before.Fog.Enabled;

        RendererState::Snapshot dirty;
        ASSERT_TRUE(RendererState::Capture(dirty));
        std::string leaked;
        EXPECT_GE(RendererState::Describe(before, dirty, leaked), 2u) << leaked;

        RendererState::Restore(before);

        RendererState::Snapshot restored;
        ASSERT_TRUE(RendererState::Capture(restored));
        std::string residue;
        EXPECT_EQ(RendererState::Describe(before, restored, residue), 0u)
            << "Restore left renderer configuration changed: " << residue;
        EXPECT_EQ(Renderer3D::GetRendererSettings().Path, originalPath);
    }
} // namespace OloEngine::Tests
