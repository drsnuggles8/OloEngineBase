#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPageStore.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef OLO_PLATFORM_WINDOWS
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace OloEngine
{
    namespace
    {
        // Two workers, not hardware_concurrency(): these threads block on file IO rather than
        // burn CPU, and the consumer applies at most m_MaxPageUploadsPerFrame page loads a
        // frame anyway. More would only deepen the queue the frame cannot drain.
        constexpr u32 kWorkerCount = 2;

        // The store's own directory, resolved once. A process-private file under the OS temp
        // directory rather than anywhere under `assets/`: the editor's asset watcher imports
        // what appears in the asset tree, the test binary runs from the repo root, and this
        // is scratch that must not outlive the process either way.
        std::filesystem::path& SpillDirectoryOverride()
        {
            static std::filesystem::path s_Override;
            return s_Override;
        }

        std::FILE* OpenFile(const std::filesystem::path& path, const char* mode)
        {
#ifdef OLO_PLATFORM_WINDOWS
            // _wfopen, not fopen: a project under a path with non-ASCII characters would
            // otherwise fail to open for reasons that read as "the spill is broken".
            std::wstring wideMode;
            wideMode.reserve(std::strlen(mode));
            for (const char* c = mode; *c != '\0'; ++c)
            {
                wideMode.push_back(static_cast<wchar_t>(*c));
            }
            return ::_wfopen(path.c_str(), wideMode.c_str());
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

        // Creates the spill file, failing if anything already exists at that path and
        // refusing to follow a symlink there (issue #1151 / CWE-377).
        //
        // The plain fopen("w+b") this replaces TRUNCATES whatever it opens and follows
        // symlinks, and the name is guessable (pid + serial) in a directory that is
        // world-writable on POSIX — so any local process could pre-place a symlink and have
        // the engine truncate a file of its choosing. Exclusive creation removes the race, and
        // 0600 keeps the geometry readable only by this user.
        std::FILE* CreateExclusive(const std::filesystem::path& path)
        {
#ifdef OLO_PLATFORM_WINDOWS
            HANDLE const handle =
                ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return nullptr;
            }
            int const fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_RDWR);
            if (fd == -1)
            {
                ::CloseHandle(handle);
                return nullptr;
            }
            std::FILE* stream = ::_fdopen(fd, "w+b");
            if (stream == nullptr)
            {
                ::_close(fd); // owns the HANDLE now
            }
            return stream;
#else
            int const fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (fd == -1)
            {
                return nullptr;
            }
            std::FILE* stream = ::fdopen(fd, "w+b");
            if (stream == nullptr)
            {
                ::close(fd);
            }
            return stream;
#endif
        }

        // Opens an EXISTING spill for reading, without following a symlink at that path.
        std::FILE* OpenForRead(const std::filesystem::path& path)
        {
#ifndef OLO_PLATFORM_WINDOWS
            int const fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
            if (fd == -1)
            {
                return nullptr;
            }
            std::FILE* stream = ::fdopen(fd, "rb");
            if (stream == nullptr)
            {
                ::close(fd);
            }
            return stream;
#else
            return OpenFile(path, "rb");
#endif
        }

        [[nodiscard]] bool SeekTo(std::FILE* stream, u64 offset)
        {
            // std::fseek takes a `long`, which is 32-bit on Windows — a spill past 2 GB is
            // exactly the case this store exists for, so never use it.
#ifdef OLO_PLATFORM_WINDOWS
            return ::_fseeki64(stream, static_cast<i64>(offset), SEEK_SET) == 0;
#else
            return ::fseeko(stream, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        [[nodiscard]] u32 CurrentProcessId()
        {
#ifdef OLO_PLATFORM_WINDOWS
            return static_cast<u32>(::_getpid());
#else
            return static_cast<u32>(::getpid());
#endif
        }

        [[nodiscard]] bool IsProcessAlive(u32 processId)
        {
            if (processId == 0)
            {
                return false;
            }
#ifdef OLO_PLATFORM_WINDOWS
            HANDLE const handle = ::OpenProcess(SYNCHRONIZE, FALSE, processId);
            if (handle == nullptr)
            {
                // No such process, or one we may not touch. Either way it is not ours to
                // second-guess: treat "cannot confirm alive" as alive so a sweep can never
                // delete a file somebody is still reading.
                return ::GetLastError() != ERROR_INVALID_PARAMETER;
            }
            bool const alive = ::WaitForSingleObject(handle, 0) != WAIT_OBJECT_0;
            ::CloseHandle(handle);
            return alive;
#else
            // kill(pid, 0) probes without signalling. EPERM means it exists but belongs to
            // someone else, which still counts as alive.
            return ::kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
#endif
        }

        // Deletes spill files left by processes that are gone (issue #1151).
        //
        // Close() removes this store's file on a graceful shutdown, but a process that is
        // KILLED never gets there — and this file is hundreds of megabytes. Measured while
        // developing #1151: nine editor sessions killed by other means left 4.1 GB of
        // `vgpages_*.ovgp` in %TEMP% that nothing would ever have collected.
        //
        // The owning pid is in the name, so the sweep is exact rather than time-based: a file
        // is removed only when its process is provably gone. A pid that has been recycled onto
        // a live process just means the file survives one more session.
        void SweepOrphanedSpills(const std::filesystem::path& directory, const std::filesystem::path& keep)
        {
            std::error_code ec;
            std::filesystem::directory_iterator it(directory, ec);
            if (ec)
            {
                return;
            }
            u32 removed = 0;
            u64 reclaimed = 0;
            for (const auto& entry : it)
            {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".ovgp" ||
                    entry.path() == keep)
                {
                    continue;
                }
                // vgpages_<pid>_<serial>.ovgp
                const std::string name = entry.path().stem().string();
                constexpr std::string_view kPrefix = "vgpages_";
                if (!name.starts_with(kPrefix))
                {
                    continue;
                }
                auto const pidStart = kPrefix.size();
                auto const pidEnd = name.find('_', pidStart);
                if (pidEnd == std::string::npos)
                {
                    continue;
                }
                u32 owner = 0;
                auto const digits = std::string_view(name).substr(pidStart, pidEnd - pidStart);
                if (std::from_chars(digits.data(), digits.data() + digits.size(), owner).ec != std::errc{} ||
                    IsProcessAlive(owner))
                {
                    continue;
                }
                auto const size = entry.file_size(ec);
                if (std::filesystem::remove(entry.path(), ec))
                {
                    ++removed;
                    reclaimed += ec ? 0u : size;
                }
            }
            if (removed > 0)
            {
                OLO_CORE_INFO("VirtualGeometryPageStore: reclaimed {} orphaned spill file(s) ({} MiB) left by "
                              "processes that no longer exist",
                              removed, reclaimed / (1024 * 1024));
            }
        }
    } // namespace

    void VirtualGeometryPageStore::SetSpillDirectory(const std::filesystem::path& directory)
    {
        SpillDirectoryOverride() = directory;
    }

    std::filesystem::path VirtualGeometryPageStore::GetSpillDirectory()
    {
        if (!SpillDirectoryOverride().empty())
        {
            return SpillDirectoryOverride();
        }
        std::error_code ec;
        auto const temp = std::filesystem::temp_directory_path(ec);
        if (ec)
        {
            // No temp directory is a genuinely broken environment, and silently spilling into
            // the working directory instead would put a multi-gigabyte scratch file somewhere
            // nobody expects. Say so and let EnsureOpen fail loudly.
            OLO_CORE_ERROR("VirtualGeometryPageStore: no temp directory available ({}) — on-disk page "
                           "streaming is unavailable",
                           ec.message());
            return {};
        }
        return temp / "OloEngine" / "VirtualGeometryPages";
    }

    VirtualGeometryPageStore::~VirtualGeometryPageStore()
    {
        Close();
    }

    bool VirtualGeometryPageStore::EnsureOpen()
    {
        if (m_File != nullptr)
        {
            return true;
        }
        if (m_OpenFailed)
        {
            return false;
        }

        auto const directory = GetSpillDirectory();
        if (directory.empty())
        {
            m_OpenFailed = true;
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec && !std::filesystem::exists(directory))
        {
            OLO_CORE_ERROR("VirtualGeometryPageStore: cannot create spill directory '{}': {} — on-disk page "
                           "streaming is unavailable",
                           directory.string(), ec.message());
            m_OpenFailed = true;
            return false;
        }
#ifndef OLO_PLATFORM_WINDOWS
        // Owner-only. The default umask would leave cooked geometry world-readable in a
        // shared /tmp, and it also narrows who can plant a file in the way of the exclusive
        // create below.
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
#endif

        // Collect dead sessions' spills BEFORE claiming a name, so a recycled pid does not
        // collide with its own predecessor's leftovers.
        SweepOrphanedSpills(directory, {});

        // Process id AND a per-store counter in the name. The pid alone is not enough: this
        // box runs several worktrees and several editor instances at once, and a store that
        // silently reopened a sibling's file would feed one process another's vertices.
        //
        // Creation is EXCLUSIVE, so the name is a claim rather than a guess: a collision (a
        // live sibling, a stale file, or something planted there) fails the create and costs a
        // retry rather than truncating whatever was in the way.
        static std::atomic<u32> s_StoreSerial{ 0 };
        constexpr u32 kNameAttempts = 64;
        for (u32 attempt = 0; attempt < kNameAttempts && m_File == nullptr; ++attempt)
        {
            auto const serial = s_StoreSerial.fetch_add(1, std::memory_order_relaxed);
            m_Path = directory / fmt::format("vgpages_{}_{}.ovgp", CurrentProcessId(), serial);
            m_File = CreateExclusive(m_Path);
        }
        if (m_File == nullptr)
        {
            OLO_CORE_ERROR("VirtualGeometryPageStore: could not create a spill file in '{}' after {} attempts "
                           "— on-disk page streaming is unavailable",
                           directory.string(), kNameAttempts);
            m_Path.clear();
            m_OpenFailed = true;
            return false;
        }

        // Open the IO workers' read handles now, while the file we just created is still the
        // one at that path. Deferring them to the first AddMesh would leave a window in which
        // the name could be swapped for something else between the create and the open.
        for (u32 i = 0; i < kWorkerCount; ++i)
        {
            std::FILE* stream = OpenForRead(m_Path);
            if (stream == nullptr)
            {
                OLO_CORE_WARN("VirtualGeometryPageStore: could not open read handle {} for '{}' — running "
                              "with {} IO worker(s)",
                              i, m_Path.string(), m_WorkerFiles.size());
                continue;
            }
            m_WorkerFiles.push_back(stream);
        }

        m_WriteCursor = 0;
        OLO_CORE_INFO("VirtualGeometryPageStore: spilling virtual-geometry pages to '{}'", m_Path.string());
        return true;
    }

    void VirtualGeometryPageStore::StartWorkers()
    {
        if (!m_Workers.empty() || m_File == nullptr)
        {
            return;
        }
        m_Stop.store(false, std::memory_order_relaxed);
        // One read handle per worker so two concurrent page reads never fight over one
        // stream's file position. The handles were opened by EnsureOpen, against the file it
        // had just created; this only spawns the threads that own them.
        for (std::FILE* stream : m_WorkerFiles)
        {
            m_Workers.emplace_back([this, stream]()
                                   { WorkerMain(stream); });
        }
        if (m_Workers.empty())
        {
            OLO_CORE_ERROR("VirtualGeometryPageStore: no IO worker could open '{}' — every asynchronous page "
                           "fault will fall back to a blocking read",
                           m_Path.string());
        }
    }

    u32 VirtualGeometryPageStore::AddMesh(const VirtualMeshGpuData& packed)
    {
        OLO_PROFILE_FUNCTION();

        if (packed.Pages.empty())
        {
            return kInvalidMesh;
        }
        if (!EnsureOpen())
        {
            return kInvalidMesh;
        }

        // uv2 is all-or-nothing per mesh, exactly as MeshHasLightmapUVs requires: a page whose
        // uv2 window did not exist would otherwise read a sibling's charts.
        bool const hasLightmapUVs =
            !packed.LightmapUVs.empty() && packed.LightmapUVs.size() == packed.Vertices.size();

        // Records are built into a LOCAL vector and appended to m_Directory under the lock at
        // the end. The workers index m_Directory, so growing it in place here would reallocate
        // under a reader; and a mesh that fails halfway must leave no directory entries behind.
        std::vector<PageRecord> records;
        records.reserve(packed.Pages.size());
        u32 base = 0;
        {
            std::lock_guard lock(m_Mutex);
            base = static_cast<u32>(m_Directory.size());
        }
        u64 writeCursor = m_WriteCursor;
        if (!SeekTo(m_File, writeCursor))
        {
            OLO_CORE_ERROR("VirtualGeometryPageStore: seek to {} failed while spilling a mesh — keeping its "
                           "payload in memory",
                           writeCursor);
            return kInvalidMesh;
        }

        for (const VirtualPageInfo& info : packed.Pages)
        {
            // Every page window must lie inside the packed arrays. A cooked blob is hostile
            // input (VirtualMeshRegistry deserializes one straight off disk), and writing a
            // page that runs past the end would read heap behind the vectors.
            auto const vertexEnd = static_cast<u64>(info.VertexOffset) + info.VertexCount;
            auto const indexEnd = static_cast<u64>(info.IndexOffset) + info.IndexCount;
            if (vertexEnd > packed.Vertices.size() || indexEnd > packed.Indices.size())
            {
                OLO_CORE_ERROR("VirtualGeometryPageStore: page {} of this mesh spans [{}, {}) vertices / "
                               "[{}, {}) indices, outside the packed arrays ({} / {}) — refusing to spill "
                               "the mesh",
                               info.GroupIndex, info.VertexOffset, vertexEnd, info.IndexOffset, indexEnd,
                               packed.Vertices.size(), packed.Indices.size());
                return kInvalidMesh;
            }

            PageRecord record;
            record.Offset = writeCursor;
            record.VertexCount = info.VertexCount;
            record.IndexCount = info.IndexCount;
            record.LightmapUVCount = hasLightmapUVs ? info.VertexCount : 0u;

            auto writeSpan = [this](const void* data, u64 bytes) -> bool
            {
                if (bytes == 0)
                {
                    return true;
                }
                return std::fwrite(data, 1, static_cast<sizet>(bytes), m_File) == static_cast<sizet>(bytes);
            };

            auto const vertexBytes = static_cast<u64>(info.VertexCount) * sizeof(VirtualGpuVertex);
            auto const uvBytes = static_cast<u64>(record.LightmapUVCount) * sizeof(glm::vec2);
            auto const indexBytes = static_cast<u64>(info.IndexCount) * sizeof(u32);
            bool ok = writeSpan(packed.Vertices.data() + info.VertexOffset, vertexBytes);
            // Never form `LightmapUVs.data() + offset` on an empty vector: data() is allowed
            // to be null and null + n is undefined even when nothing is read through it.
            ok = ok && (record.LightmapUVCount == 0 ||
                        writeSpan(packed.LightmapUVs.data() + info.VertexOffset, uvBytes));
            ok = ok && writeSpan(packed.Indices.data() + info.IndexOffset, indexBytes);
            if (!ok)
            {
                OLO_CORE_ERROR("VirtualGeometryPageStore: write failed spilling page {} to '{}' (disk full?) — "
                               "keeping the mesh's payload in memory",
                               info.GroupIndex, m_Path.string());
                return kInvalidMesh;
            }

            writeCursor += vertexBytes + uvBytes + indexBytes;
            records.push_back(record);
        }

        // Flush before any worker can be asked for these bytes: the workers read through their
        // own handles, so data still sitting in this stream's buffer would read as a short
        // read or as whatever the file held before.
        if (std::fflush(m_File) != 0)
        {
            OLO_CORE_ERROR("VirtualGeometryPageStore: flush of '{}' failed — keeping the mesh's payload in "
                           "memory",
                           m_Path.string());
            return kInvalidMesh;
        }

        {
            std::lock_guard lock(m_Mutex);
            m_Directory.insert(m_Directory.end(), records.begin(), records.end());
            m_Stats.PagesWritten += records.size();
            m_Stats.BytesWritten += writeCursor - m_WriteCursor;
        }
        m_WriteCursor = writeCursor;

        StartWorkers();
        return base;
    }

    bool VirtualGeometryPageStore::ReadRecord(std::FILE* stream, const PageRecord& record, VirtualPagePayload& out)
    {
        if (!SeekTo(stream, record.Offset))
        {
            return false;
        }

        // The three resizes allocate, and this runs on a worker thread. An exception escaping
        // a std::thread's entry point is std::terminate — the process exits with no log line,
        // no dump and a flat memory graph, which is indistinguishable from "the editor closed".
        // A read that cannot allocate is a failed read like any other: say so and let the
        // caller count it.
        try
        {
            out.Vertices.resize(record.VertexCount);
            out.LightmapUVs.resize(record.LightmapUVCount);
            out.Indices.resize(record.IndexCount);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VirtualGeometryPageStore: could not allocate a {} vertex / {} index page buffer: {}",
                           record.VertexCount, record.IndexCount, e.what());
            out = {};
            return false;
        }

        auto readSpan = [stream](void* data, u64 bytes) -> bool
        {
            if (bytes == 0)
            {
                return true;
            }
            return std::fread(data, 1, static_cast<sizet>(bytes), stream) == static_cast<sizet>(bytes);
        };

        bool ok = readSpan(out.Vertices.data(), static_cast<u64>(record.VertexCount) * sizeof(VirtualGpuVertex));
        ok = ok && readSpan(out.LightmapUVs.data(), static_cast<u64>(record.LightmapUVCount) * sizeof(glm::vec2));
        ok = ok && readSpan(out.Indices.data(), static_cast<u64>(record.IndexCount) * sizeof(u32));
        if (!ok)
        {
            // A short read leaves half a page in `out`. Clear it rather than hand back
            // plausible-looking geometry with a tail of zeroes — zeroed vertices are a
            // degenerate cluster at the origin, which draws, which is the silent-corruption
            // outcome this store must not have.
            out = {};
            return false;
        }
        return true;
    }

    void VirtualGeometryPageStore::WorkerMain(std::FILE* stream)
    {
        for (;;)
        {
            Request request;
            PageRecord record;
            {
                std::unique_lock lock(m_Mutex);
                m_WorkAvailable.wait(lock, [this]
                                     { return m_Stop.load(std::memory_order_relaxed) || !m_Queue.empty(); });
                if (m_Stop.load(std::memory_order_relaxed))
                {
                    return;
                }
                request = m_Queue.front();
                m_Queue.pop_front();
                record = m_Directory[request.Page];
            }

            Completion completion;
            completion.Page = request.Page;
            // Nothing may escape this loop. A worker that throws takes the whole process down
            // through std::terminate, silently — and a page that cannot be read is already a
            // first-class outcome here, reported as a failed read.
            try
            {
                completion.Ok = ReadRecord(stream, record, completion.Payload);
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("VirtualGeometryPageStore: IO worker threw reading page {}: {}", request.Page,
                               e.what());
                completion.Ok = false;
                completion.Payload = {};
            }
            catch (...)
            {
                OLO_CORE_ERROR("VirtualGeometryPageStore: IO worker threw a non-standard exception reading "
                               "page {}",
                               request.Page);
                completion.Ok = false;
                completion.Payload = {};
            }

            try
            {
                std::lock_guard lock(m_Mutex);
                m_Completions.push_back(std::move(completion));
            }
            catch (const std::exception& e)
            {
                // Dropping the completion leaks the page's slot in m_Requested, which the next
                // Fetch will see as "still in flight" forever. Say so rather than terminate:
                // one wedged page is a coarser cut, a terminate is the editor vanishing.
                OLO_CORE_ERROR("VirtualGeometryPageStore: IO worker could not publish page {}: {}",
                               request.Page, e.what());
            }
        }
    }

    void VirtualGeometryPageStore::NoteStagingBytesLocked(i64 delta)
    {
        if (delta >= 0)
        {
            m_Stats.StagingBytes += static_cast<u64>(delta);
        }
        else
        {
            auto const drop = static_cast<u64>(-delta);
            m_Stats.StagingBytes = (drop > m_Stats.StagingBytes) ? 0u : m_Stats.StagingBytes - drop;
        }
        m_Stats.PeakStagingBytes = std::max(m_Stats.PeakStagingBytes, m_Stats.StagingBytes);
    }

    void VirtualGeometryPageStore::DrainCompletionsLocked()
    {
        for (Completion& completion : m_Completions)
        {
            m_Requested.erase(completion.Page);
            if (completion.Ok)
            {
                ++m_Stats.ReadsCompleted;
                m_Stats.BytesRead += completion.Payload.ByteSize();
                StageLocked(completion.Page, std::move(completion.Payload));
            }
            else
            {
                ++m_Stats.ReadsFailed;
                m_Failed.insert(completion.Page);
                OLO_CORE_ERROR("VirtualGeometryPageStore: page {} could not be read back from '{}'. That "
                               "page's geometry is permanently unavailable this session and its clusters "
                               "will never become resident — the DAG cut stays at a coarser ancestor there.",
                               completion.Page, m_Path.string());
            }
        }
        m_Completions.clear();
        TrimStagedLocked();
        m_Stats.ReadsInFlight = static_cast<u32>(m_Requested.size());
        m_Stats.PagesReady = static_cast<u32>(m_Ready.size());
    }

    void VirtualGeometryPageStore::StageLocked(u32 page, VirtualPagePayload&& payload)
    {
        // A page in m_Ready is never in m_Requested, so a second completion for one cannot
        // normally arrive — but account for the replaced payload rather than rely on that, or
        // a stale byte count would make the staging cap meaningless.
        if (auto existing = m_Ready.find(page); existing != m_Ready.end())
        {
            NoteStagingBytesLocked(-static_cast<i64>(existing->second.Payload.ByteSize()));
            m_Ready.erase(existing);
        }
        // The accounting lives HERE rather than at each call site: both the worker-completion
        // path and the degraded synchronous path stage through this one function, and a caller
        // that forgot the `+=` would leave the staging cap reading low forever.
        NoteStagingBytesLocked(static_cast<i64>(payload.ByteSize()));
        auto const seq = m_NextReadySeq++;
        m_Ready.emplace(page, ReadyEntry{ std::move(payload), seq });
        m_ReadyOrder.emplace_back(page, seq);
    }

    void VirtualGeometryPageStore::TrimStagedLocked()
    {
        // A page is fetched because its group was requested, and consumed when the residency
        // pass loads it. Those are different frames, and in between the camera can move: the
        // group stops being requested, LoadPage is never called for it, and the payload sits
        // here forever. Without this, a moving camera over a large scene grows the staged set
        // without bound — the store would slowly re-accumulate in RAM exactly the payload it
        // spilled to get rid of, which is the whole point defeated.
        auto isStale = [this](const std::pair<u32, u64>& entry)
        {
            auto const it = m_Ready.find(entry.first);
            return it == m_Ready.end() || it->second.Seq != entry.second;
        };

        // Entries naming a page that was released, or an EARLIER staging of a page that has
        // since been staged again, are stale; drop them so the queue only holds live payloads.
        while (!m_ReadyOrder.empty() && isStale(m_ReadyOrder.front()))
        {
            m_ReadyOrder.pop_front();
        }

        // Oldest-first, but never a LEASED payload: a caller is holding a pointer into it
        // until its Release, and erasing it would hand that caller freed storage. Leases last
        // one LoadPage call, so a skipped entry is collectable again almost immediately —
        // which is why this scans past them rather than giving up at the front.
        //
        // `skipped` holds the leased entries passed over, in order, and they go back on the
        // front afterwards so the queue stays an arrival-ordered list rather than losing them.
        std::deque<std::pair<u32, u64>> skipped;
        while (m_Ready.size() > m_MaxStagedPages && !m_ReadyOrder.empty())
        {
            auto const entry = m_ReadyOrder.front();
            m_ReadyOrder.pop_front();
            if (isStale(entry))
            {
                continue;
            }
            auto it = m_Ready.find(entry.first);
            if (it->second.Leased)
            {
                skipped.push_back(entry);
                continue;
            }
            NoteStagingBytesLocked(-static_cast<i64>(it->second.Payload.ByteSize()));
            m_Ready.erase(it);
            ++m_Stats.StagedDiscards;
        }
        while (!skipped.empty())
        {
            m_ReadyOrder.push_front(skipped.back());
            skipped.pop_back();
        }

        // Stale entries in the MIDDLE of the deque are not reachable by the front scan above
        // (consumption is not strictly FIFO), so the deque can outgrow the map it indexes even
        // while the map stays capped. Compact rather than let a bounded cache carry an
        // unbounded index.
        if (m_ReadyOrder.size() > 4 * static_cast<sizet>(m_MaxStagedPages) + 4)
        {
            std::deque<std::pair<u32, u64>> live;
            for (const auto& entry : m_ReadyOrder)
            {
                if (!isStale(entry))
                {
                    live.push_back(entry);
                }
            }
            m_ReadyOrder.swap(live);
        }
    }

    VirtualGeometryPageStore::FetchState VirtualGeometryPageStore::Fetch(u32 meshBase, u32 pageIndex,
                                                                         const VirtualPagePayload*& outPayload)
    {
        outPayload = nullptr;
        auto const page = meshBase + pageIndex;
        if (meshBase == kInvalidMesh || page >= m_Directory.size())
        {
            return FetchState::Failed;
        }

        // Degraded mode: no worker could open a read handle, so there is nobody to answer an
        // asynchronous request. Serve it here rather than answer Pending forever — the caller
        // would otherwise retry a page that will never be queued, and every read counter would
        // stay at zero while the geometry silently stayed coarse. StartWorkers already logged
        // why; ReadPageBlocking counts the cost.
        if (m_Workers.empty())
        {
            {
                // Answer from what is already staged first. Without this the degraded path
                // re-reads the same page from disk on every Fetch — on the render thread, and
                // residency asks for the same pages every frame.
                std::lock_guard lock(m_Mutex);
                if (m_Failed.contains(page))
                {
                    return FetchState::Failed;
                }
                if (auto it = m_Ready.find(page); it != m_Ready.end())
                {
                    it->second.Leased = true;
                    outPayload = &it->second.Payload;
                    return FetchState::Ready;
                }
            }
            VirtualPagePayload payload;
            if (!ReadPageBlocking(meshBase, pageIndex, payload))
            {
                return FetchState::Failed;
            }
            std::lock_guard lock(m_Mutex);
            StageLocked(page, std::move(payload));
            TrimStagedLocked();
            auto const it = m_Ready.find(page);
            if (it == m_Ready.end())
            {
                return FetchState::Deferred; // capped out by a cap of 0-ish; retry next frame
            }
            it->second.Leased = true;
            outPayload = &it->second.Payload;
            m_Stats.PagesReady = static_cast<u32>(m_Ready.size());
            return FetchState::Ready;
        }

        std::lock_guard lock(m_Mutex);
        DrainCompletionsLocked();

        if (m_Failed.contains(page))
        {
            return FetchState::Failed;
        }
        if (auto it = m_Ready.find(page); it != m_Ready.end())
        {
            it->second.Leased = true;
            outPayload = &it->second.Payload;
            return FetchState::Ready;
        }
        if (m_Requested.contains(page))
        {
            return FetchState::Pending;
        }
        // Backpressure: staging RAM is bounded by how many reads may be outstanding, so a
        // frame that asks for more pages than it can absorb gets the rest later. Deferred, not
        // Pending — no read was started, so a caller that budgets its per-frame work must not
        // pay for this page; see FetchState.
        if (m_Requested.size() >= m_MaxReadsInFlight)
        {
            return FetchState::Deferred;
        }

        m_Requested.insert(page);
        m_Queue.push_back(Request{ page });
        ++m_Stats.ReadsIssued;
        m_Stats.ReadsInFlight = static_cast<u32>(m_Requested.size());
        m_WorkAvailable.notify_one();
        return FetchState::Pending;
    }

    void VirtualGeometryPageStore::Release(u32 meshBase, u32 pageIndex, bool keepStaged)
    {
        if (meshBase == kInvalidMesh)
        {
            return;
        }
        auto const page = meshBase + pageIndex;
        std::lock_guard lock(m_Mutex);
        auto it = m_Ready.find(page);
        if (it == m_Ready.end())
        {
            return;
        }
        it->second.Leased = false;
        if (keepStaged)
        {
            // The bytes stay for a later attempt, but the entry is trimmable again, so this
            // cannot pin memory: the staged cap governs it exactly like any other payload.
            TrimStagedLocked();
            m_Stats.PagesReady = static_cast<u32>(m_Ready.size());
            return;
        }
        NoteStagingBytesLocked(-static_cast<i64>(it->second.Payload.ByteSize()));
        m_Ready.erase(it);
        m_Stats.PagesReady = static_cast<u32>(m_Ready.size());
    }

    void VirtualGeometryPageStore::Poll()
    {
        if (m_File == nullptr)
        {
            return;
        }
        std::lock_guard lock(m_Mutex);
        DrainCompletionsLocked();
    }

    bool VirtualGeometryPageStore::ReadPageBlocking(u32 meshBase, u32 pageIndex, VirtualPagePayload& out)
    {
        OLO_PROFILE_FUNCTION();

        out = {};
        auto const page = meshBase + pageIndex;
        if (meshBase == kInvalidMesh || m_File == nullptr || page >= m_Directory.size())
        {
            return false;
        }

        PageRecord record;
        {
            std::lock_guard lock(m_Mutex);
            if (m_Failed.contains(page))
            {
                return false;
            }
            record = m_Directory[page];
        }

        bool const ok = ReadRecord(m_File, record, out);
        std::lock_guard lock(m_Mutex);
        ++m_Stats.BlockingReads;
        if (ok)
        {
            m_Stats.BytesRead += out.ByteSize();
        }
        else
        {
            ++m_Stats.ReadsFailed;
            m_Failed.insert(page);
            OLO_CORE_ERROR("VirtualGeometryPageStore: blocking read of page {} from '{}' failed. This page is "
                           "pinned or eagerly loaded, so its geometry will simply be missing from the cut.",
                           page, m_Path.string());
        }
        return ok;
    }

    void VirtualGeometryPageStore::SetMaxReadsInFlight(u32 maxReads)
    {
        m_MaxReadsInFlight = std::max(1u, maxReads);
    }

    void VirtualGeometryPageStore::SetMaxStagedPages(u32 maxStaged)
    {
        m_MaxStagedPages = std::max(1u, maxStaged);
        std::lock_guard lock(m_Mutex);
        TrimStagedLocked();
        m_Stats.PagesReady = static_cast<u32>(m_Ready.size());
    }

    void VirtualGeometryPageStore::Close()
    {
        if (!m_Workers.empty())
        {
            {
                std::lock_guard lock(m_Mutex);
                m_Stop.store(true, std::memory_order_relaxed);
                m_Queue.clear();
            }
            m_WorkAvailable.notify_all();
            for (std::thread& worker : m_Workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }
            m_Workers.clear();
        }
        for (std::FILE* stream : m_WorkerFiles)
        {
            (void)std::fclose(stream);
        }
        m_WorkerFiles.clear();

        if (m_File != nullptr)
        {
            (void)std::fclose(m_File);
            m_File = nullptr;
            std::error_code ec;
            std::filesystem::remove(m_Path, ec);
            if (ec)
            {
                OLO_CORE_WARN("VirtualGeometryPageStore: could not delete spill file '{}': {}", m_Path.string(),
                              ec.message());
            }
        }

        m_Path.clear();
        m_Directory.clear();
        m_Queue.clear();
        m_Completions.clear();
        m_Requested.clear();
        m_Ready.clear();
        m_ReadyOrder.clear();
        m_NextReadySeq = 1;
        m_Failed.clear();
        m_WriteCursor = 0;
        m_OpenFailed = false;
        m_Stop.store(false, std::memory_order_relaxed);
        m_Stats = {};
    }
} // namespace OloEngine
