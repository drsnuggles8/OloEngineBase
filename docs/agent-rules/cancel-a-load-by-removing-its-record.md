# Cancel an async load by removing its record, and prove the removal with a later request

A cancelled load's result is dropped because its record leaves the pending set, under the lock the
retrieve takes. It is not dropped because some state flag says "cancelled". Test that with a second
request for the same key that is still pending when the stale result lands. Issue #1365 built
cancellation for `RuntimeAssetSystem` (pack assets) and `SceneStreamer` (regions) on one state
machine, `TCancellableLoadSet` in `Asset/AssetSystem/CancellableLoadSet.h`. Use it for the next
async loader instead of writing another.

## The rules

1. **The pending record is the only path from result to caller.** `Cancel()` moves the record out
   of the live set: to the abandoned list if the worker is queued or running, to the caller if the
   worker has finished. `ExtractCompleted()` reads only live records. A queued load is stopped by
   one compare-exchange on `FAssetLoadTicket` (Queued to Cancelled, against the worker's Queued to
   Running), so "cancelled before start" is guaranteed, not guessed from timing.
2. **Keep abandoned tasks until they finish, and have shutdown wait for them.** An abandoned asset
   load still calls back into `RuntimeAssetManager`. If `StopAndWait()` waits only for live loads,
   the worker reads a destroyed manager.
3. **Release dropped results after unlocking.** A result can hold the last reference to an asset,
   and its destructor must not run under the non-recursive `m_AssetsMutex`. The set hands dropped
   records back through `outDropped` so the caller destroys them after it unlocks.
4. **Take finished loads out of the pending set before instantiating any of them.** Instantiating a
   region runs script `OnCreate`, which can load, unload or cancel regions. A loop that keeps an
   index or a reference into the pending array across that call reads a moved or reallocated
   element.

## What stayed green

- **The region-state check hid a missing discard.** A negative-control build removed the region
  discard: `CancelRegionLoad` left the load pending. The mid-parse tests still passed on the entity
  assertion, because `ProcessCompletedLoads` also skips a region that is no longer `Loading`. That
  check stops working once the same region is requested again: the region is `Loading` again, and
  the stale parse arrives first. `AnAbandonedParseCannotLandInALaterRequestForTheSameRegion`
  rewrites the file, re-requests the region behind a start gate, and releases the stale worker
  first. With the discard removed, the stale entity (5001) lands and the live one (5002) never does.
- **A failed assertion hung the suite instead of failing it.** The hooks captured `Signal`s and
  atomics from the test's stack. When an `ASSERT` returned early, TearDown's shutdown waited on a
  worker that was touching those destroyed locals, and the run sat until the 900 s kill. Keep hook
  state behind a `shared_ptr` that each hook captures by value (`TestAsyncLoadHooks.h`), and
  release every blocked worker and start gate in TearDown before shutting down.

## Making the race real

- Block on events, never sleeps. `OnLoadStarted` and `OnLoadFinished` run on the worker before and
  after the real pack read or parse. A `StartGate` task-event prerequisite holds a load genuinely
  queued.
- The unhooked queue-then-cancel loop prints its mix. A typical run is `beforeStart=74 inFlight=226
  completed=0` over 300 iterations, so the race is actually exercised.
- For "shutdown waits", record the order: the worker and the shutdown thread each take the next
  number from a shared counter. A 200 ms window gives a wrong implementation time to return early,
  and it cannot make a correct one fail.
