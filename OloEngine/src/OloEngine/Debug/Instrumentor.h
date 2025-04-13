#pragma once

#include "OloEngine/Core/Log.h"

#if TRACY_ENABLE
	#include <tracy/Tracy.hpp>
	#include <glad/gl.h>
	#include <tracy/TracyOpenGL.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>
#include <mutex>
#include <sstream>

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
			std::lock_guard lock(m_Mutex);
			if (m_CurrentSession)
			{
				// If there is already a current session, then close it before beginning new one.
				// Subsequent profiling output meant for the original session will end up in the
				// newly opened session instead.  That's better than having badly formatted
				// profiling output.
				if (Log::GetCoreLogger()) // Edge case: BeginSession() might be before Log::Init()
				{
					OLO_CORE_ERROR("Instrumentor::BeginSession('{0}') when session '{1}' already open.", name, m_CurrentSession->Name);
				}
				InternalEndSession();
			}
			m_OutputStream.open(filepath);

			if (m_OutputStream.is_open())
			{
				m_CurrentSession = new InstrumentationSession({ name });
				WriteHeader();
			}
			else
			{
				if (Log::GetCoreLogger()) // Edge case: BeginSession() might be before Log::Init()
				{
					OLO_CORE_ERROR("Instrumentor could not open results file '{0}'.", filepath);
				}
			}
		}

		void EndSession()
		{
			std::lock_guard lock(m_Mutex);
			InternalEndSession();
		}

		void WriteProfile(const ProfileResult& result)
		{
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

			std::lock_guard lock(m_Mutex);
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
			}
		}
	private:
		std::mutex m_Mutex;
		InstrumentationSession* m_CurrentSession{nullptr};
		std::ofstream m_OutputStream;
	};

	class InstrumentationTimer
	{
	public:
		explicit InstrumentationTimer(const char* name)
			: m_Name(name)
		{
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
		bool m_Stopped{false};
	};

	namespace InstrumentorUtils {

		template <sizet N>
		struct ChangeResult
		{
			char Data[N];
		};

		template <sizet N, sizet K>
		constexpr auto CleanupOutputString(const char(&expr)[N], const char(&remove)[K])
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
	}
}
#endif

#define OLO_PROFILE 1
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

	#define OLO_PROFILE_SCOPE_LINE2(name, line) constexpr auto fixedName##line = ::OloEngine::InstrumentorUtils::CleanupOutputString(name, "__cdecl ");\
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
	#define OLO_PROFILE_GPU(name) TracyGpuZone(name)
	#define OLO_PROFILE_GPU_COLOR(name, color) TracyGpuZoneC(name, color)
	#define OLO_PROFILE_GPU_COLLECT() TracyGpuCollect
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
