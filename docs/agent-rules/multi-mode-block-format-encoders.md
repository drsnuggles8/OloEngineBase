# Validate a multi-mode block format one mode at a time, against the decoder's own read order

Applies to: `OloEngine/src/OloEngine/Renderer/BC6HEncoder.cpp`,
`OloEditor/assets/shaders/include/BC6HEncodeCommon.glsl`, and to any future encoder for a block
format with more than one block layout (BC7 has eight, ASTC more).

A block format like BC6H is not one bitstream, it is fourteen. An encoder that picks a mode per
block writes fourteen different layouts, and a single misplaced field in **one** of them corrupts
only the blocks that chose that mode. Every aggregate quality test stays green, because the other
thirteen layouts are fine and the average barely moves. Issue #624 added ten modes to an encoder
that had one, then ported the result to a compute shader and taught the cook to pick narrower
formats; these are the things that were not obvious going in.

---

## 1. Generate the field tables from the reference decoder, then prove the inverse per mode

BC6H's per-mode bit layouts are scattered field-by-field — mode 0 writes `gy[4]`, then `by[4]`, then
`bz[4]`, then `rw[9:0]`, and so on for twenty fields, in an order with no pattern to it. Hand
transcription of fourteen of those is a guaranteed defect.

Two moves, both cheap:

- **Derive the tables mechanically** from the reference decoder's read sequence (here: bcdec's
  `bcdec_bc6h_half()` switch), so packing is that sequence run backwards. `BC6HEncoder.cpp` stores
  them as data — `{slot, shift, bits, reversed}` per field — and one packer walks the table for every
  mode, instead of fourteen hand-written packers.
- **Make the encoder state what it expects to be decoded**, and compare that against the reference
  decoder bit-for-bit, per mode. `BC6H::EncodeBlockForModeForTest` packs a block using one mode and
  returns the 48 half-float values it predicts; `BC6HModeCoverageTest` decodes the same bytes with
  bcdec and requires exact float-bit equality. A shifted field changes what bcdec reads back but not
  what the encoder predicted, so the mismatch is loud and it names the mode.

Decoding with your own decoder proves nothing: a matched pair of encoder and decoder bugs cancels.
Comparing PSNR against a tolerance proves almost nothing either — a one-bit field shift in a smooth
block is often still inside a 35 dB tolerance.

The same test also re-derives the **mode field itself** from the packed bytes
(`BC6HBlockModeReader.h`), independently of the encoder's tables, because "encoded mode 7 as mode 6"
is otherwise invisible: the block decodes cleanly, just as something else.

## 2. The interpolation space is half-float bits, not radiance — so "high contrast" is not the hard case

BC6H interpolates between endpoints in a 16-bit space that is then scaled into a **half-float bit
pattern**. That space is roughly logarithmic in radiance. A block holding a 100:1 brightness ratio —
a hard bright/dark edge, intuitively the case two-subset modes exist for — spans only about 1.6:1 in
endpoint values, comfortably inside one endpoint pair with 16 interpolation steps.

Measured on a 4x4-checkered two-cluster HDR image (`BC6HQuality.TwoClusterBlocksSurviveASingleEndpointPair`):
the encoder picks the **one-subset** mode 10 for all 256 blocks, and adding ten two-subset modes
moved its PSNR by 0.00 dB.

What actually is hard, and what each mode family buys:

| Block class | Winning modes (measured) | Why |
|---|---|---|
| Flat / near-flat | 13 (100 %) | Mode 13 stores a **16-bit** base endpoint; a constant block becomes exact. +21.9 dB over a 10-bit-endpoint mode. |
| Smooth and curved gradients | 0, 2, 5 (~90 %) | Two subsets split a curve into two shorter segments; the win is *curvature*, not contrast. |
| Wide dynamic range | 13, 11, 12 | Endpoint **precision**, not range. |

So: pick test data for what the format finds hard, not for what looks hard. And measure per block
class — an aggregate number over mixed content hides a mode that regressed one class while another
improved.

## 3. Satisfy the anchor-index constraint when you fit endpoints, not after you quantize

Every subset's anchor texel stores its interpolation index with the high bit dropped, so that index
must fall in the lower half of the range. The obvious fix — select indices, notice the anchor's index
is too high, swap the two endpoints, re-select — is wrong for any **delta** mode: endpoint 0 is the
base the other endpoints are stored as offsets from, so swapping it silently moves every other
endpoint outside its representable delta window.

Orient at fit time instead: when the PCA fit produces the two extremes, put the one the anchor texel
projects nearest into endpoint 0 (`FitEndpoints`, `anchorTexel`). Then restrict the anchor's index
search to the legal half rather than clamping afterwards, so it still gets the best index it is
allowed to have. Nothing downstream has to re-check.

Related: a delta mode's endpoints are **clamped** into the storable window rather than the mode being
rejected. A clamped endpoint still competes, every candidate is scored on the error a conformant
decoder would actually produce, and the encoder keeps the winner — so a mode that cannot represent a
block simply loses instead of needing a representability predicate that can be wrong.

## 4. Porting the encoder to a compute shader: keep the integer half identical, and return the result in an image

The block search is perfectly parallel, so #624 also ships the same encoder as a compute
shader (`BC6HEncodeCommon.glsl`). Three things made that port verifiable rather than a second
implementation to keep in sync.

**Generate the GLSL tables from the C++, not from the spec again.** The shader's mode words,
field tables and partition masks are emitted by a script that reads `BC6HEncoder.cpp`, which
itself derives them from bcdec. Three hand transcriptions of the same fourteen layouts would
have been three chances to differ.

**Port the integer half exactly and let only the fit differ.** Quantization, unquantization,
interpolation, index selection and bit packing are all integer arithmetic and translate
one-for-one, so they cannot drift. Only the PCA fit and the least-squares refit change type —
`double` on the CPU, `float` in GLSL, because doubles run at a fraction rate on consumer GPUs.
That is a deliberate, bounded difference, and it is small: measured over two 64x64 HDR images,
**99.6 %** (unsigned) and **99.2 %** (signed) of blocks came out bit-identical, with the two
paths scoring the same PSNR to two decimals.

**Test it as "the fast path must not be the worse path."** `BC6HGpuEncoderTest` encodes the
same source both ways, decodes both through bcdec, and requires the GPU within 0.5 dB of the
CPU. Asserting bit-equality would turn a harmless float difference red; asserting only "it
produced output" would miss a mis-packed field entirely.

Two mechanical traps on the way:

- **`partition` is a reserved word in GLSL.** The error is a syntax error on the line *after*
  the one that uses it, which reads as an unbalanced paren.
- **`OLO_HEAP_IMAGE` cannot declare a `readonly` image** (BindlessHeap.glsl says so): the macro
  initialises a local, and initialising a `readonly` variable is a write. A read-only input
  stays on the slot path and takes `OLO_HEAP_IMAGE_RW` under bindless, exactly as
  `Ocean_Assemble.comp` does. Note also that `glslc -DOLO_BINDLESS` rejects that macro on
  *every* shader in the repo, so a failure there is the validator, not your shader — A/B
  against an existing one before chasing it.

**The binding trick is the reusable part.** `ShaderBindingLayout` records UBO 65 as the last
free UBO number engine-wide and the storage-buffer namespace is effectively full, so a new
compute pass that wants to return bulk data has nowhere obvious to put it. This one takes
**zero** UBO and SSBO bindings: the output goes to an `rgba32ui` **image**, because one
RGBA32UI texel is exactly one 16-byte BC block, and image units are their own namespace. The
one parameter it would otherwise need — signedness — is a second shader variant instead. If
you are about to spend a scarce binding number on a bake pass, check whether an image and a
`#define` can carry it.

## 5. Narrow a texture's format only on a measurement that proves nothing is lost

"Pick a cheaper format when the data allows" is the same shape of rule as the filename guess
that kept BC5 out of the auto-cook, and it fails the same way — silently, in someone else's
asset. The version that is safe has two properties, and #624's BC4 selection has both:

- **The condition is measured, not named.** `TextureCompression::AnalyzeChannels` reports what
  the decoded pixels actually use: whether R, G and B are equal at every texel, and whether
  alpha ever leaves 255. A greyscale source narrows to BC4 (8 bytes a block instead of 16)
  because it *is* greyscale, not because it is called `_ao`.
- **The substitution is invisible.** RGTC1 samples `(R, 0, 0, 1)`; a texture swizzle set at
  upload makes it `(R, R, R, 1)`, which is exactly what the BC7 path produced for the same
  source, and `DecodeToRGBA8` replicates red for the same reason. No shader can tell.

The neighbouring rule that looks equally reasonable and is **not** safe: narrowing an RGB
source to BC5 when its blue is constant. BC5 decodes blue as 0, so unless that constant was
already 0 the data changed. It stays an explicit `.oloimport` choice, and a test pins that the
cook refuses to make it automatically.

The same measurement fixed a real defect on the way: `HasAlpha` was `channels == 4`, so every
RGBA PNG with a constant 255 alpha claimed transparency and sorted its material into the
transparent pass.

---

**The structural safeguard.** Keep the *previous* encoder's mode in the candidate set. #624's search
still evaluates mode 10, the only mode #440 emitted, so per-block quality cannot regress relative to
it for the same endpoint fit — the claim "no block got worse" is then a property of the algorithm
rather than a hope about the test data.
