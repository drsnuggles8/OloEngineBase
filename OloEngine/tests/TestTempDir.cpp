#include "OloEnginePCH.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace OloEngine::Tests
{
    namespace
    {
        // Long enough to stay readable in a post-mortem, short enough that
        // `<temp>/OloEngineTests-<pid>_<rand>/<leaf>/<file>` clears Windows'
        // 260-character MAX_PATH with room for the file name on top.
        constexpr std::size_t kMaxLeafLength = 64;

        [[nodiscard]] unsigned long CurrentPid()
        {
#if defined(_WIN32)
            return static_cast<unsigned long>(::_getpid());
#else
            return static_cast<unsigned long>(::getpid());
#endif
        }

        /// Map anything a gtest suite/case name can contain onto a filesystem-safe
        /// character. Type- and value-parameterised names carry `/`, `<`, `>` and
        /// `,` (e.g. `Suite/0.Case`, `Suite/MyType.Case`), which would otherwise
        /// silently create nested directories or fail outright on Windows.
        [[nodiscard]] std::string SanitizeLeaf(std::string_view raw)
        {
            std::string out;
            out.reserve(raw.size());
            for (const char c : raw)
            {
                const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                out.push_back(safe ? c : '_');
            }
            if (out.size() > kMaxLeafLength)
            {
                // Plain truncation would let two long, similarly-prefixed case
                // names collapse onto the SAME directory — reintroducing exactly
                // the sharing this helper exists to prevent. Keep a hash of the
                // full name so the leaf stays unique.
                const auto digest = std::hash<std::string_view>{}(raw);

                char tail[20]{};
                (void)std::snprintf(tail, sizeof(tail), "_%016llx",
                                    static_cast<unsigned long long>(digest));

                out.resize(kMaxLeafLength - (sizeof(tail) - 1));
                out += tail;
            }
            return out;
        }

        /// Try to claim a directory nobody else owns, directly under `base`.
        /// Returns an empty path if every attempt failed.
        ///
        /// PID alone is NOT sufficient: PIDs are recycled, so a directory left by
        /// a crashed earlier run can carry the PID this process just got, and
        /// adopting it means inheriting its files. `create_directory` returns
        /// false when the path already exists, which makes it the claim — we only
        /// ever proceed with a directory we ourselves created.
        ///
        /// Note there is deliberately NO shared `OloEngineTests/` parent to group
        /// these under. A fixed-name directory in a world-writable, sticky `/tmp`
        /// is exactly the hazard `AssetSceneLoadTest` documented before this
        /// helper existed: this repo's self-hosted CI runs as its own account
        /// alongside other users, so a parent created by user A is one nobody else
        /// can create inside — and the whole suite would lose its scratch space.
        /// The `OloEngineTests-` prefix keeps a manual sweep just as easy.
        [[nodiscard]] std::filesystem::path TryClaimUnder(const std::filesystem::path& base)
        {
            namespace fs = std::filesystem;

            std::random_device rd;
            std::error_code ec;
            for (int attempt = 0; attempt < 64; ++attempt)
            {
                const auto tag = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();

                char leaf[48]{};
                (void)std::snprintf(leaf, sizeof(leaf), "OloEngineTests-%lu_%016llx", CurrentPid(),
                                    static_cast<unsigned long long>(tag));

                ec.clear();
                if (const fs::path candidate = base / leaf; fs::create_directory(candidate, ec) && !ec)
                {
                    return candidate;
                }
            }
            return {};
        }

        [[nodiscard]] std::filesystem::path ClaimProcessRoot()
        {
            namespace fs = std::filesystem;

            std::error_code ec;
            const fs::path temp = fs::temp_directory_path(ec);
            if (!ec && !temp.empty())
            {
                if (fs::path root = TryClaimUnder(temp); !root.empty())
                {
                    return root;
                }
            }

            // The system temp dir is unusable (no permission, full, or every
            // candidate name lost its race 64 times running). Next best is beside
            // the test binary — never a guessed absolute path.
            const fs::path cwd = fs::current_path(ec);
            if (!ec && !cwd.empty())
            {
                if (fs::path root = TryClaimUnder(cwd); !root.empty())
                {
                    return root;
                }
            }

            // Nowhere is writable. Say so on stderr: every test that touches disk
            // is about to fail, and without this line each one reports only its own
            // unopenable file — precisely the unattributable failure this helper
            // exists to abolish. Throwing here would abort inside a static
            // initializer, which reports far worse.
            (void)std::fprintf(stderr,
                               "[TestTempDir] FATAL: could not claim a scratch directory under the "
                               "system temp dir or the working directory. Every test that writes a "
                               "file will now fail.\n");
            return fs::path{ "OloEngineTests-unclaimable" };
        }

        [[nodiscard]] bool KeepTempOnExit()
        {
#if defined(_WIN32)
            char* value = nullptr;
            std::size_t size = 0;
            if (::_dupenv_s(&value, &size, "OLO_TEST_KEEP_TEMP") != 0 || value == nullptr)
            {
                return false;
            }
            const bool keep = (value[0] != '\0' && value[0] != '0');
            std::free(value);
            return keep;
#else
            const char* value = std::getenv("OLO_TEST_KEEP_TEMP");
            return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
        }

        // Leaves already emptied during the CURRENT test. Cleared at each test's
        // start by the listener below.
        std::mutex s_PreparedMutex;
        std::set<std::string, std::less<>> s_Prepared;

        class CleanSlateListener final : public ::testing::EmptyTestEventListener
        {
            void OnTestStart(const ::testing::TestInfo&) override
            {
                const std::lock_guard lock(s_PreparedMutex);
                s_Prepared.clear();
            }
        };
    } // namespace

    void RegisterCleanSlateListener()
    {
        // gtest takes ownership.
        ::testing::UnitTest::GetInstance()->listeners().Append(new CleanSlateListener);
    }

    const std::filesystem::path& TempRoot()
    {
        // Function-local statics: thread-safe initialization, and the cleanup is
        // registered in a SECOND static so it runs only once s_Root is fully
        // constructed. atexit handlers run in reverse registration order against
        // static destructors, so a handler registered after s_Root's construction
        // is guaranteed to run before s_Root's destruction.
        static const std::filesystem::path s_Root = ClaimProcessRoot();
        static const bool s_CleanupRegistered = []
        {
            if (!KeepTempOnExit())
            {
                (void)std::atexit([]
                                  {
                    std::error_code ec;
                    std::filesystem::remove_all(TempRoot(), ec); });
            }
            return true;
        }();
        (void)s_CleanupRegistered;
        return s_Root;
    }

    std::filesystem::path TempDir(std::string_view label)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();

        // Outside a running test (a fixture's static SetUpTestSuite, a helper
        // called from a global environment) there is no case to key by. The
        // process root is still exclusive, so this is safe — just coarser.
        std::string leaf = info ? (std::string(info->test_suite_name()) + "." + info->name())
                                : std::string("_no_active_test");
        if (!label.empty())
        {
            leaf += ".";
            leaf += label;
        }
        leaf = SanitizeLeaf(leaf);

        const std::filesystem::path dir = TempRoot() / leaf;
        std::error_code ec;

        // First request for this leaf within the current test: empty it. Under
        // `--gtest_repeat` the same case runs several times in ONE process and
        // resolves to the same leaf, so without this a fixture would inherit its
        // own leftovers — the clean slate the `SetUp` `remove_all` blocks these
        // calls replaced used to provide. Later calls in the same test must NOT
        // wipe: helpers like MakeUniqueScratchDir() call TempDir() repeatedly and
        // expect earlier content to survive.
        //
        // Never wipe when there is NO active test: every such caller shares the
        // one `_no_active_test` leaf, so wiping it would let one suite's
        // SetUpTestSuite delete another's staged data — the exact cross-scope
        // clobber this helper exists to prevent, reintroduced one level up.
        if (info != nullptr)
        {
            // Insert unconditionally, even when the wipe below fails: retrying on a
            // later call would wipe MID-test, destroying content the test had
            // already written. A failed wipe is reported, not retried.
            bool first = false;
            {
                const std::lock_guard lock(s_PreparedMutex);
                first = s_Prepared.insert(leaf).second;
            }
            if (first)
            {
                std::filesystem::remove_all(dir, ec);
                if (ec)
                {
                    // The caller is about to run against a directory that still
                    // holds the previous iteration's files. Say so here, where the
                    // cause is known — otherwise it surfaces later as an
                    // inexplicable assertion about stale content.
                    ADD_FAILURE() << "TestTempDir: could not empty " << dir.string() << ": "
                                  << ec.message();
                    ec.clear();
                }
            }
        }

        // A silent failure here is the exact shape this helper exists to abolish:
        // the test would go on to open a file under a directory that does not
        // exist and report only `!is_open()`. Several migrated fixtures also
        // dropped a throwing `create_directories` in favour of this call.
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            const std::string message =
                "TestTempDir: could not create " + dir.string() + ": " + ec.message();
            if (info != nullptr)
            {
                ADD_FAILURE() << message;
            }
            else
            {
                (void)std::fprintf(stderr, "[TestTempDir] %s\n", message.c_str());
            }
        }
        return dir;
    }

    std::filesystem::path TempFile(std::string_view name)
    {
        return TempDir() / std::filesystem::path(name);
    }
} // namespace OloEngine::Tests
