#pragma once
#include <span>

// VulkanPipelineBuilder — thin PSOs + the root-data binding-mapping ABI.
// Issue #691, ADR 0011 §4 (root data) + §5 (thin PSO).
//
// THE ABI, in one place (the single source of truth both the mapping array
// and the draw-time writer consume — they structurally cannot disagree, the
// amendment (25) property one level up):
//
//   - Push data is EXACTLY 8 BYTES: the VkDeviceAddress of the draw's root
//     struct, pushed at offset 0 via vkCmdPushDataEXT. Never the payload —
//     §4's 128/256-byte push-constant warning, honoured by construction.
//   - The root struct is built per shader from its REFLECTED bindings, in
//     (set, binding) order: first one u64 GPU address per buffer block
//     (UBO/SSBO alike — INDIRECT_ADDRESS mapping reads the block's address
//     from root + Field.Offset), then one u32 heap slot index per sampled
//     texture (HEAP_WITH_INDIRECT_INDEX reads the index from root +
//     Field.Offset and scales by the heap's descriptor stride).
//   - Samplers come from the SAMPLER HEAP (#691, §1.2a's
//     deduplicated second heap): each combined-image-sampler field carries a
//     second u32 (Offset + kSamplerIndexOffset) that indexes
//     VulkanSamplerHeap; BindTexture stages it from the image's own recorded
//     state (or an explicit desc). Nothing is embedded per pipeline, so
//     sampler state is not a PSO axis.
//
// THIN PSO (§5): no VkPipelineVertexInputStateCreateInfo contents (vertex
// pulling — geometry arrives as a root-struct buffer address), dynamic
// everything the 1.3 core + EDS3 floor allows. What remains BAKED (and is
// therefore the cache key): shader identity, attachment formats + sample
// count, and — only when EDS3's blend states are unavailable — the recorded
// blend state. Dynamic rendering only (VkPipelineRenderingCreateInfo); no
// VkRenderPass objects exist on this backend.
//
// THREAD-SAFETY (issue #806, ADR 0011 amendment (92) rule 8; reworked by
// #1345): GetOrCreate* run once per draw / dispatch, from any recording
// thread, and the precache worker (VulkanPipelinePrecache) creates requested
// pipelines on its own thread. The map lookup, the request ledger and the
// in-flight bookkeeping are guarded by m_Mutex; the DRIVER CALL is not.
// Holding the exclusive lock across vkCreate*Pipelines made every recording
// thread's hit path wait for a background compile, which would have moved the
// hitch rather than removed it. So a creation:
//   1. claims the key under the lock (the ledger marks it in flight, so a
//      second thread asking for the same key waits instead of compiling it
//      again) and snapshots its inputs — the shader's modules and layout are
//      copied on the asking thread, never read by the worker later;
//   2. fetches VulkanPipelineCache::Handle() while still holding the lock
//      (that class's lazy create relies on this lock), then releases it;
//   3. calls the driver, which is thread-safe for creation and whose
//      VkPipelineCache is internally synchronised;
//   4. publishes under the lock and wakes any waiter.
// InvalidateShader waits for every in-flight creation of that shader before
// returning, because its caller destroys the old modules next. The sampler
// heap's EnsureCreated has its own lock and is now reached with this one
// either held or not; it never takes this one, so there is no order to break.
// FlushDynamicState is static and touches no builder state.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/PipelineRequestLedger.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanShader.h"

#include <volk.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{
    struct VulkanRootDataLayout
    {
        struct Field
        {
            VulkanShaderBinding Binding;
            u32 Offset = 0; ///< Byte offset of this field inside the root struct.
            // Byte offset of this field's SAMPLER index (or index array) inside
            // the root struct; 0 for a non-combined field. Explicit rather than
            // `Offset + kSamplerIndexOffset` because an ARRAY field's samplers
            // follow N image indices, not one (#1078) — the mapping and
            // AssembleAndPushRootData both read this, so they cannot disagree.
            u32 SamplerOffset = 0;
        };

        // A scalar CombinedImageSampler field is TWO u32s (#691): the image
        // heap index at Offset and the sampler heap index at
        // Offset + kSamplerIndexOffset. An ARRAY field instead lays out
        // ArrayCount image indices followed by ArrayCount sampler indices, so
        // use Field::SamplerOffset rather than this constant when it may be an
        // array.
        static constexpr u32 kSamplerIndexOffset = 4u;

        std::vector<Field> Fields;
        u32 SizeBytes = 0;

        [[nodiscard]] static VulkanRootDataLayout Build(std::span<const VulkanShaderBinding> bindings);
        [[nodiscard]] const Field* Find(u32 set, u32 binding) const;
    };

    // VulkanRenderTargetDesc moved to VulkanRendererAPI.h (#691):
    // the draw path's rendering scope holds one, and this header includes
    // that one — declaring it here would cycle.

    class VulkanPipelineBuilder
    {
      public:
        [[nodiscard]] static VulkanPipelineBuilder& Get();

        // The complete pipeline key: everything baked into a thin PSO. Public
        // so the precache can name a request and VulkanPipelineKeyTest can pin
        // which inputs separate two pipelines (issue #1345: incompatible
        // configurations must never share one). Viewport and scissor are
        // dynamic state (VIEWPORT/SCISSOR_WITH_COUNT), so the render RESOLUTION
        // is deliberately not here: one pipeline serves every target size.
        struct Key
        {
            u64 ShaderKey = 0;
            std::array<VkFormat, 8> ColorFormats{};
            VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
            u32 ColorCount = 0;
            u32 Samples = 1;
            u64 BakedBlendHash = 0; ///< 0 when blend is dynamic (EDS3 present).
            u64 LayoutHash = 0;     ///< Root-data layout — drives the baked binding mappings.
            /// Baked patch size for a tessellated pipeline; 0 when the shader
            /// has no TCS/TES stage. patchControlPoints is only dynamic under
            /// extendedDynamicState2PatchControlPoints, which is NOT on the
            /// ADR 0010 floor — so it is a PSO axis here (#691, A10).
            u32 PatchControlPoints = 0;

            bool operator==(const Key&) const = default;
        };
        struct KeyHash
        {
            [[nodiscard]] sizet operator()(const Key& key) const;
        };

        // The key a graphics draw of `shaderKey` would look up, from its inputs
        // alone. Pure (no device): the draw path, the precache and the tests all
        // derive keys through this one function, so a request and the draw it
        // prepares cannot disagree about what the key is.
        [[nodiscard]] static Key DeriveGraphicsKey(u64 shaderKey, const VulkanRootDataLayout& layout, bool tessellated,
                                                   bool dynamicBlend, const VulkanRecordedPipelineState& state,
                                                   const VulkanRenderTargetDesc& targets);
        [[nodiscard]] static Key DeriveComputeKey(u64 shaderKey, const VulkanRootDataLayout& layout);

        // Cached lookup or creation of the thin graphics pipeline for
        // (shader, targets[, baked blend]). Sampler state is NOT a pipeline
        // axis (#691): the mappings source the sampler half from the
        // SAMPLER heap, indexed per draw from root data — the embedded
        // per-pipeline sampler retired with the sampler heap.
        //
        // A shader whose module map carries VK_SHADER_STAGE_MESH_BIT_EXT
        // builds a MESH pipeline (issue #813): stages [task?, mesh, fragment],
        // no vertex-input / input-assembly state, and no topology /
        // primitive-restart dynamic states (declaring either against a mesh
        // pipeline is a validation error). Mesh-ness needs no key axis of its
        // own — ShaderKey identifies the shader, and a reload that changes
        // its stage set runs through InvalidateShader first.
        //
        // A MISS here is synchronous creation on the calling (recording)
        // thread: the hitch #1345 measures. It is classified by the request
        // ledger (never requested / requested too late / retried after a
        // failure), timed, logged for the first misses and counted always —
        // never silent — and recorded in the precache manifest so the next
        // session requests it before the draw.
        [[nodiscard]] VkPipeline GetOrCreateGraphics(VulkanShader& shader, const VulkanRootDataLayout& layout,
                                                     const VulkanRecordedPipelineState& state,
                                                     const VulkanRenderTargetDesc& targets);

        // The compute sibling (#691): same mapping chain, same
        // VK_NULL_HANDLE layout + DESCRIPTOR_HEAP flag, no fixed-function
        // state at all. Keyed on (shaderKey, layout) — target and
        // blend fields stay zero, and shader keys are process-unique so a
        // compute key can never collide with a graphics one. `shaderKey` and
        // `module` are passed directly so this header needs no
        // VulkanComputeShader dependency; InvalidateShader(shaderKey) covers
        // compute pipelines through the same reverse index. `shaderName` names
        // the shader in miss logs and the precache manifest.
        [[nodiscard]] VkPipeline GetOrCreateCompute(u64 shaderKey, VkShaderModule module,
                                                    const VulkanRootDataLayout& layout,
                                                    std::string_view shaderName = "<compute>");

        // --- Requests (issue #1345) ------------------------------------------
        // Ask for a pipeline ahead of its first draw. The shader's modules and
        // layout are snapshotted HERE, on the calling thread, so the worker
        // that creates it never reads the shader object. A key that is already
        // created, queued or in flight is a duplicate and returns false.
        // `patchControlPoints` is the recorded patch size the draw will use
        // (ignored for a shader without tessellation stages). The recorded
        // blend state is the default one: blend is dynamic on the ADR 0010
        // floor (EDS3), so it is not part of the key there; on a device
        // without EDS3 blend a request can only match a draw that records the
        // default blend, and any other draw is counted as never requested.
        // Returns the key the request names (whether it was new or a
        // duplicate), or nullopt when no request could be formed (no device,
        // shader not ready).
        std::optional<Key> RequestGraphics(VulkanShader& shader, const VulkanRenderTargetDesc& targets,
                                           u32 patchControlPoints,
                                           PipelineRequestPriority priority = PipelineRequestPriority::Selected);
        std::optional<Key> RequestCompute(u64 shaderKey, VkShaderModule module, const VulkanRootDataLayout& layout,
                                          std::string_view shaderName,
                                          PipelineRequestPriority priority = PipelineRequestPriority::Selected);

        // Drop this shader's QUEUED requests and wait for its in-flight
        // creations, without touching its created pipelines. For a caller about
        // to destroy modules a queued request snapshotted (VulkanComputeShader's
        // rebuild destroys its old module before the reload invalidates).
        void QuiesceShader(u64 shaderKey);

        // Create ONE queued request on the calling thread, outside every
        // recording region (the precache worker's loop body, and what tests
        // call to run the queue deterministically). Returns false when nothing
        // was queued.
        bool PrepareNextQueued();

        enum class RequestState : u8
        {
            Queued,
            InFlight,
            Ready,
            Failed,
        };
        // Where a key stands, for readiness: nullopt when it was never
        // requested or created.
        [[nodiscard]] std::optional<RequestState> GetRequestState(const Key& key) const;

        // A collector's readiness claim for one shader (see
        // PipelineRequestLedger): a never-requested miss on a shader declared
        // ready is counted as a ReadyClaimViolation and logged as an error.
        void DeclareShaderReady(u64 shaderKey, bool ready);

        struct RequestStats
        {
            PipelineRequestCounters Counters;
            u64 Queued = 0;
            u64 InFlight = 0;
            u64 CachedPipelines = 0;
            u64 Completions = 0; ///< Monotonic: bumps on every finished creation (readiness re-evaluation trigger).
        };
        [[nodiscard]] RequestStats GetRequestStats() const;
        void ResetRequestCounters();
        // The completion counter alone: what the per-frame readiness check
        // compares against, without walking the ledger.
        [[nodiscard]] u64 GetCompletions() const;

        // A new recording epoch (the selected configuration changed, or a new
        // session began): the next draw of every pipeline records its key in
        // the precache manifest once more, so a pipeline that keeps hitting
        // keeps its entry fresh and gains the new configuration tag. Without
        // it a precached key would never be re-recorded, look least recently
        // seen, and be the first thing Trim() drops.
        void BeginRecordingEpoch();

        // Issue every vkCmdSet* for the states the pipelines above declare
        // dynamic, from the recorded state. Must run after vkCmdBindPipeline,
        // before the draw. The pipeline KIND is derived from the currently
        // bound VulkanShader (a mesh-stage shader's pipeline declares neither
        // primitive topology nor primitive-restart dynamic, and setting an
        // undeclared dynamic state is invalid — issue #813); deriving it here
        // rather than taking a flag means no call site can pass the wrong
        // kind for the pipeline that was just bound from that same shader.
        static void FlushDynamicState(VkCommandBuffer cmd, const VulkanRecordedPipelineState& state,
                                      const VulkanRenderTargetDesc& targets);

        // §3(d)'s shader→pipeline reverse index lives HERE, because the
        // builder's key→pipeline map must be invalidated in the same act —
        // two owners meant a double-destroy AND a dangling map entry on the
        // first device run. Enqueues every dependent pipeline for deferred
        // destruction (an in-flight command buffer may still reference them),
        // erases them from the cache map and the request ledger, WAITS for any
        // creation of this shader still running on another thread (the caller
        // destroys the old modules next), and returns how many pipelines it
        // invalidated. `invalidatedKeys`, when given, receives every key that
        // was created or requested, so the precache can request the same
        // targets again against the rebuilt shader (#1345: dependent requests
        // are re-prepared, never reused stale).
        sizet InvalidateShader(u64 shaderKey, TArray<Key>* invalidatedKeys = nullptr);

        // Enqueue every cached pipeline for deferred destruction and clear
        // the maps (device teardown). Waits for in-flight creations first.
        void ReleaseAll();

        // The last creation failure, kept so a caller can report the REASON
        // rather than a bare null handle (issue #1029). GetOrCreate* return
        // VK_NULL_HANDLE and log on a vkCreate*Pipelines failure; the log is
        // not reachable from a test assertion, and "the handle was null" does
        // not say which shader or which VkResult. Recorded under the same
        // lock the creation publishes under, so it is the failure of the call
        // that just returned null on this thread as long as no other thread
        // failed in between — a diagnostic, never a control-flow input.
        struct CreationFailure
        {
            bool Valid = false; ///< False until a creation has failed.
            std::string ShaderName;
            VkResult Result = VK_SUCCESS;
        };

        [[nodiscard]] CreationFailure GetLastCreationFailure() const
        {
            std::shared_lock lock(m_Mutex);
            return m_LastCreationFailure;
        }

        void ClearLastCreationFailure()
        {
            std::lock_guard<std::shared_mutex> lock(m_Mutex);
            m_LastCreationFailure = {};
        }

        [[nodiscard]] sizet GetCachedPipelineCount() const
        {
            std::shared_lock lock(m_Mutex);
            return m_Pipelines.size();
        }

        // One shader's pipelines (issue #607). InvalidateShader erases every
        // entry for the key, so after a reload `Live` counts only pipelines
        // built from the NEW modules, and `Failed` says some variant's latest
        // creation attempt failed since then. This is what lets
        // olo_shader_reload report `ready` only when the reload reached a PSO.
        struct ShaderPipelines
        {
            sizet Live = 0;
            bool Failed = false;
            VkResult LastFailure = VK_SUCCESS;
        };
        [[nodiscard]] ShaderPipelines GetShaderPipelines(u64 shaderKey) const;

        // Test seam (#1345): the next `count` creations report `result`
        // instead of calling the driver, so the failed-creation accounting can
        // be pinned without provoking a real driver failure (#994: a driver
        // that rejects a pipeline may crash instead of returning an error).
        void FailNextCreationsForTesting(u32 count, VkResult result = VK_ERROR_UNKNOWN);

      private:
        VulkanPipelineBuilder() = default;

        // Everything one creation reads, copied on the thread that asked for
        // it. The worker builds the create info from this alone.
        struct StageModule
        {
            VkShaderStageFlagBits Stage = VK_SHADER_STAGE_VERTEX_BIT;
            VkShaderModule Module = VK_NULL_HANDLE;
        };
        struct CreateInputs
        {
            bool Compute = false;
            u64 ShaderKey = 0;
            FString ShaderName;
            u32 StageMask = 0;
            TArray<StageModule> Modules;
            bool Mesh = false;
            VulkanRootDataLayout Layout;
            VulkanRecordedPipelineState State;
            VulkanRenderTargetDesc Targets;
        };
        struct CreateResult
        {
            VkPipeline Pipeline = VK_NULL_HANDLE;
            VkResult Result = VK_SUCCESS;
            f64 Milliseconds = 0.0;
            PipelineDriverCacheResult Cache = PipelineDriverCacheResult::Unknown;
        };

        struct Entry
        {
            VkPipeline Pipeline = VK_NULL_HANDLE;
            bool Prepared = false;                           ///< Created through a request, not by a draw.
            mutable std::atomic<bool> FirstUseSeen{ false }; ///< The first draw that used it has been counted.
            FString ShaderName;                              ///< For the manifest record a hit refreshes.
            u32 StageMask = 0;
            mutable std::atomic<u32> RecordedEpoch{ 0 }; ///< The recording epoch this key was last recorded in.

            Entry(VkPipeline pipeline, bool prepared, FString shaderName, u32 stageMask, u32 recordedEpoch)
                : Pipeline(pipeline), Prepared(prepared), FirstUseSeen(!prepared), ShaderName(std::move(shaderName)),
                  StageMask(stageMask), RecordedEpoch(recordedEpoch)
            {
            }
        };

        // The §4 mapping array for one root layout — shared verbatim by the
        // graphics and compute paths so the two cannot drift. Samplers come
        // from the sampler heap (see GetOrCreateGraphics), never embedded.
        [[nodiscard]] static std::vector<VkDescriptorSetAndBindingMappingEXT>
        BuildBindingMappings(const VulkanRootDataLayout& layout);

        [[nodiscard]] static CreateInputs SnapshotGraphics(VulkanShader& shader, const VulkanRootDataLayout& layout,
                                                           const VulkanRecordedPipelineState& state,
                                                           const VulkanRenderTargetDesc& targets);
        [[nodiscard]] static CreateInputs SnapshotCompute(u64 shaderKey, VkShaderModule module,
                                                          const VulkanRootDataLayout& layout,
                                                          std::string_view shaderName);
        // The driver call. Holds no builder lock; reads only `inputs`.
        [[nodiscard]] static CreateResult CreatePipeline(const CreateInputs& inputs, const Key& key,
                                                         VkPipelineCache cache);

        // A hit: return the pipeline, counting a prepared pipeline's first use.
        [[nodiscard]] VkPipeline LookUp(const Key& key);
        // The miss path shared by graphics and compute: classify, create or
        // wait, publish. Called without the lock; `inputs` is the snapshot.
        [[nodiscard]] VkPipeline ResolveMiss(const Key& key, const CreateInputs& inputs);
        bool Request(const Key& key, CreateInputs inputs, PipelineRequestPriority priority);
        // Run one creation the caller has claimed (lock NOT held), then publish.
        VkPipeline CreateAndPublish(const Key& key, const CreateInputs& inputs, u64 ticket, bool blocking,
                                    PipelineMissKind missKind);

        // Readers (the per-draw hit path) share; a miss upgrades to exclusive
        // for the bookkeeping, re-checking first. The key is built OUTSIDE the
        // lock — hashing a root layout per draw under a mutex was the first
        // contention the #806 measurement found.
        mutable std::shared_mutex m_Mutex;
        std::condition_variable_any m_CreationDone; ///< Notified after every publish.
        std::unordered_map<Key, Entry, KeyHash> m_Pipelines;
        PipelineRequestLedger<Key, KeyHash, CreateInputs> m_Ledger;
        std::unordered_map<u64, u32> m_InFlightByShader; ///< Creations running now, per shader key.
        u64 m_Completions = 0;
        std::atomic<u32> m_RecordEpoch{ 1 }; ///< Entries start at 0, so a session's first draw records.
        u32 m_LoggedMisses = 0;
        u32 m_FailNextCreations = 0;
        VkResult m_FailNextResult = VK_ERROR_UNKNOWN;
        CreationFailure m_LastCreationFailure;
        // Pipeline keys (every variant: target formats, blend, layout) whose
        // latest creation failed. Keyed by the FULL key, so a variant that
        // succeeds cannot clear another variant's failure. Cleared per key by
        // that key's next successful creation, per shader by InvalidateShader.
        std::unordered_map<Key, VkResult, KeyHash> m_FailedPipelines;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
