#pragma once

#include "OloEngine/Core/Log.h"

#if TRACY_ENABLE
#include <tracy/Tracy.hpp>
// <tracy/TracyOpenGL.hpp> is deliberately NOT included here (issue #691).
//
// It needs the GL loader's symbols, so including it forces <glad/gl.h> into this
// header — and this header is in OloEnginePCH.h, which means EVERY translation
// unit in the engine would see the whole OpenGL API. That is the same transitive
// leak RendererAPI.h used to be (ADR 0011 §1.7's "ordering trap"), except wider:
// it bypasses the renderer entirely and would keep the sweep_glad_includes
// ratchet meaningless no matter how many per-file includes were removed.
//
// The OLO_PROFILE_GPU* macros below are therefore no-ops in every build config.
// Nothing in the engine, editor, runtime or server used them. To add GPU zones,
// include <tracy/TracyOpenGL.hpp> from a translation unit under Platform/OpenGL/
// — which may legitimately see GL — and use the TracyGpu* macros there directly.
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>
#include <sstream>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#if !TRACY_ENABLE
namespace OloEngine
{
    using FloatingPointMicroseconds = std::chrono::duration<f64, std::micro>;

    struct ProfileResult
    {
        std::string Name;

        FloatingPointMicroseconds Start;
        std::chrono::microseconds ElapsedTime;
        std::thread::id ThreadID;
    };

    struct InstrumentationSession
    {
        std::string Name;
    };

    class Instrumentor
    {
      public:
        Instrumentor(const Instrumentor&) = delete;
        Instrumentor(Instrumentor&&) = delete;

        void BeginSession(const std::string& name, const std::string& filepath = "results.json")
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            if (m_CurrentSession)
            {
                // If there is already a current session, then close it before beginning new one.
                // Subsequent profiling output meant for the original session will end up in the
                // newly opened session instead.  That's better than having badly formatted
                // profiling output.
                if (Log::Get().GetCoreLogger()) // Edge case: BeginSession() might be before Log is fully initialized
                {
                    OLO_CORE_ERROR("Instrumentor::BeginSession('{0}') when session '{1}' already open.", name, m_CurrentSession->Name);
                }
                InternalEndSession();
            }
            m_OutputStream.open(filepath);

            if (m_OutputStream.is_open())
            {
                m_CurrentSession = new InstrumentationSession({ name });
                s_SessionActive.store(true, std::memory_order_release);
                WriteHeader();
            }
            else
            {
                if (Log::Get().GetCoreLogger()) // Edge case: BeginSession() might be before Log is fully initialized
                {
                    OLO_CORE_ERROR("Instrumentor could not open results file '{0}'.", filepath);
                }
            }
        }

        void EndSession()
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            InternalEndSession();
        }

        void WriteProfile(const ProfileResult& result)
        {
            // Fast path: bail before doing any work when no session is active. Without
            // this, every InstrumentationTimer destruction (potentially in tight loops at
            // tens of kHz, e.g. per-audio-frame profiling) allocates a stringstream and
            // formats a JSON record only to throw it away. That overhead alone was enough
            // to push the audio thread from 100 Hz callbacks down to ~13-16 Hz.
            if (!s_SessionActive.load(std::memory_order_acquire))
                return;

            std::stringstream json;

            json << std::setprecision(3) << std::fixed;
            json << ",{";
            json << R"("cat":"function",)";
            json << "\"dur\":" << (result.ElapsedTime.count()) << ',';
            json << R"("name":")" << result.Name << "\",";
            json << R"("ph":"X",)";
            json << "\"pid\":0,";
            json << "\"tid\":" << result.ThreadID << ",";
            json << "\"ts\":" << result.Start.count();
            json << "}";

            TUniqueLock<FMutex> lock(m_Mutex);
            if (m_CurrentSession)
            {
                m_OutputStream << json.str();
                m_OutputStream.flush();
            }
        }

        static Instrumentor& Get()
        {
            static Instrumentor instance;
            return instance;
        }

        // Lock-free fast path for InstrumentationTimer: if no session is active, the
        // timer can skip the steady_clock::now() calls entirely. Returns the class-static
        // atomic directly — no Get() call, no function-static initialisation guard
        // branch, which matters when this gets hit ~100,000 times per second from audio
        // DSP paths.
        static std::atomic<bool>& SessionActiveRef() noexcept
        {
            return s_SessionActive;
        }

      private:
        Instrumentor()
            : m_CurrentSession(nullptr)
        {
        }

        ~Instrumentor()
        {
            EndSession();
        }

        void WriteHeader()
        {
            m_OutputStream << R"({"otherData": {},"traceEvents":[{})";
            m_OutputStream.flush();
        }

        void WriteFooter()
        {
            m_OutputStream << "]}";
            m_OutputStream.flush();
        }

        // Note: you must already own lock on m_Mutex before
        // calling InternalEndSession()
        void InternalEndSession()
        {
            if (m_CurrentSession)
            {
                WriteFooter();
                m_OutputStream.close();
                delete m_CurrentSession;
                m_CurrentSession = nullptr;
                s_SessionActive.store(false, std::memory_order_release);
            }
        }

      public:
        // Class-static so InstrumentationTimer can read it without going through Get().
        // Get() would emit a function-static initialisation guard branch on every call;
        // at ~100,000 calls/sec from audio DSP that adds up. Defined inline so a single
        // declaration suffices (no separate .cpp).
        inline static std::atomic<bool> s_SessionActive{ false };

      private:
        FMutex m_Mutex;
        InstrumentationSession* m_CurrentSession{ nullptr };
        std::ofstream m_OutputStream;
    };

    class InstrumentationTimer
    {
      public:
        explicit InstrumentationTimer(const char* name)
            : m_Name(name)
        {
            // No session → no work. Skipping steady_clock::now() is critical because this
            // ctor runs on every OLO_PROFILE_FUNCTION/SCOPE call, and Debug builds hit it
            // at audio-callback rates (tens of thousands of times per second). Each
            // steady_clock::now() on Windows is a QueryPerformanceCounter call, which is
            // cheap but not free at that rate. With m_Stopped pre-flagged, the dtor also
            // short-circuits Stop().
            if (!Instrumentor::s_SessionActive.load(std::memory_order_acquire))
            {
                m_Stopped = true;
                return;
            }
            m_StartTimepoint = std::chrono::steady_clock::now();
        }

        ~InstrumentationTimer()
        {
            if (!m_Stopped)
            {
                Stop();
            }
        }

        void Stop()
        {
            auto endTimepoint = std::chrono::steady_clock::now();
            auto highResStart = FloatingPointMicroseconds{ m_StartTimepoint.time_since_epoch() };
            auto elapsedTime = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint).time_since_epoch() - std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimepoint).time_since_epoch();

            Instrumentor::Get().WriteProfile({ m_Name, highResStart, elapsedTime, std::this_thread::get_id() });

            m_Stopped = true;
        }

      private:
        const char* m_Name;
        std::chrono::time_point<std::chrono::steady_clock> m_StartTimepoint;
        bool m_Stopped{ false };
    };

    namespace InstrumentorUtils
    {

        template<sizet N>
        struct ChangeResult
        {
            char Data[N];
        };

        template<sizet N, sizet K>
        constexpr auto CleanupOutputString(const char (&expr)[N], const char (&remove)[K])
        {
            ChangeResult<N> result = {};

            sizet srcIndex = 0;
            sizet dstIndex = 0;
            while (srcIndex < N)
            {
                sizet matchIndex = 0;
                while (matchIndex < K - 1 && srcIndex + matchIndex < N - 1 && expr[srcIndex + matchIndex] == remove[matchIndex])
                {
                    matchIndex++;
                }
                if (matchIndex == K - 1)
                {
                    srcIndex += matchIndex;
                }
                result.Data[dstIndex++] = expr[srcIndex] == '"' ? '\'' : expr[srcIndex];
                srcIndex++;
            }
            return result;
        }
    } // namespace InstrumentorUtils
} // namespace OloEngine
#endif

#if defined(OLO_DIST)
#define OLO_PROFILE 0
#else
#define OLO_PROFILE 1
#endif
#if OLO_PROFILE && !TRACY_ENABLE
  // Resolve which function signature macro will be used. Note that this only
// is resolved when the (pre)compiler starts, so the syntax highlighting
// could mark the wrong one in your editor!
#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)
#define OLO_FUNC_SIG __PRETTY_FUNCTION__
#elif defined(__DMC__) && (__DMC__ >= 0x810)
#define OLO_FUNC_SIG __PRETTY_FUNCTION__
#elif (defined(__FUNCSIG__) || (_MSC_VER))
#define OLO_FUNC_SIG __FUNCSIG__
#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
#define OLO_FUNC_SIG __FUNCTION__
#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
#define OLO_FUNC_SIG __FUNC__
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
#define OLO_FUNC_SIG __func__
#elif defined(__cplusplus) && (__cplusplus >= 201103)
#define OLO_FUNC_SIG __func__
#else
#define OLO_FUNC_SIG "OLO_FUNC_SIG unknown!"
#endif

#define OLO_PROFILE_BEGIN_SESSION(name, filepath) ::OloEngine::Instrumentor::Get().BeginSession(name, filepath)
#define OLO_PROFILE_END_SESSION() ::OloEngine::Instrumentor::Get().EndSession()

#define OLO_PROFILE_SCOPE_LINE2(name, line)                                                                 \
    constexpr auto fixedName##line = ::OloEngine::InstrumentorUtils::CleanupOutputString(name, "__cdecl "); \
    ::OloEngine::InstrumentationTimer timer##line(fixedName##line.Data)
#define OLO_PROFILE_SCOPE_LINE(name, line) OLO_PROFILE_SCOPE_LINE2(name, line)

#define OLO_PROFILE_SCOPE(name) OLO_PROFILE_SCOPE_LINE(name, __LINE__)
#define OLO_PROFILE_FUNCTION() OLO_PROFILE_SCOPE(OLO_FUNC_SIG)
#define OLO_PROFILE_FRAMEMARK_START(name)
#define OLO_PROFILE_FRAMEMARK_END(name)
#define OLO_PROFILE_SETVALUE(value)
#define OLO_PROFILE_GPU(name)
#define OLO_PROFILE_GPU_COLOR(name, color)
#define OLO_PROFILE_GPU_COLLECT()
#elif OLO_PROFILE && TRACY_ENABLE
#define OLO_PROFILE_BEGIN_SESSION(name, filepath)
#define OLO_PROFILE_END_SESSION()

#define OLO_PROFILE_SCOPE(name) ZoneScopedN(name)
#define OLO_PROFILE_FUNCTION() ZoneScoped
#define OLO_PROFILE_FRAMEMARK_START(name) FrameMarkStart(name)
#define OLO_PROFILE_FRAMEMARK_END(name) FrameMarkEnd(name)
#define OLO_PROFILE_SETVALUE(value) ZoneValue(value)
// No-ops: see the <tracy/TracyOpenGL.hpp> note at the top of this header.
#define OLO_PROFILE_GPU(name)
#define OLO_PROFILE_GPU_COLOR(name, color)
#define OLO_PROFILE_GPU_COLLECT()
#else
#define OLO_PROFILE_BEGIN_SESSION(name, filepath)
#define OLO_PROFILE_END_SESSION()
#define OLO_PROFILE_SCOPE(name)
#define OLO_PROFILE_FUNCTION()
#define OLO_PROFILE_FRAMEMARK_START(name)
#define OLO_PROFILE_FRAMEMARK_END(name)
#define OLO_PROFILE_SETVALUE(value)
#define OLO_PROFILE_GPU(name)
#define OLO_PROFILE_GPU_COLOR(name, color)
#define OLO_PROFILE_GPU_COLLECT()
#endif

// Tracy sections (Tracy 0.14): high-level program phases on the timeline, such as
// a scene load, a play session or a streaming region in flight. Unlike zones they
// may overlap, span frames and cross threads, and the profiler can filter them by
// category or limit any statistics window to one. The enter macro returns an id
// that is 0 when no profiler is connected (TRACY_ON_DEMAND), and leaving section
// 0 is a no-op, so callers never branch on the connection. Labels are printf
// formats; without Tracy their arguments are not evaluated.
namespace OloEngine
{
    enum class EProfileSection : u16
    {
        Scene = 1,     // loading or opening a scene file
        Session = 2,   // Play or Simulate, from start to stop
        Streaming = 3, // a streaming region loading or unloading
    };
} // namespace OloEngine

#if OLO_PROFILE && TRACY_ENABLE
#define OLO_PROFILE_SECTION_ENTER(category, ...) TracySectionEnterCategory(static_cast<u16>(category), __VA_ARGS__)
#define OLO_PROFILE_SECTION_LEAVE(id) TracySectionLeave(id)
#else
#define OLO_PROFILE_SECTION_ENTER(category, ...) static_cast<u32>(0)
#define OLO_PROFILE_SECTION_LEAVE(id) static_cast<void>(id)
#endif

namespace OloEngine
{
    // Names the EProfileSection categories in the profiler. Call once at startup;
    // under TRACY_ON_DEMAND Tracy replays the names to a profiler that connects later.
    inline void RegisterProfileSectionCategories()
    {
#if OLO_PROFILE && TRACY_ENABLE
        TracySectionSetup(static_cast<u16>(EProfileSection::Scene), "Scene load");
        TracySectionSetup(static_cast<u16>(EProfileSection::Session), "Play session");
        TracySectionSetup(static_cast<u16>(EProfileSection::Streaming), "Streaming");
#endif
    }

    class ScopedProfileSection
    {
      public:
        explicit ScopedProfileSection(u32 id) : m_Id(id) {}
        ~ScopedProfileSection()
        {
            OLO_PROFILE_SECTION_LEAVE(m_Id);
        }
        ScopedProfileSection(const ScopedProfileSection&) = delete;
        ScopedProfileSection& operator=(const ScopedProfileSection&) = delete;

      private:
        u32 m_Id;
    };
} // namespace OloEngine

#define OLO_PROFILE_SECTION_NAME2(line) oloProfileSection##line
#define OLO_PROFILE_SECTION_NAME(line) OLO_PROFILE_SECTION_NAME2(line)
// A section covering the rest of the enclosing scope.
#define OLO_PROFILE_SECTION(category, ...) \
    const ::OloEngine::ScopedProfileSection OLO_PROFILE_SECTION_NAME(__LINE__){ OLO_PROFILE_SECTION_ENTER(category, __VA_ARGS__) }

// Additional profiler macros for compatibility
#if OLO_PROFILE && TRACY_ENABLE
#define OLO_PROFILER_THREAD(name) // tracy::SetThreadName(name) - TODO(olbu): Implement when Tracy SetThreadName is available
#define OLO_PROFILER_SCOPE(name) OLO_PROFILE_SCOPE(name)
#else
#define OLO_PROFILER_THREAD(name)
#define OLO_PROFILER_SCOPE(name)
#endif
