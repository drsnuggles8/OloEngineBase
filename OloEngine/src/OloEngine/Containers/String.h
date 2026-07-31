#pragma once

// @file String.h
// @brief FString — a trivially-relocatable string, ported from Unreal Engine.
//
// Ported from UE 5.8's Containers/UnrealString.h.inl, which defines the class
// as a macro-parameterised template instantiated as FString (TCHAR),
// FUtf8String (UTF8CHAR) and FAnsiString (ANSICHAR). This port follows the
// FUtf8String instantiation — a `char` element type — because OloEngine's
// existing string surface (scene YAML, asset paths, ImGui, Lua/C# bindings) is
// UTF-8 `std::string` throughout, so a UTF-8 FString minimises conversion
// friction at the boundaries.
//
// WHY THIS TYPE EXISTS
// --------------------
// TArray relocates its elements BITWISE: ResizeGrow goes through the
// allocator's ResizeAllocation -> FMemory::Realloc, which moves the raw byte
// buffer; the insert/remove paths memmove via RelocateConstructItems. Neither
// consults any element trait. UE documents this contract explicitly:
//
//     "TArray (like many Unreal Engine containers) assumes that the element
//      type is trivially relocatable, meaning that elements can safely be
//      moved from one location in memory to another by directly copying raw
//      bytes."
//
// libstdc++'s std::string violates that contract: under the small-string
// optimisation its internal pointer points into its OWN inline buffer, so a
// bitwise relocation leaves that pointer aimed at the element's old address
// and the destructor frees a non-heap pointer:
//
//     free(): invalid pointer      (SIGABRT, ~TArray<Submesh> via ~MeshSource)
//
// MSVC's std::string keeps no such self-pointer, which is why this only ever
// aborted against libstdc++.
//
// UE's own string type has no such problem *by construction* — it is a single
// TArray<CharType> member with NO small-string optimisation, so its pointer
// always targets a separate heap block and byte-copying it is harmless. That
// is precisely why UE can assume relocatability engine-wide. This port keeps
// that property, which is the whole point: FString is safe to store in TArray.
//
// STORAGE INVARIANT (identical to UE)
// -----------------------------------
// `Data` is either completely empty (Num() == 0, representing "") or holds the
// characters FOLLOWED BY a null terminator, so Num() == Len() + 1. Len() must
// therefore never be `Data.Num()`.

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/ContainerAllocationPolicies.h"
#include "OloEngine/Core/Base.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace OloEngine
{
    class FString
    {
      public:
        // Matches UE: TSizedDefaultAllocator<32> — the same allocator TArray
        // uses elsewhere in the engine.
        using AllocatorType = TSizedDefaultAllocator<32>;
        using ElementType = char;

      private:
        // Array holding the character data (UE's member name and layout).
        using DataType = TArray<ElementType, AllocatorType>;
        DataType Data;

        using SizeType = typename DataType::SizeType;

        // Debug-only check of the storage invariant described in the header
        // comment. UE has the equivalent as CheckInvariants().
        void CheckInvariants() const
        {
            OLO_CORE_ASSERT(Data.Num() == 0 || Data.Last() == '\0',
                            "FString storage must be empty or null-terminated");
        }

      public:
        FString() = default;
        FString(const FString&) = default;
        FString(FString&&) noexcept = default;
        FString& operator=(const FString&) = default;
        FString& operator=(FString&&) noexcept = default;
        ~FString() = default;

        // ------------------------------------------------------------------
        // Construction
        // ------------------------------------------------------------------

        FString(const char* src)
        {
            if (src && *src)
            {
                const sizet len = std::strlen(src);
                ConstructFromPtrSize(src, static_cast<SizeType>(len));
            }
        }

        FString(const char* src, SizeType count)
        {
            ConstructFromPtrSize(src, count);
        }

        FString(std::string_view src)
        {
            ConstructFromPtrSize(src.data(), static_cast<SizeType>(src.size()));
        }

        FString(const std::string& src)
        {
            ConstructFromPtrSize(src.data(), static_cast<SizeType>(src.size()));
        }

        // ------------------------------------------------------------------
        // Size / capacity
        // ------------------------------------------------------------------

        // Length in characters, EXCLUDING the stored null terminator.
        [[nodiscard]] SizeType Len() const
        {
            return Data.Num() ? Data.Num() - 1 : 0;
        }

        [[nodiscard]] bool IsEmpty() const
        {
            return Data.Num() <= 1;
        }

        [[nodiscard]] bool IsValidIndex(SizeType index) const
        {
            return index >= 0 && index < Len();
        }

        void Empty(SizeType slack = 0)
        {
            Data.Empty(slack ? slack + 1 : 0);
        }

        void Reset(SizeType newSize = 0)
        {
            Data.Reset(newSize ? newSize + 1 : 0);
        }

        void Reserve(SizeType characters)
        {
            if (characters > 0)
                Data.Reserve(characters + 1);
        }

        void Shrink()
        {
            Data.Shrink();
        }

        [[nodiscard]] u32 GetAllocatedSize() const
        {
            return Data.GetAllocatedSize();
        }

        // ------------------------------------------------------------------
        // Element / buffer access
        // ------------------------------------------------------------------

        // UE spells the raw-pointer accessor `*Str`. Always returns a valid
        // null-terminated buffer, even when empty.
        [[nodiscard]] const char* operator*() const
        {
            return Data.Num() ? Data.GetData() : "";
        }

        [[nodiscard]] const char* GetData() const
        {
            return **this;
        }

        [[nodiscard]] DataType& GetCharArray()
        {
            return Data;
        }
        [[nodiscard]] const DataType& GetCharArray() const
        {
            return Data;
        }

        [[nodiscard]] char& operator[](SizeType index)
        {
            OLO_CORE_ASSERT(IsValidIndex(index), "FString index out of range");
            return Data.GetData()[index];
        }

        [[nodiscard]] const char& operator[](SizeType index) const
        {
            OLO_CORE_ASSERT(IsValidIndex(index), "FString index out of range");
            return Data.GetData()[index];
        }

        // ------------------------------------------------------------------
        // Interop with the std:: string surface the engine already uses
        // ------------------------------------------------------------------

        [[nodiscard]] std::string ToStdString() const
        {
            return std::string(**this, static_cast<sizet>(Len()));
        }

        [[nodiscard]] std::string_view ToView() const
        {
            return std::string_view(**this, static_cast<sizet>(Len()));
        }

        explicit operator std::string() const
        {
            return ToStdString();
        }
        explicit operator std::string_view() const
        {
            return ToView();
        }

        // ------------------------------------------------------------------
        // Append / concatenation
        // ------------------------------------------------------------------

        FString& AppendChars(const char* str, SizeType count)
        {
            if (!str || count <= 0)
                return *this;

            const SizeType oldLen = Len();
            // +1 for the terminator; Data may be completely empty here.
            Data.SetNumUninitialized(oldLen + count + 1);
            std::memcpy(Data.GetData() + oldLen, str, static_cast<sizet>(count));
            Data.GetData()[oldLen + count] = '\0';
            CheckInvariants();
            return *this;
        }

        FString& AppendChar(char c)
        {
            return AppendChars(&c, 1);
        }

        FString& Append(const FString& other)
        {
            return AppendChars(*other, other.Len());
        }
        FString& Append(const char* str)
        {
            return AppendChars(str, str ? static_cast<SizeType>(std::strlen(str)) : 0);
        }
        FString& Append(std::string_view sv)
        {
            return AppendChars(sv.data(), static_cast<SizeType>(sv.size()));
        }

        FString& operator+=(const FString& other)
        {
            return Append(other);
        }
        FString& operator+=(const char* str)
        {
            return Append(str);
        }
        FString& operator+=(std::string_view sv)
        {
            return Append(sv);
        }
        FString& operator+=(char c)
        {
            return AppendChar(c);
        }

        [[nodiscard]] friend FString operator+(FString lhs, const FString& rhs)
        {
            lhs.Append(rhs);
            return lhs;
        }

        [[nodiscard]] friend FString operator+(FString lhs, const char* rhs)
        {
            lhs.Append(rhs);
            return lhs;
        }

        [[nodiscard]] friend FString operator+(const char* lhs, const FString& rhs)
        {
            FString result(lhs);
            result.Append(rhs);
            return result;
        }

        // ------------------------------------------------------------------
        // Comparison
        // ------------------------------------------------------------------

        enum class ESearchCase
        {
            CaseSensitive,
            IgnoreCase
        };

        [[nodiscard]] bool Equals(const FString& other, ESearchCase cs = ESearchCase::CaseSensitive) const
        {
            if (Len() != other.Len())
                return false;
            return Compare(other, cs) == 0;
        }

        [[nodiscard]] i32 Compare(const FString& other, ESearchCase cs = ESearchCase::CaseSensitive) const
        {
            const char* a = **this;
            const char* b = *other;
            if (cs == ESearchCase::CaseSensitive)
                return std::strcmp(a, b);

            for (;; ++a, ++b)
            {
                const i32 ca = ToLowerChar(*a);
                const i32 cb = ToLowerChar(*b);
                if (ca != cb)
                    return ca - cb;
                if (*a == '\0')
                    return 0;
            }
        }

        [[nodiscard]] friend bool operator==(const FString& lhs, const FString& rhs)
        {
            return lhs.Equals(rhs);
        }
        [[nodiscard]] friend bool operator==(const FString& lhs, const char* rhs)
        {
            return std::strcmp(*lhs, rhs ? rhs : "") == 0;
        }
        [[nodiscard]] friend bool operator<(const FString& lhs, const FString& rhs)
        {
            return lhs.Compare(rhs) < 0;
        }

        // ------------------------------------------------------------------
        // Search
        // ------------------------------------------------------------------

        static constexpr SizeType InvalidIndex = -1;

        [[nodiscard]] SizeType Find(std::string_view sub, ESearchCase cs = ESearchCase::CaseSensitive,
                                    SizeType startPos = 0) const
        {
            if (sub.empty())
                return InvalidIndex;
            const SizeType len = Len();
            const SizeType subLen = static_cast<SizeType>(sub.size());
            if (subLen > len)
                return InvalidIndex;

            for (SizeType i = (startPos < 0 ? 0 : startPos); i + subLen <= len; ++i)
            {
                bool match = true;
                for (SizeType j = 0; j < subLen; ++j)
                {
                    const char a = (*this)[i + j];
                    const char b = sub[static_cast<sizet>(j)];
                    const bool same = (cs == ESearchCase::CaseSensitive) ? (a == b)
                                                                         : (ToLowerChar(a) == ToLowerChar(b));
                    if (!same)
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                    return i;
            }
            return InvalidIndex;
        }

        [[nodiscard]] bool Contains(std::string_view sub, ESearchCase cs = ESearchCase::CaseSensitive) const
        {
            return Find(sub, cs) != InvalidIndex;
        }

        [[nodiscard]] bool FindChar(char c, SizeType& outIndex) const
        {
            const SizeType len = Len();
            for (SizeType i = 0; i < len; ++i)
            {
                if ((*this)[i] == c)
                {
                    outIndex = i;
                    return true;
                }
            }
            outIndex = InvalidIndex;
            return false;
        }

        [[nodiscard]] bool FindLastChar(char c, SizeType& outIndex) const
        {
            for (SizeType i = Len() - 1; i >= 0; --i)
            {
                if ((*this)[i] == c)
                {
                    outIndex = i;
                    return true;
                }
            }
            outIndex = InvalidIndex;
            return false;
        }

        [[nodiscard]] bool StartsWith(std::string_view prefix, ESearchCase cs = ESearchCase::CaseSensitive) const
        {
            const SizeType n = static_cast<SizeType>(prefix.size());
            if (n > Len())
                return false;
            for (SizeType i = 0; i < n; ++i)
            {
                const char a = (*this)[i];
                const char b = prefix[static_cast<sizet>(i)];
                if (cs == ESearchCase::CaseSensitive ? (a != b) : (ToLowerChar(a) != ToLowerChar(b)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool EndsWith(std::string_view suffix, ESearchCase cs = ESearchCase::CaseSensitive) const
        {
            const SizeType n = static_cast<SizeType>(suffix.size());
            const SizeType len = Len();
            if (n > len)
                return false;
            for (SizeType i = 0; i < n; ++i)
            {
                const char a = (*this)[len - n + i];
                const char b = suffix[static_cast<sizet>(i)];
                if (cs == ESearchCase::CaseSensitive ? (a != b) : (ToLowerChar(a) != ToLowerChar(b)))
                    return false;
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Substrings
        // ------------------------------------------------------------------

        [[nodiscard]] FString Left(SizeType count) const
        {
            return FString(**this, Clamp(count, 0, Len()));
        }

        [[nodiscard]] FString Right(SizeType count) const
        {
            const SizeType len = Len();
            const SizeType n = Clamp(count, 0, len);
            return FString(**this + (len - n), n);
        }

        [[nodiscard]] FString Mid(SizeType start, SizeType count = MAX_i32) const
        {
            const SizeType len = Len();
            if (start >= len || count <= 0)
                return FString();
            const SizeType begin = Clamp(start, 0, len);
            const SizeType n = Clamp(count, 0, len - begin);
            return FString(**this + begin, n);
        }

        [[nodiscard]] FString LeftChop(SizeType count) const
        {
            return Left(Len() - Clamp(count, 0, Len()));
        }
        [[nodiscard]] FString RightChop(SizeType count) const
        {
            return Mid(Clamp(count, 0, Len()));
        }

        // ------------------------------------------------------------------
        // Case / trimming
        // ------------------------------------------------------------------

        [[nodiscard]] FString ToUpper() const
        {
            FString out(*this);
            out.ToUpperInline();
            return out;
        }

        void ToUpperInline()
        {
            const SizeType len = Len();
            char* p = Data.GetData();
            for (SizeType i = 0; i < len; ++i)
                p[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[i])));
        }

        [[nodiscard]] FString ToLower() const
        {
            FString out(*this);
            out.ToLowerInline();
            return out;
        }

        void ToLowerInline()
        {
            const SizeType len = Len();
            char* p = Data.GetData();
            for (SizeType i = 0; i < len; ++i)
                p[i] = ToLowerChar(p[i]);
        }

        [[nodiscard]] FString TrimStart() const
        {
            SizeType i = 0;
            const SizeType len = Len();
            while (i < len && std::isspace(static_cast<unsigned char>((*this)[i])))
                ++i;
            return Mid(i);
        }

        [[nodiscard]] FString TrimEnd() const
        {
            SizeType end = Len();
            while (end > 0 && std::isspace(static_cast<unsigned char>((*this)[end - 1])))
                --end;
            return Left(end);
        }

        [[nodiscard]] FString TrimStartAndEnd() const
        {
            return TrimStart().TrimEnd();
        }

        // ------------------------------------------------------------------
        // Split / formatting
        // ------------------------------------------------------------------

        // UE semantics: returns true and fills the out params when `separator`
        // is found; otherwise returns false and leaves them untouched.
        bool Split(std::string_view separator, FString* outLeft, FString* outRight,
                   ESearchCase cs = ESearchCase::CaseSensitive) const
        {
            const SizeType idx = Find(separator, cs);
            if (idx == InvalidIndex)
                return false;
            if (outLeft)
                *outLeft = Left(idx);
            if (outRight)
                *outRight = Mid(idx + static_cast<SizeType>(separator.size()));
            return true;
        }

        [[nodiscard]] static FString Printf(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            va_list copy;
            va_copy(copy, args);
            const int needed = std::vsnprintf(nullptr, 0, fmt, copy);
            va_end(copy);

            FString result;
            if (needed > 0)
            {
                result.Data.SetNumUninitialized(needed + 1);
                std::vsnprintf(result.Data.GetData(), static_cast<sizet>(needed) + 1, fmt, args);
                result.Data.GetData()[needed] = '\0';
            }
            va_end(args);
            return result;
        }

        [[nodiscard]] static FString FromInt(i64 value)
        {
            return Printf("%lld", static_cast<long long>(value));
        }

      private:
        void ConstructFromPtrSize(const char* src, SizeType count)
        {
            if (!src || count <= 0)
                return;
            Data.SetNumUninitialized(count + 1);
            std::memcpy(Data.GetData(), src, static_cast<sizet>(count));
            Data.GetData()[count] = '\0';
            CheckInvariants();
        }

        [[nodiscard]] static char ToLowerChar(char c)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        [[nodiscard]] static SizeType Clamp(SizeType v, SizeType lo, SizeType hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }
    };

    // FString is a single TArray member, which is itself trivially relocatable
    // (pointer + two integers, pointing at a SEPARATE heap block). Stating it
    // explicitly documents the property this whole type exists to provide, and
    // means TArray<FString> is safe where TArray<std::string> is not.
    template<>
    struct TIsTriviallyRelocatable<FString>
    {
        enum
        {
            Value = true
        };
    };

    [[nodiscard]] inline u32 GetTypeHash(const FString& s)
    {
        return static_cast<u32>(std::hash<std::string_view>{}(s.ToView()));
    }
} // namespace OloEngine

template<>
struct std::hash<OloEngine::FString>
{
    [[nodiscard]] std::size_t operator()(const OloEngine::FString& s) const noexcept
    {
        return std::hash<std::string_view>{}(s.ToView());
    }
};
