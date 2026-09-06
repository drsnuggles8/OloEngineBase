# A process-wide registry must outlive everything that unregisters from it

If a destructor calls `SomeRegistry::Unregister(...)`, that registry has to be **immortal** — a
deliberately leaked heap allocation, never a plain `static T x;`. Otherwise the registry is torn
down first and the unregister reads freed memory.

```cpp
// Right. Never destroyed, so it cannot be destroyed too early.
auto GetRegistry() -> Registry&
{
    static auto* s_Instance = new Registry();
    return *s_Instance;
}
```

Issue #1088 is the worked example: an ASan `heap-use-after-free` at process exit whose report named
both ends at once.

```
READ of size 8 ... _Find_last
    #3  ShaderResourceRegistry::Unregister(unsigned int)
    #4  OloEngine::OpenGLShader::~OpenGLShader
   #16  OloEngine::ShaderLibrary::~ShaderLibrary
freed by: `dynamic atexit destructor for 's_Registries'`
```

## Why the ordering is not a coin flip

It is not "unspecified and we got unlucky". For a Meyers singleton it is **reliably wrong**, and the
reason is worth knowing because it tells you which statics are at risk:

- A function-local `static` registers its destructor with `atexit` **on first use**, at runtime.
- A namespace-scope object registers its destructor during **dynamic initialisation**, before `main`.
- Static destruction runs that list **LIFO**.

So a registry first touched when the first shader links is destroyed *before*
`Renderer2D::m_ShaderLibrary` and `Renderer3D::m_ShaderLibrary`, which are namespace-scope. Every
`Ref<Shader>` those libraries release during teardown then unregisters into a destroyed container.
Lazy creation is exactly what puts the registry at the wrong end of the list.

**The namespace-scope case is worse, not better.** `Shader.cpp`'s two program-capability sets were
namespace-scope objects in a different translation unit from the shader libraries, and the standard
does not order dynamic initialisation across translation units *at all*. That is not late, it is
unspecified: it can work for years and change with an unrelated link-order edit. You cannot fix it
by moving code around. The registry has to outlive its registrants.

## Leaking is the fix, not a workaround

Two objections come up, and both have answers:

- *"It is a leak."* It is, and it releases nothing, because these registries hold **non-owning**
  data — raw pointers and `u32` ids keyed to objects that own themselves. There is nothing to free.
  Outliving every registrant is the lifetime the object should have had. (A registry that owned
  GPU memory would be a different problem: see
  [lazy-static-release-ownership.md](lazy-static-release-ownership.md).)
- *"Just skip the unregister during teardown."* That is a silent early-out over a container that is
  genuinely still needed by every registrant released *before* it — a correctness change dressed as
  a lifetime fix, and exactly what [no-silent-fallbacks.md](no-silent-fallbacks.md) is about.

Check the destructor before leaking. All of #1088's were `= default`, and the cleanup that matters
(`Shutdown()`, and `RendererMemoryTracker`'s survivor report) is an explicit call, not a destructor
— so leaking loses nothing. If a destructor *does* real work, leaking is the wrong tool.

## ASan finds one member of a set, never the set

`halt_on_error=1` aborts at the first fault, so the report names whichever registry happened to be
destroyed first and says nothing about the rest. #1088's report named one; walking `~OpenGLShader`
line by line found **six** with the same defect:

| registry | reached from |
|---|---|
| `ShaderResourceRegistry`'s process map | `Unregister` — the one the report named |
| `FrameResourceManager::Get` | `SubmitForDeletion` for the GL program |
| `RendererMemoryTracker::GetInstance` | `OLO_TRACK_DEALLOC` |
| `ShaderDebugger::GetInstance` | `OLO_SHADER_UNREGISTER` |
| `Shader.cpp`'s `BindlessPrograms` | deletion lambda → `Shader::UnregisterProgram` |
| `Shader.cpp`'s `MaterialOffsetPrograms` | same |

Fixing the first and re-running gave a **clean full ASan suite**, which is easy to read as "there
was one bug". There were six; five were simply unreachable while the first aborted. **Enumerate the
destructor's call graph by reading it — a green sanitizer run after a lifetime fix is evidence about
one path, not about the class.**

`ShaderDebugger` makes the point sharply: `OLO_SHADER_UNREGISTER` compiles to nothing outside
`OLO_DEBUG`, so no amount of Release ASan running will ever reach it.

## The guard, and why a dynamic one is not enough

`ShaderRegistryTeardownOrderTest` (`OloEngine/tests/Rendering/`) uses two, because **no CI
configuration builds Windows with ASan** — a purely dynamic guard would be green in every
configuration where the regression could be reintroduced.

1. **A source scan** over a seeded roster of all six sites, asserting each accessor keeps the
   `static auto* x = new ...` form. Runs everywhere, no GPU, no sanitizer. Seeded rather than
   discovered, for the reason
   [lazy-static-release-ownership.md](lazy-static-release-ownership.md) gives twice: a scan that
   discovers its own targets can stop checking. A new registry on this path means a new row.
2. **A static-destruction probe** — a namespace-scope object whose destructor does a
   `Register`/`Find`/`Unregister` round trip, so it runs *after* the registry's would-be destructor.
   It makes the fault deterministic for **any** invocation of the test binary rather than only the
   process shapes that populated both statics, which is what made #1088 read as a flake.

Both were red-checked, and the probe **in the compiled binary** rather than in a scratch copy of the
scan — the failure mode that shipped once already in this repo's history. Built against pre-fix
source it faults on a filter that creates no shaders, no GPU and no renderer at all.

One trap the source scan hit in review, worth copying to any scan like it: brace-matching a function
body over **raw** source is unsafe when the body carries a long comment — here a quoted ASan stack.
A single `}` written into that comment truncates the extracted body and the guard reports a
regression that is not there. A false red is the worst outcome for a guard like this, because the
obvious response is to "fix" correct code. Strip comments and string literals before matching.
