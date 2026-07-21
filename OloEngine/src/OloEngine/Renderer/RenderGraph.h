#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/TransientPool.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include <functional>
#include <limits>
#include <map>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <glm/glm.hpp>

namespace OloEngine
{
    // Lightweight string-interner used by RenderGraph to convert resource and
    // pass names into stable u32 IDs. Hot-path maps store IDs as keys so
    // lookups avoid the per-call string hashing + comparison cost that
    // dominates MSVC Debug profiles of `PopulateBlackboard`.
    //
    // Two instances live on RenderGraph: `m_ResourceNames` (texture /
    // framebuffer / buffer / view names) and `m_PassNames` (pass / node
    // names). Keep them separate so resource and pass IDs do not collide in
    // any shared map (none today, but the contract is enforced by
    // construction).
    //
    // ID 0 is reserved as the invalid sentinel; the first real interned ID
    // is 1. `Intern` adds; `Find` is read-only.
    // Heterogeneous hash + equality so a `std::unordered_map<std::string, V>`
    // can be looked up via `std::string_view` / `const char*` without
    // allocating a temporary `std::string` per call. Use these as the third
    // and fourth template arguments to `unordered_map` / `unordered_set` to
    // get free transparent lookup. Also used internally by `RGStringInterner`
    // for the same reason.
    struct RGStringTransparentHash
    {
        using is_transparent = void;
        [[nodiscard]] size_t operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view>{}(sv);
        }
        [[nodiscard]] size_t operator()(const std::string& s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
        [[nodiscard]] size_t operator()(const char* s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };
    struct RGStringTransparentEqual
    {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept
        {
            return a == b;
        }
        [[nodiscard]] bool operator()(const std::string& a, std::string_view b) const noexcept
        {
            return a == b;
        }
        [[nodiscard]] bool operator()(std::string_view a, const std::string& b) const noexcept
        {
            return a == b;
        }
        [[nodiscard]] bool operator()(const std::string& a, const std::string& b) const noexcept
        {
            return a == b;
        }
    };

    template<typename V>
    using RGTransparentStringMap = std::unordered_map<std::string, V, RGStringTransparentHash, RGStringTransparentEqual>;
    using RGTransparentStringSet = std::unordered_set<std::string, RGStringTransparentHash, RGStringTransparentEqual>;

    class RGStringInterner
    {
      public:
        // Returns a stable u32 ID for `name`. Same name -> same ID across
        // the lifetime of the interner. Empty names return 0.
        u32 Intern(std::string_view name)
        {
            if (name.empty())
                return 0u;
            if (const auto it = m_IDByName.find(name); it != m_IDByName.end())
                return it->second;
            const auto id = static_cast<u32>(m_NameByID.size());
            m_NameByID.emplace_back(name);
            // Reference into the owned NameByID vector so the map's key is
            // stable; entries are append-only so this is safe.
            m_IDByName.emplace(m_NameByID.back(), id);
            return id;
        }

        [[nodiscard]] u32 Find(std::string_view name) const
        {
            if (name.empty())
                return 0u;
            if (const auto it = m_IDByName.find(name); it != m_IDByName.end())
                return it->second;
            return 0u;
        }

        // Returns an owning std::string by value, not a view into m_NameByID.
        // m_NameByID is a std::vector<std::string>; an Intern() call between
        // a NameOf() and its use can realloc the vector and free the SSO
        // buffer the view pointed at, producing a heap-use-after-free. See
        // RenderGraph::GetResourceName for the same hazard at the public
        // API surface.
        [[nodiscard]] std::string NameOf(u32 id) const
        {
            if (id == 0u || id >= m_NameByID.size())
                return {};
            return m_NameByID[id];
        }

        [[nodiscard]] sizet Size() const
        {
            // Index 0 is reserved as the invalid sentinel.
            return m_NameByID.empty() ? 0u : (m_NameByID.size() - 1u);
        }

        void Clear()
        {
            m_IDByName.clear();
            m_NameByID.clear();
            // Reserve slot 0 as the invalid sentinel.
            m_NameByID.emplace_back();
        }

        RGStringInterner()
        {
            // Reserve slot 0 as the invalid sentinel so a default-constructed
            // ID always means "unknown".
            m_NameByID.emplace_back();
        }

      private:
        RGTransparentStringMap<u32> m_IDByName;
        std::vector<std::string> m_NameByID; // index 0 unused (invalid sentinel)
    };

    // @brief Manages a graph of render nodes forming a complete rendering pipeline.
    class RenderGraph : public RefCounted
    {
      public:
        RenderGraph() = default;
        ~RenderGraph() = default;

        friend class RGBuilder;

        void Init(u32 width, u32 height);
        void Shutdown();

        // -------------------------------------------------------------------
        // Dynamic Resolution Scaling
        // -------------------------------------------------------------------
        // Set the render scale applied to the current physical dimensions.
        // Scale is clamped to [0.25, 1.0]. Calls ApplyRenderViewport() on all
        // registered passes WITHOUT reallocating any GPU resources — only the
        // glViewport dimensions change. The physical FBO size is unchanged.
        // At scale 1.0 the DRS override is cleared (viewport == physical size).
        void SetRenderScale(f32 scale);

        // Return the render viewport dimensions (physical size * render scale).
        [[nodiscard]] u32 GetRenderWidth() const;
        [[nodiscard]] u32 GetRenderHeight() const;

        // Return the current render scale [0.25, 1.0].
        [[nodiscard]] f32 GetRenderScale() const
        {
            return m_RenderScale;
        }

        // Physical (display) framebuffer size from the last Resize(). This is the
        // full output resolution, independent of any FSR1 reduced-size scene band,
        // so callers can size the display-res post chain / EASU output correctly.
        [[nodiscard]] u32 GetPhysicalWidth() const
        {
            return m_PhysicalWidth;
        }
        [[nodiscard]] u32 GetPhysicalHeight() const
        {
            return m_PhysicalHeight;
        }

        // Return (renderWidth / physicalWidth, renderHeight / physicalHeight).
        // Shaders can use this to clamp UV coordinates so screen-space passes
        // that read from a DRS-scaled buffer don't sample uninitialized texels.
        [[nodiscard]] glm::vec2 GetRenderScaleBounds() const;

        // Clear all topology bookkeeping (passes, nodes, edges, cached
        // execution order/plans) WITHOUT touching
        // the graph entry objects themselves.
        // Used by `Renderer3D::ConfigureRenderGraph(RenderingPath)` to rebuild
        // the graph when the user switches between Forward / Forward+ /
        // Deferred at runtime. Because graph entries are owned externally as
        // `Ref<>`s on `Renderer3D::s_Data`, their framebuffers and internal
        // state survive the reset. Callers must re-`AddNode` every
        // graph entry they want in the new topology and re-issue all edges
        // before calling `SetFinalPass` / `ValidateResourceHazards` again.
        void ResetTopology();

        void AddNode(const Ref<RenderGraphNode>& node);
        // Connect two graph entries: establishes execution ordering.
        void ConnectPass(const std::string& outputPass, const std::string& inputPass);

        // Add an execution-ordering dependency without framebuffer piping.
        // Use this when the upstream pass produces outputs consumed via texture bindings
        // rather than framebuffer attachments (e.g., shadow maps).
        void AddExecutionDependency(const std::string& beforePass, const std::string& afterPass, bool persistent = true);

        // Execute all passes in the correct order
        void Execute();

        // Resize all passes in the graph
        void Resize(u32 width, u32 height);

        // Set the final pass in the graph
        void SetFinalPass(const std::string& passName);

        template<typename T>
        Ref<T> GetNode(const std::string& name)
        {
            if (m_NodeLookup.find(name) != m_NodeLookup.end())
            {
                return m_NodeLookup.at(name).As<T>();
            }
            return nullptr;
        }

        template<typename T>
        Ref<T> GetNode(const std::string& name) const
        {
            if (m_NodeLookup.find(name) != m_NodeLookup.end())
            {
                return m_NodeLookup.at(name).As<T>();
            }
            return nullptr;
        }

        [[nodiscard]] bool IsFinalPass(const std::string& passName) const;
        [[nodiscard]] const std::string& GetFinalPassName() const
        {
            return m_FinalPassName;
        }

        struct ConnectionInfo
        {
            std::string OutputPass;
            std::string InputPass;
            u32 AttachmentIndex = 0;
        };

        [[nodiscard]] std::vector<ConnectionInfo> GetConnections() const;

        struct NodeSubmissionInfo
        {
            std::string NodeName;
            bool DeclaresResources = false;
            RenderGraphPassWorkType WorkType = RenderGraphPassWorkType::Graphics; // Scheduler metadata
            bool AsyncComputeCandidate = false;                                   // Scheduler metadata
        };
        [[nodiscard]] std::vector<NodeSubmissionInfo> GetNodeSubmissionInfo() const;

        // @brief Get the topologically-sorted node execution order (for testing/inspection).
        [[nodiscard]] const std::vector<std::string>& GetExecutionOrder() const
        {
            return m_ExecutionOrder;
        }

        // Validates only the dependency topology/cycle state without
        // inspecting resource declarations. Use this before frame
        // compilation when the caller needs a cycle-free graph but compiled
        // resource validation will happen later from authoritative
        // RGBuilder-produced accesses.
        [[nodiscard]] bool ValidateExecutionTopology();

        // -------------------------------------------------------------------
        // Resource-aware hazard validation
        // -------------------------------------------------------------------
        // Validates resource hazards using the best declaration source
        // currently available:
        //  - before BuildFrameGraph(), it uses legacy DeclareRead/DeclareWrite
        //    metadata for topology-time validation;
        //  - after BuildFrameGraph() has produced dynamic RGBuilder accesses,
        //    it automatically switches to compiled-frame validation using the
        //    current frame's declarations and culling state.
        // This catches missing `ConnectPass` / `AddExecutionDependency` calls
        // during topology composition while still making built frames validate
        // against setup-authoritative resource contracts.
        // Production runtime code should treat pre-build results as
        // diagnostics only; authoritative correctness comes from
        // ValidateCompiledResourceHazards() after BuildFrameGraph().
        //
        // Call this AFTER all passes are added and connections made — the
        // first call forces topological sort if needed. Returns the list of
        // hazards (empty vector = clean). Also logs each hazard as an error
        // through the engine logger.
        enum class HazardKind
        {
            ReadAfterWrite,                 // reader does not depend on writer
            WriteAfterWrite,                // later writer does not depend on previous writer
            WriteAfterRead,                 // later writer does not depend on prior reader
            ResourceKindMismatch,           // same logical resource declared with conflicting kinds
            FeedbackWithoutDeclaration,     // same pass reads+writes overlapping subresource
            ImportedResourceLifetimeMisuse, // imported resource is used without valid backing
            Cycle,                          // dependency graph has a cycle; hazards could not be validated
        };
        struct Hazard
        {
            HazardKind Kind;
            std::string Resource;
            std::string Producer; // writer (RAW / WAW) or reader (WAR)
            std::string Consumer; // reader (RAW), later writer (WAW / WAR)
            std::string Message;
        };
        [[nodiscard]] std::vector<Hazard> ValidateResourceHazards();

        // Validates the compiled frame after BuildFrameGraph() using the
        // current frame's dynamic RGBuilder declarations and culling state.
        // This is the authoritative per-frame validation path for graph-native
        // nodes whose setup contract can legitimately declare zero accesses
        // when disabled.
        [[nodiscard]] std::vector<Hazard> ValidateCompiledResourceHazards();

        struct ResourceInfo
        {
            std::string Name;
            RGResourceDesc Desc;
            bool HasExternalBacking = false;
            RGTextureHandle TextureHandle;
            RGBufferHandle BufferHandle;
            RGFramebufferHandle FramebufferHandle;
            std::vector<std::string> Producers;
            std::vector<std::string> Consumers;
        };

        void ImportResource(std::string_view name, const RGResourceDesc& desc);
        void ClearImportedResources();
        [[nodiscard]] const std::vector<ResourceInfo>& GetRegisteredResources() const;
        [[nodiscard]] const ResourceInfo* FindRegisteredResource(std::string_view name) const;
        // Base-name lookups return the latest explicit version when one exists
        // (for example "SceneColor" -> "SceneColor@PassB"). Exact
        // versioned names still resolve verbatim. Falls through to the
        // base-name alias table (see RegisterTextureAlias / RegisterFramebufferAlias)
        // when the name itself is not registered — used for dynamic-chain
        // names like "PostProcessColor" that point at whichever upstream
        // pass produced the latest output for the active configuration.
        [[nodiscard]] RGTextureHandle GetTextureHandle(std::string_view name) const;
        [[nodiscard]] RGBufferHandle GetBufferHandle(std::string_view name) const;
        [[nodiscard]] RGFramebufferHandle GetFramebufferHandle(std::string_view name) const;

        // -------------------------------------------------------------------
        // Base-name aliases
        // -------------------------------------------------------------------
        // Register `aliasName` as a synonym for `targetBaseName` so
        // GetTextureHandle / GetFramebufferHandle queries against the alias
        // resolve through to the latest versioned producer of the target.
        // Intended for "logical chain endpoints" — e.g.
        // `RegisterFramebufferAlias("PostProcessColor", "AOApplyColor")` so
        // downstream post-process passes that read by the abstract name
        // `PostProcessColor` get the AOApply output without hard-coding
        // every possible upstream. Pipeline code re-registers each frame
        // (PopulateBlackboard) based on which upstream is active.
        // Cleared by ClearImportedResources().
        void RegisterTextureAlias(std::string_view aliasName, std::string_view targetBaseName);
        void RegisterFramebufferAlias(std::string_view aliasName, std::string_view targetBaseName);

        // Debug accessors: read-only views into the alias and latest-version
        // tables. Used by the Render Graph Debugger to surface what
        // GetTextureHandle / GetFramebufferHandle would resolve to without
        // mutating state. Not for runtime use.
        // Resolved alias snapshots for the debugger. Built on demand from
        // the id-keyed internal maps so the storage layout can stay packed
        // without forcing the debugger to learn about interned IDs.
        [[nodiscard]] std::map<std::string, std::string> GetTextureBaseNameAliases() const;
        [[nodiscard]] std::map<std::string, std::string> GetFramebufferBaseNameAliases() const;
        [[nodiscard]] const RGTransparentStringMap<RGTextureHandle>& GetLatestTextureHandlesByBaseName() const
        {
            return m_LatestTextureHandlesByBaseName;
        }
        [[nodiscard]] const RGTransparentStringMap<RGFramebufferHandle>& GetLatestFramebufferHandlesByBaseName() const
        {
            return m_LatestFramebufferHandlesByBaseName;
        }

        // Reverse-lookup: given a handle returned by ImportTexture /
        // CreateFramebufferAttachmentView / WriteNewVersion, find the
        // canonical registered name. Returns the first matching ResourceInfo
        // name, or an empty string if no registered resource owns this
        // handle. O(N) over registered resources — debug-only.
        [[nodiscard]] std::string ReverseResolveTextureName(RGTextureHandle handle) const;
        [[nodiscard]] std::string ReverseResolveFramebufferName(RGFramebufferHandle handle) const;

        // If `name` is a texture view created via CreateFramebufferAttachmentView,
        // returns the parent framebuffer's registered resource name. Empty
        // string when the name is not a known attachment view. Used by the
        // RGBuilder to propagate texture-view reads to the parent framebuffer's
        // lifetime so the transient planner doesn't alias a parent framebuffer
        // with a downstream pass that's still reading one of its attachments.
        [[nodiscard]] std::string FindAttachmentViewParent(std::string_view name) const;

        // Returns the most recent pass that wrote the named resource during
        // BuildFrameGraph's Setup loop, or an empty string if none. Updated
        // incrementally as each pass's Setup runs, so a downstream RMW pass
        // can call this from its own Setup to discover its predecessor and
        // emit an explicit DependsOnPass edge without the pipeline builder
        // needing to wire pass pointers via class-specific setters.
        [[nodiscard]] auto GetLastWriterPassName(std::string_view resourceName) const -> const std::string&;
        [[nodiscard]] bool IsTextureHandleCurrent(RGTextureHandle handle) const;
        [[nodiscard]] bool IsBufferHandleCurrent(RGBufferHandle handle) const;
        [[nodiscard]] bool IsFramebufferHandleCurrent(RGFramebufferHandle handle) const;

        // -------------------------------------------------------------------
        // Typed import / resolve / extract API
        // -------------------------------------------------------------------
        // Import a physical GL texture into the graph under a canonical name.
        // Returns a typed handle that can be stored in the FrameBlackboard or
        // passed to pass DeclareRead/DeclareWrite via the string overload.
        // `textureID == 0` is accepted for optional resources (e.g. velocity
        // when no velocity buffer exists) — the handle is still valid but
        // resolve will return 0.
        [[nodiscard]] RGTextureHandle ImportTexture(std::string_view name, u32 textureID,
                                                    const RGResourceDesc& desc = {});

        // Import a physical Framebuffer. `fb` may be null for optional targets.
        [[nodiscard]] RGFramebufferHandle ImportFramebuffer(std::string_view name,
                                                            const Ref<Framebuffer>& fb,
                                                            const RGResourceDesc& desc = {});

        // Create a texture view that resolves to a framebuffer colour
        // attachment. The returned texture handle shares the framebuffer's
        // physical storage and is intended for explicit attachment-level graph
        // declarations (for example OIT accum/revealage views over one MRT).
        [[nodiscard]] RGTextureHandle CreateFramebufferAttachmentView(std::string_view name,
                                                                      RGFramebufferHandle framebufferHandle,
                                                                      u32 colorAttachmentIndex);

        // Create a texture view that resolves to a framebuffer depth
        // attachment. The returned texture handle shares the framebuffer's
        // physical storage and is intended for explicit depth-view modelling
        // (for example deferred SceneDepth / SceneDepthMS attachment views).
        [[nodiscard]] RGTextureHandle CreateFramebufferDepthAttachmentView(std::string_view name,
                                                                           RGFramebufferHandle framebufferHandle);

        // Create a logical single-mip texture view over an existing texture
        // resource. The returned handle participates in graph declarations as
        // its own resource while preserving the parent texture's storage and
        // descriptor metadata. Under the GL backend this currently resolves to
        // the parent texture object; callers that need a specific mip still
        // select it explicitly when binding images/framebuffers.
        [[nodiscard]] RGTextureHandle CreateTextureMipView(std::string_view name,
                                                           RGTextureHandle textureHandle,
                                                           u32 mipLevel);

        // Create a logical single-layer texture-array view over an existing
        // Texture2DArray resource. Like mip views, the returned handle models
        // the declaration surface as its own resource while still resolving to
        // the parent texture object under the current GL backend.
        [[nodiscard]] RGTextureHandle CreateTextureArrayLayerView(std::string_view name,
                                                                  RGTextureHandle textureHandle,
                                                                  u32 layerIndex);

        // The array layer / cube face a *subresource view* resource addresses
        // within its parent texture, or 0 for any other resource (issue #607).
        //
        // ResolveTexture() deliberately returns the PARENT texture object for a
        // layer/face view — that is what a sampler binding wants. A CPU readback
        // (glGetTextureSubImage) does NOT: given only the parent id it reads
        // layer 0, so capturing "ShadowMapCSMCascade3" would silently hand back
        // cascade 0's pixels — a confidently wrong answer, not an error. Callers
        // that read a view back must apply this offset themselves.
        [[nodiscard]] u32 GetTextureViewLayerIndex(std::string_view name) const;

        // Create a logical single-face cube texture view over an existing
        // TextureCube resource. The handle participates in declarations as a
        // face-scoped resource while resolving to the parent cubemap object in
        // the current GL backend.
        [[nodiscard]] RGTextureHandle CreateTextureCubeFaceView(std::string_view name,
                                                                RGTextureHandle textureHandle,
                                                                u32 faceIndex);

        // Create a logical single-sample resolve view over an existing
        // multisample texture resource. The returned handle participates in
        // declarations as its own resource, resolves to `resolvedTextureHandle`
        // under the current GL backend, and inherits producer ordering from
        // `multisampleTextureHandle` so MSAA source writers can feed resolved
        // readers without inventing a parallel naming scheme.
        [[nodiscard]] RGTextureHandle CreateTextureMultisampleResolveView(std::string_view name,
                                                                          RGTextureHandle multisampleTextureHandle,
                                                                          RGTextureHandle resolvedTextureHandle);

        // Import a UBO or SSBO by its GL name/binding.
        [[nodiscard]] RGBufferHandle ImportBuffer(std::string_view name, u32 bufferID,
                                                  const RGResourceDesc& desc = {});

        // Import a temporal history texture (same as ImportTexture but records
        // the IsHistory flag so the debug dump can distinguish live from history
        // resources). Returns an invalid handle when `textureID == 0`.
        [[nodiscard]] RGTextureHandle ImportHistory(std::string_view name, u32 textureID,
                                                    const RGResourceDesc& desc = {});

        // Resolve a handle back to its physical texture ID.
        // Returns 0 for an invalid handle or a handle whose backing resource
        // was imported with textureID == 0.
        [[nodiscard]] u32 ResolveTexture(RGTextureHandle handle) const;

        // Resolve a handle back to its physical Framebuffer.
        // Returns nullptr for an invalid handle or a null-imported framebuffer.
        [[nodiscard]] Ref<Framebuffer> ResolveFramebuffer(RGFramebufferHandle handle) const;

        // Resolve a handle back to its physical buffer ID.
        // Returns 0 for an invalid handle.
        [[nodiscard]] u32 ResolveBuffer(RGBufferHandle handle) const;

        // Resolve a typed handle back to its current graph resource name.
        // For opt-in versioned writes this may be a derived version name
        // rather than the original canonical blackboard/import name.
        // Returns empty when the handle is invalid or stale.
        // Returns an owning std::string by value (not a string_view into slot
        // storage). Callers frequently hold the result across calls that
        // mutate the handle-slot vectors (e.g. AllocateTransient*),
        // which can realloc and invalidate any view into a slot's inline
        // SSO buffer — yielding a heap-use-after-free that sanitizer builds
        // (TSan/ASan) catch reliably. Returning by value makes the API
        // safe-by-construction; the SSO buffer of the returned string lives
        // in the caller's stack frame and is unaffected by graph mutations.
        [[nodiscard]] std::string GetResourceName(RGTextureHandle handle) const;
        [[nodiscard]] std::string GetResourceName(RGFramebufferHandle handle) const;
        [[nodiscard]] std::string GetResourceName(RGBufferHandle handle) const;

        // Queue an extraction callback that fires after Execute() completes.
        // The callback receives the resolved texture ID. Useful for persisting
        // a resource across frames (e.g. TAA history write-back).
        void ExtractTexture(RGTextureHandle handle, std::function<void(u32)> callback);

        struct ExternalTextureSinkContract
        {
            std::string SourceResource;
            ResourceHandle::Kind SourceKind = ResourceHandle::Kind::Unknown;
            u32 ColorAttachmentIndex = 0;
            bool SourceReachable = false;
        };

        // Register a persistent sink texture for a current-frame resource.
        // Unlike temporal histories, this does not create a previous-frame
        // readable import — it simply copies the named source resource into a
        // caller-owned texture after Execute() completes.
        void RegisterExternalTextureSink(RGTextureHandle sourceHandle,
                                         u32 textureID,
                                         u32 width,
                                         u32 height,
                                         bool* validFlag = nullptr);
        void RegisterExternalTextureSink(RGFramebufferHandle sourceHandle,
                                         u32 textureID,
                                         u32 width,
                                         u32 height,
                                         u32 colorAttachmentIndex = 0,
                                         bool* validFlag = nullptr);
        void RegisterExternalTextureSink(std::string_view sourceResource,
                                         u32 textureID,
                                         u32 width,
                                         u32 height,
                                         u32 colorAttachmentIndex = 0,
                                         bool* validFlag = nullptr);
        [[nodiscard]] const std::vector<ExternalTextureSinkContract>& GetExternalTextureSinkContracts() const
        {
            return m_ExternalTextureSinkContracts;
        }

        // Register a persistent sink texture for a temporal history resource.
        // This allows the graph to write next-frame history even when the
        // previous-frame history was not imported as a readable input.
        void RegisterHistoryTextureSink(std::string_view historyResource,
                                        u32 textureID,
                                        u32 width,
                                        u32 height,
                                        bool* validFlag = nullptr);

        // Queue a texture extraction that explicitly writes back into a named
        // imported history resource for the next frame. The callback receives
        // the resolved source texture ID after Execute() completes.
        struct TemporalHistoryContract
        {
            enum class SourceKind : u8
            {
                Texture,
                Framebuffer,
            };

            std::string HistoryResource; ///< canonical imported-history name (previous-frame input)
            std::string SourceResource;  ///< current-frame resource extracted for next-frame reuse
            SourceKind Kind = SourceKind::Texture;
            u32 ColorAttachmentIndex = 0;
            bool HistoryImported = false;
            bool SourceReachable = false;
        };
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGTextureHandle sourceHandle);
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGTextureHandle sourceHandle,
                                   std::function<void(u32)> callback);
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGFramebufferHandle sourceHandle,
                                   u32 colorAttachmentIndex = 0);
        void ExtractHistoryTexture(std::string_view historyResource,
                                   RGFramebufferHandle sourceHandle,
                                   std::function<void(u32)> callback,
                                   u32 colorAttachmentIndex = 0);
        [[nodiscard]] const std::vector<TemporalHistoryContract>& GetTemporalHistoryContracts() const
        {
            return m_TemporalHistoryContracts;
        }

        // Queue a framebuffer extraction callback.
        void ExtractFramebuffer(RGFramebufferHandle handle, std::function<void(Ref<Framebuffer>)> callback);

        // Fire all pending extraction callbacks and clear the queue.
        // Called automatically by Execute() after all passes have run.
        void FlushExtractions();

        // -------------------------------------------------------------------
        // Frame blackboard
        // -------------------------------------------------------------------
        // Frame blackboard
        // -------------------------------------------------------------------
        // The blackboard is populated by RenderPipeline::PopulateBlackboard(...)
        // at the start of each frame. Passes must not write to the
        // blackboard directly — they only read handles from it to feed their
        // DeclareRead/DeclareWrite calls, or to resolve physical resources
        // via ResolveTexture/ResolveFramebuffer inside Execute().
        [[nodiscard]] FrameBlackboard& GetBlackboard()
        {
            return m_Blackboard;
        }
        [[nodiscard]] const FrameBlackboard& GetBlackboard() const
        {
            return m_Blackboard;
        }
        void ClearBlackboard()
        {
            m_Blackboard.Reset();
        }

        // Monotonic generation of the current topology; bumped whenever the
        // graph is torn down (ResetTopology / Reset), which also wipes the
        // blackboard and imported-resource maps. Hash this into any external
        // per-frame cache that assumes the blackboard survived between calls
        // so a reconfigure invalidates it even when nothing else changed.
        [[nodiscard]] u64 GetTopologyGeneration() const
        {
            return m_TopologyGeneration;
        }

        // -------------------------------------------------------------------
        // Transient resource pool access
        // -------------------------------------------------------------------
        // Get the transient pool for acquiring/releasing resources.
        // Call ReleaseAll() after frame execution to return acquired resources.
        [[nodiscard]] TransientPool& GetTransientPool()
        {
            return m_TransientPool;
        }

        // Single chokepoint for the "a graph node's framebuffer changed size =>
        // the transient pool must be evicted" rule (issue #563). The pool keys
        // its buckets by full spec (incl. width/height), so any node resize can
        // leave stale-size (and paired stale-size) transients that the
        // alias-group resolver then hands to a downstream pass. Resize() calls
        // this after its node-resize loop; call sites that resize a node's
        // framebuffer OUT OF BAND — without going through Resize(), e.g.
        // RenderPipeline's FSR1 scene-band resize on a runtime Upscale-mode
        // toggle — must call this instead of reaching into GetTransientPool()
        // directly, so every direct-resize path shares one eviction decision.
        void NotifyNodeFramebufferResized()
        {
            m_TransientPool.Clear();
        }

        // -------------------------------------------------------------------
        // Builder support for transient resource allocation
        // -------------------------------------------------------------------
        // Allocate a handle for a new transient (virtual) resource.
        // The physical resource is allocated and managed by the graph's
        // transient pool. Until then, these are stubs.
        [[nodiscard]] RGTextureHandle AllocateTransientTextureHandle(std::string_view name, const RGResourceDesc& desc);
        [[nodiscard]] RGFramebufferHandle AllocateTransientFramebufferHandle(std::string_view name, const RGResourceDesc& desc);
        [[nodiscard]] RGBufferHandle AllocateTransientBufferHandle(std::string_view name, const RGResourceDesc& desc);

        // -------------------------------------------------------------------
        // Pre-build transient resource declaration
        // -------------------------------------------------------------------
        // Declare a transient texture resource BEFORE BuildFrameGraph().
        // Suitable for use during frame-blackboard population where a stable
        // handle is needed before pass setup callbacks run. Returns a stable
        // RGTextureHandle that remains valid until the graph is reset.
        [[nodiscard]] RGTextureHandle DeclareTransientTexture(std::string_view name, const RGResourceDesc& desc);

        // Declare a transient texture resource that resolves to caller-
        // supplied physical backing for this frame. The graph still treats
        // the resource as frame-local/transient for lifetime/diagnostic
        // purposes, but skips transient-pool materialization and resolves the
        // handle to the provided texture object.
        [[nodiscard]] RGTextureHandle DeclareTransientTexture(std::string_view name,
                                                              const RGResourceDesc& desc,
                                                              u32 backingTextureID);

        // Declare a transient framebuffer resource BEFORE BuildFrameGraph().
        // Suitable for use during frame-blackboard population (e.g. OIT MRT
        // buffers, post-process outputs) where a stable handle is needed
        // before pass setup callbacks run. MRT framebuffers: set
        // desc.Attachments with
        // one RGResourceFormat per attachment (RT0, RT1, …, Depth).
        // Returns a stable RGFramebufferHandle that remains valid until the
        // graph is reset. Calling this after EnsureResourceRegistryBuilt()
        // marks the registry dirty so the next GetXxxHandle() re-builds.
        [[nodiscard]] RGFramebufferHandle DeclareTransientFramebuffer(std::string_view name, const RGResourceDesc& desc);

        // Declare a transient framebuffer resource that resolves to caller-
        // supplied physical backing for this frame. The graph still treats
        // the resource as frame-local/transient for lifetime/diagnostic
        // purposes, but skips transient-pool materialization and resolves the
        // handle to the provided framebuffer object.
        [[nodiscard]] RGFramebufferHandle DeclareTransientFramebuffer(std::string_view name,
                                                                      const RGResourceDesc& desc,
                                                                      const Ref<Framebuffer>& backingFramebuffer);

        struct TransientPlanEntry
        {
            std::string Resource;
            ResourceHandle::Kind Kind = ResourceHandle::Kind::Unknown;
            u32 FirstPassIndex = std::numeric_limits<u32>::max();
            u32 LastPassIndex = 0;
            std::string FirstPass;
            std::string LastPass;
            std::string AliasGroup;
            u32 AliasSlot = std::numeric_limits<u32>::max();
            u64 EstimatedBytes = 0;
            bool Reachable = false;
            bool WillAllocate = false;
            std::string SkipReason;
        };

        [[nodiscard]] const std::vector<TransientPlanEntry>& GetTransientPlan() const
        {
            return m_TransientPlan;
        }

        // -------------------------------------------------------------------
        // Per-frame graph building
        // -------------------------------------------------------------------
        // Build the frame graph by running all graph-native node setup callbacks.
        // Each setup callback declares its resource reads/writes to the builder.
        // After setup, the graph is topologically ordered and ready to execute.
        //
        // `cacheFingerprint` is an optional caller-supplied hash of the inputs
        // that affect the per-pass Setup() output. When non-zero, the result
        // of a successful build is cached; on subsequent calls with the same
        // fingerprint (and no externally-marked topology changes) the entire
        // body — Setup loop, reachability, barrier plan, transient plan,
        // submission plan — is skipped. Pass 0 (default) to opt out.
        void BuildFrameGraph(u64 cacheFingerprint = 0u);

        // Marks the cached BuildFrameGraph output as invalid. Call from
        // operations whose effect on the build cannot be captured in the
        // caller's fingerprint (typically node insertion/removal).
        void InvalidateBuildFrameGraphCache()
        {
            m_HasValidBuildFrameGraphCache = false;
        }

        struct FrameBuildStats
        {
            u32 PassesVisited = 0;
            u32 DeclaredReads = 0;
            u32 DeclaredWrites = 0;
            u32 DerivedEdges = 0;
            u32 OrderSensitiveResults = 0;
        };

        [[nodiscard]] const FrameBuildStats& GetLastBuildStats() const
        {
            return m_LastBuildStats;
        }

        enum class BuildDiagnosticKind
        {
            RegistrationOrderSensitivity,
        };

        struct BuildDiagnostic
        {
            BuildDiagnosticKind Kind = BuildDiagnosticKind::RegistrationOrderSensitivity;
            std::string Resource;
            std::string CurrentBeforePass;
            std::string CurrentAfterPass;
            std::string AlternateBeforePass;
            std::string AlternateAfterPass;
            std::string Message;
        };

        [[nodiscard]] const std::vector<BuildDiagnostic>& GetBuildDiagnostics() const
        {
            return m_BuildDiagnostics;
        }

        struct PlannedBarrier
        {
            std::string BeforePass;
            std::string Resource;
            MemoryBarrierFlags Flags = MemoryBarrierFlags::None;
            RGSubresourceRange Range; ///< subresource range from the consuming access declaration
        };

        struct ExecutionTiming
        {
            std::string NodeName;
            f64 CpuMs = 0.0;
        };

        [[nodiscard]] const std::vector<PlannedBarrier>& GetPlannedBarriers() const
        {
            return m_PlannedBarriers;
        }

        enum class BarrierDiagnosticKind
        {
            MissingProducer,
            CulledProducer,
            UnmappedTransition,
            StaleExtractionHandle,
            ExtractionOfCulledResource,
            InvalidHistoryContract,
        };

        struct BarrierDiagnostic
        {
            BarrierDiagnosticKind Kind = BarrierDiagnosticKind::MissingProducer;
            std::string PassName;
            std::string Resource;
            std::string Message;
        };

        [[nodiscard]] const std::vector<BarrierDiagnostic>& GetBarrierDiagnostics() const
        {
            return m_BarrierDiagnostics;
        }

        [[nodiscard]] const std::vector<ExecutionTiming>& GetLastExecutionTimings() const
        {
            return m_LastExecutionTimings;
        }

        struct ResolveFailure
        {
            std::string PassName;
            std::string Reason;
            u32 Count = 0;
        };

        // Record execute-path resource-resolution failures (invalid or stale
        // typed handles, null framebuffer resolves, invalid history sources,
        // and similar graph-contract violations).
        void RecordResolveFailure(std::string_view passName, std::string_view reason) const;

        [[nodiscard]] const std::vector<ResolveFailure>& GetResolveFailures() const
        {
            return m_ResolveFailures;
        }

        // Get the list of passes that were culled in the last reachability
        // analysis. Useful for debugging and frame-capture metadata.
        [[nodiscard]] const std::vector<std::string>& GetCulledPasses() const
        {
            return m_CulledPasses;
        }

        // -------------------------------------------------------------------
        // Debug — Post-pass execution hooks
        // -------------------------------------------------------------------
        // Fired after each pass->Execute() returns inside RenderGraph::Execute(),
        // BEFORE the post-pass extraction phase. Used by debug tooling
        // (e.g. RenderGraphFrameCapture, the MCP afterPass snapshot) to snapshot
        // intermediate state. Multiple listeners can coexist, each registered
        // under a distinct key (issue #607 — the debugger's frame capture and
        // the MCP snapshot must not clobber each other's hook). Listeners fire
        // in registration order; a listener must not add/remove hooks from
        // inside its own callback.
        using PostPassHook = std::function<void(const std::string& passName, RenderGraph& graph)>;
        void AddPostPassHook(std::string_view key, PostPassHook hook);
        void RemovePostPassHook(std::string_view key);
        // Legacy single-slot form: equivalent to Add/Remove under a reserved
        // key. Pass the empty function (or assign {}) to disable.
        void SetPostPassHook(PostPassHook hook)
        {
            if (hook)
                AddPostPassHook("__default", std::move(hook));
            else
                RemovePostPassHook("__default");
        }
        // True when ANY post-pass listener is registered (not just the legacy
        // slot). A tool that needs to know whether ITS hook is installed must
        // track that itself (see RenderGraphFrameCapture::IsHookInstalled).
        [[nodiscard]] bool HasPostPassHook() const
        {
            return !m_PostPassHooks.empty();
        }

        // -------------------------------------------------------------------
        // Batch-event hook
        // -------------------------------------------------------------------
        // Fired when Execute() enters (isBegin=true) or exits (isBegin=false)
        // an async-compute batch boundary.  Useful for tests and profiling
        // tooling that need to verify batch boundary placement without a real
        // GL context.  Pass an empty function (or assign {}) to disable.
        using BatchEventCallback = std::function<void(u32 batchIndex, bool isBegin)>;
        void SetBatchEventHook(BatchEventCallback cb)
        {
            m_BatchEventHook = std::move(cb);
        }
        [[nodiscard]] bool HasBatchEventHook() const
        {
            return static_cast<bool>(m_BatchEventHook);
        }

        void SetRuntimeBarrierExecutionEnabled(const bool enabled)
        {
            m_RuntimeBarrierExecutionEnabled = enabled;
        }

        void SetTransientMaterializationEnabled(const bool enabled)
        {
            m_RuntimeTransientMaterializationEnabled = enabled;
        }

        // Set the maximum number of pool objects retained per descriptor bucket
        // after each frame. Excess objects are evicted by TransientPool::Trim()
        // at the end of Execute(). Default = 2 (keeps one spare for same-descriptor
        // overlapping transients; use 1 for the most aggressive trim).
        void SetTransientPoolMaxBucketSize(u32 maxPerBucket)
        {
            m_TransientPoolMaxBucketSize = maxPerBucket;
        }

        [[nodiscard]] u32 GetTransientPoolMaxBucketSize() const
        {
            return m_TransientPoolMaxBucketSize;
        }

        // @brief Dump the current graph as a Graphviz DOT file.
        //
        // Emits a directed graph where nodes are passes (insertion order,
        // with the final pass double-ringed) and edges are coloured by
        // kind — solid black for framebuffer piping (ConnectPass), dashed
        // grey for ordering-only edges (AddExecutionDependency). Useful
        // for one-off visualisation and for debugging RenderGraph topology
        // changes. Render with `dot -Tsvg graph.dot -o graph.svg`.
        //
        // Returns true on success, false if the file could not be written.
        // The call is read-only — it does not force a topo sort.
        [[nodiscard]] bool DumpToDot(const std::string& filePath) const;

        // @brief Dump compiled graph data as JSON (passes/resources/culling/barriers).
        // Useful for automated tooling and richer offline inspection than DOT.
        [[nodiscard]] bool DumpToJson(const std::string& filePath) const;

      private:
        // Returns false if the graph contains a cycle (m_ExecutionOrder will be
        // partial/empty in that case). Callers must abort further work when
        // false is returned — running validators or Execute() against a
        // partial topo order produces misleading diagnostics.
        [[nodiscard]] bool UpdateDependencyGraph();
        void ResolveFinalPass();

        // Compute-pass scheduling hoist.
        // After UpdateDependencyGraph() builds a valid topological order, this
        // method applies a modified Kahn's pass over m_ExecutionOrder: when multiple
        // passes are ready (all predecessors already scheduled), all
        // AsyncComputeCandidate passes are drained before any graphics pass is
        // advanced. The result is a still-valid topological order that gives
        // compute work a head start — simulating what a multi-queue backend
        // would achieve once async-compute scheduling lands.
        // No-ops when no AsyncComputeCandidate pass is present.
        void HoistComputePasses();

      public:
        // -------------------------------------------------------------------
        // Async-compute batch query
        // -------------------------------------------------------------------
        // A batch groups consecutive AsyncComputeCandidate passes from the
        // hoisted execution order together with the fence metadata needed for
        // queue synchronisation:
        //   WaitNodes  — non-batch nodes this batch must wait for before it
        //                 can start (predecessor graphics work).
        //   SignalNodes — non-batch nodes that must wait for this batch to
        //                 finish before they can start (successor graphics work).
        //
        // Backend mapping:
        //   GL 4.6   : glFenceSync after batch; glClientWaitSync before SignalNodes.
        //   Vulkan   : compute VkSubmitInfo with signal semaphores; graphics
        //              VkSubmitInfo with wait semaphores pointing at the batch.
        //   DX12     : D3D12_COMMAND_LIST_TYPE_COMPUTE + fence Signal/Wait pairs.

        // -------------------------------------------------------------------
        // Cross-batch resource dependency surfacing
        // -------------------------------------------------------------------
        // A resource dependency that crosses an async-compute batch boundary.
        //   ExternalNode — the non-batch node on the other side of the fence:
        //     InputResource  → the non-batch node that last WROTE this resource
        //                      before the batch starts (the producer to wait for).
        //     OutputResource → the non-batch node that first READS this resource
        //                      after the batch ends (the consumer that must wait).
        //
        // Backend usage:
        //   Vulkan : each InputResource maps to a wait-semaphore + image-layout
        //            transition on the compute queue.  Each OutputResource maps to
        //            a signal-semaphore + ownership-transfer release/acquire pair.
        //   DX12   : InputResources drive fence Wait() calls; OutputResources
        //            drive fence Signal() calls + resource-barrier transitions.
        struct BatchResourceDependency
        {
            std::string ResourceName; ///< virtual resource name registered in the graph
            std::string ExternalNode; ///< non-batch node that produces (input) or first consumes (output) this resource
        };

        enum class QueueLane : u8
        {
            Graphics,
            Compute,
            Copy,
        };

        struct AsyncComputeBatch
        {
            std::vector<std::string> ComputeNodes; ///< batch members, in execution order
            std::vector<std::string> WaitNodes;    ///< non-batch nodes this batch waits for
            std::vector<std::string> SignalNodes;  ///< non-batch nodes that wait for this batch
            QueueLane Lane = QueueLane::Compute;   ///< execution lane assignment for this batch
            // Per-resource cross-boundary dependency info
            std::vector<BatchResourceDependency> InputResources;  ///< resources entering the batch from outside
            std::vector<BatchResourceDependency> OutputResources; ///< resources leaving the batch to outside
        };

        // Partition the hoisted execution order into async-compute batches.
        // Returns an empty vector when no AsyncComputeCandidate pass exists.
        // Must be called AFTER Execute() (or after a forced topology update)
        // so that HoistComputePasses() has already run.
        [[nodiscard]] std::vector<AsyncComputeBatch> GetAsyncComputeBatches() const;

        // -------------------------------------------------------------------
        // Submission-plan IR
        // -------------------------------------------------------------------
        // A backend-portable linearised sequence of operations that a
        // Vulkan/DX12/GL renderer can execute without re-reading the graph
        // topology or barrier tables.
        //
        // Command kinds:
        //   Pass          — submit / execute the named pass on the queue
        //                   indicated by PassWorkType.
        //   MemoryBarrier — insert a pipeline/memory barrier with the given
        //                   flags before the next pass begins.
        //   BatchBegin    — start a new async-compute submission slot;
        //                   backends insert a queue-wait here.
        //   BatchEnd      — close the current async-compute slot;
        //                   backends insert a queue-signal / fence here.
        //
        // Backend mapping:
        //   GL 4.6   : BatchEnd → glFenceSync; BatchBegin → glClientWaitSync.
        //              MemoryBarrier → glMemoryBarrier(flags).
        //   Vulkan   : BatchBegin/BatchEnd bound a VkSubmitInfo on the compute
        //              queue with matching wait/signal semaphores.
        //   DX12     : BatchBegin/BatchEnd bound a compute command-list with
        //              fence Signal/Wait pairs.
        struct SubmissionCommand
        {
            enum class Kind : u8
            {
                Pass,
                MemoryBarrier,
                BatchBegin,
                BatchEnd,
            };

            Kind CommandKind = Kind::Pass;
            std::string NodeName;                                                 ///< non-empty for Pass commands
            RenderGraphNode* NodePointer = nullptr;                               ///< cached node pointer to avoid map lookups
            MemoryBarrierFlags Barriers = MemoryBarrierFlags::None;               ///< for MemoryBarrier commands
            u32 BatchIndex = 0;                                                   ///< for BatchBegin/BatchEnd: which async batch
            RenderGraphPassWorkType WorkType = RenderGraphPassWorkType::Graphics; ///< for Pass commands
            QueueLane Lane = QueueLane::Graphics;                                 ///< queue lane assignment for this command

            // Self-contained batch-boundary metadata so
            // backends can map waits/signals/resource ownership transitions
            // directly from GetSubmissionPlan() without side-channel queries.
            std::vector<std::string> WaitNodes;                   ///< for BatchBegin commands
            std::vector<std::string> SignalNodes;                 ///< for BatchEnd commands
            std::vector<BatchResourceDependency> InputResources;  ///< for BatchBegin commands
            std::vector<BatchResourceDependency> OutputResources; ///< for BatchEnd commands
        };

        // Build the submission-plan IR for the current frame.
        // Integrates barrier plan with async-compute batch boundaries
        // into a single linearised command stream.
        // Must be called AFTER Execute() so that barrier planning and
        // compute-hoist have already run.
        [[nodiscard]] std::vector<SubmissionCommand> GetSubmissionPlan() const;

        // -------------------------------------------------------------------
        // Explicit resource transition records
        // -------------------------------------------------------------------
        // For each planned barrier, captures the before-state (producer's
        // write usage) and after-state (consumer's read usage) so
        // explicit-barrier backends can insert the correct image-layout
        // transitions and pipeline-stage masks without re-querying the
        // per-pass access-declaration tables.
        //
        // Each record corresponds one-to-one with an entry in
        // GetPlannedBarriers(): same resource/consumerPass pairing, with the
        // producer's write-usage and the consumer's read-usage added.
        //
        // Backend mapping:
        //   Vulkan  : FromUsage → VkImageLayout (e.g. RenderTarget →
        //             COLOR_ATTACHMENT_OPTIMAL); ToUsage → SHADER_READ_ONLY.
        //             Flags → VkAccessFlags / VkPipelineStageFlags.
        //   DX12    : FromUsage → D3D12_RESOURCE_STATE_RENDER_TARGET;
        //             ToUsage → D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
        //   GL 4.6  : glMemoryBarrier(Flags) already inserted; transition
        //             records are informational until explicit barriers land.
        struct ResourceTransition
        {
            std::string ResourceName;                            ///< virtual resource in the graph
            std::string ProducerPass;                            ///< last writer before this transition; "external" when only imported
            std::string ConsumerPass;                            ///< pass that reads the resource (barrier inserted before it)
            RGWriteUsage FromUsage = RGWriteUsage::RenderTarget; ///< write usage of ProducerPass
            RGReadUsage ToUsage = RGReadUsage::ShaderSample;     ///< read usage of ConsumerPass
            MemoryBarrierFlags Flags = MemoryBarrierFlags::None; ///< barrier flags from PlannedBarrier
            RGSubresourceRange Range;                            ///< subresource range from the consuming access declaration

            // Cross-lane sync metadata (release/acquire intent).
            // Set when ProducerPass and ConsumerPass reside on different queue lanes.
            // On GL 4.6 this is informational only; on Vulkan/DX12 it drives
            // ownership-transfer barriers and semaphore waits.
            bool IsCrossLane = false;                     ///< true when producer and consumer are on different queue lanes
            QueueLane ProducerLane = QueueLane::Graphics; ///< lane of the producing pass; Graphics for "external" producers
            QueueLane ConsumerLane = QueueLane::Graphics; ///< lane of the consuming pass
        };

        // Derive all resource transition records from the current barrier plan
        // and access declarations. Returns an empty vector when no barriers are
        // planned (no declared reads/writes or passes not yet executed).
        // Must be called AFTER Execute() or after BuildFrameGraph() +
        // ComputeBarrierPlan() have run so that m_PlannedBarriers and
        // m_PassAccessDeclarations are populated.
        [[nodiscard]] std::vector<ResourceTransition> GetResourceTransitions() const;

        // -------------------------------------------------------------------
        // Unified resource lifetime records
        // -------------------------------------------------------------------
        // One record per registered resource giving its full first-write /
        // last-read extent in pass-execution order.  Covers ALL resource kinds
        // (transient, imported, history, extracted) so that an explicit-barrier
        // backend (Vulkan / DX12) can schedule image-layout transitions and
        // memory acquire/release without additional bookkeeping.
        //
        //   IsImported  — resource entered via ImportTexture / ImportFramebuffer /
        //                 ImportBuffer; initial layout is driver-defined.
        //   IsExtracted — ExtractTexture / ExtractFramebuffer was called; the
        //                 backend must keep the resource live after the last pass.
        //   IsHistory   — resource is a temporal-history input (imported from
        //                 the previous frame via ImportHistory / ExtractHistoryTexture).
        //   IsTransient — graph-declared frame-local resource. Pool-allocated
        //                 transients can be freed at LastPassIndex+1;
        //                 externally-backed transients retain caller-owned
        //                 backing.
        //   HasExternalBacking — resolves to caller-supplied frame-local
        //                 backing instead of transient-pool materialization.
        //                 Attachment/mip views inherit this flag from the
        //                 parent resource.
        //
        //   FirstWritePassIndex / LastReadPassIndex are indices into GetExecutionOrder().
        //   When no write is found (import-only) FirstWritePassIndex == UINT32_MAX
        //   and FirstWritePass == "external".  When no read is found (write-only /
        //   extracted) LastReadPassIndex == UINT32_MAX and LastReadPass == "".
        struct ResourceLifetime
        {
            std::string ResourceName;
            bool IsImported = false;                                   ///< entered via ImportTexture/ImportFramebuffer/ImportBuffer
            bool IsExtracted = false;                                  ///< has a pending TextureExtract or FramebufferExtract
            bool IsHistory = false;                                    ///< temporal-history resource (ImportHistory)
            bool IsTransient = false;                                  ///< graph-declared frame-local transient (pool-allocated or externally-backed)
            bool HasExternalBacking = false;                           ///< resolves to caller-supplied frame-local backing instead of pool materialization
            u32 FirstWritePassIndex = std::numeric_limits<u32>::max(); ///< index in GetExecutionOrder(); UINT32_MAX when import-only
            u32 LastReadPassIndex = std::numeric_limits<u32>::max();   ///< index in GetExecutionOrder(); UINT32_MAX when write-only
            std::string FirstWritePass;                                ///< name of the first writing pass; "external" when import-only
            std::string LastReadPass;                                  ///< name of the last reading pass; "" when no reads declared
            RGWriteUsage FirstWriteUsage = RGWriteUsage::RenderTarget; ///< usage at first write
            RGReadUsage LastReadUsage = RGReadUsage::ShaderSample;     ///< usage at last read
        };

        // Returns one ResourceLifetime per registered resource, ordered to
        // match GetRegisteredResources().  Available after Execute() or after
        // UpdateDependencyGraph() + ComputeBarrierPlan() have run.
        [[nodiscard]] std::vector<ResourceLifetime> GetResourceLifetimes() const;

      private:
        // -------------------------------------------------------------------
        // Backward reachability analysis and pass culling
        // -------------------------------------------------------------------
        // Compute backward reachability from the final pass.
        // Marks all passes that produce resources consumed (directly or
        // indirectly) by the final pass. Passes not marked as reachable
        // will be culled unless they have side effects.
        void ComputeReachability();
        void ComputeBarrierPlan();
        void LogSubmissionPlanIfChanged();
        [[nodiscard]] static MemoryBarrierFlags ResolveProducerBarrierFlags(RGWriteUsage usage);
        [[nodiscard]] static MemoryBarrierFlags ResolveConsumerBarrierFlags(RGReadUsage usage);

        // Check if a pass is marked as reachable (not culled) after the last
        // ComputeReachability() call. Reachability is recalculated in
        // BuildFrameGraph() and Execute().
        [[nodiscard]] bool IsPassReachable(const std::string& passName) const;
        [[nodiscard]] bool IsHistoryTextureResource(std::string_view resourceName) const;
        [[nodiscard]] bool IsImportedResource(std::string_view resourceName) const;
        [[nodiscard]] bool IsTransientResource(std::string_view resourceName) const;
        [[nodiscard]] bool IsExternallyBackedTransientResource(std::string_view resourceName) const;
        [[nodiscard]] bool IsResourceReachableForExtraction(std::string_view resourceName) const;
        [[nodiscard]] bool ContainsGraphEntry(std::string_view name) const;
        [[nodiscard]] bool IsGraphEntryAsyncComputeCandidate(std::string_view name) const;
        [[nodiscard]] bool IsGraphEntrySideEffecting(std::string_view name) const;
        [[nodiscard]] RenderGraphPassWorkType GetGraphEntryWorkType(std::string_view name) const;
        [[nodiscard]] std::vector<Hazard> ValidateResourceHazardsInternal();

        // String interners — see `RGStringInterner` for rationale.
        // `m_ResourceNames` covers texture / framebuffer / buffer / view
        // names; `m_PassNames` covers pass / node names. Kept separate so
        // resource and pass IDs never share an ID space.
        mutable RGStringInterner m_ResourceNames;
        mutable RGStringInterner m_PassNames;

        std::unordered_map<std::string, Ref<RenderGraphNode>> m_NodeLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_Dependencies;         // Execution ordering
        std::unordered_map<std::string, std::vector<std::string>> m_ExplicitDependencies; // Persistent ordering edges

        std::vector<std::string> m_InsertionOrder; // Graph entry names in registration order (stable topo tie-break)
        std::vector<std::string> m_ExecutionOrder;
        std::string m_FinalPassName;
        bool m_HasExplicitFinalPass = false;
        bool m_DependencyGraphDirty = false;

        // BuildFrameGraph cache: when the caller supplies a fingerprint of the
        // per-frame inputs that drive Setup() output (typically computed in
        // Renderer3D::EndScene from Renderer3DData + pipeline pass state), we
        // skip the Setup loop and all downstream compilation on cache hits.
        u64 m_LastBuildFrameGraphFingerprint = 0u;
        bool m_HasValidBuildFrameGraphCache = false;

        // Dynamic Resolution Scaling state.
        // m_PhysicalWidth/Height reflect the last Resize() call (actual GPU allocation).
        // m_RenderScale is in [0.25, 1.0]; the active render viewport is
        // floor(physical * scale). Both reset on the next Resize().
        u32 m_PhysicalWidth = 0;
        u32 m_PhysicalHeight = 0;
        f32 m_RenderScale = 1.0f;

        // Reachability tracking
        std::unordered_set<std::string> m_ReachablePasses; // Passes that are reachable from final output
        std::vector<std::string> m_CulledPasses;           // Passes that were culled in last analysis

        // Barrier planning/execution
        std::unordered_map<std::string, std::vector<RGAccessDeclaration>> m_PassAccessDeclarations;
        std::unordered_map<std::string, std::vector<RGFeedbackDeclaration>> m_PassFeedbackDeclarations;
        // Parent framebuffers whose transient lifetime a pass extends via an
        // attachment-view write (RGBuilder::GetDeclaredLifetimeExtensions).
        // Consumed only by RenderGraphTransientPlanner — deliberately kept
        // out of m_PassAccessDeclarations; see the comment in RGBuilder::Write.
        std::unordered_map<std::string, std::vector<std::string>> m_PassLifetimeExtensions;
        std::unordered_map<std::string, MemoryBarrierFlags> m_PassBarrierFlags;
        std::vector<PlannedBarrier> m_PlannedBarriers;
        std::vector<BuildDiagnostic> m_BuildDiagnostics;
        std::vector<BarrierDiagnostic> m_BarrierDiagnostics;
        std::vector<ExecutionTiming> m_LastExecutionTimings;
        mutable std::vector<ResolveFailure> m_ResolveFailures;
        bool m_RuntimeBarrierExecutionEnabled = true;
        bool m_RuntimeTransientMaterializationEnabled = false;
        u32 m_TransientPoolMaxBucketSize = 2u;

        // Keyed post-pass listeners, fired in registration order (see
        // AddPostPassHook). A small vector — at most a couple of debug tools.
        std::vector<std::pair<std::string, PostPassHook>> m_PostPassHooks;
        BatchEventCallback m_BatchEventHook;

        // Execution-ready cache — rebuilt when m_DependencyGraphDirty is set.
        // The compiled submission plan is the only execution IR used by Execute().
        std::vector<SubmissionCommand> m_CachedSubmissionPlan; ///< IR cached after barrier planning
        std::string m_LastLoggedSubmissionPlanDigest;
        std::string m_LastLoggedCulledPassDigest;
        std::string m_LastLoggedBuildDiagnosticDigest;

        std::unordered_map<std::string, RGResourceDesc> m_ImportedResources;

        mutable bool m_ResourceRegistryDirty = true;
        mutable std::unordered_map<std::string, ResourceInfo> m_ResourceRegistry;
        mutable std::vector<ResourceInfo> m_RegisteredResources;
        mutable std::vector<Hazard> m_ResourceRegistryDiagnostics;
        // Transparent string maps so `find(string_view)` is allocation-free.
        // Keys stay as `std::string` (callers still create the entries via
        // `string`, and the templated `RenderGraphHandleAllocator` operates
        // on the string key directly), but reads avoid the per-call
        // temporary-string round trip that was the actual hot-path cost.
        mutable RGTransparentStringMap<RGTextureHandle> m_TextureHandlesByName;
        mutable RGTransparentStringMap<RGBufferHandle> m_BufferHandlesByName;
        mutable RGTransparentStringMap<RGFramebufferHandle> m_FramebufferHandlesByName;
        mutable RGTransparentStringMap<RGTextureHandle> m_LatestTextureHandlesByBaseName;
        mutable RGTransparentStringMap<RGBufferHandle> m_LatestBufferHandlesByBaseName;
        mutable RGTransparentStringMap<RGFramebufferHandle> m_LatestFramebufferHandlesByBaseName;
        // Base-name aliases: alias name -> target base name. Resolved by
        // GetTextureHandle / GetFramebufferHandle through the latest-version
        // maps, so callers reading by the alias name pick up whichever
        // pass produced the latest output for the active configuration.
        // Interned aliasName ID -> interned targetBaseName ID. Both sides
        // hit the resource interner; lookups are O(1) u32 compares.
        std::unordered_map<u32, u32> m_TextureBaseNameAliases;
        std::unordered_map<u32, u32> m_FramebufferBaseNameAliases;
        mutable std::unordered_map<std::string, RGResourceDesc> m_TextureViewResourceDescs;

        enum class FramebufferAttachmentViewKind : u8
        {
            Color,
            Depth,
        };

        enum class TextureViewKind : u8
        {
            FramebufferColorAttachment,
            FramebufferDepthAttachment,
            TextureMip,
            TextureArrayLayer,
            TextureCubeFace,
            TextureMultisampleResolve,
        };

        struct TextureViewDefinition
        {
            std::string ParentResource;
            std::string BackingResource;
            TextureViewKind Kind = TextureViewKind::FramebufferColorAttachment;
            u32 AttachmentIndex = 0;
            RGSubresourceRange ParentRange = RGSubresourceRange::Full();
        };
        mutable std::unordered_map<std::string, TextureViewDefinition> m_TextureViewDefinitions;

        struct HandleSlot
        {
            u32 Generation = 1;
            bool Alive = false;
            std::string Name;
            bool IsPlaceholder = false;
            mutable bool PlaceholderWarnedThisFrame = false;
            std::string PlaceholderReason;
        };
        mutable std::vector<HandleSlot> m_TextureHandleSlots;
        mutable std::vector<HandleSlot> m_BufferHandleSlots;
        mutable std::vector<HandleSlot> m_FramebufferHandleSlots;
        mutable std::vector<u32> m_FreeTextureHandleIndices;
        mutable std::vector<u32> m_FreeBufferHandleIndices;
        mutable std::vector<u32> m_FreeFramebufferHandleIndices;

        // -------------------------------------------------------------------
        // Physical resource storage (parallel to handle slots)
        // -------------------------------------------------------------------
        struct PhysicalTexture
        {
            u32 TextureID = 0;
            bool IsHistory = false;
        };
        struct PhysicalFramebuffer
        {
            Ref<Framebuffer> FB;
        };
        struct PhysicalBuffer
        {
            u32 BufferID = 0;
        };

        // Parallel arrays — index by handle.Index (same slot system as HandleSlots).
        // Grown alongside m_TextureHandleSlots / m_FramebufferHandleSlots / m_BufferHandleSlots.
        mutable std::vector<PhysicalTexture> m_PhysicalTextures;
        mutable std::vector<PhysicalFramebuffer> m_PhysicalFramebuffers;
        mutable std::vector<PhysicalBuffer> m_PhysicalBuffers;

        // -------------------------------------------------------------------
        // Extraction queue
        // -------------------------------------------------------------------
        struct TextureExtract
        {
            RGTextureHandle Handle;
            std::function<void(u32)> Callback;
        };
        struct ExternalTextureSinkKey
        {
            std::string SourceResource;
            u32 ColorAttachmentIndex = 0;

            auto operator==(const ExternalTextureSinkKey&) const -> bool = default;
        };
        struct ExternalTextureSinkKeyHash
        {
            [[nodiscard]] auto operator()(const ExternalTextureSinkKey& key) const noexcept -> sizet
            {
                sizet seed = std::hash<std::string>{}(key.SourceResource);
                seed ^= static_cast<sizet>(key.ColorAttachmentIndex) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
                return seed;
            }
        };
        struct ExternalTextureSink
        {
            u32 TextureID = 0;
            u32 Width = 0;
            u32 Height = 0;
            bool* ValidFlag = nullptr;
        };
        struct HistoryTextureExtract
        {
            enum class SourceKind : u8
            {
                Texture,
                Framebuffer,
            };

            std::string HistoryResource;
            SourceKind Kind = SourceKind::Texture;
            RGTextureHandle SourceTextureHandle;
            RGFramebufferHandle SourceFramebufferHandle;
            u32 ColorAttachmentIndex = 0;
            std::function<void(u32)> Callback;
        };
        struct HistoryTextureSink
        {
            u32 TextureID = 0;
            u32 Width = 0;
            u32 Height = 0;
            bool* ValidFlag = nullptr;
        };
        struct FramebufferExtract
        {
            RGFramebufferHandle Handle;
            std::function<void(Ref<Framebuffer>)> Callback;
        };
        std::vector<TextureExtract> m_TextureExtracts;
        std::vector<ExternalTextureSinkContract> m_ExternalTextureSinkContracts;
        std::vector<HistoryTextureExtract> m_HistoryTextureExtracts;
        std::vector<FramebufferExtract> m_FramebufferExtracts;
        std::vector<TemporalHistoryContract> m_TemporalHistoryContracts;
        std::unordered_map<ExternalTextureSinkKey, ExternalTextureSink, ExternalTextureSinkKeyHash> m_ExternalTextureSinks;
        std::unordered_map<std::string, HistoryTextureSink> m_HistoryTextureSinks;
        // Interned resource-name IDs (via m_ResourceNames) of transient
        // resources that were imported with explicit external backing. Set
        // membership replaces the previous string-keyed sets, eliminating
        // per-lookup string construction in the hot frame-build path.
        std::unordered_set<u32> m_ExternallyBackedTransientTextures;
        std::unordered_set<u32> m_ExternallyBackedTransientFramebuffers;

        // -------------------------------------------------------------------
        // Frame blackboard
        // -------------------------------------------------------------------
        FrameBlackboard m_Blackboard;

        // Monotonic counter bumped every time the topology (and with it the
        // blackboard / imported-resource maps) is wiped via ResetTopology() or
        // the full Reset(). External per-frame caches keyed off blackboard
        // contents (e.g. RenderPipeline's blackboard-populate fingerprint) hash
        // this so a reconfigure that leaves every other hashed input identical
        // still forces a repopulate — see issue #530. Never reset; wraps
        // harmlessly at u64.
        u64 m_TopologyGeneration = 0;
        // -------------------------------------------------------------------
        // Transient resource pool
        // -------------------------------------------------------------------
        TransientPool m_TransientPool;

        // -------------------------------------------------------------------
        // Graph-native execution metadata
        // -------------------------------------------------------------------
        FrameBuildStats m_LastBuildStats;

        // Allocate or recycle a texture handle slot and record the physical
        // resource. Called by ImportTexture / ImportHistory.
        RGTextureHandle AllocateTextureHandle(std::string_view name, u32 textureID, bool isHistory, bool isPlaceholder = false, std::string_view placeholderReason = "");
        RGFramebufferHandle AllocateFramebufferHandle(std::string_view name, const Ref<Framebuffer>& fb, bool isPlaceholder = false, std::string_view placeholderReason = "");
        RGBufferHandle AllocateBufferHandle(std::string_view name, u32 bufferID, bool isPlaceholder = false, std::string_view placeholderReason = "");
        [[nodiscard]] RGTextureHandle CreateVersionedTextureHandle(RGTextureHandle sourceHandle,
                                                                   std::string_view versionedName,
                                                                   std::string_view ownerPassName);
        [[nodiscard]] RGFramebufferHandle CreateVersionedFramebufferHandle(RGFramebufferHandle sourceHandle,
                                                                           std::string_view versionedName,
                                                                           std::string_view ownerPassName);
        [[nodiscard]] RGBufferHandle CreateVersionedBufferHandle(RGBufferHandle sourceHandle,
                                                                 std::string_view versionedName,
                                                                 std::string_view ownerPassName);
        [[nodiscard]] RGResourceDesc BuildVersionedResourceDesc(std::string_view sourceResource,
                                                                ResourceHandle::Kind fallbackKind,
                                                                std::string_view versionedName) const;

        void EnsureResourceRegistryBuilt() const;
        void RebuildTransientPlan();
        void MaterializeTransientResources();

        [[nodiscard]] static std::string BuildTransientAliasGroup(const RGResourceDesc& desc);
        [[nodiscard]] static u64 EstimateTransientBytes(const RGResourceDesc& desc);
        [[nodiscard]] static bool IsTransientDescriptorAllocatable(const RGResourceDesc& desc);
        [[nodiscard]] static std::string_view GetTransientDescriptorSkipReason(const RGResourceDesc& desc);

      public:
        // Format-conversion helpers exposed publicly so the extracted
        // RenderGraphTransientPlanner module (Phase 7 slice 5) can consume them
        // without needing friend access into the graph's private region.
        [[nodiscard]] static ImageFormat ToImageFormat(RGResourceFormat format);
        [[nodiscard]] static FramebufferTextureFormat ToFramebufferFormat(RGResourceFormat format);

      private:
        void DeclareExternalTextureSink(std::string_view sourceResource,
                                        u32 colorAttachmentIndex = 0);
        void RefreshExternalTextureSinkContracts();
        void DeclareHistoryTextureExtraction(std::string_view historyResource,
                                             std::string_view sourceResource,
                                             TemporalHistoryContract::SourceKind kind = TemporalHistoryContract::SourceKind::Texture,
                                             u32 colorAttachmentIndex = 0);
        void RefreshTemporalHistoryContracts();
        [[nodiscard]] bool HasHistoryTextureSink(std::string_view historyResource) const;

        std::unordered_map<std::string, RGResourceDesc> m_TransientResourceDescs;
        std::vector<TransientPlanEntry> m_TransientPlan;
        RGTransparentStringMap<std::string> m_ExplicitVersionProducers;

        // Tracks the most recent pass that wrote each resource (by base name).
        // Populated incrementally during BuildFrameGraph's Setup loop so a
        // pass's Setup can ask "who wrote SceneColor last?" and emit an
        // explicit DependsOnPass edge for read-modify-write chains without
        // every modifier needing a typed pass-pointer setter wired by the
        // pipeline builder. Cleared at the start of every BuildFrameGraph.
        RGTransparentStringMap<std::string> m_LastWriterPassNameByResource;
    };
} // namespace OloEngine
