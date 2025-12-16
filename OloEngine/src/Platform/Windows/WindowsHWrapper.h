// WindowsHWrapper.h - Safe Windows.h include wrapper
// Ported from UE5.7 Windows/WindowsHWrapper.h
//
// This header should be used instead of directly including <windows.h>
// It handles:
// 1. Setting up minimal Windows definitions
// 2. Cleaning up problematic Windows macros after inclusion

#pragma once

#include "OloEngine/Core/Base.h"

#ifdef _WIN32

#if defined(WINDOWS_H_WRAPPER_GUARD)
    #error WINDOWS_H_WRAPPER_GUARD already defined - recursive include detected
#endif
#define WINDOWS_H_WRAPPER_GUARD

#include "Platform/Windows/PreWindowsApi.h"
#include "Platform/Windows/MinWindows.h"
#include "Platform/Windows/PostWindowsApi.h"

#undef WINDOWS_H_WRAPPER_GUARD

#endif // _WIN32
