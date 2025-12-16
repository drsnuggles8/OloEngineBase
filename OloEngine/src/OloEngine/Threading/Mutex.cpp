// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/IntrusiveMutex.h"

namespace OloEngine
{
	// FParams struct for TIntrusiveMutex delegation
	struct FMutex::FParams
	{
		static constexpr u8 IsLockedFlag = FMutex::IsLockedFlag;
		static constexpr u8 MayHaveWaitingLockFlag = FMutex::MayHaveWaitingLockFlag;
	};

	OLO_NOINLINE void FMutex::LockSlow()
	{
		TIntrusiveMutex<FParams>::LockLoop(m_State);
	}

	OLO_NOINLINE void FMutex::WakeWaitingThread()
	{
		TIntrusiveMutex<FParams>::WakeWaitingThread(m_State);
	}

	bool FMutex::TryWakeWaitingThread()
	{
		return TIntrusiveMutex<FParams>::TryWakeWaitingThread(m_State);
	}
} // namespace OloEngine
