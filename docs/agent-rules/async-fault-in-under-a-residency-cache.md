# Hold the bytes before you take the slot

Applies to: any cache whose entries are filled from an **asynchronous** source — a disk read, a
network fetch, a decode on another thread — while a **synchronous** policy decides which entry to
evict. Written from issue #1151 (on-disk page streaming for virtual geometry), but the rule is not
about geometry.

---

## 1. The rule

A fault-in has two steps that look independent and are not:

1. obtain the payload, and
2. claim a slot for it, evicting somebody if the cache is full.

**Do them in that order.** Claim the slot only once the bytes are in hand, and if the claim then
fails, hand the payload back.

Getting it backwards is the natural way to write it, because that is how a *synchronous* fill reads:
allocate, then copy into what you allocated. The moment the source becomes asynchronous, that order
means the cache evicts a live entry to make room for something that has not arrived — and then
returns "not ready", leaving the slot holding nothing.

```cpp
// WRONG once the source is async. Under pressure this trades a resident page
// for an empty one, every frame, and the harder it streams the worse it gets.
if (!cache.Allocate(page, slot))
    return false;
if (!store.Fetch(page, payload))   // Pending — the slot is already somebody else's loss
    return false;
copy(payload, slot);

// RIGHT. The eviction is paid only by a fault-in that can actually complete.
switch (store.Fetch(page, payload))
{
    case Ready:   break;
    case Pending: return false;   // nothing was disturbed
    case Failed:  MarkPermanentlyUnavailable(page); return false;
}
if (!cache.Allocate(page, slot))
{
    store.Release(page);          // do not pin staging memory on a page the budget refused
    return false;
}
copy(payload, slot);
```

## 2. Why nothing catches it

This is the reason the rule needs writing down rather than reviewing for. The bookkeeping stays
**self-consistent** under the wrong order: the evicted entry is correctly marked non-resident, the
requesting entry is correctly left non-resident, the resident count is correct, the budget is
respected, and the LRU state is exactly what it claims. Every counter is right. In
`VirtualMeshRegistry` the whole residency suite — slot cache, eviction listener, readback ring —
keeps passing, because none of it is wrong.

What degrades is only the *picture*, and it degrades in the direction that reads as a different bug:
the geometry gets **coarser** while streaming is most active, which looks like an LOD or error-
threshold problem, not a cache problem. A visual test that asserts "the mesh is still visible" passes
throughout, because the fallback to a resident ancestor is doing its job.

So the test that catches it has to compare against a **control arm on the same scene** — the same
sweep with the source made synchronous (here: the in-memory backing) — and assert that coverage
converges to it. `StreamingFromDiskKeepsTheGeometryAndBoundsItsMemory` in
`VirtualGeometryVisualEvidenceTest.cpp` is that shape.

## 3. Three corollaries

**"Not yet" is not "not there", and it must be counted.** Per `CLAUDE.md`, a path that cannot do its
job says so loudly and countably. An outstanding fetch is a normal state that resolves in a frame or
two; a fetch that will *never* resolve is a permanent quality loss. They need different names and
different counters — `PageFaultsInFlight` versus `PageReadFailures` / `FailedPages` — because the
first is healthy and the second is not, and a single "pending" number would let a broken store hide
behind a busy one.

**A failed read must be sticky.** Without a per-entry "this one is dead" flag, the requester re-asks
every frame forever: a file that cannot be read gets re-read sixty times a second, and the error log
fills with the same line. Mark it, count it, skip it.

**Never hand back a half-read buffer.** A short read leaves the tail of the destination
zero-filled, and zeroed geometry is a degenerate primitive at the origin — which *draws*. Clear the
payload and report failure instead; that is the difference between "less detail, and here is the
number" and silent corruption.

## 4. Bound the READ-BUT-UNUSED set, not just the reads in flight

Capping outstanding reads bounds nothing on its own. A fetch is issued because an entry was
requested and released because a consumer used it — different moments, and in between the request
can go away. The camera moves, the group stops being requested, nobody ever consumes the payload,
and it sits in the ready set for the rest of the session. A moving camera over a large scene then
re-accumulates in RAM exactly the payload the spill existed to remove.

So cap the ready set too, discard oldest-first past the cap, and count the discards — a large or
fast-growing discard count says the consumer is being outrun, which is a real signal about the
configuration and not an error. Measured on `VirtualGeometryStress` before the cap existed: 551
staged pages / 37 MB after a minute on a **static** camera, still climbing.

## 5. Spend the per-frame budget on "pending", not only on "loaded"

This is the one that cost the most to find, because the wrong version looks obviously right: a page
that did not load should not consume the budget for pages that did.

It is wrong because the budget's real job is to keep the number of *outstanding reads* proportional
to what a frame can absorb. A loop that walks on after a Pending issues reads at the speed of the
loop rather than the speed of the consumer: one pass over a few thousand requested groups starts
thousands of reads, they all complete into the ready set, the ready cap discards almost all of them
unread, and the next pass does it again.

Measured on `VirtualGeometryStress` (7,206 pages, 1,024-slot budget) with a per-frame budget that
only counted uploads: **413,158 reads issued, 413,042 discarded, 21 pages resident.** Counting
Pending against the same budget: **116,096 issued, 0 discarded, 1,024 resident** — identical to the
in-memory control arm on the same scene, pose and budget.

Note what the broken version does *not* do: it never renders anything wrong, never fails an
assertion, and never logs. It is slow and starved, and the only visible symptom is that the geometry
stays coarse.

## 6. The budget counts work, not uploads

Following from §5: the cap is on pages **attended to**, not pages uploaded. A page that could not be
given a slot this frame still does not spend it — that is a capacity refusal, not work done, and it
is the pre-existing behaviour on the synchronous path where Pending cannot occur. So the rule is
three-valued, not two: *loaded* and *pending* spend budget, *no slot / permanently unavailable* does
not.

---

*Issue #1151. The residency machinery this sits under is #629 (page pools), #704 (the shared paged-
cache substrate) and #719 (the non-stalling request readback); none of it changed.*
