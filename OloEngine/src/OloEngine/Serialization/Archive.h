#pragma once

/**
 * @file Archive.h
 * @brief Archive serialization interface for containers and data
 * 
 * This provides an implementation of UE's FArchive interface ported from
 * Unreal Engine's Serialization/Archive.h. The archive system handles
 * reading/writing data in a platform-independent way.
 * 
 * Key classes:
 * - FArchiveState: Base state flags and version info (inherited by FArchive)
 * - FArchive: Main serialization class with operator<< for primitives
 * - FMemoryArchive: Base for memory-backed archives
 * - FMemoryReader: Reads from a memory buffer
 * - FMemoryWriter: Writes to a memory buffer
 * - FStructuredArchive: Hierarchical serialization (JSON/XML-like)
 * 
 * Ported from Unreal Engine's Serialization/Archive.h
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Serialization/MemoryLayout.h"

#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace OloEngine
{
    // Forward declarations
    class FArchive;
    class FStructuredArchive;
    class FMemoryImageWriter;
    struct FMemoryUnfreezeContent;
    struct FPlatformTypeLayoutParameters;
    class FSHA1;

    // ============================================================================
    // Helper Macro for bitpacked booleans
    // ============================================================================

    // Helper macro to make serializing a bitpacked boolean in an archive easier.
    // NOTE: The condition is there to avoid overwriting a value that is the same
    #define FArchive_Serialize_BitfieldBool(ARCHIVE, BITFIELD_BOOL) \
        { \
            bool TEMP_BITFIELD_BOOL = BITFIELD_BOOL; \
            ARCHIVE << TEMP_BITFIELD_BOOL; \
            if (BITFIELD_BOOL != TEMP_BITFIELD_BOOL) { BITFIELD_BOOL = TEMP_BITFIELD_BOOL; } \
        }

    // ============================================================================
    // FArchiveState - Archive State Base Struct
    // ============================================================================

    /**
     * @struct FArchiveState
     * @brief Base state for archives containing all flags and version info
     * 
     * This is ported from UE's FArchiveState which separates state from the
     * serialization logic. FArchive privately inherits from this.
     */
    struct FArchiveState
    {
    private:
        // Only FArchive is allowed to instantiate this, by inheritance
        friend class FArchive;

        FArchiveState() = default;
        FArchiveState(const FArchiveState&) = default;
        FArchiveState& operator=(const FArchiveState& ArchiveToCopy) = default;
        virtual ~FArchiveState() = default;

    protected:
        /** Copies all of the members */
        void CopyTrivialFArchiveStatusMembers(const FArchiveState& ArchiveStatusToCopy)
        {
            ArIsLoading = ArchiveStatusToCopy.ArIsLoading;
            ArIsSaving = ArchiveStatusToCopy.ArIsSaving;
            ArIsTransacting = ArchiveStatusToCopy.ArIsTransacting;
            ArIsTextFormat = ArchiveStatusToCopy.ArIsTextFormat;
            ArWantBinaryPropertySerialization = ArchiveStatusToCopy.ArWantBinaryPropertySerialization;
            ArForceUnicode = ArchiveStatusToCopy.ArForceUnicode;
            ArIsPersistent = ArchiveStatusToCopy.ArIsPersistent;
            ArIsError = ArchiveStatusToCopy.ArIsError;
            ArIsCriticalError = ArchiveStatusToCopy.ArIsCriticalError;
            ArContainsCode = ArchiveStatusToCopy.ArContainsCode;
            ArContainsMap = ArchiveStatusToCopy.ArContainsMap;
            ArRequiresLocalizationGather = ArchiveStatusToCopy.ArRequiresLocalizationGather;
            ArForceByteSwapping = ArchiveStatusToCopy.ArForceByteSwapping;
            ArIgnoreArchetypeRef = ArchiveStatusToCopy.ArIgnoreArchetypeRef;
            ArNoDelta = ArchiveStatusToCopy.ArNoDelta;
            ArNoIntraPropertyDelta = ArchiveStatusToCopy.ArNoIntraPropertyDelta;
            ArIgnoreOuterRef = ArchiveStatusToCopy.ArIgnoreOuterRef;
            ArIgnoreClassGeneratedByRef = ArchiveStatusToCopy.ArIgnoreClassGeneratedByRef;
            ArIgnoreClassRef = ArchiveStatusToCopy.ArIgnoreClassRef;
            ArAllowLazyLoading = ArchiveStatusToCopy.ArAllowLazyLoading;
            ArIsObjectReferenceCollector = ArchiveStatusToCopy.ArIsObjectReferenceCollector;
            ArIsModifyingWeakAndStrongReferences = ArchiveStatusToCopy.ArIsModifyingWeakAndStrongReferences;
            ArIsCountingMemory = ArchiveStatusToCopy.ArIsCountingMemory;
            ArShouldSkipBulkData = ArchiveStatusToCopy.ArShouldSkipBulkData;
            ArIsFilterEditorOnly = ArchiveStatusToCopy.ArIsFilterEditorOnly;
            ArIsSaveGame = ArchiveStatusToCopy.ArIsSaveGame;
            ArIsNetArchive = ArchiveStatusToCopy.ArIsNetArchive;
            ArSerializingDefaults = ArchiveStatusToCopy.ArSerializingDefaults;
            ArPortFlags = ArchiveStatusToCopy.ArPortFlags;
            ArMaxSerializeSize = ArchiveStatusToCopy.ArMaxSerializeSize;
        }

    public:
        /** Returns lowest level archive state, proxy archives will override this. */
        virtual FArchiveState& GetInnermostState()
        {
            return *this;
        }

        /** Modifies current archive state, can be used to override flags. */
        void SetArchiveState(const FArchiveState& InState)
        {
            CopyTrivialFArchiveStatusMembers(InState);
        }

        /** Sets ArIsError to true. */
        void SetError() { ArIsError = true; }

        /** Sets ArIsError to false, this does not clear any CriticalErrors */
        void ClearError() { ArIsError = false; }

        /** Sets the archiver IsCriticalError and IsError to true. */
        void SetCriticalError() { ArIsCriticalError = true; ArIsError = true; }

        /** Called to get the computed size from a size-detecting archive after it has finished serializing. */
        virtual void CountBytes(sizet InNum, sizet InMax) { }

        /** Returns the name of the Archive. Useful for getting the name of the package. */
        [[nodiscard]] virtual std::string GetArchiveName() const { return "FArchive"; }

        /** Returns the current location within the backing data storage. Returns INDEX_NONE if not supported. */
        [[nodiscard]] virtual i64 Tell() { return INDEX_NONE; }

        /** Returns total size of the backing data storage. */
        [[nodiscard]] virtual i64 TotalSize() { return INDEX_NONE; }

        /** Returns true if the current location within the backing data storage is at the end. */
        [[nodiscard]] virtual bool AtEnd()
        {
            i64 Pos = Tell();
            return ((Pos != INDEX_NONE) && (Pos >= TotalSize()));
        }

        /** Returns true if data larger than 1 byte should be swapped to deal with endian mismatches. */
        [[nodiscard]] bool IsByteSwapping() const
        {
            // OloEngine targets little endian, so we swap if forcing byte swapping
            if constexpr (std::endian::native == std::endian::little)
            {
                return ArForceByteSwapping;
            }
            else
            {
                return IsPersistent();
            }
        }

        /** Sets a flag indicating that this archive contains native or generated code. */
        void ThisContainsCode() { ArContainsCode = true; }

        /** Sets a flag indicating that this archive contains a ULevel or UWorld object. */
        void ThisContainsMap() { ArContainsMap = true; }

        /** Sets a flag indicating that this archive contains data required to be gathered for localization. */
        void ThisRequiresLocalizationGather() { ArRequiresLocalizationGather = true; }

        // ========================================================================
        // State Query Methods
        // ========================================================================

        /** Returns true if this archive is for loading data. */
        [[nodiscard]] bool IsLoading() const { return ArIsLoading; }

        /** Returns true if this archive is for saving data. */
        [[nodiscard]] bool IsSaving() const { return ArIsSaving; }

        /** Returns true if this archive is transacting (undo/redo). */
        [[nodiscard]] bool IsTransacting() const { return ArIsTransacting; }

        /** Returns true if this archive serializes to a structured text format. */
        [[nodiscard]] bool IsTextFormat() const { return ArIsTextFormat; }

        /** Returns true if this archive wants properties to be serialized in binary form. */
        [[nodiscard]] bool WantBinaryPropertySerialization() const { return ArWantBinaryPropertySerialization; }

        /** Returns true if this archive wants to always save strings in UTF16 format. */
        [[nodiscard]] bool IsForcingUnicode() const { return ArForceUnicode; }

        /** Returns true if this archive saves to persistent storage. */
        [[nodiscard]] bool IsPersistent() const { return ArIsPersistent; }

        /** Returns true if this archive contains errors. */
        [[nodiscard]] bool IsError() const { return ArIsError; }

        /** For compatibility */
        [[nodiscard]] bool GetError() const { return ArIsError; }

        /** Returns true if this archive contains critical errors that cannot be recovered from. */
        [[nodiscard]] bool IsCriticalError() const { return ArIsCriticalError; }

        /** Returns true if this archive contains native or generated code. */
        [[nodiscard]] bool ContainsCode() const { return ArContainsCode; }

        /** Returns true if this archive contains a ULevel or UWorld object. */
        [[nodiscard]] bool ContainsMap() const { return ArContainsMap; }

        /** Returns true if this archive contains data required to be gathered for localization. */
        [[nodiscard]] bool RequiresLocalizationGather() const { return ArRequiresLocalizationGather; }

        /** Returns true if this archive should always swap bytes. */
        [[nodiscard]] bool ForceByteSwapping() const { return ArForceByteSwapping; }

        /** Returns true if this archive is currently serializing class/struct default values. */
        [[nodiscard]] bool IsSerializingDefaults() const { return (ArSerializingDefaults > 0); }

        /** Returns true if this archive should ignore archetype references for structs and classes. */
        [[nodiscard]] bool IsIgnoringArchetypeRef() const { return ArIgnoreArchetypeRef; }

        /** Returns true if this archive should handle delta serialization for properties. */
        [[nodiscard]] bool DoDelta() const { return !ArNoDelta; }

        /** Returns true if this archive should perform delta serialization within properties. */
        [[nodiscard]] bool DoIntraPropertyDelta() const { return !ArNoIntraPropertyDelta; }

        /** Returns true if this archive should ignore the Outer reference in UObject. */
        [[nodiscard]] bool IsIgnoringOuterRef() const { return ArIgnoreOuterRef; }

        /** Returns true if this archive should ignore the ClassGeneratedBy reference in UClass. */
        [[nodiscard]] bool IsIgnoringClassGeneratedByRef() const { return ArIgnoreClassGeneratedByRef; }

        /** Returns true if this archive should ignore the Class reference in UObject. */
        [[nodiscard]] bool IsIgnoringClassRef() const { return ArIgnoreClassRef; }

        /** Returns true if this archive should allow lazy loading of bulk / secondary data. */
        [[nodiscard]] bool IsAllowingLazyLoading() const { return ArAllowLazyLoading; }

        /** Returns true if this archive is only looking for UObject references. */
        [[nodiscard]] bool IsObjectReferenceCollector() const { return ArIsObjectReferenceCollector; }

        /** Returns true if this archive should modify/search weak object references as well as strong ones. */
        [[nodiscard]] bool IsModifyingWeakAndStrongReferences() const { return ArIsModifyingWeakAndStrongReferences; }

        /** Returns true if this archive is counting memory. */
        [[nodiscard]] bool IsCountingMemory() const { return ArIsCountingMemory; }

        /** Returns this archive's property serialization modifier flags. */
        [[nodiscard]] u32 GetPortFlags() const { return ArPortFlags; }

        /** Checks to see if any of the passed in property serialization modifier flags are set. */
        [[nodiscard]] bool HasAnyPortFlags(u32 Flags) const { return ((ArPortFlags & Flags) != 0); }

        /** Checks to see if all of the passed in property serialization modifier flags are set. */
        [[nodiscard]] bool HasAllPortFlags(u32 Flags) const { return ((ArPortFlags & Flags) == Flags); }

        /** Returns true if this archive should ignore bulk data. */
        [[nodiscard]] bool ShouldSkipBulkData() const { return ArShouldSkipBulkData; }

        /** Returns the maximum size of data that this archive is allowed to serialize. */
        [[nodiscard]] i64 GetMaxSerializeSize() const { return ArMaxSerializeSize; }

        /** Returns true if editor only properties are being filtered from the archive. */
        [[nodiscard]] bool IsFilterEditorOnly() const { return ArIsFilterEditorOnly; }

        /** Sets a flag indicating that this archive needs to filter editor-only content. */
        virtual void SetFilterEditorOnly(bool InFilterEditorOnly) { ArIsFilterEditorOnly = InFilterEditorOnly; }

        /** Indicates whether this archive is saving/loading game state */
        [[nodiscard]] bool IsSaveGame() const { return ArIsSaveGame; }

        /** Whether or not this archive is serializing data being sent/received by the netcode */
        [[nodiscard]] bool IsNetArchive() const { return ArIsNetArchive; }

        /**
         * Toggle byte order swapping.
         * @param Enabled set to true to enable byte order swapping
         */
        void SetByteSwapping(bool Enabled) { ArForceByteSwapping = Enabled; }

        /**
         * Sets the archive's property serialization modifier flags
         * @param InPortFlags the new flags to use for property serialization
         */
        void SetPortFlags(u32 InPortFlags) { ArPortFlags = InPortFlags; }

        /**
         * Checks whether the archive is used to resolve out-of-date enum indexes
         */
        [[nodiscard]] virtual bool UseToResolveEnumerators() const { return false; }

        /** Resets all of the base archive members. */
        virtual void Reset()
        {
            ArIsLoading = false;
            ArIsSaving = false;
            ArIsTransacting = false;
            ArIsTextFormat = false;
            ArWantBinaryPropertySerialization = false;
            ArForceUnicode = false;
            ArIsPersistent = false;
            ArIsError = false;
            ArIsCriticalError = false;
            ArContainsCode = false;
            ArContainsMap = false;
            ArRequiresLocalizationGather = false;
            ArForceByteSwapping = false;
            ArIgnoreArchetypeRef = false;
            ArNoDelta = false;
            ArNoIntraPropertyDelta = false;
            ArIgnoreOuterRef = false;
            ArIgnoreClassGeneratedByRef = false;
            ArIgnoreClassRef = false;
            ArAllowLazyLoading = false;
            ArIsObjectReferenceCollector = false;
            ArIsModifyingWeakAndStrongReferences = false;
            ArIsCountingMemory = false;
            ArShouldSkipBulkData = false;
            ArIsFilterEditorOnly = false;
            ArIsSaveGame = false;
            ArIsNetArchive = false;
            ArSerializingDefaults = 0;
            ArPortFlags = 0;
            ArMaxSerializeSize = 0;
        }

        // ========================================================================
        // State Setter Methods
        // ========================================================================

        /** Sets whether this archive is for loading data. */
        virtual void SetIsLoading(bool bInIsLoading) { ArIsLoading = bInIsLoading; }

        /** Sets whether this archive is for saving data. */
        virtual void SetIsSaving(bool bInIsSaving) { ArIsSaving = bInIsSaving; }

        /** Sets whether this archive is for transacting. */
        virtual void SetIsTransacting(bool bInIsTransacting) { ArIsTransacting = bInIsTransacting; }

        /** Sets whether this archive is in text format. */
        virtual void SetIsTextFormat(bool bInIsTextFormat) { ArIsTextFormat = bInIsTextFormat; }

        /** Sets whether this archive wants binary property serialization. */
        virtual void SetWantBinaryPropertySerialization(bool bInWantBinary) { ArWantBinaryPropertySerialization = bInWantBinary; }

        /** Sets whether this archive wants to force saving as Unicode. */
        virtual void SetForceUnicode(bool bInForceUnicode) { ArForceUnicode = bInForceUnicode; }

        /** Sets whether this archive is to persistent storage. */
        virtual void SetIsPersistent(bool bInIsPersistent) { ArIsPersistent = bInIsPersistent; }

    // These will be protected in FArchive but accessible via using declarations
    protected:
        /** Whether this archive is for loading data. */
        u8 ArIsLoading : 1 = false;

        /** Whether this archive is for saving data. */
        u8 ArIsSaving : 1 = false;

        /** Whether archive is transacting (undo/redo). */
        u8 ArIsTransacting : 1 = false;

        /** Whether this archive serializes to a text format. */
        u8 ArIsTextFormat : 1 = false;

        /** Whether this archive wants properties to be serialized in binary form. */
        u8 ArWantBinaryPropertySerialization : 1 = false;

        /** Whether this archive wants to always save strings in UTF16 format. */
        u8 ArForceUnicode : 1 = false;

        /** Whether this archive saves to persistent storage. */
        u8 ArIsPersistent : 1 = false;

    private:
        /** Whether this archive contains errors. */
        u8 ArIsError : 1 = false;

        /** Whether this archive contains critical errors that cannot be recovered from. */
        u8 ArIsCriticalError : 1 = false;

    public:
        /** Quickly tell if an archive contains script code. */
        u8 ArContainsCode : 1 = false;

        /** Used to determine whether FArchive contains a level or world. */
        u8 ArContainsMap : 1 = false;

        /** Used to determine whether FArchive contains data required to be gathered for localization. */
        u8 ArRequiresLocalizationGather : 1 = false;

        /** Whether we should forcefully swap bytes. */
        u8 ArForceByteSwapping : 1 = false;

        /** If true, we will not serialize archetype references for structs and classes. */
        u8 ArIgnoreArchetypeRef : 1 = false;

        /** If true, do not perform delta serialization of properties. */
        u8 ArNoDelta : 1 = false;

        /** If true, do not perform delta serialization within properties (e.g. TMaps and TSets). */
        u8 ArNoIntraPropertyDelta : 1 = false;

        /** If true, we will not serialize the Outer reference in UObject. */
        u8 ArIgnoreOuterRef : 1 = false;

        /** If true, we will not serialize ClassGeneratedBy reference in UClass. */
        u8 ArIgnoreClassGeneratedByRef : 1 = false;

        /** If true, UObject::Serialize will skip serialization of the Class property. */
        u8 ArIgnoreClassRef : 1 = false;

        /** Whether to allow lazy loading of bulk/secondary data. */
        u8 ArAllowLazyLoading : 1 = false;

        /** Whether this archive only cares about serializing object references. */
        u8 ArIsObjectReferenceCollector : 1 = false;

        /** Whether a reference collector is modifying the references and wants both weak and strong ones. */
        u8 ArIsModifyingWeakAndStrongReferences : 1 = false;

        /** Whether this archive is counting memory. */
        u8 ArIsCountingMemory : 1 = false;

        /** Whether bulk data serialization should be skipped or not. */
        u8 ArShouldSkipBulkData : 1 = false;

        /** Whether editor only properties are being filtered from the archive. */
        u8 ArIsFilterEditorOnly : 1 = false;

        /** Whether this archive is saving/loading game state. */
        u8 ArIsSaveGame : 1 = false;

        /** Whether or not this archive is sending/receiving network data. */
        u8 ArIsNetArchive : 1 = false;

        /** Whether we are currently serializing defaults. > 0 means yes, <= 0 means no. */
        i32 ArSerializingDefaults = 0;

        /** Modifier flags that be used when serializing UProperties. */
        u32 ArPortFlags = 0;

        /** Max size of data that this archive is allowed to serialize. */
        i64 ArMaxSerializeSize = 0;
    };

    // ============================================================================
    // FArchive - Base Archive Class
    // ============================================================================

    /**
     * @class FArchive
     * @brief Base class for archives that can be used for loading, saving, and garbage
     *        collecting in a byte order neutral way.
     * 
     * Archives handle reading/writing data in a platform-independent way.
     * Derived classes implement specific storage backends (file, memory, network, etc.)
     * 
     * Ported from UE's FArchive - privately inherits FArchiveState and exposes
     * functionality via using declarations (matching UE's pattern).
     */
    class FArchive : private FArchiveState
    {
    public:
        FArchive() = default;
        FArchive(const FArchive&) = default;
        FArchive& operator=(const FArchive& ArchiveToCopy) = default;
        virtual ~FArchive() = default;

        // ========================================================================
        // Using declarations to expose FArchiveState functionality
        // ========================================================================

        using FArchiveState::SetArchiveState;
        using FArchiveState::SetError;
        using FArchiveState::ClearError;
        using FArchiveState::SetCriticalError;
        using FArchiveState::GetInnermostState;
        using FArchiveState::CountBytes;
        using FArchiveState::GetArchiveName;
        using FArchiveState::Tell;
        using FArchiveState::TotalSize;
        using FArchiveState::AtEnd;
        using FArchiveState::IsByteSwapping;
        using FArchiveState::ThisContainsCode;
        using FArchiveState::ThisContainsMap;
        using FArchiveState::ThisRequiresLocalizationGather;
        using FArchiveState::IsLoading;
        using FArchiveState::IsSaving;
        using FArchiveState::IsTransacting;
        using FArchiveState::IsTextFormat;
        using FArchiveState::WantBinaryPropertySerialization;
        using FArchiveState::IsForcingUnicode;
        using FArchiveState::IsPersistent;
        using FArchiveState::IsError;
        using FArchiveState::GetError;
        using FArchiveState::IsCriticalError;
        using FArchiveState::ContainsCode;
        using FArchiveState::ContainsMap;
        using FArchiveState::RequiresLocalizationGather;
        using FArchiveState::ForceByteSwapping;
        using FArchiveState::IsSerializingDefaults;
        using FArchiveState::IsIgnoringArchetypeRef;
        using FArchiveState::DoDelta;
        using FArchiveState::DoIntraPropertyDelta;
        using FArchiveState::IsIgnoringOuterRef;
        using FArchiveState::IsIgnoringClassGeneratedByRef;
        using FArchiveState::IsIgnoringClassRef;
        using FArchiveState::IsAllowingLazyLoading;
        using FArchiveState::IsObjectReferenceCollector;
        using FArchiveState::IsModifyingWeakAndStrongReferences;
        using FArchiveState::IsCountingMemory;
        using FArchiveState::GetPortFlags;
        using FArchiveState::HasAnyPortFlags;
        using FArchiveState::HasAllPortFlags;
        using FArchiveState::ShouldSkipBulkData;
        using FArchiveState::GetMaxSerializeSize;
        using FArchiveState::IsFilterEditorOnly;
        using FArchiveState::SetFilterEditorOnly;
        using FArchiveState::IsSaveGame;
        using FArchiveState::IsNetArchive;
        using FArchiveState::SetByteSwapping;
        using FArchiveState::SetPortFlags;
        using FArchiveState::UseToResolveEnumerators;
        using FArchiveState::Reset;
        using FArchiveState::SetIsLoading;
        using FArchiveState::SetIsSaving;
        using FArchiveState::SetIsTransacting;
        using FArchiveState::SetIsTextFormat;
        using FArchiveState::SetWantBinaryPropertySerialization;
        using FArchiveState::SetForceUnicode;
        using FArchiveState::SetIsPersistent;

        // Expose public flags
        using FArchiveState::ArContainsCode;
        using FArchiveState::ArContainsMap;
        using FArchiveState::ArRequiresLocalizationGather;
        using FArchiveState::ArForceByteSwapping;
        using FArchiveState::ArIgnoreArchetypeRef;
        using FArchiveState::ArNoDelta;
        using FArchiveState::ArNoIntraPropertyDelta;
        using FArchiveState::ArIgnoreOuterRef;
        using FArchiveState::ArIgnoreClassGeneratedByRef;
        using FArchiveState::ArIgnoreClassRef;
        using FArchiveState::ArAllowLazyLoading;
        using FArchiveState::ArIsObjectReferenceCollector;
        using FArchiveState::ArIsModifyingWeakAndStrongReferences;
        using FArchiveState::ArIsCountingMemory;
        using FArchiveState::ArShouldSkipBulkData;
        using FArchiveState::ArIsFilterEditorOnly;
        using FArchiveState::ArIsSaveGame;
        using FArchiveState::ArIsNetArchive;
        using FArchiveState::ArSerializingDefaults;
        using FArchiveState::ArPortFlags;
        using FArchiveState::ArMaxSerializeSize;

        // ========================================================================
        // Archive State Access
        // ========================================================================

        /** Returns the low level archive state for this archive. */
        FArchiveState& GetArchiveState() { return static_cast<FArchiveState&>(*this); }
        const FArchiveState& GetArchiveState() const { return static_cast<const FArchiveState&>(*this); }

        // ========================================================================
        // Position / Size / Serialization
        // ========================================================================

        /** Attempts to set the current offset into backing data storage. */
        virtual void Seek(i64 InPos) { }

        /** Serialize raw bytes */
        virtual void Serialize(void* V, i64 Length)
        {
            // Base class does nothing - derived classes implement actual serialization
        }

        /** Serialize bits */
        virtual void SerializeBits(void* V, i64 LengthBits)
        {
            Serialize(V, (LengthBits + 7) / 8);

            if (IsLoading() && (LengthBits % 8) != 0)
            {
                reinterpret_cast<u8*>(V)[LengthBits / 8] &= ((1 << (LengthBits & 7)) - 1);
            }
        }

        /** Serialize an int value with a max bound */
        virtual void SerializeInt(u32& Value, u32 Max)
        {
            ByteOrderSerialize(Value);
        }

        /** Packs int value into bytes of 7 bits with 8th bit for 'more' */
        virtual void SerializeIntPacked(u32& Value)
        {
            if (IsLoading())
            {
                // Set the byte cap to 10 in order to handle serialized 64-bit integers whose type changed to 32-bit.
                constexpr sizet MaxBytes = 10;

                u32 TempValue = 0;
                u8 Count = 0;
                u8 More = 1;
                while (More)
                {
                    if (Count >= MaxBytes)
                    {
                        // Too many bytes - corrupted data
                        SetError();
                        Value = 0;
                        return;
                    }

                    u8 NextByte;
                    Serialize(&NextByte, 1);             // Read next byte
                    More = NextByte & 1;                 // Check 1 bit to see if there's more after this
                    NextByte = NextByte >> 1;            // Shift to get actual 7 bit value
                    TempValue += static_cast<u32>(NextByte) << (7 * Count++);
                }

                Value = TempValue;
            }
            else
            {
                constexpr sizet MaxBytes = 5;

                u8 PackedBytes[MaxBytes];
                i32 PackedByteCount = 0;
                u32 Remaining = Value;
                while (true)
                {
                    u8 NextByte = Remaining & 0x7f;      // Get next 7 bits to write
                    Remaining = Remaining >> 7;          // Update remaining
                    NextByte = NextByte << 1;            // Make room for 'more' bit
                    if (Remaining > 0)
                    {
                        NextByte |= 1;                   // Set more bit
                        PackedBytes[PackedByteCount++] = NextByte;
                    }
                    else
                    {
                        PackedBytes[PackedByteCount++] = NextByte;
                        break;
                    }
                }
                Serialize(PackedBytes, PackedByteCount); // Actually serialize the bytes we made
            }
        }

    /** Packs 64-bit int value into bytes of 7 bits with 8th bit for 'more' */
    virtual void SerializeIntPacked64(u64& Value)
    {
        constexpr sizet MaxBytes = 10;

        if (IsLoading())
        {
            u64 TempValue = 0;
            u8 Count = 0;
            u8 More = 1;
            while (More)
            {
                if (Count >= MaxBytes)
                {
                    // Too many bytes - corrupted data
                    SetError();
                    Value = 0;
                    return;
                }

                u8 NextByte;
                Serialize(&NextByte, 1);             // Read next byte
                More = NextByte & 1;                 // Check 1 bit to see if there's more after this
                NextByte = NextByte >> 1;            // Shift to get actual 7 bit value
                TempValue += static_cast<u64>(NextByte) << (7 * Count++);
            }

            Value = TempValue;
        }
        else
        {
            u8 PackedBytes[MaxBytes];
            i32 PackedByteCount = 0;
            u64 Remaining = Value;
            while (true)
            {
                u8 NextByte = Remaining & 0x7f;      // Get next 7 bits to write
                Remaining = Remaining >> 7;          // Update remaining
                NextByte = NextByte << 1;            // Make room for 'more' bit
                if (Remaining > 0)
                {
                    NextByte |= 1;                   // Set more bit
                    PackedBytes[PackedByteCount++] = NextByte;
                }
                else
                {
                    PackedBytes[PackedByteCount++] = NextByte;
                    break;
                }
            }
            Serialize(PackedBytes, PackedByteCount); // Actually serialize the bytes we made
        }
    }

    /** Sets a flag indicating that this archive is currently serializing class/struct defaults. */
        void StartSerializingDefaults() { ArSerializingDefaults++; }

        /** Indicate that this archive is no longer serializing class/struct defaults. */
        void StopSerializingDefaults() { ArSerializingDefaults--; }

        /** Flushes cache and frees internal data. */
        virtual void FlushCache() { }

        /** Attempts to finish writing any buffered data to disk/permanent storage. */
        virtual void Flush() { }

        /** Attempts to close and finalize any handles used for backing data storage. */
        virtual bool Close() { return !IsError(); }

        // ========================================================================
        // Byte Order Serialization
        // ========================================================================

        /** Used internally only to do byte swapping on small items. */
        void ByteSwap(void* V, i32 Length)
        {
            u8* Ptr = static_cast<u8*>(V);
            i32 Top = Length - 1;
            i32 Bottom = 0;
            while (Bottom < Top)
            {
                u8 Temp = Ptr[Top];
                Ptr[Top] = Ptr[Bottom];
                Ptr[Bottom] = Temp;
                ++Bottom;
                --Top;
            }
        }

        /** Serialize data of Length bytes, taking into account byte swapping if needed. */
        FArchive& ByteOrderSerialize(void* V, i32 Length)
        {
            if (!IsByteSwapping()) // Most likely case (hot path)
            {
                Serialize(V, Length);
                return *this;
            }
            return SerializeByteOrderSwapped(V, Length);
        }

        // ========================================================================
        // Primitive Type Serialization (Friend Functions - matches UE pattern)
        // ========================================================================

        /** Serializes an unsigned 8-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, u8& Value)
        {
            Ar.Serialize(&Value, 1);
            return Ar;
        }

        /** Serializes a signed 8-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, i8& Value)
        {
            Ar.Serialize(&Value, 1);
            return Ar;
        }

        /** Serializes an unsigned 16-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, u16& Value)
        {
            Ar.ByteOrderSerialize(Value);
            return Ar;
        }

        /** Serializes a signed 16-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, i16& Value)
        {
            Ar.ByteOrderSerialize(reinterpret_cast<u16&>(Value));
            return Ar;
        }

        /** Serializes an unsigned 32-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, u32& Value)
        {
            Ar.ByteOrderSerialize(Value);
            return Ar;
        }

        /** Serializes a signed 32-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, i32& Value)
        {
            Ar.ByteOrderSerialize(reinterpret_cast<u32&>(Value));
            return Ar;
        }

        /** Serializes an unsigned 64-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, u64& Value)
        {
            Ar.ByteOrderSerialize(Value);
            return Ar;
        }

        /** Serializes a signed 64-bit integer value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, i64& Value)
        {
            Ar.ByteOrderSerialize(reinterpret_cast<u64&>(Value));
            return Ar;
        }

        /** Serializes a single precision floating point value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, f32& Value)
        {
            static_assert(sizeof(f32) == sizeof(u32), "Expected float to be 4 bytes to swap as uint32");
            Ar.ByteOrderSerialize(reinterpret_cast<u32&>(Value));
            return Ar;
        }

        /** Serializes a double precision floating point value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, f64& Value)
        {
            static_assert(sizeof(f64) == sizeof(u64), "Expected double to be 8 bytes to swap as uint64");
            Ar.ByteOrderSerialize(reinterpret_cast<u64&>(Value));
            return Ar;
        }

        /** Serializes a Boolean value from or into an archive. */
        friend FArchive& operator<<(FArchive& Ar, bool& D)
        {
            // Serialize bool as if it were UBOOL (legacy, 32 bit int).
            u32 OldUBoolValue = 0;

            if (!Ar.IsLoading())
            {
                OldUBoolValue = D ? 1 : 0;
            }

            Ar.Serialize(&OldUBoolValue, sizeof(OldUBoolValue));

            if (OldUBoolValue > 1)
            {
                Ar.SetError();
            }

            if (Ar.IsLoading())
            {
                D = !!OldUBoolValue;
            }

            return Ar;
        }

        /** Serializes a std::string */
        friend FArchive& operator<<(FArchive& Ar, std::string& Value)
        {
            i32 Length = static_cast<i32>(Value.length());
            Ar << Length;
            
            if (Ar.IsLoading())
            {
                // Validate length to prevent corrupted/malicious data from causing issues
                if (Length < 0)
                {
                    Ar.SetError();
                    Value.clear();
                    return Ar;
                }
                
                Value.resize(Length);
            }
            
            if (Length > 0)
            {
                Ar.Serialize(Value.data(), Length);
            }
            
            return Ar;
        }

        /** 
         * Serializes enum classes as their underlying type.
         */
        template <typename EnumType>
            requires std::is_enum_v<EnumType>
        friend FArchive& operator<<(FArchive& Ar, EnumType& Value)
        {
            return Ar << reinterpret_cast<std::underlying_type_t<EnumType>&>(Value);
        }

        // ========================================================================
        // Scoped Helper Classes
        // ========================================================================

        /** Seeks to and restores the position of an archive. */
        class FScopeSeekTo
        {
        public:
            FScopeSeekTo(FArchive& InAr, i64 InPos)
                : Ar(InAr)
                , SavedPos(InAr.Tell())
            {
                Ar.Seek(InPos);
            }
            
            ~FScopeSeekTo()
            {
                Ar.Seek(SavedPos);
            }

            FScopeSeekTo(const FScopeSeekTo&) = delete;
            FScopeSeekTo& operator=(const FScopeSeekTo&) = delete;

        private:
            FArchive& Ar;
            i64 SavedPos;
        };

    private:
        // ========================================================================
        // Private ByteOrderSerialize Implementations
        // ========================================================================

        /** Used internally only to control the amount of generated code/type under control. */
        template<typename T>
        FArchive& ByteOrderSerialize(T& Value)
        {
            static_assert(std::is_unsigned_v<T>, 
                "To reduce the number of template instances, cast 'Value' to a u16&, u32& or u64& prior to the call.");

            if (!IsByteSwapping()) // Most likely case (hot path)
            {
                Serialize(&Value, sizeof(T));
                return *this;
            }
            return SerializeByteOrderSwapped(Value);
        }

        /** Not inlined to keep ByteOrderSerialize() small and fast. */
        FArchive& SerializeByteOrderSwapped(void* V, i32 Length)
        {
            Serialize(V, Length);
            ByteSwap(V, Length);
            return *this;
        }

        FArchive& SerializeByteOrderSwapped(u16& Value)
        {
            Serialize(&Value, sizeof(Value));
            Value = (Value >> 8) | (Value << 8);
            return *this;
        }

        FArchive& SerializeByteOrderSwapped(u32& Value)
        {
            Serialize(&Value, sizeof(Value));
            Value = ((Value >> 24) & 0x000000FF) |
                    ((Value >> 8)  & 0x0000FF00) |
                    ((Value << 8)  & 0x00FF0000) |
                    ((Value << 24) & 0xFF000000);
            return *this;
        }

        FArchive& SerializeByteOrderSwapped(u64& Value)
        {
            Serialize(&Value, sizeof(Value));
            Value = ((Value >> 56) & 0x00000000000000FFULL) |
                    ((Value >> 40) & 0x000000000000FF00ULL) |
                    ((Value >> 24) & 0x0000000000FF0000ULL) |
                    ((Value >> 8)  & 0x00000000FF000000ULL) |
                    ((Value << 8)  & 0x000000FF00000000ULL) |
                    ((Value << 24) & 0x0000FF0000000000ULL) |
                    ((Value << 40) & 0x00FF000000000000ULL) |
                    ((Value << 56) & 0xFF00000000000000ULL);
            return *this;
        }
    };

    // ============================================================================
    // Template for archive constructors
    // ============================================================================

    /** Template for archive constructors - serializes and returns the value. */
    template<class T>
    T Arctor(FArchive& Ar)
    {
        T Tmp;
        Ar << Tmp;
        return Tmp;
    }

    // ============================================================================
    // FMemoryArchive - Memory-backed Archive Base
    // ============================================================================

    /**
     * @class FMemoryArchive
     * @brief Archive that reads/writes to memory
     * 
     * Ported from UE's FMemoryArchive - base class for FMemoryReader and FMemoryWriter.
     */
    class FMemoryArchive : public FArchive
    {
    public:
        /**
         * Returns the name of the Archive.
         */
        [[nodiscard]] std::string GetArchiveName() const override { return "FMemoryArchive"; }

        void Seek(i64 InPos) override
        {
            Offset = InPos;
        }

        [[nodiscard]] i64 Tell() override
        {
            return Offset;
        }

    protected:
        /** Current offset into the data */
        i64 Offset = 0;
    };

    // ============================================================================
    // FMemoryReader - Read from memory buffer
    // ============================================================================

    /**
     * @class FMemoryReader
     * @brief Archive for reading from a memory buffer
     * 
     * Ported from UE's FMemoryReader - reads from a const TArray<uint8>&
     */
    class FMemoryReader : public FMemoryArchive
    {
    public:
        /**
         * Constructor
         * 
         * @param InBytes The buffer to read from
         * @param bIsPersistent Whether this archive is persistent
         * 
         * @note The caller must guarantee that InBytes remains valid for the lifetime of this FMemoryReader.
         *       This archive stores a pointer to the data and does not take ownership.
         */
        explicit FMemoryReader(const std::vector<u8>& InBytes, bool bIsPersistent = false)
            : Bytes(InBytes.data())
            , NumBytes(static_cast<i64>(InBytes.size()))
            , LimitSize(static_cast<i64>(InBytes.size()))
        {
            SetIsLoading(true);
            SetIsPersistent(bIsPersistent);
        }

        /**
         * Constructor with raw pointer
         * 
         * @param InBytes Pointer to buffer
         * @param InNumBytes Size of buffer
         * @param bIsPersistent Whether this archive is persistent
         * 
         * @note The caller must guarantee that the buffer at InBytes remains valid for the lifetime of this FMemoryReader.
         */
        FMemoryReader(const u8* InBytes, i64 InNumBytes, bool bIsPersistent = false)
            : Bytes(InBytes)
            , NumBytes(InNumBytes)
            , LimitSize(InNumBytes)
        {
            SetIsLoading(true);
            SetIsPersistent(bIsPersistent);
        }

        [[nodiscard]] i64 TotalSize() override
        {
            return LimitSize;
        }

        void Serialize(void* Data, i64 Num) override
        {
            if (Num > 0 && !IsError())
            {
                if (Offset + Num <= LimitSize)
                {
                    std::memcpy(Data, Bytes + Offset, static_cast<sizet>(Num));
                    Offset += Num;
                }
                else
                {
                    SetError();
                }
            }
        }

        [[nodiscard]] std::string GetArchiveName() const override { return "FMemoryReader"; }

        /**
         * Set a limit on how much of the buffer can be read
         * @param NewLimitSize The new limit size
         */
        void SetLimitSize(i64 NewLimitSize)
        {
            if (NewLimitSize <= NumBytes)
            {
                LimitSize = NewLimitSize;
            }
        }

    protected:
        const u8* Bytes;
        i64 NumBytes;
        i64 LimitSize;
    };

    // ============================================================================
    // FMemoryReaderView - Read from memory view (non-owning)
    // ============================================================================

    /**
     * @class FMemoryReaderView
     * @brief Archive for reading from a memory view that doesn't own the data
     * 
     * Similar to FMemoryReader but using a span-like interface.
     */
    using FMemoryReaderView = FMemoryReader;

    // ============================================================================
    // FMemoryWriter - Write to memory buffer
    // ============================================================================

    /**
     * @class FMemoryWriter
     * @brief Archive for writing to a memory buffer
     * 
     * Ported from UE's FMemoryWriter - writes to a TArray<uint8>&
     */
    class FMemoryWriter : public FMemoryArchive
    {
    public:
        /**
         * Constructor
         * 
         * @param InBytes The buffer to write to
         * @param bIsPersistent Whether this archive is persistent
         * @param bSetOffset Whether to set offset to the end of the buffer
         * @param Filename Optional filename for identification
         */
        explicit FMemoryWriter(std::vector<u8>& InBytes, bool bIsPersistent = false, bool bSetOffset = false, [[maybe_unused]] const std::string& Filename = "")
            : Bytes(InBytes)
        {
            SetIsSaving(true);
            SetIsPersistent(bIsPersistent);
            if (bSetOffset)
            {
                Offset = static_cast<i64>(InBytes.size());
            }
        }

        [[nodiscard]] i64 TotalSize() override
        {
            return static_cast<i64>(Bytes.size());
        }

        void Seek(i64 InPos) override
        {
            // Allow seeking past the end - will extend buffer on next write
            Offset = InPos;
        }

        void Serialize(void* Data, i64 Num) override
        {
            if (Num > 0)
            {
                const i64 NumBytesToAdd = Offset + Num - static_cast<i64>(Bytes.size());
                if (NumBytesToAdd > 0)
                {
                    Bytes.resize(Bytes.size() + static_cast<sizet>(NumBytesToAdd));
                }
                
                if (Num > 0)
                {
                    std::memcpy(Bytes.data() + Offset, Data, static_cast<sizet>(Num));
                    Offset += Num;
                }
            }
        }

        [[nodiscard]] std::string GetArchiveName() const override { return "FMemoryWriter"; }

        /**
         * Returns the written bytes
         */
        [[nodiscard]] const std::vector<u8>& GetWrittenBytes() const { return Bytes; }

    protected:
        std::vector<u8>& Bytes;
    };

    // ============================================================================
    // FStructuredArchive - Structured (hierarchical) Archive
    // ============================================================================

    /**
     * @class FStructuredArchive
     * @brief Archive supporting structured/hierarchical serialization
     * 
     * Provides a way to serialize data in a structured format (like JSON/XML)
     * rather than as a flat binary stream. This is useful for debugging
     * and for formats that need named fields.
     * 
     * Ported from UE's FStructuredArchive
     */
    class FStructuredArchive
    {
    public:
        class FSlot;
        class FRecord;
        class FArray;
        class FStream;
        class FMap;

        explicit FStructuredArchive(FArchive& InArchive)
            : UnderlyingArchive(InArchive)
        {
        }

        /** @return the underlying FArchive */
        [[nodiscard]] FArchive& GetUnderlyingArchive() { return UnderlyingArchive; }
        [[nodiscard]] const FArchive& GetUnderlyingArchive() const { return UnderlyingArchive; }

        /** Get the root slot */
        [[nodiscard]] FSlot GetSlot();

        /**
         * @class FSlot
         * @brief A slot in a structured archive that can hold a value
         */
        class FSlot
        {
        public:
            explicit FSlot(FStructuredArchive& InArchive)
                : Archive(InArchive)
            {
            }

            /** @return the underlying archive */
            [[nodiscard]] FArchive& GetUnderlyingArchive() { return Archive.GetUnderlyingArchive(); }

            /** Enter an array with the given element count */
            [[nodiscard]] FArray EnterArray(i32& NumElements);

            /** Enter a record */
            [[nodiscard]] FRecord EnterRecord();

            /** Serialize a value directly */
            template <typename T>
            FSlot& operator<<(T& Value)
            {
                Archive.GetUnderlyingArchive() << Value;
                return *this;
            }

        private:
            FStructuredArchive& Archive;
        };

        /**
         * @class FArray
         * @brief An array in a structured archive
         */
        class FArray
        {
        public:
            FArray(FStructuredArchive& InArchive, i32 InNumElements)
                : Archive(InArchive)
                , NumElements(InNumElements)
                , CurrentIndex(0)
            {
            }

            /** Enter an element slot */
            [[nodiscard]] FSlot EnterElement()
            {
                ++CurrentIndex;
                return FSlot(Archive);
            }

        private:
            FStructuredArchive& Archive;
            i32 NumElements;
            i32 CurrentIndex;
        };

        /**
         * @class FRecord
         * @brief A record (object) in a structured archive
         */
        class FRecord
        {
        public:
            explicit FRecord(FStructuredArchive& InArchive)
                : Archive(InArchive)
            {
            }

            /** Enter a named field slot */
            [[nodiscard]] FSlot EnterField([[maybe_unused]] const char* Name)
            {
                return FSlot(Archive);
            }

            /** @return the underlying archive */
            [[nodiscard]] FArchive& GetUnderlyingArchive() { return Archive.GetUnderlyingArchive(); }

        private:
            FStructuredArchive& Archive;
        };

        /**
         * @class FStream
         * @brief A stream in a structured archive for sequential access
         */
        class FStream
        {
        public:
            explicit FStream(FStructuredArchive& InArchive)
                : Archive(InArchive)
            {
            }

            /** Enter an element slot */
            [[nodiscard]] FSlot EnterElement()
            {
                return FSlot(Archive);
            }

        private:
            FStructuredArchive& Archive;
        };

        /**
         * @class FMap
         * @brief A map in a structured archive
         */
        class FMap
        {
        public:
            FMap(FStructuredArchive& InArchive, i32 InNumElements)
                : Archive(InArchive)
                , NumElements(InNumElements)
            {
            }

            /** Enter an element with key */
            template<typename KeyType>
            [[nodiscard]] FRecord EnterElement(KeyType& Key)
            {
                Archive.GetUnderlyingArchive() << Key;
                return FRecord(Archive);
            }

        private:
            FStructuredArchive& Archive;
            i32 NumElements;
        };

    private:
        FArchive& UnderlyingArchive;
    };

    inline FStructuredArchive::FSlot FStructuredArchive::GetSlot()
    {
        return FSlot(*this);
    }

    inline FStructuredArchive::FArray FStructuredArchive::FSlot::EnterArray(i32& NumElements)
    {
        Archive.GetUnderlyingArchive() << NumElements;
        return FArray(Archive, NumElements);
    }

    inline FStructuredArchive::FRecord FStructuredArchive::FSlot::EnterRecord()
    {
        return FRecord(Archive);
    }

    // ============================================================================
    // Memory Image Support (Stubs for API compatibility)
    // ============================================================================

    /**
     * @struct FTypeLayoutDesc
     * @brief Describes the layout of a type for memory image serialization
     */
    struct FTypeLayoutDesc
    {
        sizet Size = 0;
        sizet Alignment = 0;
        const char* Name = nullptr;
    };

    /**
     * @struct FPlatformTypeLayoutParameters
     * @brief Platform-specific layout parameters
     */
    struct FPlatformTypeLayoutParameters
    {
        bool Is32BitTarget = false;
    };

    /**
     * @class FMemoryImageWriter
     * @brief Writer for memory image serialization (cooked data)
     */
    class FMemoryImageWriter
    {
    public:
        [[nodiscard]] bool Is32BitTarget() const { return bIs32BitTarget; }

        void WriteBytes([[maybe_unused]] const void* Data, [[maybe_unused]] sizet Size)
        {
            // Stub implementation
        }

        template <typename T>
        void WriteBytes(const T& Value)
        {
            WriteBytes(&Value, sizeof(T));
        }

        FMemoryImageWriter WritePointer([[maybe_unused]] const FTypeLayoutDesc& TypeDesc)
        {
            // Stub - return a copy for nested writes
            return *this;
        }

        void WriteAlignment([[maybe_unused]] sizet Alignment) {}
        
        template <typename T>
        u32 WriteAlignment()
        {
            WriteAlignment(alignof(T));
            return 0; // Return current offset (stub)
        }
        
        void WritePaddingToSize([[maybe_unused]] sizet Size) {}

        void WriteObject([[maybe_unused]] const void* Data, [[maybe_unused]] const FTypeLayoutDesc& TypeDesc)
        {
            // Stub implementation
        }

        void WriteNullPointer()
        {
            // Stub implementation
        }

        template <typename T>
        void WriteObjectArray([[maybe_unused]] const T* Data, [[maybe_unused]] const FTypeLayoutDesc& TypeDesc, [[maybe_unused]] i32 Count)
        {
            // Stub implementation
        }

    private:
        bool bIs32BitTarget = false;
    };

    /**
     * @struct FMemoryUnfreezeContent
     * @brief Context for unfreezing memory images
     */
    struct FMemoryUnfreezeContent
    {
        template <typename T>
        void UnfreezeObject([[maybe_unused]] const T* Src, [[maybe_unused]] const FTypeLayoutDesc& TypeDesc, T* Dst) const
        {
            // Stub - just copy
            *Dst = *Src;
        }
    };

    /**
     * @class FSHA1
     * @brief SHA1 hash computation
     */
    class FSHA1
    {
    public:
        void Update([[maybe_unused]] const void* Data, [[maybe_unused]] sizet Size) {}
        void Final() {}
        void GetHash([[maybe_unused]] u8 OutHash[20]) {}
    };

    // ============================================================================
    // Type Layout Helpers
    // ============================================================================

    // Note: THasTypeLayout is defined in MemoryLayout.h

    /**
     * @brief Get the type layout descriptor for a type
     */
    template <typename T>
    [[nodiscard]] inline const FTypeLayoutDesc& StaticGetTypeLayoutDesc()
    {
        static FTypeLayoutDesc Desc{ sizeof(T), alignof(T), nullptr };
        return Desc;
    }

    // ============================================================================
    // Freeze Namespace Helpers
    // ============================================================================

    namespace Freeze
    {
        template <typename T>
        void AppendHash([[maybe_unused]] const FTypeLayoutDesc& TypeDesc, [[maybe_unused]] const FPlatformTypeLayoutParameters& Params, [[maybe_unused]] FSHA1& Hasher)
        {
            // Stub
        }

        // Note: UE's version has this as a template but template argument T is never used
        // Making this non-template to avoid deduction issues
        inline u32 DefaultAppendHash([[maybe_unused]] const FTypeLayoutDesc& TypeDesc, [[maybe_unused]] const FPlatformTypeLayoutParameters& Params, [[maybe_unused]] FSHA1& Hasher)
        {
            return 0;
        }
    }

} // namespace OloEngine
