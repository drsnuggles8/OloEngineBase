#pragma once

// Platform detection using predefined macros
#ifdef _WIN32
/* Windows x64/x86 */
#ifdef _WIN64
/* Windows x64  */
#define OLO_PLATFORM_WINDOWS
#else
/* Windows x86 */
#error "x86 Builds are not supported!"
#endif
#elif defined(__APPLE__) || defined(__MACH__)
#include <TargetConditionals.h>
/* TARGET_OS_MAC exists on all the platforms
 * so we must check all of them (in this order)
 * to ensure that we're running on MAC
 * and not some other Apple platform */
#if TARGET_IPHONE_SIMULATOR == 1
#error "IOS simulator is not supported!"
#elif TARGET_OS_IPHONE == 1
#define OLO_PLATFORM_IOS
#error "IOS is not supported!"
#elif TARGET_OS_MAC == 1
#define OLO_PLATFORM_MACOS
#error "MacOS is not supported!"
#else
#error "Unknown Apple platform!"
#endif
/* We also have to check __ANDROID__ before __linux__
 * since android is based on the linux kernel
 * it has __linux__ defined */
#elif defined(__ANDROID__)
#define OLO_PLATFORM_ANDROID
#error "Android is not supported!"
#elif defined(__linux__)
#define OLO_PLATFORM_LINUX
#else
/* Unknown compiler/platform */
#error "Unknown platform!"
#endif
// End of platform detection

// Pointer size / bitness detection
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64) || defined(__ppc64__)
#define OLO_PLATFORM_64BITS 1
#else
#define OLO_PLATFORM_64BITS 0
#endif

// Build configuration macros (defaults; overridden by CMake for specific configs)
#ifndef OLO_BUILD_SHIPPING
#define OLO_BUILD_SHIPPING 0
#endif

#ifndef OLO_TRACK_MEMORY
#define OLO_TRACK_MEMORY 0
#endif
