#pragma once

#include <string>

namespace OloEngine::BuildInfo
{
    /// Static semantic version baked in at configure time (CMAKE_PROJECT_VERSION,
    /// via the OLO_ENGINE_VERSION compile definition).
    const char* GetEngineVersion();

    /// Short (10-char) git commit hash the binary was built from, or "unknown"
    /// outside a git checkout (a source tarball, or a stripped CI archive).
    const char* GetGitHash();

    /// `git describe --tags --always --dirty` at configure time — falls back to
    /// the abbreviated hash when the checkout has no tags, and is suffixed
    /// "-dirty" when the working tree had uncommitted changes. "unknown" outside
    /// a git checkout.
    const char* GetGitDescribe();

    /// UTC configure-time timestamp, ISO 8601 (e.g. "2026-08-31T12:00:00Z").
    const char* GetBuildTimestamp();

    /// Whether the working tree had uncommitted changes to tracked files at
    /// configure time (`git diff-index --quiet HEAD --`, the same check
    /// `git describe --dirty` uses internally). Computed independently of
    /// GetGitDescribe()'s text — a real tag can legitimately end in "-dirty"
    /// without the tree actually being dirty, so this must never be derived
    /// by parsing that string.
    bool IsWorkingTreeDirty();

    /// Human-readable build identity for logs, crash reports, the game manifest
    /// and any in-game display: "<version>+<git hash>", with a "-dirty" suffix
    /// when IsWorkingTreeDirty() is true — so a build made from a modified
    /// checkout never looks identical to a clean build of the same commit.
    /// Falls back to just the version when the git hash is unavailable.
    std::string GetBuildId();
} // namespace OloEngine::BuildInfo
