# SonarQube ↔ local `/code-review` alignment

Read this before running `/code-review` (or any review pass) on this repo, so local
review **matches the SonarCloud scan** instead of fighting it: don't re-flag what we
deliberately suppress, use the same thresholds, and pre-empt the high-severity rules the
slow cloud scan (~1.5–2 h) would otherwise catch first.

The authoritative *rationale* for every suppression lives in
[../sonarqube-rule-suggestions.md](../sonarqube-rule-suggestions.md); the path-scoped
exclusions are enforced in [`sonar-project.properties`](../../sonar-project.properties).
This file is the **operational digest** for a reviewer.

**Profile:** `C++ Extended` (cpp) — **579 active rules** (83 BLOCKER / 132 CRITICAL /
240 MAJOR / 116 MINOR / 8 INFO). Source of truth is the SonarCloud Quality Profile
(*Quality Profiles → C++ Extended*); a point-in-time export lives at the repo root as
`c++ extended profile.xml` (it predates the S5536/S1271/S1712 deactivation below, so it
still lists 582 — those three are the entire 582 → 579 delta. The fourth rule deactivated
below, `cpp:S909`, was *already* inactive when the export was taken, so it is counted in
neither 582 nor 579). Editing that XML changes nothing until it is re-imported in the
SonarCloud UI — treat it as read-only reference.

---

## 1. Do NOT flag these — deliberate suppressions (don't fight the config)

These fire in idiomatic OloEngine code where the rule's concern doesn't apply. They are
**scoped out per-path** in `sonar-project.properties` (so they stay active everywhere
else) — a local reviewer must not raise them inside the listed scopes:

| Rule | Scope (don't flag here) | Why |
|---|---|---|
| `cpp:S5443` writable temp dir | `OloEngine/tests/**` | GoogleTest fixtures legitimately use `temp_directory_path()`; the multi-user-race model doesn't apply on CI/dev. |
| `cpp:S3656` protected members | `OloEngine/tests/**` | `TEST_F` fixtures require subclass-accessible (protected) members. |
| `cpp:S997` declare in namespace | `OloEngine/tests/**` | GoogleTest fixture classes are conventionally file-scoped. |
| `cpp:S1001` using-directive | `OloEngine/tests/**` | `using namespace OloEngine;` is the established test-suite convention. |
| `cpp:S5000` `==` for non-trivially-copyable | `OloEngine/src/OloEngine/Math/Math.h` | `Math::BitwiseEqual` is `static_assert`-guarded `std::is_trivially_copyable_v<T>`; misuse is a compile error. |
| `cpp:S1067` ≤3 conditional operators | `OloEngine/src/OloEngine/Scene/**` | ECS `HasComponent<>()` predicate chains / serializer dispatch are intrinsically long; naming each sub-predicate just restates it. |
| `cpp:S963` parenthesize macro params | `OloEngine/src/OloEngine/Scene/Prefab.cpp` | X-macro pastes component types into `<CompType>` template-arg lists where the grammar forbids parentheses. |
| `cpp:S1244` float `==` | `Scene/Components.h`, `Renderer/Commands/RenderCommand.h` | Defaulted/field-wise **bit-exact** equality is the intended change-detection (undo-redo / render-command dedup) semantics — **only in these two files**; real float-`==` bugs elsewhere stay flagged. |

CPD (copy-paste) is excluded on `OloEngine/tests/**` — the scaffold-uniform math/visual
test harness is structurally same by design, not copy-paste rot. **Don't propose
"deduplicate these tests".**

**Deactivated profile-wide** (not just scoped) — never raise these at all:

- `cpp:S5536` "unused functions should be removed" — an engine is a *library codebase*; public API is invoked by games / scripts / reflection / serialization / tests the analyzer can't see. (`cpp:S1144`, unused *private* members, stays **active** — those are real dead code.)
- `cpp:S1271` "use `::` to access globals" — huge churn across a 312k-LOC engine for negligible gain.
- `cpp:S1712` "no default parameters" — default arguments are idiomatic C++.
- `cpp:S909` "no `continue`" — MISRA-style; `continue` is idiomatic here.

---

## 2. Use THESE thresholds — not stricter

The profile relaxes many size/complexity limits to fit a real-time engine. Match these
when judging "too long / too complex"; don't hold code to tighter defaults:

| Rule | Limit | |
|---|---|---|
| `S103` line length | **≤ 230** chars | (engine has wide signatures) |
| `S104` file length | **≤ 1000** LOC | |
| `S3776` cognitive complexity | **≤ 40** / function | renderer/serialiser state machines are branchy by nature |
| `S107` function parameters | **≤ 7** | |
| `S1142` returns / function | **≤ 3** | |
| `S1479` switch cases | **≤ 30** | |
| `S1448` methods / class | **≤ 35** | |
| `S1820` fields / class | **≤ 20** | |
| `S110` inheritance depth | **≤ 5** | |
| `S1151` switch-case body lines | **≤ 15** | |
| `S1188` lambda lines | **≤ 20** | |
| `S6184` coroutine lines | **≤ 100** | |
| `S6192` / `S6194` coroutine cyclomatic / cognitive | **≤ 10 / ≤ 25** | |
| `S924` `break`/`goto` per loop | **≤ 1** (MISRA) | so a loop with 2+ `break`s *will* be flagged by Sonar |
| `S1707` `TODO`/`FIXME` | needs an attribution `(name)` after it | bare `// TODO` is flagged (MINOR) |

Secret/credential detection is active and worth a local pass: `S2068` (hardcoded
password — hints `password,passwd,pwd,passphrase`), `S6418` (hardcoded secrets — hints
`apikey,api_key,auth,credential,secret,token`).

---

## 3. Pre-empt these — high-severity rules the cloud scan enforces

Catching these locally saves a full scan cycle. All are BLOCKER unless noted; the list is
representative (83 BLOCKERs total — query the profile or `show_rule cpp:<key>` for the
rest). Most relevant to OloEngine's C/C++-interop, asset I/O, networking and memory code:

**Memory / reliability (BUG):**
- `S3519` — memory access must be explicitly bounded (buffer overflow / off-by-one; prefer `std::array`/`std::vector`/`std::string`).
- `S3590` — don't `free`/`delete` stack-allocated or non-owned (static/const/code) memory.
- `S2095` — resources (`fopen`/`open`, handles) must be closed; prefer RAII. (`S3588`: no use-after-`fclose`.)
- `S2275` — `printf`-style format string must match args (UB otherwise); prefer C++23 `std::print`/`std::format`.
- `S5267` — a `[[noreturn]]` function must not actually return.
- `S3584` — memory leak (allocation never released on some path). *(Note: one known FP in `ClosableMpscQueue.h` is annotated — see the suggestions doc.)*
- `S2259` (MAJOR) — null-pointer dereference. *(Analyzer can't always correlate a `bool` guard with the pointer — verify before raising.)*

**Security (VULNERABILITY):**
- `S2076` — OS command injection (avoid `system()`/`popen()` with untrusted data; validate + use `exec*`/`posix_spawn`).
- `S2083` — path-traversal injection (canonicalize with `std::filesystem::canonical` and confirm the result stays under the base dir).
- `S5782` — POSIX buffer-size args must not exceed the buffer (`memchr(buf,c,sizeof buf)`).
- `S2755` — XXE (disable external entities in XML parsing).

When you raise one of these, prefer **fix in code** over suppression. The suggestions doc
records which historical hits were genuine fixes vs. analyzer false-positives — check it
before assuming a finding is new.

---

## 4. How to use this in a review

1. Skim §1 — silently drop any finding that lands in a suppressed scope.
2. Apply §2 thresholds when judging length/complexity — don't invent stricter limits.
3. Actively look for §3 classes in changed C/C++ (especially C-interop, file/network I/O,
   manual buffer/pointer work). Pin genuine ones with a code fix.
4. For anything ambiguous, the rationale and prior triage are in
   [../sonarqube-rule-suggestions.md](../sonarqube-rule-suggestions.md).

---

## 5. Per-subsystem MAINTAINABILITY cleanups — fix patterns & recurring false-positives

The merged per-subsystem code-smell sweeps (#406 Animation, #407 Audio, #415 Gameplay,
#418 Physics3D, Video) converged on a consistent bar. Reuse it for the next subsystem.

**The scan can be stale relative to HEAD.** Issues carry the line numbers from the scan
that created them; a later commit shifts them. **Verify every finding against the *current*
file** — don't trust the reported line. Some findings are already fixed (e.g. a rule-of-5
added after the scan); some moved.

**Fix these (mechanical, semantics-preserving):**
- `S6166` add `[[nodiscard]]` message → `[[nodiscard("<thing> must be used")]]` (only on
  *existing* attributes — adding new `[[nodiscard]]` creates *new* findings).
- `S6004` / `S5523` declare-then-assign → if/for **init-statement**, or an immediately-invoked
  lambda when a lock scopes the assignment: `const bool x = [this]{ std::scoped_lock l(m_M); return …; }();`.
- `S126` if/else-if with no trailing `else` → add a **commented** empty `else { /* … */ }`
  (the comment keeps `S108` "empty block" quiet).
- `S5421` global → `const` **only when never mutated** (a vtable/table, yes; mutable module
  state, no — see below).
- `S1067` >3 `&&`/`||`/`?:` in one expression → hoist a sub-predicate into a named `const bool`.
- `S3630` `reinterpret_cast` → `static_cast` when the source is genuinely a `void*` (e.g. a C
  handle `typedef`'d to `void`, like miniaudio's `ma_data_source`).
- `S3624` rule-of-5 → for a class with a **custom destructor that frees a raw resource**, declare
  the copy/move members (delete them for a `Scope`/`unique_ptr`-held, never-copied object). This
  is a genuine double-free guard, not noise.
- `S926` name unnamed prototype params; `S5350` read-only local pointer → pointer-to-const.

**Deliberately defer / don't fight (note in the PR, leave the code):**
- `S8417` explicit `std::memory_order_*` — usually a *deliberate* `relaxed` on a counter/flag;
  "fixing" to `seq_cst` is a behaviour/perf change and a real risk. Deferred as noise in Audio + Video.
- `S4136` group overloaded members — judgement/format churn; deferred in Audio + Video.
- `S6168` `std::thread` → `std::jthread` — changes join/stop semantics ⇒ **behaviour change**.
- `S1142` (>3 returns), `S1820` (>20 fields), `S5414` (mixed public/private members) — invasive
  structural refactors, not mechanical; leave for a dedicated change.

**Recurring false-positives in C-ABI / pImpl / interop code (don't "fix", optionally mark won't-fix):**
- `S5008` `void*` → meaningful type — false when the `void*` is mandated by a C callback
  signature (e.g. a miniaudio `ma_data_source_vtable` callback) or a `void**` C API
  (`ma_pcm_rb_acquire_read/write`). Can't change without breaking the ABI.
- `S5421` global → const — false for genuinely **mutable** module state (e.g. Video's
  `s_FullscreenPlayer` / `s_FullscreenSkippable`, reassigned at runtime).
- `S3624` rule-of-5 — false for a **pImpl** class that already declares move-only semantics with
  an out-of-line `= default` destructor (the idiom *requires* defaulted-but-out-of-line specials;
  SonarCpp wants a non-defaulted body it can't have).
- `S859` / `M23_090` `const_cast` removing const — often forced by a **downstream API that isn't
  const-correct** (e.g. `Texture2D::SetData(void*)` called with a `const u8*` frame). The real fix
  is const-correcting that API; out of scope for a one-subsystem sweep.
- `S988` remove a libc include — **false on a vendored single-header amalgam wrapper.**
  `OloEngine/src/OloEngine/Video/PlMpeg.cpp` is the pl_mpeg TU (`#define PL_MPEG_IMPLEMENTATION`);
  its `#include <stdio.h>` is **required and documented** (pl_mpeg's `FILE*` API needs it before the
  impl, and the no-PCH cacheable build can't force-include it). Treat that file like `vendor/` — don't
  clean it; only its first-party wrapper `PlMpegBackend.cpp` and the rest of `Video/` are fair game.
- `S953` "replace union with std::variant" in `Audio/SampleBufferOperations.h` — **false: the
  `union`s are the `__m128` / `__m256` SIMD intrinsic types** (MSVC/Clang model them as unions).
  `std::variant` cannot back a vector register; do **not** "fix" these. **BUT** don't let the union
  noise mask the file's real content: `SampleBufferOperations.h` had a genuine SIMD-vs-scalar
  divergence in `ApplyGainRamp` (the per-lane gain index was `(i/numChannels)+(lane/numChannels)`
  instead of `(i+lane)/numChannels`, wrong for 3/5/6/7-channel buffers — fixed by gating the fast
  path on `width % numChannels == 0`; pinned by `SampleBufferOps.ApplyGainRamp*ChannelMatchesScalar`
  in `AudioSpatializerTest.cpp`). When a hot SIMD file lights up with `S953`, skip the union hits but
  **read the vectorized index math against the scalar fallback** — that's where the real bugs hide.
