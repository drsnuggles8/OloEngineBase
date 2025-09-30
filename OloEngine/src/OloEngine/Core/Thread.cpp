#include "OloEnginePCH.h"
#include "Thread.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <Windows.h>

// TODO(olbu): Refactor this Windows-specific code to follow the usual platform-based approach.
// This entire file contains Windows-specific implementations (CreateEventExW, MultiByteToWideChar, etc.)
// and should be moved to Platform/Windows/ directory structure to maintain platform abstraction.

namespace OloEngine {
namespace Detail {

	// Helper function to convert UTF-8 string to wide string
	std::wstring Utf8ToWide(const std::string& utf8Str)
	{
		if (utf8Str.empty())
			return std::wstring();

		// Query required buffer size
		int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, nullptr, 0);
		if (wideSize <= 0) [[unlikely]]
		{
			DWORD lastError = ::GetLastError();
			OLO_CORE_ERROR("Utf8ToWide: Failed to query buffer size for UTF-8 string '{}': GetLastError() = {}", utf8Str, lastError);
			return std::wstring();
		}

		// Allocate buffer and perform conversion
		std::wstring wideStr(static_cast<sizet>(wideSize), L'\0');
		int result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, wideStr.data(), wideSize);
		if (result <= 0) [[unlikely]]
		{
			DWORD lastError = ::GetLastError();
			OLO_CORE_ERROR("Utf8ToWide: Failed to convert UTF-8 string '{}' to wide string: GetLastError() = {}", utf8Str, lastError);
			return std::wstring();
		}

		// Remove the null terminator that MultiByteToWideChar adds
		if (!wideStr.empty() && wideStr.back() == L'\0') [[likely]]
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

	Thread::~Thread()
	{
		if (m_Thread.joinable())
			m_Thread.join();
	}

	Thread::Thread(Thread&& other) noexcept
		: m_Name(std::move(other.m_Name))
		, m_Thread(std::move(other.m_Thread))
	{
	}

	Thread& Thread::operator=(Thread&& other) noexcept
	{
		if (this != &other)
		{
			// Clean up current thread if it's joinable
			if (m_Thread.joinable())
				m_Thread.join();
			
			// Move the data from other
			m_Name = std::move(other.m_Name);
			m_Thread = std::move(other.m_Thread);
		}
		return *this;
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
		// Compute flags for the modern CreateEventExW API
		DWORD dwFlags = manualReset ? CREATE_EVENT_MANUAL_RESET : 0;
		DWORD dwDesiredAccess = EVENT_MODIFY_STATE | SYNCHRONIZE;
		
		// Convert name to wide string only if non-empty
		const wchar_t* namePtr = nullptr;
		std::wstring wideName;
		if (!name.empty())
		{
			wideName = OloEngine::Detail::Utf8ToWide(name);
			namePtr = wideName.c_str();
		}
		
		// Use modern CreateEventExW API with unified logic
		m_SignalHandle = CreateEventExW(nullptr, namePtr, dwFlags, dwDesiredAccess);
		
		if (m_SignalHandle == nullptr)
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("Failed to create thread signal '{}': GetLastError() = {}", name, lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal creation failed");
		}
	}

	ThreadSignal::~ThreadSignal()
	{
		if (m_SignalHandle != nullptr)
		{
			CloseHandle(m_SignalHandle);
			m_SignalHandle = nullptr;
		}
	}

	ThreadSignal::ThreadSignal(ThreadSignal&& other) noexcept
		: m_SignalHandle(other.m_SignalHandle)
	{
		other.m_SignalHandle = nullptr;
	}

	ThreadSignal& ThreadSignal::operator=(ThreadSignal&& other) noexcept
	{
		if (this != &other)
		{
			// Clean up our current handle
			if (m_SignalHandle != nullptr)
			{
				CloseHandle(m_SignalHandle);
			}
			
			// Transfer ownership
			m_SignalHandle = other.m_SignalHandle;
			other.m_SignalHandle = nullptr;
		}
		return *this;
	}

	void ThreadSignal::Wait()
	{
		// Validate handle before calling WaitForSingleObject
		if (m_SignalHandle == nullptr || m_SignalHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		{
			OLO_CORE_ERROR("ThreadSignal::Wait failed: Invalid handle (m_SignalHandle = {})", reinterpret_cast<uintptr_t>(m_SignalHandle));
			OLO_CORE_ASSERT(false, "ThreadSignal Wait called with invalid handle");
			return;
		}

		DWORD result = WaitForSingleObject(m_SignalHandle, INFINITE);
		if (result == WAIT_FAILED) [[unlikely]]
		{
			DWORD lastError = ::GetLastError();
			OLO_CORE_ERROR("ThreadSignal::Wait failed: WaitForSingleObject returned WAIT_FAILED ({}), GetLastError() = {}", result, lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal Wait failed");
		}
		else if (result != WAIT_OBJECT_0) [[unlikely]]
		{
			// This should not happen with INFINITE timeout, but handle unexpected results
			OLO_CORE_ERROR("ThreadSignal::Wait unexpected result: WaitForSingleObject returned {} (expected WAIT_OBJECT_0)", result);
			OLO_CORE_ASSERT(false, "ThreadSignal Wait returned unexpected result");
		}
	}

	void ThreadSignal::Signal()
	{
		// Validate handle before calling SetEvent
		if (m_SignalHandle == nullptr || m_SignalHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		{
			OLO_CORE_ERROR("ThreadSignal::Signal failed: Invalid handle (m_SignalHandle = {})", reinterpret_cast<uintptr_t>(m_SignalHandle));
			OLO_CORE_ASSERT(false, "ThreadSignal Signal called with invalid handle");
			return;
		}

		BOOL result = SetEvent(m_SignalHandle);
		if (!result) [[unlikely]]
		{
			DWORD lastError = GetLastError();
			OLO_CORE_ERROR("ThreadSignal::Signal failed: SetEvent returned FALSE, GetLastError() = {}", lastError);
			OLO_CORE_ASSERT(false, "ThreadSignal Signal failed");
		}
	}

	void ThreadSignal::Reset()
	{
		// Validate handle before calling ResetEvent
		if (m_SignalHandle == nullptr || m_SignalHandle == INVALID_HANDLE_VALUE) [[unlikely]]
		{
			OLO_CORE_ERROR("ThreadSignal::Reset failed: Invalid handle (m_SignalHandle = {})", reinterpret_cast<uintptr_t>(m_SignalHandle));
			OLO_CORE_ASSERT(false, "ThreadSignal Reset called with invalid handle");
			return;
		}

		BOOL result = ResetEvent(m_SignalHandle);
		if (!result) [[unlikely]]
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
