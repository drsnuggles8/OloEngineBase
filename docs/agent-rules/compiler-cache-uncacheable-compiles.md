# A warm compiler cache can still rebuild everything

Print `ccache -s -v` (or the sccache equivalent) after every CI build, and read the
**uncacheable** and **miss-reason** lines, not the hit rate. A cache directory that is
present, warm and growing proves nothing about whether the compiles in front of it are
cacheable at all. Two defects in this repo made every engine object a miss on every run
while every cache counter looked healthy.

## 1. The precompiled header made the compile uncacheable

ccache does not cache a compile that uses a PCH unless `CCACHE_SLOPPINESS` contains
`pch_defines,time_macros`, and GCC additionally needs `-fpch-preprocess` on the compile
(ccache manual, *Precompiled headers*; clang needs nothing beyond what CMake emits). Every
engine target here uses a PCH, so on the AMD job the statistics read:

```
Cacheable calls:                      51 / 1468 ( 3.47%)
Uncacheable calls:                  1417 / 1468 (96.53%)
  Could not use precompiled header: 1417 / 1417 (100.0%)
```

The 51 that hit were the vendor and test objects without a PCH, which is also why #1009
measured a ~46 % per-run hit rate on the olo-ci runners and called it a working cache.
The job printed no statistics, so nothing said so. Fix: `CCACHE_SLOPPINESS` in both
self-hosted setups, `-fpch-preprocess` for GCC in `olo_enable_pch`.

## 2. A per-commit macro on every translation unit

`OLO_BUILD_GIT_HASH` and `OLO_BUILD_TIMESTAMP` were target-wide compile definitions on
the engine. The hash changes with every commit and the timestamp with every configure,
and every compiler cache hashes the command line, so every engine object missed on every
CI run and on every local reconfigure. Only `BuildInfo.cpp` reads them; they are now a
per-source property on that file. Anything that embeds a hash, a date or a counter must
be scoped to the one file that consumes it.

## How to tell

- The build step takes compile time on a change that touched no C++ (24 of the AMD
  job's 31 minutes for a shader edit).
- The cache is small relative to its limit after weeks of runs.
- The hit rate is steady at a fraction that matches "everything without a PCH".

Related: [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) for the Actions
cache side of the same lesson.
