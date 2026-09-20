# Size a bump allocator's new block from an address it will actually use

When a linear/bump allocator runs out of room and rolls over to a new block, **do not size the new
block using alignment padding computed for the block you are leaving.** Two ways out, and the second
is the one to prefer:

1. reserve the worst case — `size + (alignment - 1)` — so any padding the new base turns out to need
   is already covered; or
2. **allocate every block over-aligned to the largest alignment you support.** A fresh block's base
   is then already aligned for every request, its cursor is 0, and the padding there is *zero*. The
   rollover path reserves exactly `size`, and there is no second padding to get wrong.

`ThreadLocalCache` (issue #1328) now does (2): `BLOCK_ALIGNMENT == MAX_ALIGNMENT == 256`, blocks come
from `::operator new(n, std::align_val_t(BLOCK_ALIGNMENT), std::nothrow)`.

Three rules that travel with it:

- **Validate the alignment, not just its sign.** The aligned-address expression
  `(addr + alignment - 1) & ~(alignment - 1)` is a power-of-two mask. A non-power-of-two alignment
  does not round badly — it yields an address that is neither aligned nor necessarily inside the
  block. `alignment > 0` does not catch that; `IsPowerOfTwo(alignment) && alignment <= MAX_ALIGNMENT`
  does.
- **Check bounds after the recompute, not only before the rollover.** Even when the reservation is
  believed exact, the final `padding + size <= remaining` test costs one compare and is the only
  thing standing between a wrong reservation and a cursor advanced out of the block.
- **A rejected allocation advances nothing.** Compute the wasted-bytes figure before rolling over,
  add it only after the new block exists, and return `nullptr` with the old block still current if
  it does not. A caller that gets `nullptr` must be able to keep using the allocator.

## The trap

The reviewed `Allocate` computed `alignmentPadding` against the *old* block's base, sized the new
block `max(defaultBlockSize, size + alignmentPadding)`, then recomputed `alignmentPadding` against
the *new* base and advanced the cursor with no further check. Blocks came from `new u8[n]`, which is
16-byte aligned — so for any request above 16-byte alignment the new base's residue is a coin flip.
When the new padding exceeded the old, the cursor advanced past the block end and the returned
pointer's tail ran into memory the block did not own.

Reachability was narrow — every engine path allocates at `COMMAND_ALIGNMENT == 16`, where blocks from
`new u8[]` already satisfy the request — but the allocator is generic and the templates advertise a
payload-alignment contract, so the defect was in the contract rather than in any current caller.

## The validation is cheap; the log call next to it is not

Adding the checks above to `Allocate` cost **+2.25 ns/allocation (+30%)** on the 16-byte hot path,
measured in Release against master's binary. None of that was the arithmetic. It was the four
`OLO_CORE_ERROR` calls: master's `Allocate` had no error paths at all, and a formatted log inlined
into a small hot function costs registers and code size on the path that never takes it.

**Put each rejection's report in an `OLO_NOINLINE` helper and mark the branch `[[unlikely]]`.** With
the reports out of line the same checks measured **+0.12 ns (+1.6%)**, inside master's own
run-to-run spread — so the validation is effectively free and only its reporting was ever expensive.

Measure this interleaved, alternating the two binaries round by round. The first A/B here ran
A-then-B and produced a bimodal fixed arm (reps 1–10 ≈ 11.9 ns, reps 11–15 ≈ 9.2 ns) that would have
supported almost any conclusion. Copy the two `.exe`s side by side **inside the build output
directory** — they need their sibling DLLs, and a copy in a scratch directory exits with code 53.

## The testing corollary

**A rollover test that does not vary the block base address passes against the broken code.** The old
and new paddings differ only when the two bases have different alignment residues; one cache,
allocated once, at one address, will often agree by luck. Sweep: several freshly constructed caches
per case, several alignments, and a request larger than the default block size so the *request*
drives the new block's size rather than the default absorbing the error.

Assert containment directly rather than hoping a corrupted neighbour shows up — `GetCurrentBlock()`
exists so a test can check `base <= p && p + size <= base + block->Size` and `Offset <= Size`. Then
`memset` the whole returned range, which turns the same bug into an ASan report.

## Related

- A rejection path that is *documented behaviour* must not `OLO_CORE_ASSERT` — that is a debugbreak
  in Debug and makes the contract untestable on exactly the config CI never builds. See
  [notes-core-and-threading.md §8](notes-core-and-threading.md).
- A rule enforced in one of two entry points is not enforced. `CreateCommandPacket` and
  `AllocatePacketWithCommand` now share one pair of predicates
  (`PayloadFitsMaxCommandSize`, `PayloadPlacementIsAligned`); the placement path previously had no
  size bound at all.
- The size bound on a payload is the cap **minus the header**, because the packet and its payload are
  one allocation. A compile-time check against the cap alone lets a payload build and then be refused
  at run time, which surfaces as a null packet at draw time.
