#include "OloEnginePCH.h"
#include "Thread.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <Windows.h>

namespace
{
	// Helper function to convert UTF-8 string to wide string
	std::wstring Utf8ToWide(const std::string& utf8Str)
	{
		if (utf8Str.empty())
			return std::wstring();

		int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
		if (wideSize <= 0)
			return std::wstring();

		std::wstring wideStr(wideSize - 1, 0); // -1 to exclude null terminator
		MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wideStr[0], wideSize);
		return wideStr;
	}
}

namespace OloEngine {

	Thread::Thread(const std::string& name)
		: m_Name(name)
	{
	}

	void Thread::SetName(const std::string& name)
	{
		HANDLE threadHandle = m_Thread.native_handle();

		std::wstring wName(name.begin(), name.end());
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
		std::wstring wideName = Utf8ToWide(name);
		m_SignalHandle = CreateEventW(NULL, (BOOL)manualReset, FALSE, wideName.c_str());
		
		if (m_SignalHandle == NULL)
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("Failed to create thread signal '{}': GetLastError() = {}", name, lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal creation failed");
		}
	}

	void ThreadSignal::Wait()
	{
		WaitForSingleObject(m_SignalHandle, INFINITE);
	}

	void ThreadSignal::Signal()
	{
		SetEvent(m_SignalHandle);
	}

	void ThreadSignal::Reset()
	{
		ResetEvent(m_SignalHandle);
	}

	std::thread::id Thread::GetID() const
	{
		return m_Thread.get_id();
	}

}
