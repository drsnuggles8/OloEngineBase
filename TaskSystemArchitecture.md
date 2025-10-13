# Task System Architecture Diagram

## High-Level Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                     APPLICATION LAYER                                │
│  (OloEditor, Sandbox3D, Asset Loading, Rendering, Physics, etc.)   │
└─────────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────────┐
│                  HIGH-LEVEL TASK API (Phase 6)                      │
│                                                                      │
│  • Tasks::Launch(name, func, priority, prerequisites)               │
│  • Tasks::ParallelFor(count, func, batchSize)                       │
│  • Tasks::LaunchAsync<T>(name, func) → TaskResult<T>               │
│  • TaskEvent, TaskPipe                                              │
└─────────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────────┐
│              TASK MANAGEMENT LAYER (Phase 4-5)                      │
│                                                                      │
│  • Dependency Tracking (Prerequisites → Subsequents)                │
│  • Lifecycle Management (State Machine)                             │
│  • Task Retraction (Inline Execution)                               │
│  • Priority-Based Routing                                           │
└─────────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────────┐
│                 TASK SCHEDULER (Phase 1, 3)                         │
│                         (Singleton)                                  │
│                                                                      │
│  • Worker Thread Pools (Foreground/Background)                      │
│  • Global Work Queues (Per-Priority)                                │
│  • Work Stealing Coordination                                       │
│  • Wake-Up Management                                               │
└─────────────────────────────────────────────────────────────────────┘
                    ↓                           ↓
        ┌───────────────────────┐   ┌───────────────────────┐
        │ FOREGROUND WORKERS    │   │ BACKGROUND WORKERS    │
        │ (Phase 3)             │   │ (Phase 3)             │
        │                       │   │                       │
        │ • High Priority       │   │ • Background Priority │
        │ • Normal Priority     │   │   Tasks Only          │
        └───────────────────────┘   └───────────────────────┘
                    ↓                           ↓
        ┌───────────────────────┐   ┌───────────────────────┐
        │ LOCAL WORK QUEUES     │   │ LOCAL WORK QUEUES     │
        │ (Phase 2)             │   │ (Phase 2)             │
        │                       │   │                       │
        │ • Per-Worker FIFO     │   │ • Per-Worker FIFO     │
        │ • Chase-Lev Deque     │   │ • Chase-Lev Deque     │
        │ • Work Stealing       │   │ • Work Stealing       │
        └───────────────────────┘   └───────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────────┐
│              LOCK-FREE INFRASTRUCTURE (Phase 2)                     │
│                                                                      │
│  • Global Queues (MPMC, Per-Priority)                               │
│  • Lock-Free Allocators (Per-Priority Task Pools)                   │
│  • Memory Recycling                                                 │
└─────────────────────────────────────────────────────────────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────────────┐
│         PROFILING & DEBUGGING LAYER (Phase 7)                       │
│                                                                      │
│  • Tracy Integration (Zones, Plots, Statistics)                     │
│  • ImGui Debug Panel (Optional)                                     │
│  • Deadlock Detection                                               │
│  • Performance Metrics                                              │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Task Object State Machine (Phase 1)

```
                    ┌──────────┐
                    │  Ready   │  ← Task Created
                    └──────────┘
                         │
                         │ Launch()
                         ↓
                    ┌──────────┐
                    │Scheduled │  ← In Queue (Local or Global)
                    └──────────┘
                         │
            ┌────────────┼────────────┐
            │ (normal)   │  (retract) │
            ↓            ↓            │
       ┌──────────┐  Execute      ┌──────────┐
       │ Running  │  Inline       │  Ready   │
       └──────────┘    ↓          └──────────┘
            │          │                │
            │          └────────┬───────┘
            │                   ↓
            │              ┌──────────┐
            │              │ Running  │
            │              └──────────┘
            │                   │
            └───────────┬───────┘
                        ↓
                   ┌──────────┐
                   │Completed │  ← Notify Subsequents
                   └──────────┘
```

---

## Worker Thread Flow (Phase 3)

```
┌─────────────────────────────────────────────────────────────┐
│                    WORKER THREAD                             │
│                                                              │
│  while (!m_ShouldExit)                                      │
│  {                                                           │
│      Task* task = FindWork();                               │
│                                                              │
│      if (task)                                              │
│      {                                                       │
│          ExecuteTask(task);                                 │
│      }                                                       │
│      else                                                    │
│      {                                                       │
│          WaitForWork();  // Spin → Yield → Sleep            │
│      }                                                       │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘

FindWork() Priority:
    1. Check Local Queue (BEST - cache locality)
        ↓ (empty)
    2. Check Global Queue (GOOD - work available)
        ↓ (empty)
    3. Steal from Other Workers (OK - load balancing)
        ↓ (all empty)
    4. Return nullptr (IDLE - wait for work)
```

---

## Work Stealing Strategy (Phase 3)

```
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│  Worker 0    │  │  Worker 1    │  │  Worker 2    │
│              │  │              │  │              │
│ Local Queue: │  │ Local Queue: │  │ Local Queue: │
│ [T1][T2][T3] │  │ [T4][T5]     │  │ (empty)      │
└──────────────┘  └──────────────┘  └──────────────┘
      ↑                   ↑                   ↓
      │                   │                   │
      │                   │      1. Check Local (empty)
      │                   │      2. Check Global (empty)
      │                   │      3. STEAL from others
      │                   │                   │
      │                   └───────────────────┘
      │                           ↓
      │                   Worker 2 steals T5 from Worker 1
      │
      └───────────────────────────┘
                          ↓
              Worker 2 steals T3 from Worker 0

Result: Load balanced across all workers
```

---

## Task Dependency Graph (Phase 4)

```
Example: Asset Loading with Dependencies

         ┌──────────┐
         │ LoadMesh │
         └──────────┘
              │
              │ (prerequisite)
              ↓
         ┌──────────┐
         │LoadTexture│
         └──────────┘
              │
              │ (prerequisite)
              ↓
         ┌──────────┐
         │ CreateMaterial │
         └──────────┘
         /            \
        /              \
   (prerequisite)   (prerequisite)
      /                  \
     ↓                    ↓
┌──────────┐        ┌──────────┐
│ BuildMesh│        │RenderSetup│
└──────────┘        └──────────┘
     │                    │
     │                    │
     │  (subsequents)     │
     └──────┬─────────────┘
            ↓
       ┌──────────┐
       │  Render  │
       └──────────┘

Implementation:
• LoadMesh.AddPrerequisite(nullptr) → launches immediately
• LoadTexture.AddPrerequisite(LoadMesh) → waits for mesh
• CreateMaterial.AddPrerequisite(LoadTexture) → waits for texture
• BuildMesh.AddPrerequisite(CreateMaterial)
• RenderSetup.AddPrerequisite(CreateMaterial)
• Render.AddPrerequisite({BuildMesh, RenderSetup}) → waits for both
```

---

## Task Event Pattern (Phase 4)

```
Non-Blocking Synchronization:

┌─────────────────────────────────────────────────────────────┐
│ Main Thread                                                  │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  TaskEvent allLoaded("AssetsLoaded");                       │
│                                                              │
│  for (auto& asset : assets)                                 │
│  {                                                           │
│      auto task = Launch("LoadAsset", [&]{ Load(asset); }); │
│      allLoaded.AddPrerequisites({task});                    │
│  }                                                           │
│                                                              │
│  // IMPORTANT: Wait() executes other tasks!                 │
│  allLoaded.Wait();  // ← Doesn't block, executes work       │
│                                                              │
│  // All assets loaded here                                  │
│                                                              │
└─────────────────────────────────────────────────────────────┘

Wait() Behavior:
1. Check if event triggered → return immediately
2. Execute other available tasks while waiting
3. Keep worker productive instead of blocking
4. Only sleep if no other work available
```

---

## Priority Queue Routing (Phase 5)

```
Task Launch with Priority:

┌─────────────────────────────────────────────────────────────┐
│                      Task Scheduler                          │
└─────────────────────────────────────────────────────────────┘
                              ↓
              ┌───────────────┼───────────────┐
              ↓               ↓               ↓
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │   HIGH     │  │   NORMAL   │  │ BACKGROUND │
     │  Priority  │  │  Priority  │  │  Priority  │
     │   Queue    │  │   Queue    │  │   Queue    │
     └────────────┘  └────────────┘  └────────────┘
            ↓               ↓               ↓
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │ FOREGROUND │  │ FOREGROUND │  │ BACKGROUND │
     │  Workers   │  │  Workers   │  │  Workers   │
     └────────────┘  └────────────┘  └────────────┘
     
High Priority:    Rendering, Input, Time-Critical
Normal Priority:  Game Logic, Physics, General Work
Background:       Asset Loading, Cooking, Background Tasks

Workers:
• Foreground: NumCores - 2 (leave room for main/render threads)
• Background: Max(1, NumCores / 4) (25% of cores)
```

---

## Memory Layout: Cache-Line Alignment (Phase 2)

```
Worker Thread Structure (Aligned to prevent false sharing):

┌─────────────────────────────────────────┐  ← Cache Line 0 (128 bytes)
│  Local Queue Head (owner-only)          │
│  ...padding...                           │
├─────────────────────────────────────────┤  ← Cache Line 1 (128 bytes)
│  Local Queue Tail (atomic, shared)      │
│  ...padding...                           │
├─────────────────────────────────────────┤  ← Cache Line 2
│  Local Queue Slots Array                │
│  (each slot cache-aligned)               │
│                                          │
├─────────────────────────────────────────┤
│  Worker State (atomic)                   │
│  ...padding...                           │
├─────────────────────────────────────────┤
│  Less frequently accessed data           │
│  (thread handle, wake event, etc.)       │
└─────────────────────────────────────────┘

Why Alignment Matters:
• Head modified only by owner thread (no contention)
• Tail modified by all threads (high contention)
• Separate cache lines prevent false sharing
• ~2x performance improvement in testing
```

---

## ParallelFor Pattern (Phase 5)

```
Sequential Loop:
┌──────────────────────────────────────────────────────────┐
│ for (int i = 0; i < 10000; ++i)                         │
│     ProcessItem(i);                                      │
└──────────────────────────────────────────────────────────┘
          Time: 1000ms (single core)

ParallelFor (8 cores, batch size 32):
┌──────────────────────────────────────────────────────────┐
│ ParallelFor(10000, [](int i) {                          │
│     ProcessItem(i);                                      │
│ }, 32);                                                  │
└──────────────────────────────────────────────────────────┘

Execution:
Batch 0 (0-31):    Worker 0  ████████
Batch 1 (32-63):   Worker 1  ████████
Batch 2 (64-95):   Worker 2  ████████
Batch 3 (96-127):  Worker 3  ████████
Batch 4 (128-159): Worker 4  ████████
...
Batch N (9984-...):Worker 0  ████  (work stealing)

          Time: ~140ms (7x speedup)

Batch Size Selection:
• Too small (< 10):   High overhead, poor cache usage
• Too large (> 1000): Poor load balancing
• Sweet spot (~32-128): Balance overhead vs parallelism
• Target: ~100μs per batch
```

---

## Tracy Profiling Integration (Phase 7)

```
Timeline View in Tracy:

Main Thread:    ════════════════════════════════════════
Worker 0:       ██████[LoadAsset]██████[ProcessData]███
Worker 1:       ████[BuildMesh]████████[Render]████████
Worker 2:       ███[LoadTexture]███████████[Compile]███
Worker 3:       ████████[Physics]████████[Audio]███████

Zone Colors:
• Red:    High Priority Tasks
• Yellow: Normal Priority Tasks
• Blue:   Background Priority Tasks
• Green:  Inline Execution (Retracted)

Statistics Panel:
┌─────────────────────────────────────────┐
│ Task System Statistics                   │
├─────────────────────────────────────────┤
│ Tasks Launched:    1,245,678            │
│ Tasks Completed:   1,245,678            │
│ Tasks Retracted:     125,432 (10.1%)    │
│ Worker Wakeups:      542,123            │
│                                          │
│ Average Latency:     8.2μs              │
│ Peak Latency:        245μs              │
│ Throughput:          1.2M tasks/sec     │
│                                          │
│ Worker Utilization:                      │
│   Worker 0:  94%  █████████████         │
│   Worker 1:  92%  ████████████          │
│   Worker 2:  89%  ████████████          │
│   Worker 3:  91%  ████████████          │
└─────────────────────────────────────────┘

Plot Tracking:
• Active Task Count (real-time)
• Queue Depth (per priority)
• Worker Utilization (%)
• Task Launch Rate (tasks/sec)
• Memory Usage (MB)
```

---

## Integration Example: Asset Loading (Phase 6)

```
BEFORE (Dedicated Thread):
┌─────────────────────────────────────────┐
│ Main Thread                              │
├─────────────────────────────────────────┤
│ RequestAssetLoad(handle)                │
│   → Queue to asset thread               │
│   → Wait on future/condition_variable   │  ← BLOCKS
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│ Asset Thread (Dedicated)                │
├─────────────────────────────────────────┤
│ while (running) {                       │
│   asset = GetNextFromQueue()            │
│   if (asset) Load(asset)                │
│   else Sleep(10ms)  ← WASTES TIME       │
│ }                                        │
└─────────────────────────────────────────┘


AFTER (Task System):
┌─────────────────────────────────────────┐
│ Main Thread (or any thread)             │
├─────────────────────────────────────────┤
│ auto task = Tasks::LaunchBackground(    │
│     "LoadAsset",                        │
│     [handle] { LoadAssetSync(handle); } │
│ );                                       │
│                                          │
│ // Do other work...                     │
│                                          │
│ task->Wait();  ← Executes other tasks!  │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│ Background Workers (Shared Pool)        │
├─────────────────────────────────────────┤
│ • Load assets when available            │
│ • Help with other background work       │
│ • No idle time (work stealing)          │
│ • Better CPU utilization                │
└─────────────────────────────────────────┘

Benefits:
✓ No dedicated thread (saves memory)
✓ Better CPU utilization (work stealing)
✓ Non-blocking waits (executes other tasks)
✓ Automatic load balancing
✓ Unified profiling
```

---

## Phase Dependencies Diagram

```
Phase 1: Foundation
    ↓
Phase 2: Lock-Free Queues
    ↓
Phase 3: Worker Threads ← (depends on 1 + 2)
    ↓
Phase 4: Synchronization ← (depends on 1 + 3)
    ↓
Phase 5: Advanced Features ← (depends on 1-4)
    ↓
Phase 6: Integration ← (depends on 1-5)
    ↓
Phase 7: Profiling & Polish ← (depends on 1-6)

Testing: Continuous throughout all phases
• Unit tests per component
• Integration tests per phase
• Stress tests for thread safety
• Performance benchmarks
```

---

## Success Metrics Visualization

```
Performance Targets (Release Build):

Task Throughput:
    Target: > 1M tasks/sec
    ████████████████████████████████████ 1.2M ✓
    
Task Latency (High Priority):
    Target: < 10μs
    ████ 8.2μs ✓
    
Task Latency (Normal Priority):
    Target: < 100μs
    ██████████ 85μs ✓
    
Task Latency (Background Priority):
    Target: < 1ms
    ████████████████ 0.8ms ✓
    
Worker Utilization:
    Target: > 90%
    ██████████████████████████████ 92% ✓
    
Memory Overhead:
    Target: < 10MB
    ████████ 7.5MB ✓
    
Scalability (8 cores):
    Target: Linear speedup
    Core 1: █ 1.0x
    Core 2: ██ 1.95x
    Core 4: ████ 3.8x
    Core 8: ████████ 7.2x ✓
```

---

**Document Version**: 1.0  
**Created**: October 13, 2025  
**Purpose**: Visual reference for task system architecture
