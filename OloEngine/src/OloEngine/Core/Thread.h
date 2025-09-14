#pragma once

#include <string>
#include <thread>

namespace OloEngine {

	class Thread
	{
	public:
		Thread(const std::string& name);

		template<typename Fn, typename... Args>
		void Dispatch(Fn&& func, Args&&... args)
		{
			m_Thread = std::thread(func, std::forward<Args>(args)...);
			SetName(m_Name);
		}

		void SetName(const std::string& name);

		void Join();

		std::thread::id GetID() const;
	private:
		std::string m_Name;
		std::thread m_Thread;
	};

	class ThreadSignal
	{
	public:
		ThreadSignal(const std::string& name, bool manualReset = false);
		~ThreadSignal();

		// Disable copy semantics to prevent double-closing handles
		ThreadSignal(const ThreadSignal&) = delete;
		ThreadSignal& operator=(const ThreadSignal&) = delete;

		void Wait();
		void Signal();
		void Reset();
	private:
		void* m_SignalHandle = nullptr;
	};

}
