# A re-populated blackboard forces every Setup() to re-run

**Rule.** When `PopulateBlackboard` runs its body, `CompileFrameGraph` invalidates the build
cache before `BuildFrameGraph`, whatever the key says. Never invalidate the build cache from
*inside* `PopulateBlackboard` to take effect next frame: this frame's `BuildFrameGraph` runs after
it and re-arms the cache under the same key.

## Why

The declaration cache has two layers keyed on one value: the blackboard (`PopulateBlackboard`) and
the graph build (`BuildFrameGraph`, which runs every pass's `Setup()`). They usually miss together.
They part when a populate invalidates the blackboard cache for the next frame without the key
moving. The SSR history resize does this: `EnsureHistoryStorage` clears the valid flag after the
key was hashed from it.

The old code invalidated both caches there, and called the second call "the load-bearing half".
But `BuildFrameGraph` ran later in the same frame and re-armed the build cache. So on the next
frame:

- the blackboard re-populated, and `ClearImportedResources` retired every texture view;
- the build cache hit, and no `Setup()` ran;
- every pass executed with handles that had just gone stale.

That frame came with every upscale toggle in Deferred with SSR on, including `ReferenceHead.olo`'s
defaults. GTAO published AO = 1, and `AOApplyRenderPass` / `SSRRenderPass` hit their
"enabled without resolved graph input/output" asserts. In a Debug editor that killed or hung the
process on both backends.

It hid for three reasons:

- the verify lever rebuilds every cache hit, so the cache tests could not see it;
- the headless upscale tests run with SSR off;
- the live Deferred × upscale verification cell had only been run through a Release build, or not
  at all.

## How to check

`FrameGraphDeclarationCacheEvidenceTest.ARepopulatedBlackboardNeverServesACachedGraph` runs
Deferred + SSR + GTAO with the verifier **off** and toggles upscale, asserting zero graph resolve
failures on each of the following frames. On the old code the test binary aborts on SSR's assert.
