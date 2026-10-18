# Repository history: measured state and a remediation plan (#1610)

**Status: recommendation only. Not executed; awaiting an explicit decision.** #1610 authorized an
audit and an ordinary cleanup PR, not a rewrite of published history. Nothing below has been run
against any shared ref.

Measured 2026-10-18 on `master` at `0cc2cd225`, before the #1610 cleanup.

## Three different savings

They are often confused, so they are measured separately.

| Kind | What it changes | Saving | Cost |
|---|---|---:|---|
| Current-tree cleanup (#1610, done) | files a checkout and a worktree contain | tracked tree 2,192.7 MB → 1,055.6 MB (9,440 → 6,566 files) | none; ordinary commit |
| Unreachable-object reclamation | loose and orphaned objects in one local clone | local only (this box: 1.64 GiB loose, 142 MiB garbage) | `git gc` per clone; no effect on GitHub or other clones |
| Reachable-history rewrite | what every fresh clone downloads | fresh `master` clone 3.25 GiB → about 1.08 GiB (packed estimate 3.26 → 1.08 GiB, −67%) | new SHAs for every commit after the first evidence commit; force-push; every clone and worktree migrates |

The cleanup does not shrink a clone. Each removed blob is still reachable from every commit that
contained it.

## Reachable history

- **Refs on GitHub:** 2 branches (`master` and one feature branch), 0 tags and 954 `refs/pull/*`
  refs (477 PRs, a head ref and a merge ref each). A default clone fetches branches only.
- **Fresh clone of `master`:** `git clone --no-local --bare --single-branch` packs to **3.25 GiB**
  (117,119 objects). Git LFS content (the dog sources and grooms) is separate and not counted.
- **What a rewrite would drop:** 9,568 blobs, 2,417 MiB logical, from paths that ever lived under
  `OloEditor/assets/tests/{visual,exports,memory}/` or `docs/testing/evidence/` and are not still
  tracked as a golden or a retained curated file. Packing everything reachable from `master` gives
  3.26 GiB; the same set without those blobs gives **1.08 GiB**. This comes from `git pack-objects`
  on the two object lists, not from a simulated rewrite. A real rewrite also rewrites trees and
  commits; that difference is not part of this estimate.

| Path family | Blobs | Packed |
|---|---:|---:|
| `OloEditor/assets/tests/visual/` | 8,826 | 1,977.6 MiB |
| `docs/testing/evidence/integrated-renderer-1338/` | 229 | 190.1 MiB |
| `docs/testing/evidence/hair1593/` | 54 | 46.5 MiB |
| `docs/testing/evidence/integrated-acceptance-1259/` | 29 | 8.7 MiB |
| everything else | 430 | 5.8 MiB |

The largest families in `visual/` history are already gone from the tree: the #1524 shadow-family
campaign (`ShadowFamilyLive_*` 255 MB in 2,270 blobs, `ShadowFamily_*` 138 MB, `ShadowFamilyOff_*`
90 MB), `After_*` (99 MB) and `OceanFFT_*` (60 MB). The largest single blobs are 4–6 MB HDR buffers
and motion WebPs from #1338 and #1593.

## Recommendation

Rewrite only if clone size is a real cost: CI cold checkouts, new worktrees and a collaborator's
first clone. In this repo it is, because `/start-work` creates several worktrees a day. The worktrees
share one object store, so a rewrite mostly helps fresh clones and CI. If that is worth one
coordinated migration, do the following.

1. **Freeze.** Merge or close every open PR. Stop all worktree sessions. Note every live branch.
   Then **preserve what exists only in history.** Every write-up that says
   `git show 0cc2cd225:<path>` breaks under a rewrite: `git grep -l "0cc2cd225"` lists them (the
   `*-live` and `unified-layouts-1181` write-ups, cpu-scope-ring-1594, digital-human-hair-quality-1593,
   integrated-acceptance-1259, integrated-renderer-budgets-1338, render-graph-attachment-feedback-1554,
   restir-pt-1211, groom-vegetation-streaming-residency-1257 and foliage-hierarchical-wind). Re-running a
   protocol gives new numbers, not the ones those pages quote, and the hand-taken live captures cannot be
   re-run at all. For each, either keep its blobs out of the drop list and re-point the page at the
   rewritten commit, or archive the files outside Git first (for example as a release asset) and update
   the page. Do this before step 3.
2. **Back up.** Run `git clone --mirror https://github.com/drsnuggles8/OloEngineBase backup.git`
   and keep it offline. It holds every branch and `refs/pull/*`. Rollback is
   `git push --mirror --force` from it, which needs the same force-push approval.
3. **Rewrite on a fresh mirror** with `git filter-repo --strip-blobs-with-ids <file>`, where the file
   lists the dropped blob IDs: the 9,568 this note measured, minus anything step 1 decided to keep. The
   size estimate above assumes the full 9,568. Select by blob rather than by path: goldens share `visual/` with
   the dropped files, and an old version of a golden is dropped while the current one stays.
4. **Verify before pushing.** Run `git fsck`, and check that the tip tree of `master` equals the
   pre-rewrite tip tree (`git diff --stat <old> <new>` is empty). Pack and record the size, then run
   the golden-reading suites from a clone of the rewritten mirror.
5. **Push** with `git push --force --mirror`, then ask GitHub Support to run a GC on the repository.
   Without that, GitHub keeps the old objects alive.
6. **Migrate every clone.** The shared clone `C:/repos/OloEngineBaseBase` and all of its worktrees are
   re-cloned, not rebased. Remove the worktrees with `/cleanup-worktree`, then re-clone and re-create
   them. An old clone that pushes reintroduces the whole history, so fetch-and-reset is not enough.
   The CI runners' caches on `olo-ci` and the self-hosted GPU runner are wiped.

## Affected refs, PRs and links

- **Every commit SHA after the first evidence commit changes.** Commit links in issues, ADRs, docs
  (`git show 0cc2cd225:…` in several write-ups, including this one) and memory files stop resolving,
  unless GitHub keeps them through `refs/pull/*`.
- **`refs/pull/*` cannot be rewritten by a push.** GitHub owns those refs. They keep every old PR head
  and its blobs alive on the server, so a rewrite shrinks what clones download, not GitHub's storage,
  unless Support removes them. The 275 evidence-image links in PR descriptions are all pinned to a
  commit SHA. They keep working for as long as those refs exist.
- **The feature branch on origin** (`feature/ci-lavapipe-rt-suites-nightly-1294`, live work) must be
  rebased onto the rewritten `master` or recreated. Coordinate with its owner first.

## Not recommended

- A partial rewrite of only `visual/`: it is 89% of the saving and costs the same migration, so it
  is not a cheaper option.
- Moving the goldens to Git LFS as part of the same change: the 189 goldens are 109 MB and change
  rarely. That is a separate decision with its own CI cost.
