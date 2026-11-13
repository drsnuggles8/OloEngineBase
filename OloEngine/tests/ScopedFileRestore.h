#pragma once

// ScopedFileRestore: put a file back the way a test found it.
//
// For a test that has to run against a real, tracked file that the code under
// test writes. The case it was made for: a scene test that loads the real
// Sandbox project with an EditorAssetManager, whose Shutdown writes the
// registry into the active project's AssetRegistry.oar. Construct it before the
// writer runs; Restore() (or the destructor) writes the original bytes back,
// or removes the file if there was none.
//
// Prefer a temporary copy of the project when the test can use one
// (TestTempDir.h); this is for content too large to copy per test.
//
// The restore replaces the file atomically, as the registry's own writer does:
// other test processes may read the same file at the same moment under a
// parallel ctest, and must see the old bytes or the whole new ones, never a
// truncated file.

#include "OloEngine/Core/FileSystem.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace OloEngine::Tests
{
    class ScopedFileRestore
    {
      public:
        explicit ScopedFileRestore(std::filesystem::path path) : m_Path(std::move(path))
        {
            if (std::ifstream in(m_Path, std::ios::binary); in)
            {
                std::ostringstream bytes;
                bytes << in.rdbuf();
                m_Bytes = bytes.str();
            }
        }

        ~ScopedFileRestore()
        {
            (void)Restore();
        }

        ScopedFileRestore(const ScopedFileRestore&) = delete;
        ScopedFileRestore& operator=(const ScopedFileRestore&) = delete;
        ScopedFileRestore(ScopedFileRestore&&) = delete;
        ScopedFileRestore& operator=(ScopedFileRestore&&) = delete;

        // Write the original bytes back (or remove a file that did not exist).
        // Once: later calls, and the destructor, do nothing. False on failure.
        [[nodiscard]] bool Restore()
        {
            if (m_Restored)
            {
                return true;
            }
            m_Restored = true;
            if (!m_Bytes.has_value())
            {
                std::error_code ec;
                std::filesystem::remove(m_Path, ec);
                return !ec;
            }
            return FileSystem::WriteFileAtomically(m_Path, std::string_view(*m_Bytes));
        }

        [[nodiscard]] const std::filesystem::path& GetPath() const
        {
            return m_Path;
        }

      private:
        std::filesystem::path m_Path;
        std::optional<std::string> m_Bytes; ///< nullopt: the file did not exist.
        bool m_Restored = false;
    };
} // namespace OloEngine::Tests
