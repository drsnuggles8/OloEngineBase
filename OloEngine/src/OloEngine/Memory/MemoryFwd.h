// MemoryFwd.h - Forward declarations for memory types
// Ported from UE5.7 Memory/MemoryFwd.h

#pragma once

namespace OloEngine
{

// MemoryView

template <typename DataType>
class TMemoryView;

/** A non-owning view of a contiguous region of memory. */
using FMemoryView = TMemoryView<const void>;

/** A non-owning mutable view of a contiguous region of memory. */
using FMutableMemoryView = TMemoryView<void>;

// SharedBuffer
class FBufferOwner;
class FUniqueBuffer;
class FSharedBuffer;
class FWeakSharedBuffer;

// CompositeBuffer
class FCompositeBuffer;

} // namespace OloEngine
