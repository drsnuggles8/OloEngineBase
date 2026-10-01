# A delayed-read test must prove the next version was written

**Before a delayed-read test checks that the old buffer version survived, it reads the NEW
version back and requires the new bytes.** Otherwise a publish that never happened (an early
return, a producer that wrote the same bytes) passes the survival check without testing anything.

## The pattern

The fresh-allocation families in
[vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) are pinned by
device tests of one shape: a copy of the OLD version is submitted on the async compute queue behind
an unsignalled timeline semaphore (a consumer from an earlier frame that has not run yet), the
production call publishes the next version, the semaphore is released, and the old bytes are
required. In-place reuse overwrites them first, so the test fails when the producer reverts.

That last clause is a claim, and it has to be run: revert each producer to in-place reuse, rebuild,
and require every test to fail **at the old-bytes check**. A test that survives the revert, or
fails at some other assertion, has not been shown to test anything.

## What happened (#1526)

Five tests were written for the foliage cull layer input, fluid emit staging, fluid body proxies,
the RT skeletal palette and the groom BLAS input. Under the negative-control build:

- **Foliage passed.** Both regenerations used the same height field, and the registry advances
  its generation only when a placement changes, so the second `BuildLayer` returned early. No new
  version, no overwrite, a green test.
- **Groom passed.** The probe compared the first 256 bytes, which belong to strands rooted on the
  unbent half of the hinged grid; their bytes are the same in both bends.
- **The palette failed at the wrong line.** It asserted the staged size, but `PaletteBytes` is the
  buffer's capacity (8192 for one matrix). The failure looked like a working control until the
  line number was read.

Each test now requires the published version to hold the new bytes (foliage: the new header after
a regeneration on raised ground that does advance the generation; groom: the whole vertex stream
differs), and all five fail at the old-bytes check under the revert.

## Also

- A second read of the same buffer must not go on the async compute queue while the gated read is
  pending: a driver that runs one queue in submission order can make it wait on the gated one,
  which waits on the test. Take immediate reads on the graphics queue.
- The deferred reclaim frees a retired version after two completed frames, so at most one
  `SubmitFrame` may run between the publish and the release.
