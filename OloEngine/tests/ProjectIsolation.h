#pragma once

// Every test and every suite leaves the active Project and its asset manager as
// it found them.
//
// Project::s_ActiveProject and Project::s_AssetManager are process-wide, and the
// asset manager owns every asset loaded through it. A test that loads a project
// and returns without unloading it keeps those assets resident for every test
// after it, until some later test happens to load another project. In a
// whole-suite process that is gigabytes: FoxCoatEvidenceTest left its fox models,
// coats and textures active (+1.3 GB that stayed), and the AMD nightly crossed its
// 6 GiB ceiling in AcceptanceSceneTest on what the evidence suites before it had
// kept. Forty-odd test files switch the project without restoring it, and a test
// that reuses "whatever project is active" picks up the previous test's assets.
//
// This listener is the one owner of that restore, as CleanSlateListener is for
// temp directories: it snapshots both references when a suite and a test start
// and puts them back when they end, logging which test left a project behind.
// gtest destroys the test object before OnTestEnd, so the fixture's own
// references are gone by then and the restore releases what only the asset
// manager still held.

namespace OloEngine::Tests::ProjectIsolation
{
    // Appends the listener; call after InitGoogleTest and before RUN_ALL_TESTS.
    void RegisterListener();
} // namespace OloEngine::Tests::ProjectIsolation
