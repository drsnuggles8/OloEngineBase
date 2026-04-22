# Platform Code Reorganization Plan

**Branch:** `refactor/platform-code-reorganization`
**Goal:** Consolidate all OS-specific code under `OloEngine/src/Platform/<OS>/`, keep `OloEngine/src/OloEngine/<Subsystem>/` platform-agnostic, and remove the parallel `HAL/Windows` + `HAL/Linux` subtree that accidentally duplicated the pattern.

## Guiding Pattern

```text
OloEngine/src/OloEngine/<Subsystem>/Foo.h       ← public API / generic interface
OloEngine/src/OloEngine/<Subsystem>/Foo.cpp     ← generic code + platform-agnostic dispatch
OloEngine/src/Platform/Windows/WindowsFoo.cpp   ← Win32 impl
OloEngine/src/Platform/Linux/LinuxFoo.cpp       ← POSIX impl
```

Rules:

- Public headers expose only a platform-neutral interface. No `<Windows.h>` leaks.
- Platform selection lives in one place: either a factory in the generic `.cpp`, or a `using FXxx = FWindowsXxx;` alias under `OLO_PLATFORM_*` in the public header.
- `Platform/<OS>/` files are added to `SOURCES` in the matching CMake `if(WIN32) / elseif(UNIX AND NOT APPLE)` block in [OloEngine/src/CMakeLists.txt](../OloEngine/src/CMakeLists.txt).
- `#ifdef OLO_PLATFORM_WINDOWS` blocks of more than ~5 lines inside otherwise-generic files are candidates for extraction.
- Tiny branches (2–4 lines, e.g. path separator, exe extension) may remain inline — call them out explicitly as "acceptable inline" rather than letting them proliferate.

---

## Phase A — COMPLETED

Moved misplaced `HAL/Windows/` + `HAL/Linux/` event headers into the canonical `Platform/` tree.

- `OloEngine/HAL/Windows/WindowsEvent.h` → `Platform/Windows/WindowsEvent.h`
- `OloEngine/HAL/Linux/LinuxEvent.h` → `Platform/Linux/LinuxEvent.h`
- Updated include paths in `OloEngine/HAL/Event.cpp`.
- Updated CMake sources list.
- Removed now-empty `HAL/Windows/` and `HAL/Linux/` directories.
- Debug build verified.

---

## Phase B — Split HAL `.h` headers that contain inline platform impls

These are UE-ported single-header "all platforms inline" designs. Target: keep the public interface in `HAL/`, move OS bodies to `Platform/<OS>/`.

### B1. `HAL/Semaphore.h` (smallest — header-only split)

**Current:** defines `FWindowsSemaphore` (Win32 `CreateSemaphoreW`/`WaitForSingleObject`) and `FStdSemaphore` (C++20 `std::counting_semaphore`) inline, aliases `using FSemaphore = …` by macro.

**Target:**

- `HAL/Semaphore.h` — keep only the `FSemaphore` alias; `#include "Platform/<OS>/<OS>Semaphore.h"` under platform guard.
- `Platform/Windows/WindowsSemaphore.h` — `FWindowsSemaphore` class.
- `Platform/Linux/LinuxSemaphore.h` — rename `FStdSemaphore` → `FLinuxSemaphore`.

**Notes:** header-only move. Remove `<Windows.h>` from `HAL/Semaphore.h`. No new `.cpp`.

### B2. `HAL/ThreadManager.h`

**Current:** Win32 `SetThreadDescription` dynamic-lookup and thread-name helpers inline at the top; pthread `pthread_setname_np` branch further down.

**Target:**

- `HAL/ThreadManager.h` — declare free functions / namespace `ThreadManagerPlatform::SetOSThreadName(...)`, `GetOSThreadId()`, etc. No bodies.
- `Platform/Windows/WindowsThreadManager.cpp` — Win32 definitions (includes `<Windows.h>`, does dynamic `GetProcAddress` for `SetThreadDescription`).
- `Platform/Linux/LinuxThreadManager.cpp` — pthread definitions.
- Keep the generic `ThreadManager` class (if any) in `HAL/ThreadManager.cpp` untouched aside from replacing the inline blocks with calls into the new namespace.

### B3. `HAL/RunnableThread.h` (largest — ~520 lines)

**Current:** `FRunnableThread` interface plus full Win32 and pthread subclasses inline, gated by `#ifdef _WIN32` / `#elif defined(__linux__) || defined(__APPLE__)`.

**Target:**

- `HAL/RunnableThread.h` — interface + static `Create()` factory, no platform bodies.
- `HAL/RunnableThread.cpp` (new) — implements `Create()` dispatching by macro to the platform subclass.
- `Platform/Windows/WindowsRunnableThread.h` + `.cpp` — `FWindowsRunnableThread : public FRunnableThread`.
- `Platform/Linux/LinuxRunnableThread.h` + `.cpp` — `FLinuxRunnableThread : public FRunnableThread`.

**Watch:** any `inline` / template methods referencing Win32 or pthread types must either move into the platform subclass headers, or stay in `HAL/` behind `#ifdef` guards with platform types forward-declared opaquely.

### B4. `HAL/PlatformProcess.h`

**Current:** UE-style `FPlatformProcess` "static namespace" with `SetThreadName`, `SetThreadAffinity`, `GetProcessId`, etc. — all inline per OS.

**Target:**

- `HAL/PlatformProcess.h` — declarations only: `namespace FPlatformProcess { void SetThreadName(...); bool SetThreadAffinity(...); ... }`.
- `Platform/Windows/WindowsPlatformProcess.cpp` — Win32 impls (`SetThreadDescription`, `SetThreadAffinityMask`, `GetCurrentProcessId`, …).
- `Platform/Linux/LinuxPlatformProcess.cpp` — pthread / `sched_setaffinity` / `getpid` impls.

### B5. `HAL/PlatformMisc.h`

**Current:** CPU topology (`GetLogicalProcessorInformationEx`), cache line sizes, big-small-core detection, etc. inline per OS.

**Target:** same treatment as B4 — interface in `HAL/PlatformMisc.h`, bodies in `Platform/<OS>/<OS>PlatformMisc.cpp`.

### Phase B risks / gotchas

- Moving `<Windows.h>` out of headers into `.cpp` files should **improve** compile times (PCH-adjacent hot paths currently drag the Win32 SDK in everywhere).
- Templates / truly inline helpers that must stay in headers: gate with `#ifdef`, or template on a type-trait to avoid baking in platform types.
- Watch for `OLO_CORE_ASSERT` / `OLO_CORE_CHECK_SLOW` macro availability — add `#include "OloEngine/Core/Assert.h"` in new `.cpp` files.
- Don't silently change signatures. If UE-ported code uses `HANDLE`, `DWORD`, `pthread_t`, keep them in platform headers only.

---

## Phase C — Extract Windows blocks from non-HAL subsystems

Ordered by payoff (biggest ugly-inline-block first).

### C1. `Debug/CrashReporter.cpp` — highest payoff (~300 lines of inline Win32)

**Current blocks:** SEH `SetUnhandledExceptionFilter`, `MiniDumpWriteDump` via DbgHelp, `RtlGetVersion` dynamic lookup, `CaptureStackBackTrace` + symbol resolution.

**Target:**

- `Debug/CrashReporter.cpp` — keep generic logic (message formatting, user-callback dispatch, log-file write, file path assembly).
- `Debug/CrashReporterPlatform.h` (new) — declare:
  ```cpp
  namespace OloEngine::CrashReporterPlatform {
      void Install(UnhandledCallback cb);
      void Uninstall();
      std::string CaptureStackTrace();
      std::string GetOSVersion();
      bool WriteMinidump(const std::filesystem::path& out, void* platformContext);
  }
  ```
- `Platform/Windows/WindowsCrashReporter.cpp` — SEH filter + DbgHelp impls. **Only file** including `<Windows.h>` + `<DbgHelp.h>`.
- `Platform/Linux/LinuxCrashReporter.cpp` — `sigaction(SIGSEGV/SIGABRT)` + `backtrace()` + `backtrace_symbols()`. Initial Linux impl may be a partial stub.

**CMake:** ensure `if(WIN32) target_link_libraries(OloEngine PRIVATE DbgHelp)` in [OloEngine/CMakeLists.txt](../OloEngine/CMakeLists.txt). Do this in the same commit as the split.

### C2. `Memory/GenericPlatformMemory.*` + `Memory/MallocAnsi.cpp`

**Current:** numerous `VirtualAlloc` / `HeapAlloc` / `GlobalMemoryStatusEx` blocks interleaved with generic code.

**Target:**

- `Memory/PlatformMemoryBackend.h` (new) — declare:
  ```cpp
  namespace OloEngine::PlatformMemoryBackend {
      u64 GetPhysicalMemoryBytes();
      u64 GetAvailableMemoryBytes();
      void* ReserveVirtual(size_t);
      void CommitVirtual(void* ptr, size_t);
      void DecommitVirtual(void* ptr, size_t);
      void ReleaseVirtual(void* ptr, size_t);
      size_t GetPageSize();
  }
  ```
- `Platform/Windows/WindowsPlatformMemory.cpp` — `VirtualAlloc`/`Free`, `GlobalMemoryStatusEx`, `GetSystemInfo`.
- `Platform/Linux/LinuxPlatformMemory.cpp` — `mmap`/`munmap`, `madvise(MADV_DONTNEED)`, `sysconf(_SC_PAGESIZE)`, `/proc/meminfo`.
- `Memory/GenericPlatformMemory.cpp` — becomes genuinely generic; calls the namespace.
- `Memory/MallocAnsi.cpp` — split into `FMallocAnsiWin` (`HeapAlloc`/`HeapFree`) under `Platform/Windows/WindowsMallocAnsi.cpp`, and a POSIX variant. Generic file selects by macro.

### C3. `Server/ServerConsole.cpp`

**Current:** Win32 console handle + VT-100 mode enable blocks at top-of-file and inside init routines.

**Target:**

- `Server/ServerConsolePlatform.h` (new) — `void ConfigureConsoleForANSI()`.
- `Platform/Windows/WindowsServerConsole.cpp` — `SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)` + UTF-8 code page setup.
- `Platform/Linux/LinuxServerConsole.cpp` — empty body (TTY already handles ANSI).

### C4. `Physics3D/JoltCaptureManager.cpp`

**Current:** three Win32 blocks — async-save file handle, `ShellExecute` to open the capture, path canonicalization.

**Target:**

- `Physics3D/JoltCaptureManagerPlatform.h` — `void OpenFileInShell(const path&)`, `path MakeAbsolute(const path&)`.
- `Platform/Windows/WindowsJoltCaptureManager.cpp` — `ShellExecuteW`, `GetFullPathNameW`.
- `Platform/Linux/LinuxJoltCaptureManager.cpp` — `xdg-open` via `fork/exec`, `std::filesystem::canonical`.

### C5. Small batch — can land as a single commit

- **`Core/Log.cpp`** — `SetConsoleOutputCP(CP_UTF8)` → `Platform/Windows/WindowsLogInit.cpp` behind `LogPlatform::ConfigureConsole()`.
- **`Networking/Core/NetworkLobby.cpp`** — `WSAStartup` / `WSACleanup` → `Platform/Windows/WindowsNetworking.cpp` behind `NetworkingPlatform::Init/Shutdown`. Linux stub empty.
- **`Build/GameBuildPipeline.cpp`** — executable extension / path handling behind `PlatformBuild::GetExecutableExtension()`. (Very small — borderline "acceptable inline" but easy win.)

### C6. Acceptable inline (do not split — document decision)

These have only 2–4 lines of platform-specific code and don't justify a file split; keep them inline but ensure they use `OLO_PLATFORM_WINDOWS` (not raw `_WIN32`):

- `Physics3D/JoltShapes.cpp`
- `Renderer/Debug/CommandPacketDebugger.cpp`
- `Utils/PlatformUtils.h` (already a pure-interface header — impls in `Platform/<OS>/<OS>PlatformUtils.cpp` exist).
- `Core/Window.cpp` (already a thin factory).
- `Core/PlatformDetection.h` (macro definitions only — untouchable).
- `Templates/UnrealTypeTraits.h` (type-trait branch only).
- `Debug/CrashReporter.cpp` — `localtime_s` / `localtime_r` 3-line CRT difference inside `GetTimestamp()`.
- `Physics3D/JoltCaptureManager.cpp` — same `localtime_s` / `localtime_r` 3-line CRT difference.
- `Memory/MallocAnsi.cpp` — `_aligned_malloc` vs `posix_memalign`, `_heapchk`, `HeapSetInformation`. CRT-family branching, not OS integration.
- `Memory/GenericPlatformMemory.h` — `_alloca` vs `alloca` macro (single line).
- `Memory/Platform.h` — small constant selection.
- `Core/Log.cpp` — `ShowAssertMessageBox` (3 lines, `MessageBoxA`); only active when `OLO_ASSERT_MESSAGE_BOX` is on.
- `Core/Log.h` — `__declspec(dllexport)` vs visibility attribute.
- `Core/EntryPoint.h` — `WinMain` vs `main` entry-point dispatch.
- `Core/Base.h` — primitive type aliasing / calling-convention macros.
- `Core/CriticalSection.h` — `CRITICAL_SECTION` vs `pthread_mutex_t` inline primitives (hot-path sync).
- `Core/PlatformTLS.h` — FastPlatformTLS kept header-inline for zero-overhead TLS access.
- `HAL/Semaphore.h` — thin dispatch header (post-B1).
- `HAL/Event.cpp` / `HAL/RunnableThread.cpp` / `HAL/ThreadManager.*` — factory dispatch; interiors moved to `Platform/<OS>/`.
- `HAL/ManualResetEvent.h/.cpp` — 6 header-inline branches for hot-path signal primitives (WaitOnAddress / futex). Ported verbatim from UE5.7; inline-per-branch is intentional for perf. Splitting into separate classes per platform would be a major refactor for a performance-critical primitive and is out of scope for this cleanup.
- `Networking/Core/NetworkLobby.cpp` — BSD-vs-WinSock socket shim at top of TU. Only consumer, well-isolated; extracting to a shared `SocketPlatform.h` can follow if/when a second networking TU needs the same shim.

### Phase C risks / gotchas

- **CrashReporter is the riskiest file.** SEH filters must remain in a TU with `<Windows.h>` and ideally nothing else. Keep `WindowsCrashReporter.cpp` minimal — no other engine headers beyond what's required for the interface it implements.
- **Link libraries:** `DbgHelp.lib` (C1), `Ws2_32.lib` / winsock (C5), `Shell32.lib` (C4 — usually default). Verify and add under `if(WIN32)` in CMake.
- **Static state:** platform `.cpp` must not reach into generic-file statics. Expose via functions or `extern` symbols.
- **Pre-commit / formatting:** run `pre-commit run --all-files` once after each phase's commits.

---

## Status — COMPLETED

All planned phases completed on branch `refactor/platform-code-reorganization`.

- **Phase A** — HAL/Windows + HAL/Linux moved into Platform/ tree (commit 8735ed84).
- **B1 Semaphore** — `FWindowsSemaphore`, `FLinuxSemaphore` extracted (commit 5544ce63).
- **B2 ThreadManager** — stale `<windows.h>` include removed (commit 96024695).
- **B3 RunnableThread** — opaque `uptr m_NativeHandle` + `RunnableThread.cpp` (commit 7993d849).
- **B4 PlatformProcess** — `WindowsPlatformProcess.cpp` + `LinuxPlatformProcess.cpp` (commit ca93f355).
- **B5 PlatformMisc** — `WindowsPlatformMisc.cpp` + `LinuxPlatformMisc.cpp` (commit c05f2c1e).
- **C1 CrashReporter** — `CrashReporterPlatform` namespace, `WindowsCrashReporter.cpp` (SEH + DbgHelp), Linux stub (commit 35ef3677).
- **C2 Memory** — `PlatformMemoryBackend` namespace, `WindowsPlatformMemory.cpp`, `LinuxPlatformMemory.cpp` (commit ed553257).
- **C3 ServerConsole** — `ServerConsolePlatform` namespace with opaque AbortState, `WindowsServerConsole.cpp` (CancelIoEx), `LinuxServerConsole.cpp` (self-pipe + poll) (commit 8012e51c).
- **C4 JoltCaptureManager** — `JoltCapturePlatform` namespace, `WindowsJoltCapture.cpp` (APPDATA), `LinuxJoltCapture.cpp` (XDG_DATA_HOME) (commit d6977043).
- **C5 GameBuildPipeline** — `BuildPipelinePlatform` namespace, `WindowsBuildPipeline.cpp` (PE RT_GROUP_ICON resource update via `UpdateResourceW`), Linux stub (commit 35a311e5).
- **C6** — acceptable-inline files documented above.

---

## Execution Order

1. **B1 Semaphore** — smallest, header-only, good smoke test of the process.
2. **B2 ThreadManager** → **B4 PlatformProcess** → **B5 PlatformMisc** — same pattern, low–medium risk.
3. **B3 RunnableThread** — biggest HAL split; do after the pattern is validated.
4. **C1 CrashReporter** — largest payoff in Phase C.
5. **C2 Memory** — two coordinated files.
6. **C3 ServerConsole** + **C4 JoltCaptureManager** + **C5 batch** — mechanical cleanup.

After each step:

- Build `OloEngine`, `OloEditor`, `OloEngine-Tests` in Debug.
- Run `run-tests-debug`.
- Stop and fix immediately on any break — do not stack splits on a broken tree.

---

## Acceptance Criteria

- [x] No files remain under `OloEngine/src/OloEngine/HAL/<OS>/`.
- [x] No `.cpp` file under `OloEngine/src/OloEngine/` contains more than ~5 lines of inline `#ifdef OLO_PLATFORM_WINDOWS` code (exceptions listed in C6).
- [x] `<Windows.h>` is not included from any header under `OloEngine/src/OloEngine/` outside of the C6-documented acceptable-inline list.
- [x] Debug build of `OloEngine` passes.
- [ ] Debug + Release builds of `OloEditor`, `OloEngine-Tests` pass.
- [ ] `OloEngine-Tests` all green.
- [ ] `pre-commit run --all-files` clean.
- [ ] (Optional, if CI/runner available) Linux build unchanged or improved.
