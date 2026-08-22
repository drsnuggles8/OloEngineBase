# A "correct fix that doesn't work" can be the build, not the code — clean-rebuild before you trust a live symptom

Issue #841. A one-line, obviously-correct fix (replace an owning `Ref<Shader>` member
with a non-owning raw pointer) appeared to do nothing across four separate rebuild-and-relaunch
cycles: the same 115 shaders still "survived" `Renderer::Shutdown()`, live refcount tracing showed
every survivor pinned at exactly 1 with no reachable second owner, and — after adding more
diagnostics — a live editor session started reproducibly double-freeing an unrelated shader on
close. All of this evaporated the moment the affected translation units were deleted and rebuilt
from scratch. The code had been right since the first edit.

## The trap

`ShaderResourceRegistry` is a **by-value member** of every `OpenGLShader`
(`OpenGLShader::m_ResourceRegistry`), and its destructor is declared `~ShaderResourceRegistry() =
default;` **inline, in the header**. Changing `m_Shader` from an owning `Ref<Shader>` to a
non-owning `const Shader*` changes the class's layout and, because the destructor body is
generated per-translation-unit from the header it sees, changes what that inline destructor's
generated machine code actually does — from "decrement a `Ref<Shader>`" to "do nothing to that
field at all".

With an **incremental, ccache-backed** build (`dev-cached` — the repo's own recommended default
tree), some translation units that transitively depend on the header were recompiled against the
new layout and some were not (or a stale `.obj` was reused, or the linker's COMDAT/`/OPT:ICF`
folding picked an old copy of the inline destructor to keep). The result links **without error**:
every symbol resolves, the binary runs, and the specific `.cpp` files actually edited show their
new log lines firing exactly as written — because those files' *own* code really did recompile.
What silently didn't follow was every other TU's copy of the header-inline destructor for the
*embedded member type*, which is a different, easy-to-miss dependency edge than "did the file I
edited get recompiled".

## What it looked like from the debugging seat

Four rounds of "add a diagnostic, rebuild, relaunch, read the log" each *confirmed a different,
increasingly implausible finding*, in this order:

1. **The self-ref fix appeared to change nothing.** Before-and-after `[Teardown]` reports both
   showed 115 survivors. Since this was compared across separate sessions rather than a true
   same-binary A/B, it read as "the fix didn't help" rather than "the fix isn't fully linked".
2. **Every survivor's live `RefCounted::GetRefCount()` read exactly 1, with only one known owner.**
   For `ShaderLibrary::s_FallbackShader` specifically: the log traced its address at creation, at
   the *start* of `ShutdownFallbackShader()`, and confirmed `s_FallbackShader.Reset()` runs
   unconditionally on a call path proven to complete (the survivor report itself only prints from
   the last line of `Renderer::Shutdown()`). No second owner was ever found by code reading, and
   none should have existed under the (correct) fixed header. This is the shape of a build problem
   presenting as "the object structurally cannot have a second owner, yet behaves as if it does".
3. **A live session started reliably crashing** with `RefCounted: DecRefCount() underflow` on an
   *unrelated* shader (`AssetPreviewRenderer`'s `MaterialPreview`), immediately after that shader's
   own destructor ran to completion. Two independent background-agent investigations, plus manual
   tracing of every member and base-class destructor in the object graph, found no plausible
   second-release site — because there wasn't one in the *current* source.
4. **A live debugger attach on the hung (assert-blocked) process produced the answer that ended
   the hunt**: the call stack showed `Ref<Shader>::Reset` → `Ref<Shader>::DecRef` → (legitimate)
   `~OpenGLShader` → **`ShaderResourceRegistry::~ShaderResourceRegistry` → `Ref<Shader>::~Ref`** →
   `RefUtils::Release` → the assert. That inner call is *impossible* under the fixed header —
   `ShaderResourceRegistry` no longer has a `Ref<Shader>` member for anything to destruct. The
   only explanation is a stale/ODR-mismatched destructor body, decrementing the shader's *own*
   refcount a second time because the raw-pointer field's bit pattern (the shader's own address,
   under the correct layout) was being read as if it were still a `Ref<Shader>::m_Instance`.

Deleting `build-cached/{OloEngine,OloEngineRenderer,OloEditor}/**/CMakeFiles/*.dir` and rebuilding
from scratch made both symptoms disappear in the very next run: zero survivors, zero crash, on a
binary whose *only* source change since the previous (broken-looking) run was the diagnostic
prints being removed again.

## The rule

**When a fix you have re-derived correctly by reading the code multiple times produces a *live*
result that makes no logical sense — not "still buggy in an understandable way", but "the object
graph as written cannot behave like this" — stop adding diagnostics and do a clean rebuild of the
affected static libraries before spending more time on code-level theories.** The tell that
distinguishes this from an actual logic bug: your *own* newly-added log lines, in the files you
just edited, print correctly and prove those specific `.cpp`s recompiled — while the *behavior*
still matches the pre-fix code path for a type those files don't define. That combination (my
edits ran, but something depending on my header change didn't) is the specific signature of
partial/inconsistent incremental linking, not a mistaken fix.

This is expensive to reach empirically (four rebuild-and-test rounds, two background-agent
investigations, and a live debugger attach, none of which found anything wrong with the code)
compared to just trying a clean rebuild early once the live behavior stops matching what the
source says it should do. **Full-clean is not the *first* thing to reach for** — most incremental
staleness is caught correctly by Ninja's own dependency tracking, and the routine advice elsewhere
in this repo (`build-result-verification`, the `msbuild-node-reuse-fakes-a-live-build` memory) is
about *build success* being misleading, not correctness of a build that genuinely reports and
looks green. This is narrower: a struct-layout change to a type with an inline `=default`
destructor, embedded **by value** in a widely-instantiated class, under an incremental
ccache-backed tree — reach for a clean rebuild specifically when *that* shape is present and the
live symptom contradicts what the current source can produce.

## Why this is plausible specifically here

- `dev-cached` is this repo's own recommended default build tree *because* it's fast — which means
  it is exercised far more than a from-scratch build, and staleness bugs in it get far less
  practice being caught.
- `ShaderResourceRegistry` is instantiated as a **by-value member**, not a pointer/`Ref` — so its
  ABI (`sizeof`, member offsets, and any header-inline special member function) leaks directly into
  every translation unit that touches `OpenGLShader`, `sizeof(OpenGLShader)`, or constructs/destroys
  one. A pointer-to-`ShaderResourceRegistry` member would not have this exposure.
- The destructor is the *implicit* `= default` kind, generated fresh per-TU from whatever the
  header looks like to that TU at that time — there is no single out-of-line definition for the
  linker to deduplicate from one authoritative source.

## A second, mechanical way into exactly this state (issue #858)

Everything above assumes ninja *knew* about the dependency and the staleness came in some other way.
There was also a period where ninja knew nothing at all: with the compiler cache in front of
clang-cl, an object served from cache was recorded with **zero header dependencies**, so a header
edit rebuilt nothing — measured at 699 of 701 records in one worktree. That is this document's
symptom arriving through the build graph rather than through a `=default` destructor, and it is
fixed (`cmake/CompilerCache.cmake` forces `/showIncludes`) with a check wired into `build-lock.ps1`.
If a fix with no effect ever shows up again in a cached tree, run
`pwsh -File scripts/Check-NinjaHeaderDeps.ps1 -BuildDir build-cached` before re-deriving anything —
it answers "is the build graph even watching that header?" in a second. Details:
[build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) §6.

## Guard

None added. This is a build-environment failure mode, not a source-level invariant a unit test can
pin — the fix, once actually linked, is exactly what `ShaderResourceRegistrySelfRefTest.cpp`
(`OloEngine/tests/Rendering/`) already verifies: drop every external `Ref<Shader>` after
`InitializeResourceRegistry()` has run, and assert the `WeakRef` goes dead. What generalizes is the
debugging discipline above, not a mechanism to catch the staleness itself.

## Related

[build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) (the `dev-cached` tree's design
and its own known traps) · [dev-cached tree facts, project memory] (measured cold/warm costs) ·
[lazy-static-release-ownership.md](lazy-static-release-ownership.md) (the leak-class this issue was
originally filed under, and the actual, unrelated self-referential-`Ref` root cause once the build
staleness was cleared out of the way).
