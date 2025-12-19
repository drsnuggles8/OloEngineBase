# Allocation Tracking Debug Guide

This guide explains how to use OloEngine's built-in allocation tracking system for debugging memory leaks and monitoring object lifetimes using Visual Studio and Visual Studio Code.

## Overview

OloEngine includes a lightweight allocation tracking system that monitors object creation and destruction for specific classes. It provides:

- **Live object count** - Number of objects currently alive
- **Peak object count** - Maximum objects alive at any point
- **Total allocations** - Total objects created since startup
- **Zero overhead in Release builds** - Completely disabled for production

## Debugging with Visual Studio

### Method 1: Watch Window

1. **Set a breakpoint** where you want to inspect allocations
2. **Add watches** in the Watch window:
   ```
   OloEngine::Entity::GetLiveCount()
   OloEngine::Entity::GetPeakCount()
   OloEngine::Entity::GetTotalCreated()
   OloEngine::Entity::GetStatsString()
   ```
3. **Step through code** and watch the counters change in real-time

### Method 2: Immediate Window

While debugging, use the Immediate Window (`Debug > Windows > Immediate`):
```cpp
// Check current allocation status
OloEngine::Entity::GetLiveCount()
OloEngine::Entity::GetStatsString()

// Check for specific object types
OloEngine::AnimationClip::GetLiveCount()
```

### Method 3: Diagnostic Tools

1. **Enable Diagnostic Tools** (`Debug > Windows > Diagnostic Tools`)
2. Use in combination with allocation tracking:
   - Set breakpoints before/after suspected leak areas
   - Check allocation counters at each breakpoint
   - Compare with built-in memory usage graphs

## Debugging with Visual Studio Code

### Method 1: Debug Console

When stopped at a breakpoint, use the Debug Console:
```cpp
-exec call OloEngine::Entity::GetLiveCount()
-exec call (char*)OloEngine::Entity::GetStatsString().c_str()
```

### Method 2: Watch Expressions

Add watch expressions in the Watch panel:
- `OloEngine::Entity::GetLiveCount()`
- `OloEngine::Entity::GetPeakCount()`
- `OloEngine::Entity::GetTotalCreated()`

### Method 3: Launch Configuration

Add allocation tracking calls to your launch configuration for startup logging:
```json
{
    "name": "OloEngine Debug with Allocation Tracking",
    "type": "cppvsdbg",
    "request": "launch",
    "program": "${workspaceFolder}/bin/Debug/Sandbox3D/Sandbox3D.exe",
    "cwd": "${workspaceFolder}/OloEditor",
    "setupCommands": [
        {
            "description": "Enable allocation tracking logs",
            "text": "-exec call OLO_CORE_INFO(\"Startup allocation check: {}\", OloEngine::Entity::GetStatsString())",
            "ignoreFailures": true
        }
    ]
}
```

## Common Debugging Workflows

### 1. Leak Detection in Unit Tests

```cpp
#include "OloEngine/Debug/AllocationTracker.h"

TEST_CASE("Entity Lifecycle") {
    // Take a snapshot before test
    auto snapshot = OLO_ALLOCATION_SNAPSHOT(Entity);
    
    {
        // Test code that creates/destroys entities
        Entity entity1;
        Entity entity2;
        // ... test operations ...
    } // Objects should be destroyed here
    
    // Assert no leaks occurred
    OLO_ASSERT_NO_LEAKS(Entity, snapshot);
}
```

### 2. Finding Memory Leaks in Development

**Step 1: Set baseline breakpoint**
- Place breakpoint at application start
- Note initial allocation counts

**Step 2: Set checkpoint breakpoints**
- Add breakpoints at key lifecycle points (scene loading, entity creation, etc.)
- Check if allocations are growing unexpectedly

**Step 3: Identify leak sources**
- If `GetLiveCount()` keeps growing, you have a leak
- Use `GetStatsString()` for detailed information
- Enable extended tracking for stack traces (see Advanced Features)

### 3. Performance Impact Analysis

```cpp
// Measure allocation frequency
auto start_count = Entity::GetTotalCreated();
// ... run performance-critical code ...
auto end_count = Entity::GetTotalCreated();
auto allocations = end_count - start_count;
// High allocation counts may indicate performance issues
```

## Advanced Features

### Stack Trace Tracking (C++23)

For detailed leak analysis, the system can capture stack traces:

```cpp
// Available in AllocationTrackerExtended (requires linking .cpp file)
#include "OloEngine/Debug/AllocationTracker.h" // Note: includes .cpp features

// Extended tracking provides:
// - Stack traces for each allocation
// - Object lifetime analysis
// - Thread-specific tracking
```

### Custom Class Integration

To add allocation tracking to your own classes:

```cpp
// For regular classes
class MyClass : OLO_ALLOCATION_TRACKED(MyClass) {
    // Your class implementation
};

// For RefCounted classes
class MyRefCounted : OLO_TRACKED_REFCOUNTED(MyRefCounted) {
    // Your class implementation
};
```

### Conditional Compilation

The allocation tracking system is automatically disabled in Release builds:

```cpp
// This code only runs in Debug builds
#if OLO_ENABLE_ALLOCATION_TRACKING
    if (Entity::GetLiveCount() > expected_count) {
        // Investigation logic
    }
#endif
```

## Best Practices

### 1. Use in Development Only
- Allocation tracking is for development/debugging
- It's automatically disabled in Release/Dist builds
- Don't rely on it for production logic

### 2. Set Clear Checkpoints
- Take snapshots before major operations
- Check allocation counts at logical boundaries
- Use meaningful variable names for snapshots

### 3. Automate Leak Detection
- Add allocation assertions to unit tests
- Use CI/CD to catch regressions early
- Document expected allocation patterns

### 4. Combine with Professional Tools
- Use alongside Visual Studio Diagnostic Tools
- Complement with Application Verifier (Windows)
- Consider Intel Inspector for comprehensive analysis

## Examples

### Basic Leak Check
```cpp
void TestEntityCreation() {
    auto snapshot = OLO_ALLOCATION_SNAPSHOT(Entity);
    
    // Create some entities
    for (int i = 0; i < 10; ++i) {
        Entity entity; // Should be destroyed at end of scope
    }
    
    // Verify no leaks
    OLO_ASSERT_NO_LEAKS(Entity, snapshot);
}
```

### Manual Inspection
```cpp
void InspectAllocations() {
    // Check current status
    OLO_CORE_INFO("Entity allocations: {}", Entity::GetStatsString());
    
    // Create and check
    Entity entity;
    OLO_CORE_INFO("After creating entity: {}", Entity::GetStatsString());
    
    // Entity destroyed here
    OLO_CORE_INFO("After entity destruction: {}", Entity::GetStatsString());
}
```

This allocation tracking system provides a lightweight, integrated approach to memory debugging that works seamlessly with your existing Visual Studio and VS Code debugging workflows.
