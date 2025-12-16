// PreWindowsApi.h - Preparation before including Windows headers
// Ported from UE5.7 Windows/PreWindowsApi.h
//
// This header saves macros that Windows will redefine and sets up
// third-party include warning suppression.

#pragma once

#ifdef _WIN32

// This file should only be included from WindowsHWrapper.h
#if !defined(WINDOWS_H_WRAPPER_GUARD)
    #pragma message("WARNING: do not include PreWindowsApi.h directly. Use Platform/Windows/WindowsHWrapper.h instead")
#endif

// Save these macros for later; Windows redefines them
#pragma push_macro("TEXT")
#pragma push_macro("TRUE")
#pragma push_macro("FALSE")

// Undefine the TEXT macro for winnt.h to redefine it, unless it's already been included
#ifndef _WINNT_
    #undef TEXT
#endif

// Disable specific warnings that Windows headers trigger
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4668) // 'symbol' is not defined as a preprocessor macro
    #pragma warning(disable: 5105) // macro expansion producing 'defined' has undefined behavior
#endif

#endif // _WIN32
