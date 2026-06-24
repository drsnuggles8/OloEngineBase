// OLO_TEST_LAYER: unit
// =============================================================================
// AssetFileWatchPolicyTest.cpp
//
// Unit test for DecideFileWatchAction (OloEngine/Asset/AssetFileWatchPolicy.h),
// the pure decision the editor's filesystem watcher uses to choose between
// importing a new asset, hot-reloading a changed one, or ignoring the event.
//
// This is the auto-import feature's control-flow core, factored out of
// EditorAssetManager::OnFileSystemEvent specifically so it can be exercised
// without a live filewatch thread, asset registry, or GL context. The live
// editor wiring (event broadening, registry persist, Content Browser refresh)
// is verified by running the editor; here we pin the branch logic that decides
// *whether* to act so a future refactor can't silently regress it — e.g.
// re-introducing the "reload an unloaded asset" behavior that would spam
// partial-content loads while a large new file is still copying onto disk.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetFileWatchPolicy.h"
#include "OloEngine/Asset/AssetTypes.h"

using namespace OloEngine;

namespace
{
    // Baseline: a present, importable, untracked file that exists on disk —
    // the canonical "new asset dropped in" case. Individual tests flip one
    // field to isolate each branch.
    FileWatchDecisionInput NewImportableFile()
    {
        FileWatchDecisionInput in;
        in.IsPresenceEvent = true;
        in.Type = AssetType::Texture2D;
        in.ExistsAsRegularFile = true;
        in.AlreadyTracked = false;
        in.CurrentlyLoaded = false;
        return in;
    }
} // namespace

// ----------------------------------------------------------------------------
// Import branch
// ----------------------------------------------------------------------------

TEST(AssetFileWatchPolicyTest, UntrackedExistingImportableFileIsImported)
{
    EXPECT_EQ(DecideFileWatchAction(NewImportableFile()), FileWatchAction::Import);
}

TEST(AssetFileWatchPolicyTest, UntrackedFileThatVanishedIsIgnored)
{
    // The event may describe a file already deleted, or a directory that merely
    // shares an asset extension — only import a real, existing regular file.
    auto in = NewImportableFile();
    in.ExistsAsRegularFile = false;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Ignore);
}

// ----------------------------------------------------------------------------
// Reload branch — only for already-loaded tracked assets
// ----------------------------------------------------------------------------

TEST(AssetFileWatchPolicyTest, TrackedLoadedAssetIsReloaded)
{
    auto in = NewImportableFile();
    in.AlreadyTracked = true;
    in.CurrentlyLoaded = true;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Reload);
}

TEST(AssetFileWatchPolicyTest, TrackedButUnloadedAssetIsIgnored)
{
    // Hot-reloading an asset nobody has loaded is wasted work, and suppresses
    // the burst of partial-content reloads a large new file would otherwise
    // trigger as it streams onto disk (imported on the first event, then every
    // subsequent write event would land here). It loads current on first use.
    auto in = NewImportableFile();
    in.AlreadyTracked = true;
    in.CurrentlyLoaded = false;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Ignore);
}

TEST(AssetFileWatchPolicyTest, ExistenceIsIrrelevantOnceTracked)
{
    // The existence stat is only consulted on the untracked import path; a
    // tracked-loaded asset reloads regardless (ReloadData handles a missing
    // file by substituting a placeholder).
    auto in = NewImportableFile();
    in.AlreadyTracked = true;
    in.CurrentlyLoaded = true;
    in.ExistsAsRegularFile = false;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Reload);
}

// ----------------------------------------------------------------------------
// Ignore branch — non-actionable events / non-assets
// ----------------------------------------------------------------------------

TEST(AssetFileWatchPolicyTest, NonPresenceEventIsAlwaysIgnored)
{
    // removed / renamed_old map to IsPresenceEvent == false; never act on a drop,
    // even for a tracked-loaded asset.
    auto in = NewImportableFile();
    in.AlreadyTracked = true;
    in.CurrentlyLoaded = true;
    in.IsPresenceEvent = false;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Ignore);
}

TEST(AssetFileWatchPolicyTest, NonAssetTypeIsIgnored)
{
    // AssetType::None covers extensions outside the map: temp/partial-download
    // files, .meta sidecars, and the registry's own AssetRegistry.oar — so
    // persisting the registry after an import can't re-trigger an import loop.
    auto in = NewImportableFile();
    in.Type = AssetType::None;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Ignore);

    // ...even if it happens to be a tracked, loaded, existing path.
    in.AlreadyTracked = true;
    in.CurrentlyLoaded = true;
    EXPECT_EQ(DecideFileWatchAction(in), FileWatchAction::Ignore);
}
