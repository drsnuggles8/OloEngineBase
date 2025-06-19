# OloEngine Renderer Debugging Tools

This document describes the new renderer debugging tools integrated into OloEngine, designed to help game developers optimize rendering performance and debug rendering issues.

## Overview

Three new debugging tools have been added to enhance the renderer debugging capabilities:

1. **CommandPacketDebugger** - Visualizes every CommandPacket in the current frame
2. **RendererMemoryTracker** - Tracks memory usage for performance optimization
3. **RendererProfiler** - Provides timing and performance profiling data

## Integration

The debugging tools are integrated into the Sandbox3D application and can be accessed through the ImGui interface.

### UI Access

In the Sandbox3D application, the debugging tools are integrated into the **Lighting Settings** window. You can access them by:

1. Opening the **Lighting Settings** window (automatically displayed)
2. Scrolling down to the **Renderer Debugging Tools** section
3. Expanding the collapsible headers for each tool:
   - **Command Packet Debugger** - Visualize and analyze command packets
   - **Memory Tracker** - Monitor memory usage and allocation patterns  
   - **Renderer Profiler** - View timing and performance statistics

Each tool can be toggled on/off with checkboxes and includes additional controls for resetting data and exporting information.

## Tools Description

### 1. CommandPacketDebugger

**Location**: `OloEngine/src/OloEngine/Renderer/Debug/CommandPacketDebugger.h/.cpp`

**Purpose**: Provides real-time visualization of all CommandPackets being processed in the current frame.

**Features**:
- Lists all command packets with their types and details
- Shows command packet statistics and counts
- Export functionality to CSV for offline analysis
- Real-time filtering and search capabilities

**Usage**:
```cpp
// In the Lighting Settings window UI:
// 1. Check "Show Command Packets" to enable the debugger
// 2. Click "Export to CSV" to save data for offline analysis
// 3. The debugger automatically displays data from the active renderer's command bucket

// Programmatic access:
const auto* commandBucket = OloEngine::Renderer3D::GetCommandBucket();
if (commandBucket)
{
    m_CommandPacketDebugger.RenderDebugView(commandBucket, &showFlag, "Command Packets");
}
```

### 2. RendererMemoryTracker

**Location**: `OloEngine/src/OloEngine/Renderer/Debug/RendererMemoryTracker.h/.cpp`

**Purpose**: Tracks and monitors memory usage patterns for rendering operations.

**Features**:
- Real-time memory usage monitoring
- Memory allocation/deallocation tracking
- Peak memory usage statistics
- Memory leak detection capabilities
- Memory usage trends and graphs

**Key Statistics**:
- Current memory usage
- Peak memory usage
- Total allocations
- Active allocations
- Memory allocation rate

### 3. RendererProfiler

**Location**: `OloEngine/src/OloEngine/Renderer/Debug/RendererProfiler.h/.cpp`

**Purpose**: Provides detailed timing and performance profiling for rendering operations.

**Features**:
- Frame time tracking
- Begin/End scene timing
- GPU/CPU sync monitoring
- Performance bottleneck identification
- Historical timing data

**Key Metrics**:
- Frame time (current, average, min, max)
- Begin scene time
- End scene time
- GPU wait time
- Total frames rendered

## Implementation Details

### Initialization

The debugging tools are automatically initialized in `Sandbox3D::OnAttach()`:

```cpp
void Sandbox3D::OnAttach()
{
    // ... other initialization code ...
    
    // Initialize debugging tools
    OloEngine::RendererMemoryTracker::GetInstance().Initialize();
    OloEngine::RendererProfiler::GetInstance().Initialize();
}
```

### UI Integration

The tools are integrated into the main Lighting Settings window in `Sandbox3D::OnImGuiRender()`:

```cpp
// Debugging Tools Section
ImGui::Separator();
ImGui::Text("Renderer Debugging Tools");

// Command Packet Debugger
if (ImGui::CollapsingHeader("Command Packet Debugger"))
{
    ImGui::Checkbox("Show Command Packets##CommandDebugger", &m_ShowCommandPacketDebugger);
    ImGui::SameLine();
    if (ImGui::Button("Export to CSV##CommandDebugger"))
    {
        const auto* commandBucket = OloEngine::Renderer3D::GetCommandBucket();
        if (commandBucket)
        {
            m_CommandPacketDebugger.ExportToCSV(commandBucket, "command_packets.csv");
        }
    }
    
    if (m_ShowCommandPacketDebugger)
    {
        const auto* commandBucket = OloEngine::Renderer3D::GetCommandBucket();
        if (commandBucket)
        {
            m_CommandPacketDebugger.RenderDebugView(commandBucket, &m_ShowCommandPacketDebugger, "Command Packets");
        }
    }
}

// Similar integration for Memory Tracker and Renderer Profiler...
```

- **CommandPacketDebugger**: Accesses command data through `Renderer3D::GetCommandBucket()`
- **RendererProfiler**: Hooks into `Renderer3D::BeginScene()` and `EndScene()` calls
- **RendererMemoryTracker**: Monitors memory operations throughout the rendering pipeline

### Reset Functionality

All tools provide reset capabilities to clear historical data:

```cpp
// Reset buttons available in each tool's UI
if (ImGui::Button("Reset Statistics"))
{
    m_RendererMemoryTracker.Reset();
    m_RendererProfiler.Reset();
}
```

## Code Style Compliance

The implementation follows OloEngine coding standards:

- **Naming**: PascalCase for classes, m_PascalCase for members
- **Memory Management**: Uses RAII patterns and smart pointers where appropriate
- **Error Handling**: Includes appropriate validation and error checking
- **Documentation**: Clear comments and structured code organization

## Performance Considerations

- Tools are designed to have minimal impact on runtime performance
- Profiling overhead is kept to a minimum using high-resolution timers
- Memory tracking uses efficient data structures
- UI rendering only occurs when panels are visible

## Future Enhancements

Potential additions to the debugging toolkit:

1. **Resource Inspector** - Visual inspection of textures, buffers, and other GPU resources
2. **State Change Analyzer** - Tracking of OpenGL state changes and optimization suggestions
3. **Shader Debugger** - Real-time shader parameter monitoring and debugging
4. **Draw Call Analyzer** - Detailed analysis of draw call patterns and batching efficiency

## Recent Bug Fixes and Improvements

### Fixed Issues

1. **Memory Tracker Crash** - Resolved duplicate `std::mutex` declaration in `RendererMemoryTracker.h` that was causing potential runtime crashes.

2. **Command Packet Debugger Data Display** - Updated the debugger to use real command packet data instead of placeholder information:
   - Fixed command packet type detection and filtering
   - Implemented proper ImGui ID management to prevent conflicts
   - Added real memory usage calculations based on actual command packet sizes
   - Fixed CSV export to output actual command data

3. **UI Integration** - Merged debugging tools into the main Lighting Settings window to eliminate duplicate UI elements:
   - Removed separate "Renderer Debugging" window
   - Integrated all tools into collapsible sections within Lighting Settings
   - Eliminated duplicate frametime/FPS displays

4. **Data Accuracy** - Enhanced data extraction from CommandBucket and CommandPacket:
   - Updated selected packet details to show real metadata
   - Fixed memory and performance statistics calculations
   - Improved color coding for different command types

### Validation

All fixes have been validated through:
- Successful compilation with no warnings or errors
- Runtime testing of Sandbox3D application
- Verification of debugging tools functionality
- Memory usage and performance impact assessment

## Usage Examples

### Basic Usage in Game Development

```cpp
// Enable debugging tools for performance analysis
void MyGameLayer::OnAttach()
{
    // Tools are automatically available in debug builds
    // Access through Sandbox3D menu system
}

// Export command data for analysis
void MyGameLayer::ExportFrameData()
{
    const auto* bucket = OloEngine::Renderer3D::GetCommandBucket();
    if (bucket)
    {
        m_CommandPacketDebugger.ExportToCSV(bucket, "frame_analysis.csv");
    }
}
```

### Performance Optimization Workflow

1. Enable **RendererProfiler** to identify frame time bottlenecks
2. Use **MemoryTracker** to monitor memory allocation patterns
3. Analyze **CommandPacketDebugger** data to optimize draw calls
4. Export data for offline analysis and optimization

## Troubleshooting

**Issue**: Debugging panels not showing
- **Solution**: Ensure you're in Debug configuration and tools are properly initialized

**Issue**: Command packet data appears empty
- **Solution**: Verify that rendering is active and command bucket is being populated

**Issue**: Memory statistics seem incorrect
- **Solution**: Check that memory tracker is initialized before rendering begins

## Conclusion

These debugging tools provide comprehensive insight into OloEngine's rendering pipeline, enabling developers to:

- Identify performance bottlenecks
- Optimize memory usage
- Debug rendering issues
- Analyze command patterns
- Export data for detailed analysis

The tools are designed to be lightweight, non-intrusive, and provide actionable insights for game developers working with OloEngine.
