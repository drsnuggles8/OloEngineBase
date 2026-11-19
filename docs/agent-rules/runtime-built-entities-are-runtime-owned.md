# Entities a runtime system builds are runtime-owned

**Mark every entity a runtime system creates for itself with `Scene::MarkRuntimeOwned`: UI it lays
out, overlays, debris, projectiles, decals. The save-game skips a marked entity and every entity
parented under it, so the system must rebuild what it owns after a load, and the saved state it
rebuilds from goes on an authored entity (or in a framed settings block).**

- **Mark the root of a hierarchy, at creation.** `Scene::IsRuntimeOwned` is true for the marked
  entity and its descendants, so a menu parented under one marked canvas needs one call. Mark
  separately anything not parented under the root, such as the dialogue choice buttons.
- **Rebuild after a load into a running scene.** `SaveGameSerializer::RestoreSceneState` swaps the
  whole registry, so a system that keeps running holds the UUIDs of entities that are gone. Check
  that the hierarchy is alive on update and rebuild it: `DialogueUIController::IsHierarchyIntact`,
  `SubtitleSystem::EnsureEntities`, `RuntimeInputRebindMenu::CanvasIsLive`. Hold UUIDs, not raw
  `Entity` handles: after the swap a stale handle names a restored entity, and destroying through it
  deletes authored content.
- **Save the state, not the picture of it.** The dialogue UI is rebuilt from the
  `DialogueStateComponent` on the speaking NPC, which the save carries.
- **Never exclude by tag.** An authored entity may share a display name with a runtime one; only the
  mark tells them apart.
- `RuntimeOwnedTag` is deliberately not a `*Component`, so it stays out of every generated list
  ([component-serializer-codegen.md](component-serializer-codegen.md) rule 4). Scene YAML needs no
  filter: Play runs on a copy of the edit scene, and only that copy holds runtime-owned entities.
- Excluding a component from the save (`kComponentsNotInSaveGame`) does **not** exclude its entity.
  The entity is still saved without it and comes back as a husk.

## The failure (#1712)

The save captured every entity with an `IDComponent`. `DialogueUIController::Initialize` builds four
entities at runtime start; the save stored them, and Continue restored them and then built four
more. Each save, Continue, save cycle grew a 30-entity scene by four, with no dialogue needed. The
save also did not carry the conversation itself, so a Continue mid-dialogue showed the old UI over
nothing.

The same sweep found three more instances of the class. Debris, projectiles and impact decals had
their marker components in `kComponentsNotInSaveGame`, so a save mid-explosion stored each chunk as
a permanent body without its lifetime, and each decal without its fade-out. The caption overlay and
the open rebind menu were saved like the dialogue UI.

Pinned by `DialogueUIRebuiltOnContinueTest` (the cycle in three dialogue states, a load into the
running scene, a refused v50 save) and `SaveGameRuntimeOwned.MarkedSubtreesAreNotCapturedAndSavedParentsForgetThem`.
