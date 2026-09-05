# Validate a multi-mode block format one mode at a time, against the decoder's own read order

Applies to: `OloEngine/src/OloEngine/Renderer/BC6HEncoder.cpp`, and to any future encoder for a
block format with more than one block layout (BC7 has eight, ASTC more).

A block format like BC6H is not one bitstream, it is fourteen. An encoder that picks a mode per
block writes fourteen different layouts, and a single misplaced field in **one** of them corrupts
only the blocks that chose that mode. Every aggregate quality test stays green, because the other
thirteen layouts are fine and the average barely moves. Issue #624 added ten modes to an encoder
that had one; these are the three things that were not obvious going in.

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

---

**The structural safeguard.** Keep the *previous* encoder's mode in the candidate set. #624's search
still evaluates mode 10, the only mode #440 emitted, so per-block quality cannot regress relative to
it for the same endpoint fit — the claim "no block got worse" is then a property of the algorithm
rather than a hope about the test data.
