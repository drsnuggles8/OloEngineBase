# Windows compiler-cache health: incidents and measurements

Companion to [windows-sccache-warm-reuse.md](windows-sccache-warm-reuse.md), which holds the rules.

## #1678: a correct cache read as near-zero-warm

The push run for #1678's merge ([41709157021](https://github.com/drsnuggles8/OloEngineBase/actions/runs/41709157021))
built successfully and then failed the health gate: 106 hits for 2126 requests (4.99%),
just under the 5% floor. The restored snapshot came from the #1677 merge run 20 seconds earlier,
with the same toolchain and dependency lineage. The cache was fine. #1678 had added Tracy section
macros to `Debug/Instrumentor.h`, which almost every translation unit includes, so almost every
object had new inputs and had to miss. #1673's vcpkg baseline bump had failed the same gate the
same way (96/2119, 4.53%); keying the lineage on the dependency set fixed that one instance, not
the class.

A raw hit rate cannot tell those cases from a broken cache, because both look like "almost
nothing hit". What separates them is whether the objects that missed had changed inputs. The
object manifest records exactly that. Rebuilding the manifest on a local tree with the
pre-#1678 `Instrumentor.h` predicts 33 of 1725 objects reusable and names
`oloengine/src/oloengine/debug/instrumentor.h` as the one changed input, which is the measured
CI result.

The coverage check came from review of the same change: a manifest that records no objects
(a ninja deps format or launcher command shape the builder no longer reads) also predicts 0
reusable objects, and without the check it would have passed as `inputs-changed`.

## Attributing hits to the unchanged objects

sccache reports hits in aggregate, so the gate cannot see which objects hit. Review of #1679 found
two ways the gate credited hits to the unchanged objects that other compiles may have taken. A
compile the manifest does not predict (an unpredictable command, or one it did not record) may be
a hit. So may a changed object: sccache keys an MSVC object on its preprocessed source, so an edit
to a header comment or an inactive `#if` branch changes the manifest hash but not the key, and a
changed object can also match an entry older than the producer. The lower bound therefore
subtracts every compile that is not an unchanged object.

That bound makes a healthy cache ambiguous after a wide change. Every unchanged object hits and
every changed one misses, but once more than a third of the covered objects changed, the bound
drops under 50%. Failing those builds would repeat #1678's false red on every PR that edits a
widely included header, and the statistics cannot separate them from a cache whose changed
objects hit while its unchanged ones missed. That mix needs the cache to hold the changed
objects' keys while losing the unchanged ones', which eviction does not produce, so the band
passes as `inputs-changed` and says reuse could not be measured. When compiles the manifest
does not predict are what blocks the verdict, the manifest step is incomplete, and the gate
fails as `unavailable`.
