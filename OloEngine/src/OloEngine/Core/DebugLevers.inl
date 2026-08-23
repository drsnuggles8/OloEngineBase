// =============================================================================
// THE list of the engine's debug/diagnostic levers. Not a header — a table,
// included several times with different macro definitions (the same trick
// Scene.cpp uses for the generated OnComponent*Noop lists).
//
// Add a lever HERE and nowhere else. The declarations, the definitions, the
// environment seeding and the enumeration used by the startup log and the MCP
// tool are all generated from these lines, so there is no second list to keep
// in sync. That is the whole point: before this file the levers were 21
// independent `Env::IsTruthy(...)` reads with no way to ask what existed.
//
// Six shapes, because the levers genuinely differ and flattening them would
// change behaviour:
//
//   OLO_LEVER_TOGGLE  (Id, "ENV", "help")            Env::IsTruthy — lenient. "1"/"true"/"yes" on.
//   OLO_LEVER_EXACT   (Id, "ENV", "help")            Env::IsExactly "1" — a typo must NOT read as on.
//   OLO_LEVER_TRISTATE(Id, "ENV", "help")            "0"/"false" off, "1"/"true" on, anything else
//                                                    leaves the hardware-derived default alone.
//   OLO_LEVER_INT     (Id, "ENV", min, "help")       below min or unparseable => unset, never 0.
//   OLO_LEVER_NUMBER  (Id, "ENV", min, max, "help")  outside [min,max] or non-finite => unset.
//   OLO_LEVER_TEXT    (Id, "ENV", "help")            a path/string. Read-only: no setter, because
//                                                    every one of these is consumed once at init.
//
// Anything with a runtime setter is settable from code, tests and
// olo_debug_levers_set. TEXT levers deliberately are not.
// =============================================================================

// --- Render graph -----------------------------------------------------------
OLO_LEVER_TOGGLE(RenderGraphDiagnostics, "OLO_RENDERGRAPH_DIAGNOSTICS",
                 "Verbose render-graph build/execute tracing, plus the registration-order-sensitivity diagnostic.")
OLO_LEVER_TOGGLE(PoisonTransients, "OLO_RG_POISON_TRANSIENTS",
                 "Clear every pool-acquired transient to a per-resource hue at materialize time, so a texel "
                 "that reaches a consumer unwritten this frame is unmistakable (poison hue = stale pool "
                 "content; black = something actively wrote black mid-frame).")
OLO_LEVER_TOGGLE(DisableTransientAliasing, "OLO_RG_DISABLE_ALIASING",
                 "Give every transient its own physical backing. If an artifact disappears under this, the "
                 "transient planner's lifetime analysis let two live resources share one GPU object.")
OLO_LEVER_TOGGLE(BlackSquareHunt, "OLO_RG_BLACKSQUARE_HUNT",
                 "Extra per-pass logging for the transient black-square artifact hunt.")

// --- RHI --------------------------------------------------------------------
OLO_LEVER_TOGGLE(BindlessDescriptorHeap, "OLO_RHI_BINDLESS",
                 "Route texture binding through the bindless descriptor heap. Defaults off so a machine "
                 "without the extension, and every headless test, takes the slot-based path.")
OLO_LEVER_TOGGLE(VulkanTraceBuffers, "OLO_VK_TRACE_BUFFERS",
                 "Log every Vulkan vertex/index buffer's device-address range at create time — the currency "
                 "for pairing a GPU fault address back to its buffer.")

// --- Assets and bakes -------------------------------------------------------
// TEXT, not a toggle, and that is a correction: the variable NAMES THE OUTPUT
// DIRECTORY (IBLPrecompute writes "<value>/face0.png"), but two of its three
// read sites treated it as a plain on/off. So the documented `=1` switched the
// logging on and then wrote the face PNGs into a relative directory called "1",
// which does not exist, so stbi_write_png failed and no images appeared.
// Pass a real directory.
OLO_LEVER_TEXT(EnvironmentBakeDump, "OLO_ENV_BAKE_DUMP",
               "Directory to write the environment-bake diagnostics into: per-face HDR means, plus a "
               "tonemapped PNG per cubemap face. Set it to an existing directory; presence turns the "
               "diagnostics on.")
OLO_LEVER_TOGGLE(ModelImportDiagnostics, "OLO_MODEL_IMPORT_DIAGNOSTICS",
                 "Dump per-submesh material resolution and which texture landed in each PBR slot — the best "
                 "signal for a material/texture misbinding.")
OLO_LEVER_TEXT(PhysicsCacheDir, "OLO_PHYSICS_CACHE_DIR",
               "Override the directory Jolt cooked-shape caches are read from and written to.")
OLO_LEVER_TEXT(ShaderCacheDir, "OLO_SHADER_CACHE_DIR",
               "Override the directory the shader cache (SPIR-V, cross-compiled GLSL, program "
               "binaries) is read from and written to. Defaults to "
               "%LOCALAPPDATA%\\OloEngine\\ShaderCache — shared across every worktree on this "
               "machine, since the cache is content-addressed (issue #906).")
OLO_LEVER_TEXT(UsdPluginPath, "OLO_USD_PLUGIN_PATH",
               "Override where the USD importer looks for OpenUSD plugins.")

// --- Simulation bisection levers -------------------------------------------
// These answer "is this bug a threading bug?" by removing the parallelism
// without changing anything else. Exact-match where a typo silently disabling
// the parallel path would be a confusing performance cliff rather than a
// visible failure.
OLO_LEVER_EXACT(TerrainCpuLod, "OLO_TERRAIN_CPU_LOD",
                "Force the CPU terrain LOD selection path instead of the GPU quadtree descent.")
OLO_LEVER_EXACT(TerrainCpuPick, "OLO_TERRAIN_CPU_PICK",
                "Force the CPU terrain raycast (a 1-unit march over the CPU heightmap mirror) instead of the "
                "GPU pick pass (issue #717). The twin of OLO_TERRAIN_CPU_LOD, and for the same reason: when "
                "the brush cursor sits in the wrong place, the first question is which of the two paths put "
                "it there, and that has to be answerable without a rebuild.")
OLO_LEVER_EXACT(TerrainVtFullRebuild, "OLO_TERRAIN_VT_FULL_REBUILD",
                "Publish the terrain virtual texture's indirection map by rebuilding the whole thing (issue "
                "#715) instead of by incremental deltas (slice 2). The two are required to produce the "
                "SAME map, so a frame that changes with this on is an indirection bug and one that does not is "
                "somewhere else — and it is how the rebuild's GPU cost gets measured under real camera "
                "movement rather than only on the first frame.")
OLO_LEVER_EXACT(GameplaySchedulerSequential, "OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL",
                "Run the gameplay system schedule on one thread — same systems, same derived order, no "
                "worker dispatch and a synchronous physics step.")
OLO_LEVER_TOGGLE(FluidSequential, "OLO_FLUID_SEQUENTIAL",
                 "Run the fluid solver sequentially.")

// --- Task system ------------------------------------------------------------
OLO_LEVER_TOGGLE(NoThreading, "OLO_NO_THREADING",
                 "Run ParallelFor inline on the calling thread.")
OLO_LEVER_TOGGLE(ForceMultithread, "OLO_FORCE_MULTITHREAD",
                 "Use the threaded ParallelFor path even on a single-core host. Applied after "
                 "OLO_NO_THREADING, so setting both leaves threading ON.")
OLO_LEVER_TOGGLE(DisableOversubscription, "OLO_DISABLE_OVERSUBSCRIPTION",
                 "Stop ParallelFor from oversubscribing the worker pool.")
OLO_LEVER_TRISTATE(TaskGraphDynamicPrioritization, "OLO_TASK_GRAPH_DYNAMIC_PRIORITIZATION",
                   "Force task-graph dynamic prioritization on or off, overriding the hardware-derived default.")
OLO_LEVER_TRISTATE(TaskGraphDynamicThreadCreation, "OLO_TASK_GRAPH_DYNAMIC_THREAD_CREATION",
                   "Force task-graph dynamic thread creation on or off, overriding the hardware-derived default.")
OLO_LEVER_INT(TaskGraphNumWorkers, "OLO_TASK_GRAPH_NUM_WORKERS", 1,
              "Pin worker-pool sizing to a fixed logical-core count regardless of the host's real core "
              "count, so a many-core dev box can reproduce a 2-core CI runner's scheduling regime "
              "(issue #281 — hardware_concurrency() ignores process affinity, so an affinity pin alone "
              "still spawns one worker per physical core).")
OLO_LEVER_INT(ParallelForYieldMs, "OLO_PARALLEL_FOR_YIELD_MS", 0,
              "Background-yielding timeout for ParallelFor, in milliseconds.")
// Bounds mirror Scheduler.h's kMaxOversubscriptionRatio. Non-finite must be
// rejected here, not downstream: inf >= 1.0f is true, and the resulting
// ceil(workers * inf) cast to i32 is undefined behaviour.
OLO_LEVER_NUMBER(TaskGraphOversubscriptionRatio, "OLO_TASK_GRAPH_OVERSUBSCRIPTION_RATIO", 1.0f, 64.0f,
                 "Worker-pool oversubscription ratio.")
