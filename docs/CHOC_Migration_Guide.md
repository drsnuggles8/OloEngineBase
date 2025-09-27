# CHOC Integration Migration Guide

This guide explains how to migrate OloEngine's SoundGraph system to use CHOC instead of custom Value/ValueView classes.

## Overview

We're replacing OloEngine's custom `Value`, `ValueView`, and `ValueType` classes with CHOC's battle-tested equivalents:

- `OloEngine::Audio::SoundGraph::Value` → `choc::value::Value`
- `OloEngine::Audio::SoundGraph::ValueView` → `choc::value::ValueView`  
- `OloEngine::Audio::SoundGraph::ValueType` → `choc::value::Type`

## Migration Steps

### Step 1: Update Includes

**Before:**
```cpp
#include "OloEngine/Audio/SoundGraph/Value.h"
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
```

**After:**
```cpp
#include "OloEngine/Audio/SoundGraph/CHOCIntegration.h"
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
```

### Step 2: Value Creation

**Before:**
```cpp
// Old OloEngine Value system
Value value = Value::CreateFloat32(42.0f);
ValueType type = ValueType::CreatePrimitive<f32>();
```

**After:**
```cpp
// CHOC Value system
Value value = CreateValue(42.0f);  // Helper function
// or directly:
Value value = choc::value::Value(42.0f);
ValueType type = GetValueType<f32>();  // Helper function
```

### Step 3: Value Access

**Before:**
```cpp
// Old system
if (value.GetType() == ValueType::Kind::Float32)
{
    f32 floatVal = value.Get<f32>();
}
```

**After:**
```cpp
// CHOC system
if (value.isFloat32())
{
    f32 floatVal = value.getFloat32();
}
// or with helper:
f32 floatVal = GetValueOr<f32>(value, 0.0f);
```

### Step 4: Array Handling

**Before:**
```cpp
// Old system
ValueType arrayType = ValueType::CreateArray<f32>(10);
```

**After:**
```cpp
// CHOC system
std::vector<f32> data = {1, 2, 3, 4, 5};
Value arrayValue = CreateArrayValue(data);
// or directly:
Value arrayValue = choc::value::Value::createArray(data);
```

### Step 5: Node Parameter Updates

**Before:**
```cpp
// In node classes
template<typename T>
void SetParameterValue(const Identifier& id, T value)
{
    // Custom parameter handling
}
```

**After:**
```cpp
// In node classes  
template<typename T>
void SetParameterValue(const Identifier& id, T value)
{
    Value chocValue = CreateValue(value);
    // Use CHOC value in parameter system
}
```

### Step 6: Thread Communication

**Before:**
```cpp
// Custom event queue (if any)
std::queue<GraphEvent> m_EventQueue;
```

**After:**
```cpp
// CHOC lock-free FIFO
EventQueue m_EventQueue;  // Uses choc::fifo::SingleReaderSingleWriterFIFO
MessageQueue m_MessageQueue;
ParameterQueue m_ParameterQueue;
```

### Step 7: Parameter Interpolation

**Before:**
```cpp
// No interpolation system
f32 parameter = newValue;
```

**After:**
```cpp
// CHOC-based interpolation
InterpolatedFloat m_FrequencyParam;
// In audio callback:
m_FrequencyParam.SetTarget(newFrequency, 480); // 10ms @ 48kHz
f32 currentFreq = m_FrequencyParam.Process();
```

## Key Benefits After Migration

1. **Real-Time Safety**: CHOC's lock-free FIFOs eliminate audio glitches
2. **Robust Type System**: Handles primitives, arrays, objects, and complex data
3. **Memory Management**: Pool allocators for real-time thread allocations
4. **JSON Serialization**: Built-in serialization for presets and state saving
5. **Production Tested**: Used in commercial audio software (Tracktion)
6. **No Maintenance**: Let Tracktion maintain the core audio infrastructure

## Node-Specific Changes

### AddNode Example

**Before:**
```cpp
class AddNode : public NodeProcessor
{
    OloEngine::Audio::SoundGraph::Value m_Result;
    // ...
};
```

**After:**
```cpp
class AddNode : public NodeProcessor
{
    Value m_Result;  // Now uses choc::value::Value
    // ...
};
```

### SineNode Example

**Before:**
```cpp
void Process(f32** inputs, f32** outputs, u32 numSamples) override
{
    // Custom value handling
    f32 frequency = GetParameterValue<f32>("Frequency");
}
```

**After:**
```cpp
void Process(f32** inputs, f32** outputs, u32 numSamples) override
{
    // CHOC value handling with interpolation
    f32 frequency = m_FrequencyParam.Process();
}
```

## Files to Update

1. **NodeProcessor.h/cpp** - Replace value system includes
2. **SoundGraph.h/cpp** - Add FIFO queues for thread communication
3. **SoundGraphSource.h/cpp** - Add parameter interpolation
4. **All Node files** (~45 files) - Update value creation/access patterns
5. **Value.h** - Either delete or create compatibility wrapper

## Testing Strategy

1. **Start Small**: Migrate one simple node (AddNode) first
2. **Unit Tests**: Verify value creation/access works correctly
3. **Audio Tests**: Ensure no glitches or performance regressions
4. **Integration Tests**: Test full SoundGraph with CHOC values
5. **Performance Tests**: Verify FIFO queues improve thread safety

## Rollback Plan

If issues arise:
1. Keep the old `Value.h` system alongside CHOC temporarily
2. Use preprocessor flags to switch between systems
3. Gradual migration allows testing both systems in parallel

## Timeline Estimate

- **Phase 1**: Add CHOC dependency and integration header (1 day)
- **Phase 2**: Migrate core NodeProcessor and SoundGraph (2-3 days)  
- **Phase 3**: Migrate all node classes (1 week, can be done incrementally)
- **Phase 4**: Add parameter interpolation and thread safety (2-3 days)
- **Phase 5**: Testing and performance validation (1 week)

**Total**: ~2-3 weeks for complete migration

## Questions & Issues

- **Q**: Will this break existing SoundGraph assets?
- **A**: JSON serialization changes may require conversion utility

- **Q**: Performance impact?
- **A**: CHOC should be faster due to optimized lock-free algorithms

- **Q**: Memory usage?
- **A**: CHOC is header-only, minimal overhead, better real-time allocators

- **Q**: Thread safety guarantees?
- **A**: Much better - CHOC FIFOs are production-tested for audio threads