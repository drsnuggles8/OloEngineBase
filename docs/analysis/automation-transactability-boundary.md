# Which automation commands are transactable (issue #1127)

**The rule: a command may be a transaction step only if it has DECLARED how it is taken back, and
declared that it is safe to run on the main thread.** `AutomationCommand::Undo` is the first
declaration and `MainMarshaled` the second; the layer default-denies anything that has not made both.
Measured over the real registered surface on 2026-09-11:

| `AutomationUndo` | Commands | Transactable | Why |
|---|---:|---|---|
| `EditorUndoStack` | 10 | 9 | Its `CommandHistory` entry is what rollback replays in reverse. |
| `None` | 6 | 3 | It does not mutate, so there is nothing to take back. |
| `Irreversible` | 6 | no | The effect cannot be undone from inside the process. |
| `HistoryControl` | 3 | no | It walks or rewrites the undo stack a transaction is already grouping. |
| `Unspecified` | 96 | no | Never declared. Assuming reversibility here is the failure this layer exists to prevent. |
| **total** | **121** | **12** | |

The four reversible commands that are nonetheless refused are the second rule: `olo_asset_get`,
`olo_asset_references`, `olo_asset_move` and `olo_build_list` are registered `MainMarshaled = false`
*on purpose*, because each does a whole-project or whole-build-tree scan that must stay on the
handler thread. A batch runs every step inside one main-thread critical section, so admitting one
would freeze the editor for the scan's duration — the exact thing their registration comment warns
against. Reversibility was necessary but not sufficient, and that only became visible once the
execution model existed.

The census is asserted, printed and ratcheted by
`McpAutomationTransactionCensus.EveryRegisteredCommandsTransactabilityIsClassifiedAndRatcheted`, so
these numbers cannot go stale without a test failing.

## The 12, by family

- **Entity authoring** — `olo_entity_create`, `_destroy`, `_duplicate`, `_reparent`.
- **Component authoring** — `olo_component_add`, `_remove`, `_get`, `_list_types`.
- **Scene document** — `olo_scene_status`, `_new`, `_save`, `_save_as`.

That is the whole structural-authoring surface, which is the surface batching was asked for.

## What counts as a step FAILING

The registry refusing to run it, or the handler returning an error result. Nothing else. A command
that completes and reports inside its own payload that it declined to do something (`refused: true`,
`changed: false`) has succeeded as far as this layer is concerned, and the batch continues. Reading a
domain's payload fields to decide whether a batch aborts would put one domain's vocabulary inside the
cross-cutting layer, which is the coupling the issue exists to avoid. A command that wants to abort a
batch says so with an error result.

## The answer the issue wanted: irreversible commands are simply not transactable

The issue scored `confidence: 0.5` on exactly this question and said the honest answer might be that
some operations are not transactable rather than pretending to roll back. It is.

The six `Irreversible` commands are `olo_asset_create`, `olo_asset_delete`, `olo_asset_import`,
`olo_asset_reimport`, `olo_asset_import_settings` and `olo_build_invoke`. Each writes outside the
scene graph — a file created, a file deleted, an importer's output, a build tree — and nothing in the
editor's undo stack can retract that. A transaction containing one is **refused at build time**, with
a message saying to run it on its own before or after the batch. No partial-rollback mode was built,
because a rollback that restores the scene while leaving an imported asset behind is a worse lie than
a refusal.

## What the spike found that the metadata could not say

`AutomationUndo` had four values and none of them fit `olo_editor_undo` / `olo_editor_redo`. They are
not irreversible — undoing an undo is a redo, and Ctrl-Z reaches them normally — but they cannot be
a transaction *step*, because a transaction is itself a rewrite of the stack they would be walking.
Left as `EditorUndoStack` they would have been admitted and then failed somewhere inside the batch.

So the enum gained a fifth value, `HistoryControl`, and those two adopted it. `olo_transaction_apply`
declares it too, which is what stops a transaction nesting inside another — one rule, three cases, no
name blacklist inside the transaction layer.

## Why `Unspecified` is 96 and why that is not a backlog

Those 96 are the older diagnostics, capture, camera, render-override and perf commands. Most are
read-only and would classify as `None` in a minute's work — but doing that as part of this issue
would have been 96 unreviewed declarations riding along with the layer that consumes them, and a
wrong one is invisible until a batch half-applies. They stay denied until someone declares them
deliberately, which costs a batch nothing it can do today: everything structural authoring needs is
already in the 12.

The reverse is what the ratchet protects. A command that declares `ProjectWrite` **and** `Undo::None`
is a contradiction — mutating with nothing to take back — and it is the one shape that would let a
non-reversible write into a batch through the front door. The census test refuses the combination
outright rather than trusting the declaration; nothing in the surface has it today.

## Where the boundary is enforced

`ClassifyTransactability` in
[`AutomationTransaction.h`](../../OloEditor/src/Automation/AutomationTransaction.h) is the only place
the rule is written down, and `BuildTransaction` is the only thing that reads it. Nothing executes
until every step in the batch is admissible, so a refusal is always a refusal *before* the first
mutation — never half-way through.
