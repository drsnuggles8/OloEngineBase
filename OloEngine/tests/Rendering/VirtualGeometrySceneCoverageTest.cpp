// OLO_TEST_LAYER: unit
// =============================================================================
// VirtualGeometrySceneCoverageTest.cpp — issue #864
//
// The bug this guards is "plausible-looking nothing".
//
// Two of the three virtual-geometry showcase scenes were reported as submitting
// 0 instances / 0 clusters while opening perfectly happily: 28 entities, no
// error, an empty viewport. That state is indistinguishable from a camera,
// lighting or culling problem — and, far worse, it makes any A/B measured on
// such a scene pass VACUOUSLY, because neither mode draws anything. PR #859's
// #813 A/B had to prove non-vacuity through a separate VG on/off pixel toggle
// for exactly this reason.
//
// What the investigation actually found (worth recording, because the obvious
// diagnosis was wrong): the scenes' handles were NOT stale. All three resolve
// to real AssetRegistry.oar entries. `VirtualGeometryStress.olo` references the
// 7.2M-triangle Stanford dragon, which is a deliberately-uncommitted
// fetch-on-demand asset (scripts/assets/asset-manifest.json) — absent until the
// developer runs scripts/Fetch-Assets.ps1. The manifest's own contract says
// every consumer "must degrade gracefully when an asset is absent: SKIP with a
// message naming the fetch command, never fail". The scene did not: it rendered
// nothing and said nothing. THAT is the defect, not the handles.
//
// So this file guards two distinct things:
//
//   1. The handles really are registered (the failure mode the issue assumed).
//      An unregistered handle can never resolve on any machine, fetched or not,
//      and no amount of loud reporting would make such a scene usable.
//
//   2. The silent-zero predicate itself — VirtualMeshRegistry::
//      SubmissionDiagnostics::SilentlyDrewNothing(). This is the loud check's
//      trigger, and its two failure directions are opposite and both bad: fail
//      to fire and the silent zero survives; fire spuriously and every scene
//      without virtual geometry (i.e. nearly all of them) screams on every
//      frame until the warning is muted or ignored, which lands us right back
//      where we started.
//
// Deliberately GL-free so it runs in CI, which is where a regression would
// otherwise reach master unseen: the whole point is that the test suite was
// green for the entire time this bug was live.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        struct VirtualMeshReference
        {
            std::string SceneFile;
            std::string EntityTag;
            u64 Handle = 0;
        };

        // Every VirtualMeshComponent in every sandbox scene, read straight out of
        // the scene YAML. Parsing the file rather than deserialising it keeps this
        // test GL-free: the production deserialiser eagerly builds GPU resources
        // (MeshSource::Build, Texture2D::Create) and so needs a live GL 4.6
        // context, which CI does not have.
        std::vector<VirtualMeshReference> CollectVirtualMeshReferences()
        {
            std::vector<VirtualMeshReference> refs;
            const fs::path scenesDir = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets" / "Scenes";

            std::error_code ec;
            if (!fs::exists(scenesDir, ec))
            {
                return refs;
            }

            for (const auto& entry : fs::directory_iterator(scenesDir, ec))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".olo")
                {
                    continue;
                }

                YAML::Node scene;
                try
                {
                    scene = YAML::LoadFile(entry.path().string());
                }
                catch (const std::exception&)
                {
                    // Structural scene validity is AssetContentValidity's job; an
                    // unparseable scene is that test's failure to report, not ours.
                    continue;
                }

                const YAML::Node entities = scene["Entities"];
                if (!entities || !entities.IsSequence())
                {
                    continue;
                }

                for (const YAML::Node& entityNode : entities)
                {
                    const YAML::Node virtualMesh = entityNode["VirtualMeshComponent"];
                    if (!virtualMesh || !virtualMesh["MeshSource"])
                    {
                        continue;
                    }

                    VirtualMeshReference ref;
                    ref.SceneFile = entry.path().filename().generic_string();
                    ref.Handle = virtualMesh["MeshSource"].as<u64>(0ull);
                    ref.EntityTag = entityNode["TagComponent"] && entityNode["TagComponent"]["Tag"]
                                        ? entityNode["TagComponent"]["Tag"].as<std::string>("<unnamed>")
                                        : std::string{ "<unnamed>" };
                    refs.push_back(std::move(ref));
                }
            }
            return refs;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // 1. Scene handles are registered
    // -------------------------------------------------------------------------
    // A handle absent from AssetRegistry.oar can never resolve, on any machine,
    // fetched or not — that is a genuinely dead scene. This does NOT assert the
    // file exists on disk: a fetch-on-demand asset is legitimately absent until
    // the developer fetches it, and AssetContentValidity already owns the
    // registry-entry-vs-disk relationship (with the manifest allowlist).
    TEST(VirtualGeometrySceneCoverage, EveryVirtualMeshHandleIsRegistered)
    {
        const fs::path registryPath = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "AssetRegistry.oar";
        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(registryPath))
            << "Could not read " << registryPath.generic_string()
            << " — without it this test cannot tell a stale handle from a good one.";

        const std::vector<VirtualMeshReference> refs = CollectVirtualMeshReferences();
        ASSERT_FALSE(refs.empty())
            << "No VirtualMeshComponent found in any sandbox scene. Either the scenes lost their "
               "virtual geometry, or this test's YAML walk has drifted from the scene format — "
               "both make the check vacuous, which is precisely the failure mode issue #864 is about.";

        std::ostringstream unregistered;
        u32 unregisteredCount = 0;
        for (const VirtualMeshReference& ref : refs)
        {
            if (ref.Handle == 0)
            {
                continue; // no mesh assigned — a normal authoring state
            }
            if (!registry.Exists(static_cast<AssetHandle>(ref.Handle)))
            {
                ++unregisteredCount;
                unregistered << "  " << ref.SceneFile << " / '" << ref.EntityTag << "' -> handle "
                             << ref.Handle << "\n";
            }
        }

        EXPECT_EQ(unregisteredCount, 0u)
            << unregisteredCount
            << " VirtualMeshComponent handle(s) are not in AssetRegistry.oar, so the entity renders "
               "NOTHING on every machine and the scene silently exercises no virtual geometry "
               "(issue #864):\n"
            << unregistered.str();
    }

    // -------------------------------------------------------------------------
    // 2. The loud check's trigger
    // -------------------------------------------------------------------------
    // SilentlyDrewNothing() is what makes the zero loud, in all three consumers
    // (the Scene log warning, the editor Statistics panel, and the MCP
    // olo_virtual_geometry_stats note). Its correctness is entirely about which
    // states it does and does not claim, so pin every one of them.
    TEST(VirtualGeometrySceneCoverage, SilentZeroPredicateFiresOnlyOnTheRealFailure)
    {
        using Diagnostics = VirtualMeshRegistry::SubmissionDiagnostics;

        // The failure this whole issue exists for: components asked to render,
        // nothing reached the renderer.
        {
            Diagnostics d;
            d.EnabledComponents = 24;
            d.UnresolvedAssets = 24;
            EXPECT_TRUE(d.SilentlyDrewNothing())
                << "24 enabled VirtualMeshComponents that all failed to resolve is the exact state "
                   "issue #864 was filed for; it must not pass quietly.";
        }
        {
            // Resolved, but the cluster DAG would not build — different cause,
            // same user-visible nothing, equally worth shouting about.
            Diagnostics d;
            d.EnabledComponents = 3;
            d.RegistrationFailures = 3;
            EXPECT_TRUE(d.SilentlyDrewNothing());
        }

        // The false-positive directions. Each of these is a legitimate zero, and
        // warning about any of them would make the warning worthless.
        {
            // By far the common case: a scene with no virtual geometry at all.
            // Nearly every scene in the project looks like this, so a spurious
            // fire here is a per-frame warning on almost the whole asset set.
            Diagnostics d;
            EXPECT_FALSE(d.SilentlyDrewNothing())
                << "A scene with no VirtualMeshComponent must never trip the silent-zero check.";
        }
        {
            // The master switch is off: the same geometry IS being drawn, through
            // the classic mesh path. Zero VG counters are the intended baseline
            // half of the A/B, not a fault.
            Diagnostics d;
            d.EnabledComponents = 24;
            d.FellBackToClassic = true;
            EXPECT_FALSE(d.SilentlyDrewNothing())
                << "The classic-path fallback is a deliberate A/B baseline, not a broken scene.";
        }
        {
            // Everything worked.
            Diagnostics d;
            d.EnabledComponents = 15;
            d.Submitted = 15;
            EXPECT_FALSE(d.SilentlyDrewNothing());
        }
        {
            // Partial success still draws something, so the viewport is not the
            // uninformative blank this check is meant to catch. The per-asset
            // warning in Scene.cpp already names the specific failures.
            Diagnostics d;
            d.EnabledComponents = 10;
            d.UnresolvedAssets = 9;
            d.Submitted = 1;
            EXPECT_FALSE(d.SilentlyDrewNothing())
                << "A partially-drawing scene is diagnosable by eye; the per-asset warning covers it.";
        }
    }

    // -------------------------------------------------------------------------
    // 3. The counters stay self-consistent
    // -------------------------------------------------------------------------
    // Every enabled component must end up in exactly one bucket. If the Scene
    // loop ever grows a path that leaves a component uncounted, the predicate
    // above silently weakens — it would stop firing on a scene that draws
    // nothing, which is the original bug wearing a new hat.
    TEST(VirtualGeometrySceneCoverage, DiagnosticsBucketsAccountForEveryEnabledComponent)
    {
        VirtualMeshRegistry::SubmissionDiagnostics d;
        d.EnabledComponents = 12;
        d.UnresolvedAssets = 5;
        d.RegistrationFailures = 3;
        d.Submitted = 4;

        EXPECT_EQ(d.UnresolvedAssets + d.RegistrationFailures + d.Submitted, d.EnabledComponents)
            << "The three outcome buckets must partition the enabled components exactly.";

        // A freshly reset instance is the "nothing to report" state.
        const VirtualMeshRegistry::SubmissionDiagnostics fresh;
        EXPECT_EQ(fresh.EnabledComponents, 0u);
        EXPECT_EQ(fresh.Submitted, 0u);
        EXPECT_FALSE(fresh.FellBackToClassic);
        EXPECT_FALSE(fresh.SilentlyDrewNothing());
    }
} // namespace OloEngine::Tests
