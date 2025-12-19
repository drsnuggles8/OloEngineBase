// MinWindows.h - Minimal Windows.h include
// Ported from UE5.7 Windows/MinWindows.h
//
// Includes Windows.h with minimal extra definitions to reduce
// compile times and namespace pollution.

#pragma once

#ifdef _WIN32

// This file should only be included from WindowsHWrapper.h
#if !defined(WINDOWS_H_WRAPPER_GUARD)
#pragma message("WARNING: do not include MinWindows.h directly. Use Platform/Windows/WindowsHWrapper.h instead")
#endif

#if defined(_WINDOWS_) && !defined(OLO_MINIMAL_WINDOWS_INCLUDE)
#pragma message(" ")
#pragma message("You have included windows.h before MinWindows.h")
#pragma message("All useless stuff from the windows headers won't be excluded!")
#pragma message(" ")
#endif

#define OLO_MINIMAL_WINDOWS_INCLUDE

// WIN32_LEAN_AND_MEAN excludes rarely-used services from windows headers
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Exclude additional unused services
#define NOGDICAPMASKS    // CC_*, LC_*, PC_*, CP_*, TC_*, RC_
#define OEMRESOURCE      // OEM Resource values
#define NOATOM           // Atom Manager routines
#define NOKERNEL         // All KERNEL #defines and routines
#define NOMEMMGR         // GMEM_*, LMEM_*, GHND, LHND, associated routines
#define NOMETAFILE       // typedef METAFILEPICT
#define NOMINMAX         // Macros min(a,b) and max(a,b)
#define NOOPENFILE       // OpenFile(), OemToAnsi, AnsiToOem, and OF_*
#define NOSCROLL         // SB_* and scrolling routines
#define NOSERVICE        // All Service Controller routines, SERVICE_ equates, etc.
#define NOSOUND          // Sound driver routines
#define NOCOMM           // COMM driver routines
#define NOKANJI          // Kanji support stuff
#define NOHELP           // Help engine interface
#define NOPROFILER       // Profiler interface
#define NODEFERWINDOWPOS // DeferWindowPos routines
#define NOMCX            // Modem Configuration Extensions
#define NOCRYPT          // Cryptography
#define NOTAPE           // Tape API
#define NOIMAGE          // Image API
#define NOPROXYSTUB      // Proxy/stub
#define NORPC            // RPC

// Forward declare IUnknown for COM
struct IUnknown;

// Finally include Windows.h
#include <Windows.h>

#endif // _WIN32
