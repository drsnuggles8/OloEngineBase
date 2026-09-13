#pragma once

// Prefab automation: instantiate, unpack, override query, apply/revert, and
// create-from-subtree (issue #1129, Epic G of the automation control plane).
//
// The engine already had the prefab OPERATIONS (Scene/Prefab.h: Instantiate,
// DetectOverrides, ApplyComponentToPrefab, RevertComponent,
// UpdateInstanceFromPrefab). What it had no notion of was UNDO, and that is the
// hard half, because an apply is the one editor operation in this codebase that
// mutates state in THREE places at once:
//
//   * the source prefab's own Scene, which is not the active scene;
//   * every other open instance of that prefab in the active scene, which
//     re-syncs from the source;
//   * the .oloprefab file, or the change is lost on the next editor start.
//
// One Ctrl-Z has to take all three back together, so the memento here is keyed
// by (scene, entity) rather than by entity, and the file half rides in the same
// undo entry. This is NOT what the transaction layer (#1127) provides: that
// makes N automation CALLS one undo entry, whereas this is ONE call whose single
// entry spans several objects. A transaction over per-entity commands could not
// express it at all -- there is no registry command that writes into a prefab's
// private scene.

namespace OloEngine::Automation
{
    class AutomationRegistry;

    void RegisterPrefabCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
