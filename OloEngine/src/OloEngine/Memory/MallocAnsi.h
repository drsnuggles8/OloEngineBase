// OloEngine Memory System
// Ported from Unreal Engine's HAL/MallocAnsi.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/MemoryBase.h"
#include "OloEngine/Memory/UnrealMemory.h"

namespace OloEngine
{

    [[nodiscard]] void* AnsiMalloc(sizet Size, u32 Alignment);
    [[nodiscard]] void* AnsiRealloc(void* Ptr, sizet NewSize, u32 Alignment);
    void AnsiFree(void* Ptr);

    //
    // ANSI C memory allocator.
    //
    class FMallocAnsi final : public FMalloc
    {
      public:
        // Constructor enabling low fragmentation heap on platforms supporting it.
        FMallocAnsi();

        // FMalloc interface.
        [[nodiscard]] void* Malloc(sizet Size, u32 Alignment) override;

        [[nodiscard]] void* TryMalloc(sizet Size, u32 Alignment) override;

        [[nodiscard]] void* Realloc(void* Ptr, sizet NewSize, u32 Alignment) override;

        [[nodiscard]] void* TryRealloc(void* Ptr, sizet NewSize, u32 Alignment) override;

        void Free(void* Ptr) override;

        bool GetAllocationSize(void* Original, sizet& SizeOut) override;

        // Returns if the allocator is guaranteed to be thread-safe and therefore
        // doesn't need an unnecessary thread-safety wrapper around it.
        //
        // @return true as we're using system allocator
        bool IsInternallyThreadSafe() const override;

        // Validates the allocator's heap
        bool ValidateHeap() override;

        const char* GetDescriptiveName() override
        {
            return "ANSI";
        }
    };

} // namespace OloEngine
