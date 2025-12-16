// IntrusiveUniqueLock.h - RAII lock wrapper for intrusive mutexes
// Ported from UE5.7 UE::TIntrusiveUniqueLock

#pragma once

#include "OloEngine/Threading/IntrusiveMutex.h"
#include <atomic>

namespace OloEngine
{
	/**
	 * An intrusive mutex wrapper that locks on construction and unlocks on destruction.
	 * For details on how to set up an intrusive mutex, see the IntrusiveMutex.h header.
	 */
	template <CIntrusiveMutexParams ParamsType>
	class TIntrusiveUniqueLock
	{
		using StateType = TIntrusiveMutexStateType_T<ParamsType>;

	public:
		TIntrusiveUniqueLock(TIntrusiveUniqueLock& NoCopyConstruction) = delete;
		TIntrusiveUniqueLock& operator=(TIntrusiveUniqueLock& NoAssignment) = delete;

		[[nodiscard]] explicit TIntrusiveUniqueLock(std::atomic<StateType>& InState)
			: m_State(InState)
		{
			TIntrusiveMutex<ParamsType>::Lock(m_State);
		}

		~TIntrusiveUniqueLock()
		{
			TIntrusiveMutex<ParamsType>::Unlock(m_State);
		}

	private:
		std::atomic<StateType>& m_State;
	};

} // namespace OloEngine
