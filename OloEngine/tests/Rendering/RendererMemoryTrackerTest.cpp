// OLO_TEST_LAYER: plumbing
//
// The "Double allocation detected at address ..." warning, and the bug class behind it.
//
// RendererMemoryTracker keys its allocation map on `this` — the CPU HEAP ADDRESS of the
// wrapper object (OpenGLVertexBuffer, OpenGLTexture2D, EnvironmentMap, ...), not on the GL
// object id. Heap addresses are recycled by the allocator, aggressively and immediately: free
// an object and the next allocation of the same size class very often lands on the exact same
// block. So a type whose constructor calls OLO_TRACK_*_ALLOC(this, ...) but whose destructor
// forgets OLO_TRACK_DEALLOC(this) does not merely "leak an entry" — it plants a corpse at an
// address that a LIVE object will shortly occupy, and the tracker then reports a double
// allocation that never happened.
//
// That is exactly how EnvironmentMap hid: it tracked in its constructor
// (EnvironmentMap.cpp) and its destructor was `~EnvironmentMap() = default;`. Every sky/IBL
// rebuild — scene load, sky-config change, procedural-sky or reflection-probe re-bake, the
// editor's `m_EnvironmentMap = nullptr; // Force reload` — left another corpse, and the
// replacement EnvironmentMap (identical size, so: same block) tripped the warning.
//
// Two tests, deliberately different in kind:
//
//   1. AllocTrackingIsPairedWithDeallocTracking — a SOURCE scan. This is the one that catches
//      the NEXT EnvironmentMap. The invariant is per-file (a class's constructor and
//      destructor live in the same translation unit), so any file that tracks must also
//      untrack. A runtime test cannot express this: it would have to construct and destroy
//      every tracked type, several of which need a live GL context and a loaded asset.
//
//   2. The accounting tests — RUNTIME, against the real singleton, using fake addresses. They
//      pin the two secondary defects that shipped alongside the missing destructor: the
//      double-alloc path used to CLOBBER the stale entry without subtracting its bytes (so the
//      per-type totals in the Statistics panel drifted upward forever), and a correct
//      alloc/dealloc/realloc cycle at a reused address must not drift at all.
//
// The m_IsShutdown latch (Shutdown() set it; nothing cleared it, so after one shutdown
// TrackDeallocation early-returned forever while TrackAllocation kept recording, turning every
// type into a phantom leaker across an Init/Shutdown/Init cycle) is checked at SOURCE level
// too. It cannot be checked at runtime here: doing so would mean calling Shutdown() on the
// process-wide singleton mid-suite, which clears the allocation map out from under every live
// GPU resource other tests have created.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using Tracker = RendererMemoryTracker;
        using ResourceType = RendererMemoryTracker::ResourceType;

        // Captured during static init, before any test body runs — some other test in this
        // binary chdir()s, so a relative path resolved inside a test body can miss the file.
        // (RendererShutdownTest learned this the hard way: it passed under --gtest_filter and
        // failed in a full run, purely on cwd.)
        const std::filesystem::path s_StartCwd = std::filesystem::current_path();

        [[nodiscard]] std::filesystem::path RepoRoot()
        {
            std::error_code ec;
            for (std::filesystem::path dir = s_StartCwd; !dir.empty(); dir = dir.parent_path())
            {
                if (std::filesystem::exists(dir / "OloEngine" / "src" / "OloEngine", ec))
                {
                    return dir;
                }
                if (!dir.has_relative_path())
                {
                    break;
                }
            }
            return s_StartCwd;
        }

        [[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Strip // line comments and /* block */ comments. Without this, a call that has merely
        // been COMMENTED OUT still satisfies the pairing search below — which would make the
        // test pass against the very bug it exists to catch. (RendererShutdownTest hit exactly
        // this: its first version matched the fix even when the fix was commented out.) It also
        // matters here specifically, because the fix's own explanatory comments NAME the macros.
        [[nodiscard]] std::string StripComments(const std::string& source)
        {
            std::string out;
            out.reserve(source.size());

            bool inLine = false;
            bool inBlock = false;
            for (sizet i = 0; i < source.size(); ++i)
            {
                const char c = source[i];
                const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

                if (inLine)
                {
                    if (c == '\n')
                    {
                        inLine = false;
                        out += c;
                    }
                    continue;
                }
                if (inBlock)
                {
                    if (c == '*' && next == '/')
                    {
                        inBlock = false;
                        ++i;
                    }
                    continue;
                }
                if (c == '/' && next == '/')
                {
                    inLine = true;
                    ++i;
                    continue;
                }
                if (c == '/' && next == '*')
                {
                    inBlock = true;
                    ++i;
                    continue;
                }
                out += c;
            }
            return out;
        }
    } // namespace

    // THE BUG-CLASS GUARD. Any file that tracks an allocation must also track a deallocation.
    // Add OLO_TRACK_GPU_ALLOC(this, ...) to a new resource type's constructor and forget the
    // matching OLO_TRACK_DEALLOC(this) in its destructor, and this fails by name.
    TEST(RendererMemoryTracker, AllocTrackingIsPairedWithDeallocTracking)
    {
        const std::filesystem::path root = RepoRoot();
        const std::filesystem::path engineSrc = root / "OloEngine" / "src";
        ASSERT_TRUE(std::filesystem::exists(engineSrc))
            << "could not locate OloEngine/src from cwd " << s_StartCwd.string();

        std::vector<std::string> offenders;
        u32 scanned = 0;

        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(engineSrc, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::filesystem::path& path = entry.path();
            if (path.extension() != ".cpp" && path.extension() != ".h")
            {
                continue;
            }
            // The macros are DEFINED here; it is not a tracked type itself.
            if (path.filename() == "RendererMemoryTracker.h" || path.filename() == "RendererMemoryTracker.cpp")
            {
                continue;
            }

            const std::string source = StripComments(ReadFile(path));
            const bool tracksAlloc = source.find("OLO_TRACK_GPU_ALLOC") != std::string::npos ||
                                     source.find("OLO_TRACK_CPU_ALLOC") != std::string::npos;
            if (!tracksAlloc)
            {
                continue;
            }

            ++scanned;
            if (source.find("OLO_TRACK_DEALLOC") == std::string::npos)
            {
                offenders.push_back(std::filesystem::relative(path, root).generic_string());
            }
        }

        // If the scan finds nothing, the macro names have drifted and this test is vacuous.
        ASSERT_GT(scanned, 5u)
            << "found only " << scanned << " files calling OLO_TRACK_*_ALLOC — the macro names have "
                                           "drifted and this test is no longer checking anything";

        std::string names;
        for (const std::string& n : offenders)
        {
            names += "\n    " + n;
        }

        EXPECT_TRUE(offenders.empty())
            << "these files track an allocation but never track a deallocation:" << names
            << "\n\nRendererMemoryTracker is keyed on the CPU heap address (`this`), and the allocator\n"
               "recycles addresses immediately. A constructor that tracks without a destructor that\n"
               "untracks leaves a corpse entry at an address a LIVE object will soon occupy — which\n"
               "reports a 'Double allocation detected at address ...' that never happened, and inflates\n"
               "the per-type memory totals for the rest of the session.\n\n"
               "Add `OLO_TRACK_DEALLOC(this);` to the destructor (and make sure the destructor is not\n"
               "`= default`). That is exactly how EnvironmentMap hid.";
    }

    // A correct alloc → dealloc → alloc-at-the-same-address cycle must not drift the accounting.
    // This is the normal case for every recycled heap block, so drift here is drift everywhere.
    TEST(RendererMemoryTracker, ReusedAddressAfterDeallocDoesNotDriftAccounting)
    {
        Tracker& tracker = Tracker::GetInstance();
        constexpr ResourceType kType = ResourceType::Other;

        const sizet usageBefore = tracker.GetMemoryUsage(kType);
        const u32 countBefore = tracker.GetAllocationCount(kType);

        // A fake, stable address. The tracker never dereferences it.
        int stackObject = 0;
        void* const address = &stackObject;

        tracker.TrackAllocation(address, 1024, kType, "test-A", true, __FILE__, __LINE__);
        tracker.TrackDeallocation(address, __FILE__, __LINE__);

        // Same address handed back to a different-sized object — the recycled-block case.
        tracker.TrackAllocation(address, 4096, kType, "test-B", true, __FILE__, __LINE__);

        EXPECT_EQ(tracker.GetMemoryUsage(kType), usageBefore + 4096)
            << "only the live 4096-byte allocation should be counted";
        EXPECT_EQ(tracker.GetAllocationCount(kType), countBefore + 1);

        tracker.TrackDeallocation(address, __FILE__, __LINE__);

        EXPECT_EQ(tracker.GetMemoryUsage(kType), usageBefore) << "accounting must return to its starting point";
        EXPECT_EQ(tracker.GetAllocationCount(kType), countBefore);
    }

    // The double-allocation path itself: when a stale entry IS found (i.e. some type is missing
    // its OLO_TRACK_DEALLOC), the tracker must retire the corpse's bytes before overwriting it.
    // The old code clobbered the entry and added the new size on top, so the type total grew by
    // the old allocation's size on every single rebuild — this is why the Statistics panel's
    // memory numbers crept upward.
    TEST(RendererMemoryTracker, DoubleAllocationRetiresTheStaleEntryInsteadOfInflatingTotals)
    {
        Tracker& tracker = Tracker::GetInstance();
        constexpr ResourceType kType = ResourceType::Other;

        const sizet usageBefore = tracker.GetMemoryUsage(kType);
        const u32 countBefore = tracker.GetAllocationCount(kType);

        int stackObject = 0;
        void* const address = &stackObject;

        // Simulate the leaker: track, and never untrack.
        tracker.TrackAllocation(address, 1024, kType, "leaked-env-map", true, __FILE__, __LINE__);

        // The replacement lands on the recycled block. This is the double-allocation warning path.
        tracker.TrackAllocation(address, 1024, kType, "replacement", true, __FILE__, __LINE__);

        EXPECT_EQ(tracker.GetMemoryUsage(kType), usageBefore + 1024)
            << "the stale entry's bytes must be retired, not added to — one live object, one object's "
               "worth of memory. Inflation here is what made the memory totals drift upward.";
        EXPECT_EQ(tracker.GetAllocationCount(kType), countBefore + 1)
            << "the stale entry's count must be retired too";

        tracker.TrackDeallocation(address, __FILE__, __LINE__);
        EXPECT_EQ(tracker.GetMemoryUsage(kType), usageBefore);
        EXPECT_EQ(tracker.GetAllocationCount(kType), countBefore);
    }

    // Shutdown() sets m_IsShutdown; Initialize() and Reset() must each clear it. They used not to,
    // which left the tracker permanently half-dead after the first shutdown: TrackDeallocation
    // early-returns on the latch while TrackAllocation keeps recording, so every resource type
    // becomes a phantom leaker across an Init/Shutdown/Init cycle (the editor does exactly that on
    // project reload). Checked at source level because asserting it at runtime would require
    // calling Shutdown() on the process-wide singleton mid-suite, which would clear the allocation
    // map out from under every live GPU resource the other tests hold.
    TEST(RendererMemoryTracker, InitializeAndResetClearTheShutdownLatch)
    {
        const std::filesystem::path source =
            RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Debug" / "RendererMemoryTracker.cpp";
        const std::string text = StripComments(ReadFile(source));
        ASSERT_FALSE(text.empty()) << "could not read " << source.string();

        const sizet initAt = text.find("void RendererMemoryTracker::Initialize()");
        const sizet shutdownAt = text.find("void RendererMemoryTracker::Shutdown()");
        const sizet resetAt = text.find("void RendererMemoryTracker::Reset()");
        ASSERT_NE(initAt, std::string::npos) << "Initialize() not found — repoint this test";
        ASSERT_NE(shutdownAt, std::string::npos) << "Shutdown() not found — repoint this test";
        ASSERT_NE(resetAt, std::string::npos) << "Reset() not found — repoint this test";
        ASSERT_LT(initAt, shutdownAt) << "Initialize() is expected to precede Shutdown() in the file";
        ASSERT_LT(shutdownAt, resetAt) << "Shutdown() is expected to precede Reset() in the file";

        const std::string initBody = text.substr(initAt, shutdownAt - initAt);
        const std::string resetBody = text.substr(resetAt);

        EXPECT_NE(initBody.find("m_IsShutdown = false"), std::string::npos)
            << "RendererMemoryTracker::Initialize() does not clear m_IsShutdown.\n"
               "Shutdown() sets the latch and nothing else clears it, so after one Init/Shutdown/Init\n"
               "cycle TrackDeallocation early-returns forever while TrackAllocation keeps recording —\n"
               "every resource type silently becomes a phantom leaker.";

        EXPECT_NE(resetBody.find("m_IsShutdown = false"), std::string::npos)
            << "RendererMemoryTracker::Reset() does not clear m_IsShutdown — a Reset() that leaves the\n"
               "tracker refusing every deallocation is worse than useless.";
    }
} // namespace OloEngine::Tests
