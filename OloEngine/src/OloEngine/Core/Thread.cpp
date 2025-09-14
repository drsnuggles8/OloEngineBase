#include "OloEnginePCH.h"
#include "Thread.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <Windows.h>

namespace OloEngine {
namespace Detail {

	// Helper function to convert UTF-8 string to wide string
	std::wstring Utf8ToWide(const std::string& utf8Str)
	{
		if (utf8Str.empty())
			return std::wstring();

		int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, nullptr, 0);
		if (wideSize <= 0)
			return std::wstring();

		std::wstring wideStr(wideSize, 0); // Allocate space for null terminator
		int result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, &wideStr[0], wideSize);
		if (result <= 0)
			return std::wstring();

		// Remove the null terminator that MultiByteToWideChar adds
		if (!wideStr.empty() && wideStr.back() == L'\0')
			wideStr.pop_back();

		return wideStr;
	}

} // namespace Detail
} // namespace OloEngine

namespace OloEngine {

	Thread::Thread(const std::string& name)
		: m_Name(name)
	{
	}

	void Thread::SetName(const std::string& name)
	{
		HANDLE threadHandle = m_Thread.native_handle();

		std::wstring wName = Detail::Utf8ToWide(name);
		SetThreadDescription(threadHandle, wName.c_str());
		SetThreadAffinityMask(threadHandle, 8);
	}

	void Thread::Join()
	{
		if (m_Thread.joinable())
			m_Thread.join();
	}

	ThreadSignal::ThreadSignal(const std::string& name, bool manualReset)
	{
		if (name.empty())
		{
			m_SignalHandle = CreateEventW(NULL, static_cast<BOOL>(manualReset), FALSE, nullptr);
		}
		else
		{
			std::wstring wideName = OloEngine::Detail::Utf8ToWide(name);
			m_SignalHandle = CreateEventW(NULL, static_cast<BOOL>(manualReset), FALSE, wideName.c_str());
		}
		
		if (m_SignalHandle == NULL)
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("Failed to create thread signal '{}': GetLastError() = {}", name, lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal creation failed");
		}
	}

	ThreadSignal::~ThreadSignal()
	{
		if (m_SignalHandle != NULL)
		{
			CloseHandle(m_SignalHandle);
			m_SignalHandle = NULL;
		}
	}

	void ThreadSignal::Wait()
	{
		DWORD result = WaitForSingleObject(m_SignalHandle, INFINITE);
		if (result != WAIT_OBJECT_0)
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("ThreadSignal::Wait failed: WaitForSingleObject returned {}, GetLastError() = {}", result, lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal Wait failed");
		}
	}

	void ThreadSignal::Signal()
	{
		BOOL result = SetEvent(m_SignalHandle);
		if (!result)
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("ThreadSignal::Signal failed: SetEvent returned FALSE, GetLastError() = {}", lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal Signal failed");
		}
	}

	void ThreadSignal::Reset()
	{
		BOOL result = ResetEvent(m_SignalHandle);
		if (!result)
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("ThreadSignal::Reset failed: ResetEvent returned FALSE, GetLastError() = {}", lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal Reset failed");
		}
	}

	std::thread::id Thread::GetID() const
	{
		return m_Thread.get_id();
	}

}
