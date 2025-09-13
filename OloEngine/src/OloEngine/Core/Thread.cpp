#include "OloEnginePCH.h"
#include "Thread.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <Windows.h>

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
		m_SignalHandle = CreateEventA(NULL, (BOOL)manualReset, FALSE, name.c_str());
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
