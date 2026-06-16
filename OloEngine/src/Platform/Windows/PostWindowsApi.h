// PostWindowsApi.h - Cleanup after including Windows headers
// Ported from UE5.7 Windows/PostWindowsApi.h
//
// This header undoes problematic Windows macro definitions that would
// otherwise conflict with our codebase.

#pragma once

#ifdef _WIN32

// This file should only be included from WindowsHWrapper.h
#if !defined(WINDOWS_H_WRAPPER_GUARD)
#pragma message("WARNING: do not include PostWindowsApi.h directly. Use Platform/Windows/WindowsHWrapper.h instead")
#endif

// Re-enable warnings
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Hide Windows-only types that conflict with our types
#undef INT
#undef UINT
#undef DWORD
#undef FLOAT

// Undo Windows defines that conflict with common identifiers
#undef CaptureStackBackTrace
#undef CopyFile
#undef CreateDirectory
#undef DeleteFile
#undef DrawText
#undef FindWindow
#undef GetClassName
#undef GetCommandLine
#undef GetCurrentTime
#undef GetEnvironmentVariable
#undef GetFileAttributes
#undef GetFreeSpace
#undef GetMessage
#undef GetObject
#undef GetTempFileName
#undef IsMaximized
#undef IsMinimized
#undef LoadString
#undef MemoryBarrier
#undef MoveFile
#undef PostMessage
#undef ReportEvent
#undef SendMessage
#undef SetPort
#undef UpdateResource
#undef Yield // Conflicts with FPlatformProcess::Yield()

// Undefine atomics that we implement ourselves
#undef InterlockedIncrement
#undef InterlockedDecrement
#undef InterlockedAdd
#undef InterlockedExchange
#undef InterlockedExchangeAdd
#undef InterlockedCompareExchange
#undef InterlockedCompareExchangePointer
#undef InterlockedExchange64
#undef InterlockedExchangeAdd64
#undef InterlockedCompareExchange64
#undef InterlockedIncrement64
#undef InterlockedDecrement64
#undef InterlockedAnd
#undef InterlockedOr
#undef InterlockedXor

// Restore any previously defined macros
#pragma pop_macro("TEXT")
// NOTE: TRUE/FALSE are intentionally left as <Windows.h> defined them (1/0) — see
// PreWindowsApi.h for the rationale (Win32 BOOL constants used by engine + vendored
// code such as miniaudio.h / FileWatch.hpp).

#endif // _WIN32
