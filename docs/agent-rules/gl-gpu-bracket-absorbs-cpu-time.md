# On OpenGL, a GPU sub-pass bracket can hold CPU time that ran after it closed

**Rule.** Before blaming a GL GPU bracket (`GPUPassTimerPool`'s `Pass/SubPass` pairs) for a spike,
check the CPU time of the same pass. When the two match, the spike is CPU work, and the bracket
that reads it is only the one whose end timestamp was still unflushed while that work ran. Time the
CPU side (`RenderGraph::GetLastExecutionTimings()` gives each pass's CPU execute time, plus a
`steady_clock` pair around the suspect calls) before cutting anything inside the bracket.

**Why it happens.** A GL timestamp is written when the GPU reaches it, and the GPU only reaches
what the driver has flushed. `EndSubPass` queues the end stamp into the open command buffer. When
the pass then runs pure CPU work, issuing no GL calls that flush, the GPU drains everything before
the stamp and sits idle. The stamp lands at the next flush, and the idle time counts as the
bracket's. The work inside the bracket can be microseconds.

**How to apply.**
- Line the frames up first: a frame's CPU record (`renderCallMs`, `cpuMs`) is one frame ahead of
  its pass record, and `gpuMs` two (correlate with offsets 0–3 and take the peak).
- A spike that a `glFinish` around the bracket's own work makes vanish was never that work's.
- The reliable owner of a CPU stall is a CPU timer. Leave the GPU bracket as it is: flushing to
  make it read true costs every frame.

## The story (#1589)

At the acceptance head close-up on GL, frames stalled for 450–650 ms in phases. The pass timings put
the whole delta in `GroomPass/RootFrames`, inside one coat's `Dispatch` child: 0.07 ms calm,
430–540 ms in a spike.

- Isolated between `glFinish` calls, no dispatch ran longer than 19 ms.
- The CPU loop around the dispatches took 45–75 ms in calm and spike frames alike.
- `GroomPass` as a whole executed for 60 ms of CPU calm and 600–720 ms in a spike.

The cost was the head hair's deformed coat-volume rebake in `AcquireCoatVolume`, later in the same
pass, issuing no GL calls until its texture upload. The `GroomPass/CoatVolumes` bracket around it
read 0.3 ms. The rebake itself is #1818. That it cost 480 ms rather than 115 ms is the capture host
having no task workers ([substituted-seams-compound.md](substituted-seams-compound.md) rule 14).
