#pragma once
n// @file MemStackUtility.h
// @brief Utility functions for FMemStack string and array allocation
// 
// Provides helper functions for allocating strings and arrays from a memory stack.
// 
// Ported from Unreal Engine's MemStackUtility.h

#include "OloEngine/Memory/MemStack.h"

#include <string>
#include <string_view>
#include <span>
#include <cstring>
#include <format>

namespace OloEngine::MemStackUtils
{
    // @brief Allocate a string from a memory stack
    // @param Allocator The memory stack to allocate from
    // @param String Pointer to string data
    // @param Length Length of string (not including null terminator)
    // @return Pointer to allocated null-terminated string
    inline const char* AllocateString(FMemStackBase& Allocator, const char* String, i32 Length)
    {
        char* Result = New<char>(Allocator, Length + 1);
        std::memcpy(Result, String, Length);
        Result[Length] = '\0';
        return Result;
    }

    // @brief Allocate a string from a memory stack
    // @param Allocator The memory stack to allocate from
    // @param String String view to copy
    // @return Pointer to allocated null-terminated string
    inline const char* AllocateString(FMemStackBase& Allocator, std::string_view String)
    {
        return AllocateString(Allocator, String.data(), static_cast<i32>(String.length()));
    }

    // @brief Allocate a string and return as string_view
    // @param Allocator The memory stack to allocate from
    // @param String String view to copy
    // @return String view of allocated string
    inline std::string_view AllocateStringView(FMemStackBase& Allocator, std::string_view String)
    {
        return std::string_view(AllocateString(Allocator, String), String.length());
    }

    // @brief Allocate a wide string from a memory stack
    // @param Allocator The memory stack to allocate from
    // @param String Pointer to wide string data
    // @param Length Length of string (not including null terminator)
    // @return Pointer to allocated null-terminated wide string
    inline const wchar_t* AllocateWideString(FMemStackBase& Allocator, const wchar_t* String, i32 Length)
    {
        wchar_t* Result = New<wchar_t>(Allocator, Length + 1);
        std::memcpy(Result, String, Length * sizeof(wchar_t));
        Result[Length] = L'\0';
        return Result;
    }

    // @brief Allocate a wide string from a memory stack
    // @param Allocator The memory stack to allocate from
    // @param String Wide string view to copy
    // @return Pointer to allocated null-terminated wide string
    inline const wchar_t* AllocateWideString(FMemStackBase& Allocator, std::wstring_view String)
    {
        return AllocateWideString(Allocator, String.data(), static_cast<i32>(String.length()));
    }

    // @brief Allocate a wide string and return as wstring_view
    // @param Allocator The memory stack to allocate from
    // @param String Wide string view to copy
    // @return Wide string view of allocated string
    inline std::wstring_view AllocateWideStringView(FMemStackBase& Allocator, std::wstring_view String)
    {
        return std::wstring_view(AllocateWideString(Allocator, String), String.length());
    }

    // @brief Allocate a formatted string from a memory stack
    // @tparam Args Format argument types
    // @param Allocator The memory stack to allocate from
    // @param Fmt Format string
    // @param args Format arguments
    // @return Pointer to allocated null-terminated string
    template <typename... Args>
    inline const char* AllocateStringf(FMemStackBase& Allocator, std::format_string<Args...> Fmt, Args&&... args)
    {
        std::string Formatted = std::format(Fmt, std::forward<Args>(args)...);
        return AllocateString(Allocator, Formatted);
    }

    // @brief Allocate a formatted string and return as string_view
    // @tparam Args Format argument types
    // @param Allocator The memory stack to allocate from
    // @param Fmt Format string
    // @param args Format arguments
    // @return String view of allocated string
    template <typename... Args>
    inline std::string_view AllocateStringViewf(FMemStackBase& Allocator, std::format_string<Args...> Fmt, Args&&... args)
    {
        std::string Formatted = std::format(Fmt, std::forward<Args>(args)...);
        return AllocateStringView(Allocator, Formatted);
    }

    // @brief Allocate a copy of a span from a memory stack
    // @tparam T Element type
    // @param Allocator The memory stack to allocate from
    // @param View Span to copy
    // @return Span of allocated copy
    template <typename T>
    inline std::span<T> AllocateSpan(FMemStackBase& Allocator, std::span<T> View)
    {
        T* Data = nullptr;
        if (!View.empty())
        {
            Data = New<T>(Allocator, static_cast<i32>(View.size()));
            for (sizet i = 0; i < View.size(); ++i)
            {
                Data[i] = View[i];
            }
        }
        return std::span<T>(Data, View.size());
    }

    // @brief Allocate a copy of a span from a memory stack (const version)
    // @tparam T Element type
    // @param Allocator The memory stack to allocate from
    // @param View Span to copy
    // @return Span of allocated copy
    template <typename T>
    inline std::span<const T> AllocateSpan(FMemStackBase& Allocator, std::span<const T> View)
    {
        T* Data = nullptr;
        if (!View.empty())
        {
            Data = New<T>(Allocator, static_cast<i32>(View.size()));
            for (sizet i = 0; i < View.size(); ++i)
            {
                Data[i] = View[i];
            }
        }
        return std::span<const T>(Data, View.size());
    }

} // namespace OloEngine::MemStackUtils
