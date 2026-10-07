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

#include <array>
#include <cctype>
#include <atomic>
#include <filesystem>
#include <thread>
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
            // Untracking is either immediate (OLO_TRACK_DEALLOC) or deferred through a retire
            // ticket (OLO_TRACK_RETIRE, #1342). A retire with no OLO_TRACK_RELEASE_RETIRED is the
            // same corpse one step later: the bytes sit in "retiring" forever.
            const bool deallocates = source.find("OLO_TRACK_DEALLOC") != std::string::npos;
            const bool retires = source.find("OLO_TRACK_RETIRE(") != std::string::npos;
            const bool releases = source.find("OLO_TRACK_RELEASE_RETIRED") != std::string::npos;
            if ((!deallocates && !retires) || (retires && !releases))
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
               "`= default`) — or, when a deferred-deletion lambda deletes the GL object,\n"
               "`OLO_TRACK_RETIRE(this)` there and `OLO_TRACK_RELEASE_RETIRED(ticket)` inside the lambda.\n"
               "That is exactly how EnvironmentMap hid.";
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
    // A deallocation must never be dropped because another thread holds the tracker's mutex.
    // TrackDeallocation used TryLock() and, on contention, logged a warning and RETURNED:
    // the bytes stayed booked forever, a leak the tracker itself invented. Any reader — the
    // Statistics panel, olo_memory_report, the benchmark sampler — was enough to trigger it.
    // A reader thread hammers the mutex here while this thread cycles allocations; with the
    // old code the totals drift up by every dropped deallocation.
    TEST(RendererMemoryTracker, ConcurrentReadersDoNotCostADeallocation)
    {
        Tracker& tracker = Tracker::GetInstance();
        constexpr ResourceType kType = ResourceType::Other;
        const sizet usageBefore = tracker.GetMemoryUsage(kType);
        const u32 countBefore = tracker.GetAllocationCount(kType);

        std::atomic<bool> stop{ false };
        std::thread reader([&tracker, &stop]
                           {
            while (!stop.load(std::memory_order_relaxed))
                static_cast<void>(tracker.GetTotalMemoryUsage()); });

        std::array<int, 16> objects{};
        for (u32 cycle = 0; cycle < 2000; ++cycle)
        {
            for (int& object : objects)
                tracker.TrackAllocation(&object, 256, kType, "contended", true, __FILE__, __LINE__);
            for (int& object : objects)
                tracker.TrackDeallocation(&object, __FILE__, __LINE__);
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        EXPECT_EQ(tracker.GetMemoryUsage(kType), usageBefore) << "deallocations were dropped under contention";
        EXPECT_EQ(tracker.GetAllocationCount(kType), countBefore);
    }

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

    TEST(RendererMemoryTracker, ExactBackingReattributionPreservesTotalsAndRetiresOnlyTheSelectedResource)
    {
        Tracker& tracker = Tracker::GetInstance();
        constexpr ResourceType kType = ResourceType::Other;
        constexpr u64 backingKey = 0x1257A501u;
        constexpr u64 unrelatedKey = 0x1257A502u;
        constexpr u64 aliasKey = 0x1257A503u;
        constexpr std::string_view sourceOwner = "Reattribute test source";
        constexpr std::string_view detailOwner = "Reattribute test detail";
        int selected = 0;
        int unrelated = 0;
        struct Cleanup
        {
            Tracker& Memory;
            void* Selected;
            void* Unrelated;
            u64 AliasKey;
            u64 RetirementTicket = 0;
            ~Cleanup()
            {
                Memory.UntrackAliasOfHandle(AliasKey);
                Memory.TrackDeallocation(Selected, __FILE__, __LINE__);
                Memory.TrackDeallocation(Unrelated, __FILE__, __LINE__);
                Memory.ReleaseRetired(RetirementTicket);
            }
        } cleanup{ tracker, &selected, &unrelated, aliasKey };
        const auto gpuBefore = tracker.GetGpuResidentBytes();
        const auto cpuBefore = tracker.GetCpuResidentBytes();
        const auto countBefore = tracker.GetAllocationCount(kType);
        {
            RendererMemoryOwnerScope sourceScope(sourceOwner, MemoryLifetime::Asset);
            tracker.TrackAllocation(&selected, 256, kType, "selected backing", true, __FILE__, __LINE__);
            tracker.TrackAllocation(&unrelated, 512, kType, "unrelated backing", true, __FILE__, __LINE__);
            tracker.BindResourceHandle(&selected, backingKey);
            tracker.BindResourceHandle(&unrelated, unrelatedKey);
            tracker.TrackAliasOfHandle(aliasKey, backingKey, kType, "selected view", __FILE__, __LINE__);
        }
        EXPECT_FALSE(tracker.ReattributeBackingResource(0, detailOwner, MemoryLifetime::PassOwned));
        EXPECT_FALSE(tracker.ReattributeBackingResource(aliasKey, detailOwner, MemoryLifetime::PassOwned));
        EXPECT_FALSE(tracker.ReattributeBackingResource(aliasKey + 1, detailOwner, MemoryLifetime::PassOwned));
        ASSERT_TRUE(tracker.ReattributeBackingResource(backingKey, detailOwner, MemoryLifetime::PassOwned));
        EXPECT_EQ(tracker.GetGpuResidentBytes(), gpuBefore + 768);
        EXPECT_EQ(tracker.GetCpuResidentBytes(), cpuBefore);
        EXPECT_EQ(tracker.GetAllocationCount(kType), countBefore + 2);
        const auto detail = tracker.GetLargestAllocations(detailOwner, 8);
        ASSERT_TRUE(detail);
        ASSERT_EQ(detail->Num(), 1);
        EXPECT_EQ((*detail)[0].m_Address, &selected);
        EXPECT_EQ((*detail)[0].m_Size, 256u);
        EXPECT_EQ((*detail)[0].m_Lifetime, MemoryLifetime::PassOwned);
        EXPECT_EQ((*detail)[0].m_SizeSource, MemorySizeSource::FormatEstimate);
        const auto source = tracker.GetLargestAllocations(sourceOwner, 8);
        ASSERT_TRUE(source);
        ASSERT_EQ(source->Num(), 2); // unrelated backing and the untouched alias
        EXPECT_EQ((*source)[0].m_Address, &unrelated);
        EXPECT_EQ((*source)[0].m_Lifetime, MemoryLifetime::Asset);
        EXPECT_TRUE((*source)[1].IsAlias());
        EXPECT_EQ((*source)[1].m_Lifetime, MemoryLifetime::Asset);
        cleanup.RetirementTicket = tracker.RetireAllocation(&selected);
        ASSERT_NE(cleanup.RetirementTicket, 0u);
        cleanup.Selected = nullptr;
        EXPECT_EQ(tracker.GetOwnerGpuRetiringBytes(detailOwner), 256u);
        EXPECT_EQ(tracker.GetOwnerGpuRetiringBytes(sourceOwner), 0u);
        EXPECT_EQ(tracker.GetGpuResidentBytes(), gpuBefore + 768);
        EXPECT_FALSE(tracker.ReattributeBackingResource(backingKey, sourceOwner, MemoryLifetime::Asset));
        tracker.ReleaseRetired(cleanup.RetirementTicket);
        cleanup.RetirementTicket = 0;
        EXPECT_EQ(tracker.GetOwnerGpuRetiringBytes(detailOwner), 0u);
        EXPECT_EQ(tracker.GetGpuResidentBytes(), gpuBefore + 512);
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
    // Every VMA allocation goes through VulkanTrackedAllocation (#1342). A raw vmaCreate*/
    // vmaDestroy* anywhere else is an allocation the memory report cannot see: on Vulkan the
    // tracker's committed bytes then disagree with vmaGetHeapBudgets, and the report can only
    // say "untracked" without saying where. This scan says where.
    TEST(RendererMemoryTracker, VulkanAllocationsGoThroughTheTrackedWrappers)
    {
        const std::filesystem::path root = RepoRoot();
        const std::filesystem::path engineSrc = root / "OloEngine" / "src";
        ASSERT_TRUE(std::filesystem::exists(engineSrc)) << "could not locate OloEngine/src from cwd " << s_StartCwd.string();

        // No whitespace before the parenthesis: a call, not a log string naming the function.
        static constexpr std::array<std::string_view, 5> kRawCalls = {
            "vmaCreateBuffer(",
            "vmaCreateBufferWithAlignment(",
            "vmaCreateImage(",
            "vmaDestroyBuffer(",
            "vmaDestroyImage(",
        };
        std::vector<std::string> offenders;
        u32 wrappedCalls = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(engineSrc, ec))
        {
            if (!entry.is_regular_file() || (entry.path().extension() != ".cpp" && entry.path().extension() != ".h"))
                continue;
            const std::string name = entry.path().filename().string();
            // The wrapper itself, and the one VMA_IMPLEMENTATION TU.
            if (name == "VulkanTrackedAllocation.cpp" || name == "VulkanMemoryAllocator.cpp")
                continue;
            const std::string source = StripComments(ReadFile(entry.path()));
            for (sizet at = source.find("TrackedVma"); at != std::string::npos; at = source.find("TrackedVma", at + 1))
                ++wrappedCalls;
            for (const std::string_view call : kRawCalls)
            {
                for (sizet at = source.find(call); at != std::string::npos; at = source.find(call, at + 1))
                {
                    const bool partOfLongerName = at > 0 && (std::isalnum(static_cast<unsigned char>(source[at - 1])) || source[at - 1] == '_');
                    if (!partOfLongerName)
                        offenders.push_back(std::filesystem::relative(entry.path(), root).generic_string() + ": " + std::string(call));
                }
            }
        }

        ASSERT_GT(wrappedCalls, 40u) << "found only " << wrappedCalls << " tracked VMA calls — the wrapper names drifted and this "
                                                                         "scan is no longer checking anything";
        std::string names;
        for (const std::string& n : offenders)
            names += "\n    " + n;
        EXPECT_TRUE(offenders.empty())
            << "raw VMA allocation calls outside VulkanTrackedAllocation:" << names
            << "\n\nUse the TrackedVma* wrapper with the same arguments, or the memory report cannot see the allocation.";
    }
} // namespace OloEngine::Tests
