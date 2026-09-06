# A cache entry's identity includes the path STRING, not the directory

Issue [#1073](https://github.com/drsnuggles8/OloEngineBase/issues/1073).

**The rule: a cache's restore and its save must be handed the same path string, character
for character. Derive both from one expression; never spell the path twice.**

`actions/cache` computes a `version` for every entry by hashing the `path` strings it was
given — the raw strings, before any resolution. A restore only ever sees entries whose
version matches. So two spellings of one directory are two different caches, and the
failure is silent in the worst way: **the key matches perfectly and the entry is still
invisible.** Nothing in the log says "version mismatch". The restore just reports
`Cache not found for input keys: …` and lists keys you can see in the cache listing.

## What it looked like here

`setup-vcpkg` restored with

```yaml
path: ${{ github.workspace }}/.vcpkg-binary-cache
```

and saved through its `cache-path` output, which is built in bash as

```bash
dir="${GITHUB_WORKSPACE//\\//}/.vcpkg-binary-cache"
```

On a Windows runner `github.workspace` expands with **backslash** separators, so the
restore asked for a mixed-separator string while the save wrote an all-forward-slash one.
Same directory. Two versions. Every restore a guaranteed miss.

The normalisation on the save side is correct and deliberate — that value goes into
`GITHUB_ENV` and is read back by later bash steps, where a backslash is a escape
character. It was added for a good reason and silently desynchronised the two halves.

**On Linux the two spellings are identical**, so Linux was unaffected — which is why this
only ever surfaced as a Windows complaint (a 53-minute configure step with no Linux twin)
and never as a caching bug.

## The measurement that names it in one command

You do not need a log. Ask whether the entry has ever been *read*:

```console
$ gh api --paginate "repos/<owner>/<repo>/actions/caches?per_page=100" \
    --jq '.actions_caches[] | [(if .created_at == .last_accessed_at then "NEVER-READ" else "read" end), .key] | @tsv'
NEVER-READ  vcpkg-Windows-x64-windows-static-md-<hash>-34021696485-1
NEVER-READ  vcpkg-Windows-x64-windows-static-md-<hash>-34027362531-1
NEVER-READ  vcpkg-Windows-x64-windows-static-md-<hash>-34027467561-1
read        Windows-vulkan-prebuilt-sdk-true-1.4.321.0
read        ffmpeg-Windows-n7.1-637be53f…
read        sccache-tsan-linux-33539973579-1
```

Six vcpkg entries written in one morning by five different workflows, **not one of them
ever read**, while a Vulkan SDK entry created two months earlier showed a read that same
hour. `last_accessed_at == created_at` on every entry of a prefix is conclusive: that
cache has no reader. A cold cache still gets read.

This is the opposite reading of the same field from
[ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md), which warns that
`last_accessed_at` keeps ticking on restores and makes a frozen store look busy. Both are
true and they answer different questions: `created_at` tells you whether anything is
being *written*; `created_at == last_accessed_at` tells you whether anything is being
*read*. Check both.

## Where this bites

- A path built from `${{ github.workspace }}` on one side and `$GITHUB_WORKSPACE` on the
  other, on Windows.
- A composite action whose restore is inline and whose save goes through an output.
- Any trailing slash, `./` prefix, or case difference on a case-insensitive filesystem.
- Reordering a multi-line `path:` list — the strings are joined in order before hashing.

Swept the rest of this repo when this was found: every other restore/save pair already
shares one expression (`env.SCCACHE_DIR`, a literal `OloEngine/vendor/ffmpeg-install`).
vcpkg was the only one, and it had been broken for its whole existence.

## See also

- [actions-cache-budget.md](actions-cache-budget.md) — the other half of #1073: the store
  goes read-only past the cap and refuses every save, which is why the *sccache* entry did
  not exist at all. Two independent faults, one four-hour job.
- [vcpkg-dependency-management.md](vcpkg-dependency-management.md) — what the binary cache
  holds and what a manifest bump costs when it works.
