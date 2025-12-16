// TransactionallySafeMutex.h - Mutex variants for transactional memory
// Ported from UE5.7 - simplified without AutoRTFM support
//
// In UE5.7, these mutexes integrate with the AutoRTFM software transactional
// memory system. Since OloEngine doesn't have AutoRTFM, these are simple
// aliases to the regular mutex types, providing API compatibility.

#pragma once

#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/RecursiveMutex.h"
#include "OloEngine/Threading/SharedMutex.h"

namespace OloEngine
{
	/**
	 * A transactionally safe mutex.
	 * 
	 * Without AutoRTFM support, this is simply an alias for FMutex.
	 * In UE5.7 with AutoRTFM enabled, this provides special handling for
	 * lock/unlock operations within software transactions.
	 */
	using FTransactionallySafeMutex = FMutex;

	/**
	 * A transactionally safe recursive mutex.
	 * 
	 * Without AutoRTFM support, this is simply an alias for FRecursiveMutex.
	 */
	using FTransactionallySafeRecursiveMutex = FRecursiveMutex;

	/**
	 * A transactionally safe shared mutex.
	 * 
	 * Without AutoRTFM support, this is simply an alias for FSharedMutex.
	 */
	using FTransactionallySafeSharedMutex = FSharedMutex;

} // namespace OloEngine

// Compatibility - some code may reference without namespace prefix
using FTransactionallySafeMutex = OloEngine::FTransactionallySafeMutex;
using FTransactionallySafeRecursiveMutex = OloEngine::FTransactionallySafeRecursiveMutex;
using FTransactionallySafeSharedMutex = OloEngine::FTransactionallySafeSharedMutex;
