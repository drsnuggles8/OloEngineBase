# A static archive has a hard 4 GiB ceiling, and `LNK1248` under-reports how far over you are

Issue [#762](https://github.com/drsnuggles8/OloEngineBase/issues/762). Every app target —
`OloEditor`, `OloRuntime`, `OloServer` — failed to link in **Dist** on a clean `master`, and had
done for an unknown length of time:

```
OloEngine.lib : fatal error LNK1248: image size (1007EBBC4) exceeds maximum allowable size (FFFFFFFF)
```

The COFF archive format stores each member's offset in the second linker member as a 32-bit
value. There is no extended or 64-bit variant, `lib.exe` will not emit one, and no linker flag
changes it. **4 GiB is a format limit, not a tunable.**

---

## 1. The number in the error message is not the archive's size

`0x1007EBBC4` is 4,303,272,900 bytes — 7.9 MiB past the ceiling. The same error at an earlier
commit read 4,295,472,878, which is 494 KiB past it. Both look like the archive is a rounding
error away from fitting, and #762's own analysis and its handover both drew that conclusion: a
modest reduction should be enough.

It is not, and the tell is that two independent measurements landed within a few MiB of a 4 GiB
boundary. That does not happen by chance.

Measure the members instead of trusting the message. The failing build still leaves every `.obj`
behind, because `lib.exe` gives up before writing anything:

```bash
find build/OloEngine/src/OloEngine.dir/Dist -name '*.obj' -printf '%s\n' | awk '{s+=$1} END {print s}'
# 4,981,361,672
```

Then calibrate what `lib.exe` does to that total — archive the largest 100 objects and compare:

| | bytes |
|---|---|
| sum of the 100 members | 3,173,212,111 |
| resulting `.lib` | 3,247,790,476 |
| **ratio** | **1.0235** |

An archive is *larger* than its members — the symbol index costs about 2.35% here. So a 4.98 GB
member set cannot produce a 4.30 GB archive. The real archive was **~5.10 GB, 119% of the
ceiling**; the overshoot was ~1.1 GB, not 494 KiB.

**`LNK1248` reports the running offset at the point it crossed 4 GiB, then stops.** Treat it as
"you are over", never as "you are over by this much".

## 2. Why nothing was watching

Two independent reasons, and only the first is the obvious one:

- No CI workflow builds **Dist** at all. It is the distribution config and it had no gate.
- Every workflow that builds Release passes **`-DOLO_ENABLE_LTO=OFF`** (`Windows.yml`, so
  `/GL` does not defeat sccache). `/GL` is what makes an object carry compiler IL instead of
  machine code, one to two orders of magnitude larger. **No CI job in this repo has ever produced
  a `/GL` object**, in any configuration.

So "add a size assertion to the existing Release job" does not work: that archive is small for an
unrelated reason. Only an LTO build measures the thing that fails.

## 3. Per-TU `/GL-` cannot close a gap this size — check the arithmetic first

The tempting fix is to exclude the monstrous translation units from whole-program optimization.
Measured Dist object sizes say why that is not enough on its own:

| translation unit | IL object |
|---|---|
| `LuaScriptGlue.cpp` | 348.4 MiB |
| `Scene.cpp` | 88.0 MiB |
| `ScriptGlue.cpp` | 84.6 MiB |
| `SceneSerializer.cpp` | 63.3 MiB |
| `SaveGameSerializer.cpp` | 58.2 MiB |
| `Prefab.cpp` | 41.3 MiB |

The distribution is not as top-heavy as those numbers suggest — median object is 2.0 MiB, but
**90 objects sit in the 20–35 MiB band and carry ~2.2 GiB between them**. The top 6 are only 14.4%
of the mass; you need the top ~50 to reach 40%, and by then you are disabling whole-program
optimization across the entire renderer. Reaching a *healthy* margin needs roughly 40% of the
bytes gone.

Note which TU is second on that list. `Scene.cpp` is **hot** — it runs every tick — so it is not a
candidate for exclusion at any size. That is the trap this section is about: the objects big enough
to matter and the objects safe to exclude are mostly different objects.

So `/GL-` is worth doing where it is free — one-shot registration and (de)serialization TUs that
run at startup or scene load, never in a frame loop. Measured on the five chosen here, it takes
**624.7 MB of IL down to 153.9 MB**, recovering ~471 MB:

| | /GL | /GL- | residual |
|---|---:|---:|---:|
| `LuaScriptGlue.cpp` | 348.4 MiB | 94.0 MiB | 27.0% |
| `ScriptGlue.cpp` | 84.6 MiB | 18.8 MiB | 22.3% |
| `SceneSerializer.cpp` | 63.3 MiB | 15.3 MiB | 24.2% |
| `SaveGameSerializer.cpp` | 58.2 MiB | 11.6 MiB | 19.8% |
| `Prefab.cpp` | 41.3 MiB | 7.0 MiB | 17.1% |
| **total** | **595.8 MiB** | **146.7 MiB** | **24.6%** |

Note the residual: a non-`/GL` object is ~25% of its IL counterpart, not the ~5–10% "a few MB at
most" folklore suggests. That matters, because it means the lever is weaker than it looks — ~449 MiB
against a ~1.1 GB overshoot. It is not the fix. The archive has to stop being one archive.

## 4. The fix: partition by regex, never by a hand-split source list

`OloEngine/src/CMakeLists.txt` holds one explicit ~1700-line `SOURCES` list. Splitting it by hand
into three lists would have been a large diff in the exact file that every concurrent feature
branch adds files to — a guaranteed conflict, and a guaranteed source of *silent* mistakes when
someone resolves one.

Instead the partition is **computed from the one list** by directory prefix:

```cmake
set(OLO_RENDERER_SOURCES ${SOURCES})
list(FILTER OLO_RENDERER_SOURCES INCLUDE REGEX "^(OloEngine/Renderer/|Platform/OpenGL/|Platform/Vulkan/)")
...
list(REMOVE_ITEM OLO_CORE_SOURCES ${OLO_RENDERER_SOURCES} ${OLO_CONTENT_SOURCES})
```

Two properties worth keeping if this is ever re-cut:

- **Adding a source file requires no edit here.** It lands in the part its directory implies, and
  a branch adding to `SOURCES` merges cleanly.
- **The regexes are asserted, not trusted.** Configure fails if any part is empty (a renamed
  directory would otherwise move sources into another archive silently), if two parts overlap (the
  same TU compiled into two archives — which `/FORCE:MULTIPLE`, on for the USD build, would paper
  over at link time rather than report), or if the parts do not cover `SOURCES` exactly (a file in
  no part is simply not built).

Measured archive sizes after the split (Dist, `/GL`, USD + FFmpeg on):

| archive | bytes | % of 4 GiB |
|---|---:|---:|
| `OloEngine.lib` | 2,053,474,988 | 47.8% |
| `OloEngineRenderer.lib` | 1,593,328,912 | 37.1% |
| `OloEngineContent.lib` | 994,144,118 | 23.2% |

All three app targets link, and all three Dist binaries pass `--smoke-test`.

### The two traps in wiring the parts

1. **PRIVATE settings do not propagate.** The parts inherit `OloEngine`'s PUBLIC usage
   requirements through `target_link_libraries`, but *not* its PRIVATE ones — including
   `olo_set_common_definitions`, i.e. `OLO_DIST` / `OLO_RELEASE` / `OLO_DEBUG`, and the
   `MSVC_RUNTIME_LIBRARY` property. A part compiled without `OLO_DIST` is an ODR violation that
   **links cleanly and misbehaves at runtime**. They are applied explicitly per part and must stay
   in step with the ones applied to `OloEngine`.
2. **The plain and keyword `target_link_libraries` signatures cannot be mixed on one target.**
   `OloEngine/CMakeLists.txt` uses the plain form throughout, so additions must too. The link
   between core and parts is deliberately *mutual* — they call into each other — which CMake
   documents as supported for STATIC libraries and resolves by repeating them on the link line.

## 5. The guard, and what to do when it fires

`cmake/CheckArchiveSize.cmake` runs as a POST_BUILD step on each archive and prints its size and
headroom on every build, warns at `OLO_ARCHIVE_WARN_PERCENT` (75) and fails at
`OLO_ARCHIVE_FAIL_PERCENT` (90) with a message that names the cause. That converts a cliff into a
slope: the number is in front of you long before the wall is.

**When it fires, cut the offending part further along the same directory lines. Do not raise the
threshold, and do not reach for `/GL-` on a hot translation unit to buy a few months** — §3 is the
arithmetic showing why that runs out.

`.github/workflows/dist-archive.yml` builds the real LTO Dist archives on a schedule so the number
is watched even though nobody builds Dist by hand. It is deliberately **not** per-PR: it is a cold
build with no compiler cache (`/GL` objects are not cacheable), for a quantity that moves by a few
MB per feature. A weekly cadence catches the creep with days of warning; per-PR would roughly
double Windows CI cost for every change.
