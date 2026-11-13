# Git LFS bandwidth is a monthly budget, and one cold CI pull is a tenth of it

Rules for any CI job, workflow or cache change that reaches the Git LFS objects (the dog models,
grooms and authoring sources). Applies to: `.github/actions/restore-lfs-assets`, every workflow
that calls it, and `cache-prune.yml`. Issue #1682.

## Rules

1. **Budget LFS bandwidth like money: 10 GiB a month, ~1 GB per full pull.** The account's free
   Git LFS bandwidth is 10 GiB/month and the current set is 12 objects, ~1.0 GB on the wire.
   Ten full pulls in a month exhaust it, from CI, a local clone or any outside clone of this
   public repository. [Details](ci-lfs-bandwidth-details.md#the-numbers)
2. **Fetch LFS objects in CI only through `.github/actions/restore-lfs-assets`.** It restores the
   master `olo-lfs-v1-<digest>` cache entry, or an artifact from earlier in the run, before
   `git lfs pull`. A bare `git lfs pull`, or `actions/checkout` with `lfs: true`, pulls the full
   set on every run. [Details](ci-lfs-bandwidth-details.md#how-ci-gets-the-objects)
3. **One LFS fetch per workflow run: a workflow with several builds gets one job that fetches, and
   the others take its artifact.** asan.yml's `lfs-objects` job is the pattern; a new parallel
   build `needs:` it and passes `restore-artifact: 'true'`. Four builds that each restore the
   cache become four full pulls when the entry is missing.
   [Details](ci-lfs-bandwidth-details.md#2026-11-02-four-builds-four-pulls)
4. **A job that fetches LFS bytes says so.** The action prints `LFS fetched: N object(s), B bytes`
   in every job and raises a `::warning::` with the job and runner name when B > 0. Read it before
   calling a CI change free; never route around it.
   [Details](ci-lfs-bandwidth-details.md#how-ci-gets-the-objects)
5. **The master entry is lost by cap eviction, not by `cache-prune.yml`; protect it by keeping the
   store under the cap.** Past the 25 GB cap GitHub evicts in last-access order between sweeps.
   On 2026-11-02 a PR's vcpkg saves pushed the store over and the eviction took `olo-lfs-v1-*`.
   `save-vcpkg-cache` now skips a non-master save that would cross the working ceiling, and takes
   its own entry back when a parallel save crossed it
   ([actions-cache-budget.md](actions-cache-budget.md) rule 2).
   [Details](ci-lfs-bandwidth-details.md#why-the-master-entry-disappeared)
6. **Save the master entry with the current object set only.** A save after a restore-keys match
   used to bank the old objects too; the evicted entry was 1402 MiB against a 589 MiB current set.
   The action drops unreferenced objects before saving; keep it that way.
   [Details](ci-lfs-bandwidth-details.md#the-entry-only-ever-grew)
7. **Spend at most one forced cold pull to verify an LFS change, and probe the batch API first.**
   A `download` batch request costs no bandwidth. A `403` means stop; a `200` does not prove
   headroom (it answered 200 at 100 % usage), so read the billing usage when it matters.
   Verify a miss by salting the cache key in one `workflow_dispatch` only, never on a PR run and
   never by deleting the master entry. [Details](ci-lfs-bandwidth-details.md#verifying-a-change-without-spending-the-quota)

---

Full text, with the measurements and the 2026-11 incident:
[ci-lfs-bandwidth-details.md](ci-lfs-bandwidth-details.md).
