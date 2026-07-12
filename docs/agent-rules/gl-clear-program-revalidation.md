# NVIDIA revalidates the *bound* program at glClear — unbind before clearing

## The symptom

`OloEngine.log` shows OpenGL debug-callback performance warnings with **id 131218**:

```
OpenGL performance warning (source: API, id: 131218, occurrence: N):
Program/shader state performance warning: Vertex shader in program <N> is being
recompiled based on GL state. [program <N> = '<ShaderName>']
```

Each affected program pays a driver-side JIT recompile of its vertex shader —
a real (if one-off) runtime cost, not just log noise. The set of affected
programs looks random (in the original triage: a skinned shadow shader, a GPU
particle shader, FXAA, a fullscreen blit, an IBL convolution) and varies by
scene, which makes the classic explanations (integer vertex attributes, VAO
format mismatches, program-binary cache reloads) look plausible. **They were
all wrong here** — every VAO layout in the engine matches its shaders, and the
warning reproduced identically with the program-binary cache deleted.

## The actual mechanism

On NVIDIA, `glClear` / `glClearBuffer*` **revalidates the currently bound
program against the currently bound framebuffer**. If pass A ends with program
P bound and pass B then binds a framebuffer with a different configuration
(sample count, attachment set, depth-only vs MRT…) and clears it, the driver
recompiles P's vertex shader for a framebuffer **P will never draw into**.
The recompile is pure waste, and it strikes whichever program *happened to be
bound last* before a cross-FBO clear — which is why the affected set looks
arbitrary and scene-dependent, and why siblings drawn in the same pass (e.g.
`ShadowDepth` static vs `ShadowDepthSkinned`) differ: only the pass's **last**
drawn program is still bound at the next pass's clear.

This engine's stateless command dispatch makes the pattern systematic: the
`CommandDispatch` shader-bind cache deliberately leaves programs bound across
pass boundaries to skip redundant `glUseProgram` calls, so *every* frame's
pass sequence hands a stale program to the next pass's clear.

## The fix (in place since this doc was written)

`Utils::GLClearProgramGuard` (in `Platform/OpenGL/OpenGLUtilities.h`) is a
scoped guard that saves `GL_CURRENT_PROGRAM`, binds program 0 for the duration
of the clear, and **restores the previous program on scope exit**. It wraps
every clear:

- `OpenGLRendererAPI::Clear` / `ClearDepthOnly` / `ClearColorAndDepth` / `ClearStencil`
- `OpenGLFramebuffer::ClearAllAttachments` / `ClearAttachment(vec4)`
- `WaterRenderPass`'s raw depth clear

If you add a new clear call site (raw `glClear*`, or a new RendererAPI/
Framebuffer clear helper), put a `Utils::GLClearProgramGuard` in scope around
it.

**The restore half is not optional.** The first attempt unbound without
restoring, which silently broke every *bind-once, clear-and-draw-per-face*
loop — `SkyCubemapBake` binds the sky shader once and then clears + draws six
cubemap faces; with a bare unbind the six draws ran with program 0 and every
sky/IBL bake went black (caught by `ProceduralSkyBakeTest` /
`StarNestSkyBakeTest` / the reflection-probe visual tests, 13 failures).
Restoring keeps every caller's — and the `CommandDispatch` shader-bind
cache's — view of the bind state truthful, so no cache invalidation is needed
either. Rebinding after the clear does not re-trigger the recompile: the
driver revalidates at *work* commands (draws/clears), not at `glUseProgram`.

## How to debug id 131218 (or any per-program driver warning) fast

1. **Program ids are resolved to shader names automatically.** Programs are
   `glObjectLabel`-ed and registered in a CPU-side id→name registry at link
   time (`OpenGLShader::FinalizeProgram`, `OpenGLComputeShader::Compile`);
   the GL debug callback appends `[program N = 'Name']` to any performance
   message that mentions a program id. (The registry must stay CPU-side:
   KHR_debug makes calling GL — e.g. `glGetObjectLabel` — from inside the
   debug callback undefined behavior.)
2. **The callback captures a native call stack for id 131218** (first few
   occurrences). Because the debug context is synchronous
   (`GL_DEBUG_OUTPUT_SYNCHRONOUS`), the stack pins the exact GL call the
   driver recompiled under. That is what cracked this case: every stack ended
   in a **clear**, not a draw — instantly killing the VAO/attribute theories.
3. The throttle in `OpenGLDebug.cpp` counts occurrences **per message id**,
   not per program — a stable occurrence count means one-off recompiles; a
   growing count means something recompiles per frame and is urgent.

## A related failure: restoring a program deleted while it was "current" (issue #625)

`GLClearProgramGuard`'s restore half assumes the saved `m_PreviousProgram` id is
still valid at scope exit. It can stop being valid *because of the guard's own
constructor*: if that program was already flagged for deletion by
`glDeleteProgram` while still bound elsewhere (GL defers real deletion of a
current program until something unbinds it), the guard's own `glUseProgram(0)`
is what completes the deletion — and the destructor's restore then fails with
`GL_INVALID_VALUE`. The destructor now guards with `glIsProgram()` before
restoring, and the engine's shader teardown paths unbind-if-current before
`glDeleteProgram` so this should no longer arise in practice. Full writeup —
including why the engine's deferred-deletion queue makes the flagged-for-deletion
state outlive the deleting test by several frames — is in
`docs/agent-rules/testing-architecture.md` §6.6.

## Reusable lessons

- **Don't trust the folk etiology for driver warnings.** Id 131218's usual
  suspects (integer attributes, VAO/shader format mismatches, disabled
  attribute arrays) were all systematically checked and all innocent. The
  synchronous-callback stack trace was decisive evidence; get it before
  theorizing.
- **The warning fires where the driver does the work, not where you caused
  it.** The "offending" call was a `glClear` in a *later* pass; the cause was
  the *earlier* pass leaving its program bound. Read the stack as "driver
  flush point", not "buggy call".
- **State-leak hygiene at pass boundaries matters even for "free" state.** A
  bound-but-unused program costs nothing by the spec, but drivers attach
  validation work to it at surprising points (clears here; potentially blits
  and TexImage paths elsewhere).
