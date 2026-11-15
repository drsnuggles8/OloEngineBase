// =============================================================================
// RendererBackendCheck.h
//
// Cross-test guard for the process's RENDERER BACKEND (issue #1693).
//
// The rule: a test ends with the backend its process started with. Three
// process-global values make up "the backend", and a test can leave each one
// changed:
//
//   1. `RendererAPI::GetAPI()`, the selector every resource factory and
//      `GraphicsContext::CreateWindowless` switches on.
//   2. The dynamic type of the RenderCommand facade (`RenderCommand::
//      GetRendererAPI()`): a test that swaps in a Vulkan facade and does not
//      swap back leaves every later test dispatching through the wrong one.
//   3. In the `--olo-rhi=vulkan` process only, the facade's IDENTITY. The
//      windowless VulkanContext records through the facade object that
//      `TestRenderBackend::SelectFromOptions` created. Recreating it, even as
//      another Vulkan facade, destroys the object that context is using.
//
// The binary runs as two processes, so "the value it started with" is not a
// constant: OpenGL in the default process, Vulkan under `--olo-rhi=vulkan`.
// Nine device-test scopes restored the constant OpenGL. In the Vulkan process
// the first one to run left it on OpenGL, `CreateWindowless` refused every
// later scene test, the cached refusal skipped all of them, and the run still
// printed ~9,980 passed.
//
// What the listener does after every test:
//
//   * FAILS the test that left any of the three changed, skipped or not, at
//     the test's own source location. Not behind a strict flag (compare
//     RendererStateCheck): this leak does not degrade one later measurement,
//     it switches off a whole process's worth of tests.
//   * Restores what it can, so one leak fails one test rather than the rest
//     of the run: the selector always; in the OpenGL process also the facade,
//     recreated and re-initialized as ScopedVulkanRenderCommandSelection
//     does. A replaced facade in the Vulkan process cannot be restored (the
//     context's object is gone), and the failure says so.
//
// The same check runs at the end of each test suite, for a leak from
// `SetUpTestSuite` / `TearDownTestSuite`.
// =============================================================================
#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <string>
#include <typeinfo>

namespace OloEngine::Tests::RendererBackendCheck
{
    struct Snapshot
    {
        RendererAPI::API Api = RendererAPI::API::None;
        // Null when no facade exists (never, after static init).
        const RendererAPI* Facade = nullptr;
        const std::type_info* FacadeType = nullptr;
    };

    /// The backend as it is now.
    [[nodiscard]] Snapshot Capture();

    /// Appends a description of every way `now` differs from `expected` to
    /// `outDetail` and returns how many differed. The facade's identity counts
    /// only when `compareFacadeIdentity` (the Vulkan process).
    u32 Describe(const Snapshot& expected, const Snapshot& now, bool compareFacadeIdentity, std::string& outDetail);

    /// Records the process's backend and installs the listener. Call once from
    /// main(), after `TestRenderBackend::SelectFromOptions` and after every
    /// other listener, so this one's OnTestEnd runs first and the selector is
    /// back before another listener's teardown runs under it. Idempotent.
    void RegisterListener();

    /// The backend RegisterListener recorded.
    [[nodiscard]] const Snapshot& ProcessBackend();

    /// Tests (and suites) that left the backend changed so far this run.
    [[nodiscard]] u32 LeakCount();
} // namespace OloEngine::Tests::RendererBackendCheck
