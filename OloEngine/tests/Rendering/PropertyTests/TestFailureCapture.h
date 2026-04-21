// =============================================================================
// TestFailureCapture.h
//
// Auto-capture on test failure. When a GoogleTest assertion fails inside a
// test body that previously touched the GPU, we dump — into
// `OloEditor/assets/tests/captures/<suite>__<name>/`:
//
//   - `gl_state.txt`       — full GLStateSnapshot (bound FBO / program / VAO,
//                            viewport, blend / depth / stencil / cull, all
//                            texture + UBO bindings). Reuses the engine's
//                            production snapshot type so the dump matches
//                            what GLStateGuard inspects at pass boundaries.
//   - `framebuffer.png`    — pixel-perfect readback of the current draw
//                            framebuffer at its current viewport, clamped to
//                            8-bit sRGB (HDR attachments are tone-mapped
//                            trivially via clamp + gamma so the PNG is always
//                            useful as a quick "what did the GPU actually
//                            produce?" artefact).
//   - `command_bucket.txt` — compact human-readable summary of the most
//                            recent captured frame from FrameCaptureManager
//                            (pre/post-sort command counts, timings). Only
//                            written if the test opted into recording.
//   - `metadata.txt`       — test id, timestamp, GL vendor / renderer /
//                            driver version, recoverable assertion details.
//
// The dump is driven by a GoogleTest event listener installed from
// OloEngineTest::main(); tests pay zero cost when passing. The directory
// is cleared per test at OnTestStart, so stale captures never pile up.
// =============================================================================
#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    namespace TestFailureCapture
    {
        // Root directory for captures. Relative to CWD (OloEditor/).
        std::filesystem::path CaptureRoot();

        // Per-test directory under CaptureRoot().
        std::filesystem::path DirectoryFor(std::string_view suiteName, std::string_view testName);

        // Lower-level primitives. Public so tests can invoke them directly
        // (covered by TestFailureCaptureTest.cpp).

        // Captures `GLStateSnapshot::Capture()` and writes a field-by-field
        // text dump. Requires a live GL context. Returns true on success.
        bool WriteGLStateSnapshot(const std::filesystem::path& path);

        // Reads the color content of the currently bound draw framebuffer at
        // the current GL viewport, clamps + gamma-encodes any HDR values, and
        // writes a PNG. Requires a live GL context. Returns true on success.
        // Gracefully returns false (no crash, no assert) if viewport is
        // degenerate or readback fails.
        bool WriteCurrentDrawFboPng(const std::filesystem::path& path);

        // Writes a one-page summary of the most recent captured frame from
        // FrameCaptureManager::GetInstance(). Returns false (no-op) if the
        // manager has no frames recorded. Does not require a GL context.
        bool WriteLatestFrameSummary(const std::filesystem::path& path);

        // Writes metadata (test id, timestamp, GL vendor/renderer/version).
        // GL context is optional — if unavailable, the GL strings are
        // recorded as "unavailable".
        bool WriteMetadata(const std::filesystem::path& path,
                           std::string_view suiteName,
                           std::string_view testName,
                           std::string_view assertionMessage);

        // One-shot: captures everything useful for a failed test into
        // DirectoryFor(suite, name). Idempotent / safe to call even if a
        // subset of the captures fail. Returns the directory path.
        std::filesystem::path CaptureAll(std::string_view suiteName,
                                         std::string_view testName,
                                         std::string_view assertionMessage);

        // Installs a gtest event listener that invokes CaptureAll() on any
        // test failure. Call once from main() after InitGoogleTest.
        void RegisterFailureListener();
    } // namespace TestFailureCapture
} // namespace OloEngine::Tests
