"""Where do the `RendererID` mentions actually live? (issue #691 step 3)

Reproduces the distribution behind the worklist in
docs/agent-rules/rhi-abstraction-boundary.md. Run it before choosing the next
migration unit.

Written because three slices in a row were scoped WITHOUT checking whether the
work intersected the thing being counted, and all three missed. The by-spelling
breakdown is the part the ratchet test does not give you: it separates
`m_RendererID` (mostly backend-internal, exempt) from `GetRendererID` (the real
consumer-side target) from the attachment getters.

    python OloEngine/tests/scripts/measure_rendererid.py

Not a test — an analysis aid. The authoritative counter is
RHIBoundaryRatchetTest / rhi_boundary_baseline.json; this only explains where
that number comes from.
"""
import os
import re
import collections

ROOT = r"e:\repos\OloEngine-vulkan-rhi-phase2-resource-handles-691"
PAT = re.compile(r'\w*RendererID\w*')

by_spelling = collections.Counter()
by_file = collections.Counter()
by_dir = collections.Counter()

for dp, _, fns in os.walk(os.path.join(ROOT, 'OloEngine', 'src')):
    parts = dp.replace('\\', '/').split('/')
    if 'vendor' in parts:
        continue
    for fn in fns:
        if not fn.endswith(('.h', '.cpp')):
            continue
        p = os.path.join(dp, fn)
        with open(p, encoding='utf-8', errors='replace') as fh:
            t = fh.read()
        hits = PAT.findall(t)
        if not hits:
            continue
        rel = os.path.relpath(p, ROOT).replace('\\', '/')
        by_file[rel] = len(hits)
        by_spelling.update(hits)
        by_dir[os.path.dirname(rel)] += len(hits)

print(f"TOTAL {sum(by_file.values())} across {len(by_file)} files\n")
print("by spelling:")
for s, c in by_spelling.most_common(14):
    print(f"  {c:4d}  {s}")
print("\nworst files:")
for f, c in by_file.most_common(14):
    print(f"  {c:4d}  {f}")
print("\nworst dirs:")
for d, c in by_dir.most_common(10):
    print(f"  {c:4d}  {d}")
