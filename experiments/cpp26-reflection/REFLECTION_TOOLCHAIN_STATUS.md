# C++26 reflection — toolchain availability tracker

A dated snapshot of P2996 (C++26 static reflection) support across the compilers that matter for
OloEngine. This gates productionising the reflection-based OloHeaderTool replacement (see ADR 0009
and `OLOHEADERTOOL_REPLACEMENT.md`): the design is proven on GCC, but the shipping engine builds on
**MSVC** (primary) and **clang-cl** (secondary), so *those* are the gate.

**Update cadence:** re-check ~monthly (see the checklist at the bottom). Append to the changelog;
edit the table in place.

## Status — last checked **2026-07-29**

| Compiler | Reflection status | Shippable for OloEngine? |
|---|---|---|
| **GCC 16.1** | ✅ **Shipped in a released compiler** (Apr 2026) — `-freflection`, `^^` operator. Currently the most mature P2996 implementation. | Builds only `OloServer` (WSL) — not a shipping-engine target, but this is what the prototype runs on. |
| **Clang (Bloomberg fork)** | 🟡 `bloomberg/clang-p2996` (Dan Katz) — most *complete* Clang impl; `-freflection-latest` enables P1036/P3096/P3394/P3491. Self-described "highly experimental… occasional crashes." | Experiments only. |
| **Clang (mainline LLVM)** | 🟡 Upstreaming **in progress, targeting Clang 22**, behind a `cc1` flag; generative features (`define_aggregate`) deferred. First PR (operator parsing) merged ~Feb 2026; `meta::info` still being built. | Not yet — watch Clang 22. |
| **MSVC** | ❌ **No public support, no published ETA.** Jul 2026 build-tools preview doesn't mention reflection; still shipping C++23 conformance. | **Blocked** — the primary toolchain, and the least far along. |

## What this means

- **Feasibility: done.** The whole OloHeaderTool-C++-side + Lua + C#-schema prototype works on GCC 16.1.
- **Productionisation: gated on MSVC**, realistically **2027–2028** for a usable, non-experimental impl,
  and MSVC hasn't even shipped an experimental one yet. clang-cl may become an experimentation path in
  the **Clang 22** window (~2026-H2 → 2027, cc1-gated).
- **Action:** keep OloHeaderTool as the shipping source of truth; treat this table's MSVC row flipping
  to 🟡 as the trigger to start real migration work, and Clang 22 shipping as the trigger to re-validate
  the entire prototype on a second compiler.

## Monthly re-check checklist

1. **MSVC** — newest [MSVC C++ Team Blog](https://devblogs.microsoft.com/cppblog/) build-tools/VS preview
   post: any mention of reflection / P2996 / an `/experimental` flag? (Highest-signal row to watch.)
2. **Clang mainline** — [LLVM reflection discourse thread](https://discourse.llvm.org/t/dedicated-meeting-for-c-26-reflection/88927)
   and the Clang 22 release notes: has P2996 landed behind a flag? Which release?
3. **Bloomberg fork** — [bloomberg/clang-p2996](https://github.com/bloomberg/clang-p2996): upstreaming
   progress ([issue #189](https://github.com/bloomberg/clang-p2996/issues/189)), stability notes.
4. **GCC** — any P2996 fixes in GCC 16.x / 17 release notes (esp. the pointer-to-member-splice and
   type-driven-recursion bugs this experiment hit).

## Changelog

- **2026-07-29** — initial snapshot. GCC 16.1 shipped (production). Clang: Bloomberg fork experimental,
  mainline targeting Clang 22 (cc1-gated, in progress). MSVC: nothing, no ETA (confirmed against the
  Jul 2026 build-tools preview). Sources in the review that produced this file.
