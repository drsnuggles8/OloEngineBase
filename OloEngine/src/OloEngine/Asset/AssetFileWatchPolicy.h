#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"

namespace OloEngine
{
    /**
     * @brief What the editor file watcher should do with a single filesystem event.
     *
     * Decoupled from the watcher library so the decision is a pure function that
     * can be unit-tested without a live filewatch thread or asset registry.
     */
    enum class FileWatchAction : u8
    {
        Ignore = 0, ///< Not actionable: drop event, non-asset, vanished file, or unloaded tracked asset.
        Import,     ///< Untracked importable file that exists on disk -> register it in the registry.
        Reload,     ///< Already-tracked asset that is currently loaded changed -> hot-reload it.
    };

    /**
     * @brief Inputs the auto-import / hot-reload decision needs, gathered by the caller.
     *
     * Keeping these as plain booleans (rather than the filewatch enum or live
     * registry handles) is what makes DecideFileWatchAction a pure function.
     */
    struct FileWatchDecisionInput
    {
        /// added / modified / renamed_new — i.e. "the file now exists at this path".
        /// removed / renamed_old are drops and never set this.
        bool IsPresenceEvent = false;
        /// AssetType resolved from the extension; None means "not an importable file".
        AssetType Type = AssetType::None;
        /// The path resolves to a real, regular file on disk right now.
        bool ExistsAsRegularFile = false;
        /// The path already has a handle in the asset registry.
        bool AlreadyTracked = false;
        /// The tracked asset is currently in the loaded-asset cache.
        bool CurrentlyLoaded = false;
    };

    /**
     * @brief Decide how to react to a filesystem event for a path under the project.
     *
     * Rules (in order):
     *  - A non-presence event (removed / renamed_old) or a path that maps to no
     *    AssetType is ignored — this also covers the registry's own AssetRegistry.oar,
     *    .meta sidecars, and partial-download / temp extensions, none of which are
     *    in the extension map, so persisting the registry can't re-trigger an import.
     *  - An already-tracked asset is hot-reloaded only when it is currently loaded.
     *    Reloading an asset nobody has loaded is wasted work, and — critically —
     *    suppresses the burst of partial-content reloads that a large new file
     *    would otherwise generate as it streams onto disk during a copy (the file
     *    is auto-imported on the first event, then every subsequent write event
     *    would hit this branch). An unloaded asset just loads current on first use.
     *  - An untracked path is imported only if it is a real file on disk now (the
     *    event may describe a file that was already deleted, or a directory that
     *    coincidentally shares an asset extension).
     */
    [[nodiscard]] constexpr FileWatchAction DecideFileWatchAction(const FileWatchDecisionInput& in) noexcept
    {
        if (!in.IsPresenceEvent || in.Type == AssetType::None)
            return FileWatchAction::Ignore;

        if (in.AlreadyTracked)
            return in.CurrentlyLoaded ? FileWatchAction::Reload : FileWatchAction::Ignore;

        return in.ExistsAsRegularFile ? FileWatchAction::Import : FileWatchAction::Ignore;
    }

} // namespace OloEngine
