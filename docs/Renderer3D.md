# Renderer3D System Documentation

## Overview

The Renderer3D is a stateless, batch-oriented, (not yet) multi-threading-ready rendering architecture designed for high-performance graphics rendering with minimal CPU-GPU synchronization and optimal state management. The system is built around the concept of command packets that encapsulate rendering operations, allowing for efficient sorting, batching, and execution across multiple threads. It is based on the Molecule Engine blog post.

## Core Design Principles

1. **Stateless Rendering**: All rendering operations are expressed as self-contained command packets, eliminating reliance on global state.
2. **Memory Efficiency**: Uses custom memory allocators to minimize allocation overhead during rendering.
3. **Optimal Sorting**: Commands can be sorted to minimize state changes and maximize batch sizes.
4. **Thread Safety**: Designed for multi-threaded command generation with thread-local memory caches.
5. **Flexibility**: Supports a wide range of rendering operations through a unified command interface.
6. **Parallelism**: Architected for future parallel command generation, sorting, and execution.

## System Components

### Command Structure

Commands are small, POD (Plain Old Data) structures that represent rendering operations:

- **RenderCommand**: Base structure that all commands inherit from, containing common header information.
- **Command Types**: Specific command types like DrawMeshCommand, DrawIndexedCommand, SetViewportCommand, etc.

Each command contains:
- A type identifier.
- A dispatch function pointer.
- All data needed to execute the command without external context.

### Command Packet System

Command packets wrap commands with additional metadata for efficient processing:

- **CommandPacket**: Contains a command and associated metadata.
  - Stores sorting keys for efficient rendering.
  - Provides linked list functionality for chaining packets.
  - Supports batch comparison for finding compatible commands.

The packet system enables:
- Command sequencing via linked lists.
- Efficient command sorting based on various criteria.
- Command batching for reduced state changes.

### Command Memory Management

Memory management is handled through a specialized allocation system:

- **CommandAllocator**: A linear allocator for command packet memory.
  - Uses block-based allocation to reduce fragmentation.
  - Aligns memory for optimal access patterns.
  - Provides fast reset capability for frame-based reuse.

- **ThreadLocalCache**: Per-thread memory cache to minimize synchronization.
  - Each thread gets its own memory blocks.
  - Eliminates contention between threads.
  - Manages memory blocks in a linked list.

- **CommandMemoryManager**: Central coordinator for allocator management.
  - Provides thread-safe allocator access.
  - Manages allocator pool for easy reuse.
  - Tracks allocation statistics.

## Execution Flow

1. **Command Creation**:
   - Application code requests a command packet from the CommandMemoryManager.
   - Command data is initialized with all necessary parameters.
   - The packet is added to a command list or buffer.

2. **Command Processing**:
   - Commands are sorted based on material, shader, and texture keys.
   - Compatible commands are batched together.
   - Redundant state changes are eliminated.

3. **Command Execution**:
   - Commands are dispatched through their function pointers.
   - State transitions are minimized through sorted execution.
   - Batched commands reduce draw call overhead.

4. **Memory Reset**:
   - After frame completion, command memory is reset.
   - Memory blocks are retained but marked as empty.
   - Allocators are returned to the pool for reuse.

## Memory Management Details

### Allocation Strategy

The CommandAllocator uses a block-based linear allocation strategy:

1. Memory is allocated in large blocks (e.g., 64KB).
2. Commands are sequentially allocated within blocks.
3. When a block is full, a new block is added to the chain.
4. At the end of a frame, blocks are reset rather than freed.

### Thread-Local Caches

To support multi-threaded command generation:

1. Each thread gets its own ThreadLocalCache.
2. Thread-local caches eliminate synchronization needs for most allocations.
3. Only cache creation requires synchronization.
4. Each thread's commands are eventually merged into a single command queue.

### Memory Reset vs Free

The system distinguishes between:

- **Reset**: Marks memory as available for reuse without deallocation.
- **Free**: Actually returns memory to the system.

For performance reasons, memory is typically reset between frames rather than freed, reducing allocation overhead.

## Command Sorting and Batching

### Sorting Keys

Commands use multiple keys for efficient sorting:

- **Shader Key**: Based on the shader program ID.
- **Material Key**: Based on material properties.
- **Texture Key**: Based on texture bindings.

Sorting is performed using these keys to minimize state changes during rendering.

### Batching Process

1. Sort commands based on their keys.
2. Identify sequences of commands with compatible states.
3. Merge compatible commands where possible.
4. Execute batched commands to minimize API calls.

## Usage Example

See Sandbox3D sample code.

## Benefits of the Command Queue Architecture

1. **Reduced CPU-GPU Synchronization**: Commands are recorded then executed in batches.
2. **Optimal State Management**: Sorting minimizes expensive state changes.
3. **Memory Efficiency**: Custom allocators reduce allocation overhead.
4. **Thread-Friendly Design**: Multiple threads can generate commands simultaneously.
5. **Predictable Performance**: Memory usage patterns are consistent and controllable.
6. **Scalability**: The architecture scales well with scene complexity.

---

## ToDos / Planned Enhancements

- [ ] **Implement Multi-Threaded Command Generation**  
  Enable multiple threads to generate command packets in parallel, each using its own ThreadLocalCache.

- [ ] **Parallel Command Sorting and Merging**  
  Sort command buckets in parallel and merge per-thread command lists efficiently before execution.

- [ ] **Dedicated Render Thread**  
  Move command execution and GPU submission to a dedicated render thread for improved CPU-GPU parallelism.

- [ ] **Command Bucket System**  
  Group commands by material/shader for parallel processing and further batch reduction.

- [ ] **Advanced Key Generation**  
  Implement more sophisticated sorting key generation for finer-grained state change minimization.

- [ ] **Adaptive Batching**  
  Automatically adjust batch sizes based on hardware capabilities and scene complexity.

- [ ] **SPIR-V Shader Management**  
  Integrate SPIR-V compilation and reflection for GLSL shaders, ensuring efficient shader program management.

- [ ] **Resource Lifetime Management**  
  Improve resource tracking and destruction, leveraging C++ RAII and smart pointers throughout.

- [ ] **Profiling and Diagnostics**  
  Add tools for profiling command buffer usage, memory allocation, and draw call statistics.

---
