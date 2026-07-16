# A setter-expression-based MCP field path for private-member components (issue #607, AudioSourceComponent slice)

`AudioSourceComponent`'s 16 authored parameters live behind `private
std::unique_ptr<AudioSourceColdData> m_Cold`, reached only through `OLO_PROPERTY`
Get/Set expression strings (the same ones Lua/C# scripting compiles). Making them
MCP-writable needed a second write mechanism, not just a bigger field scan. Four
non-obvious pitfalls, in the order they bite.

## 1. A whole-object-copy-then-swap write path is unsound for a component whose `operator=` doesn't preserve runtime state

The existing `MakeFieldAccess`/`MakeField` path (`McpGenericFieldWrite.h`) copies the
component, mutates the copy, and pushes a `ComponentChangeCommand<C>` whose
Execute/Undo do `entity.GetComponent<T>() = newData/oldData`. That is exactly wrong
for `AudioSourceComponent`: its `operator=` unconditionally resets `ActiveEventID` to
`0` and never re-invokes `Source->SetVolume()`/`SetPitch()`/etc — so a write would
silently detach a *playing* sound's event handle on every field change, and the live
`Ref<AudioSource> Source` would never actually receive the new value even though the
stored `Config` looked right.

The fix is `MakeSetterField<C, F>` + `PropertySetCommand<C, F, SetFn>`
(`McpGenericFieldWrite.h` / `ComponentCommands.h`): it calls the setter **directly on
the live component**, both on Execute and Undo, and never constructs a second
component instance. Reach for this whenever a component's copy/assignment can't be
trusted to reproduce a field write's side effects — not just for private members.

## 2. Reuse the OLO_PROPERTY scan's `{v}`-substitution machinery, don't re-derive it

`OloHeaderTool::EmitMcpSetterFields` builds its Get/Set lambdas from the exact same
`PropertyDef::customGet`/`customSet` strings `EmitCppBindings` already compiles for
scripting, via the same `ReplaceAll(expr, "{v}", "v")` + `EmitStatements` pair
(multi-statement setters, `if (...) body` split onto its own line). Two independent
parsers of the same `Get = "..."`/`Set = "..."` text would drift; there's exactly one
scan (`ParseHeaders`) feeding both the scripting emitter and this one.

## 3. `OLO_PROPERTY(Type = "ulong")` must map to `AssetHandle`, not raw `u64`, for MCP

`McpGenericFieldWrite.h`'s `CoerceJson`/`FieldToJson` only support a 64-bit field
through the `UUID` (== `AssetHandle`) specialisation — a decimal-digit JSON string,
since a raw `u64` exceeds JSON's safe-integer range. A plain `is_integral_v<F>` path
`static_assert`s `sizeof(F) <= 4`. So the setter-emitter's `PropType::U64 ->
"AssetHandle"` mapping (`McpSetterCppType` in `main.cpp`) is load-bearing, not
cosmetic — a raw `u64` lambda return type would fail to compile. Every current
`OLO_PROPERTY(Type = "ulong")` property in the codebase already round-trips through
`AssetHandle(u64)`/`static_cast<u64>` in its own Get/Set text, so the mapping is safe
without re-deriving it per property — but if a genuinely non-handle 64-bit
`OLO_PROPERTY` ever appears, this assumption needs revisiting.

## 4. A component wrapping a real backend (audio, video, network) usually can't be constructed in a headless unit test

`AudioSource`'s constructor takes a real file path and initializes miniaudio state —
`AudioEventsManagerTest.cpp` already documents that the headless test environment
provides no audio file, so `Ref<AudioSource>::Create()` cannot be exercised there.
The regression pin for "a field write must not reset `ActiveEventID`" therefore
tests that half of the bug with `Source == nullptr` (the setter's `if (comp.Source)`
guard just no-ops) rather than constructing a live `AudioSource` — the "the write
actually reaches `Source->SetVolume()`" half needs either a manual/live-editor check
or a mock backend, not a plain `OLO_TEST_LAYER: unit` test.

## 5. The editor's MCP write-consent gate ("Agent writes") is human-only — a non-interactive agent session can't unlock it to verify a write end-to-end

There is no env var or headless bypass: `Disabled`/`Prompt`/`Allow all` is a
per-session ImGui toggle in the MCP panel, off by default and never persisted (see
`docs/guides/mcp-diagnostics-server.md` § Write consent). `olo_input_inject` (which
could in principle click the toggle) is itself a consented write, so it can't be used
to unlock consent — by design, a human has to be sitting at the editor. If you reach
this same wall doing MCP write-path work from a non-interactive agent session:
confirm what you can with unit tests + a read of the generated code, and say
explicitly that the live-write half needs a human at the editor, rather than
skipping the verification silently.
