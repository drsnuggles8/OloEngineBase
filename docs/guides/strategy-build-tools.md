# Strategy camera and build tools

Open `OloEditor/KestrelProject/Kestrel.oloproj` and enter Play. This scene uses a procedural 10 km prototype mountain.

| Input | Action |
| --- | --- |
| WASD, arrow keys, or viewport edges | Pan |
| Middle drag | Pan by the pointer displacement |
| Right drag or Q/E | Orbit. Right drag also changes pitch |
| Wheel or Page Up/Down | Zoom between 20 m and the full mountain view |
| 1–5 | Select ticket office, ski school, restaurant, parking, or hut |
| Left click / Esc | Confirm the green preview or cancel the active tool |
| Ctrl+Z / Ctrl+Y | Undo or redo the last placement |

## Add a build tool

Use one `BuildHistory` per playing scene and share it between buildings, lifts, and trails. Keep the
history in the runtime controller so stopping Play clears it. Implement `IBuildCommand` for a fixed
candidate, with enough data to recreate and revert the edit:

- `Preview()` synchronously returns a validity reason and the current price. The tool supplies the
  pricing policy and queries terrain and occupancy. `BuildFootprint` checks one axis-aligned piece
  against a normal sampler and another footprint. A lift can supply one piece per tower. A trail
  can supply pieces along its route and add grading constraints.
- `ShowGhost(preview)` updates the tool's transient visual using that quote. The tool reuses and
  clears its own preview entities when the candidate leaves the terrain, is confirmed, or is
  cancelled.
- `BuildHistory.TryConfirm(command)` requests a fresh quote and records a step only after
  `TryCommit(cost)` succeeds. Revalidate the gameplay constraints before mutating anything. If
  an operation has several failure points, roll back its partial changes before returning false.
- `TryRevert(cost)` reverses the exact edit and refunds the recorded amount. Redo receives that
  same amount, even if pricing changes later. A failed undo, redo, or confirm leaves the history
  in place. Only a successful new confirm clears redo.

KESTREL uses the existing scene spawn queue. `Scene.CreateBuildBox` queues an ordinary tinted
primitive and returns the reserved entity ID. Wait for the scene tick to drain before reading the
entity's components. If the command is undone, wait for the destroy to drain before asking redo to
restore that ID. Keep the original pose, dimensions and ID in the command.

## Camera and picking

`StrategyCamera` drives an unparented perspective camera. Supply XZ focus limits, zoom limits, and
upper and lower bounds that enclose the terrain and colliders. `StepFromInput` reads viewport-local
input. For scripted motion use `Step`, where pan is a rate and drag, orbit and zoom are per-call
displacements. The rig follows the terrain with a downwards Jolt ray and keeps the whole near plane
clear with a sphere sweep around the camera.

Supply focus and ground limits in the scene frame at the first step. When floating-origin rebasing
shifts the scene, the controller moves the focus and those limits by the same amount.
KestrelStart keeps a fixed origin. Tools in a rebasing scene should store placement positions in
absolute coordinates and resolve them into the current scene frame when applying or reverting.

The KESTREL controller queries `Camera.RenderedScreenToWorldRay` against the displayed primary
camera, including its interpolated pose. It converts the top-left viewport input Y to the ray API's bottom-left
coordinate. The ray origin follows a rebase between render and click, and the controller accepts
placement ray hits only from the prototype terrain. The preview rejects
occupied footprints and slopes over 18°. The engine's async GPU editor picker remains available
for authoring tools.

## Verify the ten-placement replay

1. Enter Play and record the initial entity IDs and balance. With MCP, list the entities using
   `olo_scene_list_entities`. Read each building's `TransformComponent` with
   `olo_scene_get_entity`. Exclude the transient entity named `Placement preview`.
2. Select tools 1 through 5 twice. Confirm each at a separate green preview on flat terrain.
   Pan or zoom out to find another site when a preview turns red. Record the ten buildings'
   UUIDs, translations, rotations, and scales. The balance should be 2,600 after spending 7,400.
3. Cancel the preview and press Ctrl+Z ten times, releasing the keys between steps. The entity IDs
   should match the initial list and the balance should return to 10,000.
4. Press Ctrl+Y ten times. Compare the ten UUIDs and all transform values to the placed snapshot.
   The balance should return to 2,600. Clicking an overlapping site or a steep site should leave
   the entity list, history counts, and balance unchanged.
