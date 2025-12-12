---
applyTo: "OloEngine/src/OloEngine/Memory/**"
---

# Memory System Porting Instructions (UE5 → OloEngine)

When working on files in the Memory folder, these are being ported from Unreal Engine 5's memory system. Follow these conventions:

## Type Substitutions

When porting UE5 code, replace these types:
- `SIZE_T` → `sizet`
- `uint64` → `u64`
- `uint32` → `u32`
- `uint16` → `u16`
- `uint8` → `u8`
- `int64` → `i64`
- `int32` → `i32`
- `int16` → `i16`
- `int8` → `i8`
- `TCHAR*` → `char*`

## Thread-Safe Counter Substitutions

**IMPORTANT:** UE5 has deprecated `FThreadSafeCounter` and `FThreadSafeCounter64`. Replace with `std::atomic`:

- `FThreadSafeCounter` → `std::atomic<i32>`
- `FThreadSafeCounter64` → `std::atomic<i64>`

Method translations:
- `counter.Increment()` → `++atomic` (or `atomic.fetch_add(1) + 1`) - returns NEW value
- `counter.Decrement()` → `--atomic` (or `atomic.fetch_sub(1) - 1`) - returns NEW value
- `counter.Add(n)` → `atomic.fetch_add(n)` - returns OLD value
- `counter.Subtract(n)` → `atomic.fetch_sub(n)` - returns OLD value
- `counter.Set(v)` → `atomic.exchange(v)`
- `counter.Reset()` → `atomic.exchange(0)`
- `counter.GetValue()` → `atomic.load()`

## Macro Substitutions

- `UE_BUILD_DEBUG` → `OLO_DEBUG`
- `UE_BUILD_DEVELOPMENT` → `OLO_RELEASE`
- `UE_BUILD_SHIPPING` → `OLO_DIST`
- `check(x)` → `OLO_CORE_ASSERT(x)`
- `checkf(x, fmt, ...)` → `OLO_CORE_ASSERT(x, fmt, ...)`
- `ensure(x)` → `OLO_CORE_VERIFY(x)`
- `UE_LOG(Category, Level, ...)` → `OLO_CORE_INFO/WARN/ERROR(...)`
- `FORCEINLINE` → `OLO_FINLINE`
- `FORCENOINLINE` → `OLO_NOINLINE`
- `UE_FORCEINLINE` → `OLO_FINLINE`
- `RESTRICT` → `OLO_RESTRICT`

## Features NOT Being Ported

Skip or stub out these UE5 features:
- AutoRTFM (transactional memory) - not needed
- LLM (Low Level Memory tracker) - use OloEngine's profiling instead
- FCoreDelegates - would require full delegate system
- Memory tracing with GMallocProfiler - defer to OloEngine profiling
- `IsInGameThread()` checks - stub or implement later

## Code Style

- Use `#pragma once` for include guards
- Place code in `namespace OloEngine`
- Use OloEngine naming: classes `PascalCase`, members `m_PascalCase`, statics `s_PascalCase`
- Use `Ref<T>` for smart pointers where appropriate
- Braces on new lines except trivial cases
