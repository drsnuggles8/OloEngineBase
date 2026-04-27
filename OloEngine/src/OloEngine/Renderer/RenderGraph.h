#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <unordered_set>

namespace OloEngine
{
    // @brief Manages a graph of render passes forming a complete rendering pipeline.
    class RenderGraph : public RefCounted
    {
      public:
        RenderGraph() = default;
        ~RenderGraph() = default;

        void Init(u32 width, u32 height);
        void Shutdown();

        // Clear all topology bookkeeping (passes, edges, framebuffer piping,
        // cached execution order) WITHOUT touching the passes themselves.
        // Used by `Renderer3D::ConfigureRenderGraph(RenderingPath)` to rebuild
        // the graph when the user switches between Forward / Forward+ /
        // Deferred at runtime. Because passes are owned externally as
        // `Ref<>`s on `Renderer3D::s_Data`, their framebuffers and internal
        // state survive the reset. Callers must re-`AddPass` every pass they
        // want in the new topology and re-issue all edges before calling
        // `SetFinalPass` / `ValidateResourceHazards` again.
        void ResetTopology();

        // Only support RenderPass
        void AddPass(const Ref<RenderPass>& pass);
        // Connect two passes: establishes execution ordering AND framebuffer piping
        void ConnectPass(const std::string& outputPass, const std::string& inputPass);

        // Add an execution-ordering dependency without framebuffer piping.
        // Use this when the upstream pass produces outputs consumed via texture bindings
        // rather than framebuffer attachments (e.g., shadow maps).
        void AddExecutionDependency(const std::string& beforePass, const std::string& afterPass);

        // Execute all passes in the correct order
        void Execute();

        // Resize all passes in the graph
        void Resize(u32 width, u32 height);

        // Set the final pass in the graph
        void SetFinalPass(const std::string& passName);

        // @brief Get all render passes in the graph for debugging or inspection.
        // @return Vector of render passes in the execution order
        [[nodiscard]] std::vector<Ref<RenderPass>> GetAllPasses() const;

        // Get a pass by name and cast to the requested type
        template<typename T>
        Ref<T> GetPass(const std::string& name)
        {
            if (m_PassLookup.find(name) != m_PassLookup.end())
            {
                return m_PassLookup.at(name).As<T>();
            }
            return nullptr;
        }

        [[nodiscard]] bool IsFinalPass(const std::string& passName) const;

        struct ConnectionInfo
        {
            std::string OutputPass;
            std::string InputPass;
            u32 AttachmentIndex = 0;
        };

        [[nodiscard]] std::vector<ConnectionInfo> GetConnections() const;

        struct PassSubmissionInfo
        {
            std::string PassName;
            RenderPass::SubmissionModel Submission = RenderPass::SubmissionModel::Unknown;
            bool DeclaresResources = false;
        };
        [[nodiscard]] std::vector<PassSubmissionInfo> GetPassSubmissionInfo() const;

        // @brief Get the topologically-sorted pass execution order (for testing/inspection).
        [[nodiscard]] const std::vector<std::string>& GetPassOrder() const
        {
            return m_PassOrder;
        }

        // -------------------------------------------------------------------
        // Resource-aware hazard validation
        // -------------------------------------------------------------------
        // Walks the topologically-sorted execution order and, using each
        // pass's declared reads/writes (see RenderPass::DeclareRead/Write),
        // validates that every read has a transitive execution dependency
        // on its producer, that no two parallel passes write the same
        // resource, and that writers don't overwrite live reads. Catches
        // missing `ConnectPass` / `AddExecutionDependency` calls.
        //
        // Call this AFTER all passes are added and connections made — the
        // first call forces topological sort if needed. Returns the list of
        // hazards (empty vector = clean). Also logs each hazard as an error
        // through the engine logger.
        enum class HazardKind
        {
            ReadAfterWrite,       // reader does not depend on writer
            WriteAfterWrite,      // later writer does not depend on previous writer
            WriteAfterRead,       // later writer does not depend on prior reader
            ResourceKindMismatch, // same logical resource declared with conflicting kinds
            Cycle,                // dependency graph has a cycle; hazards could not be validated
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

        struct ResourceInfo
        {
            std::string Name;
            RGResourceDesc Desc;
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
        [[nodiscard]] RGTextureHandle GetTextureHandle(std::string_view name) const;
        [[nodiscard]] RGBufferHandle GetBufferHandle(std::string_view name) const;
        [[nodiscard]] RGFramebufferHandle GetFramebufferHandle(std::string_view name) const;
        [[nodiscard]] bool IsTextureHandleCurrent(RGTextureHandle handle) const;
        [[nodiscard]] bool IsBufferHandleCurrent(RGBufferHandle handle) const;
        [[nodiscard]] bool IsFramebufferHandleCurrent(RGFramebufferHandle handle) const;

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

      private:
        // Returns false if the graph contains a cycle (m_PassOrder will be
        // partial/empty in that case). Callers must abort further work when
        // false is returned — running validators or Execute() against a
        // partial topo order produces misleading diagnostics.
        [[nodiscard]] bool UpdateDependencyGraph();
        void ResolveFinalPass();

        std::unordered_map<std::string, Ref<RenderPass>> m_PassLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_Dependencies;           // Execution ordering
        std::unordered_map<std::string, std::vector<std::string>> m_FramebufferConnections; // Framebuffer piping
        std::vector<std::string> m_InsertionOrder;                                          // Pass names in AddPass() order (stable topo tie-break)
        std::vector<std::string> m_PassOrder;
        std::string m_FinalPassName;
        bool m_DependencyGraphDirty = false;

        // Execution-ready cache — rebuilt when m_DependencyGraphDirty is set.
        // Avoids per-frame hash lookups in Execute().
        //
        // Lifetime: FramebufferPipe and its RenderPass* members are non-owning raw
        // pointers into m_PassLookup.  They must remain valid until
        // RebuildExecutionCache() is called.  Do not remove or destroy passes from
        // m_PassLookup while the cache is in use.
        struct FramebufferPipe
        {
            RenderPass* OutputPass = nullptr;
            std::vector<RenderPass*> InputPasses;
        };
        std::vector<FramebufferPipe> m_CachedPipes;
        std::vector<RenderPass*> m_CachedExecutionOrder;

        std::unordered_map<std::string, RGResourceDesc> m_ImportedResources;

        mutable bool m_ResourceRegistryDirty = true;
        mutable std::unordered_map<std::string, ResourceInfo> m_ResourceRegistry;
        mutable std::vector<ResourceInfo> m_RegisteredResources;
        mutable std::vector<Hazard> m_ResourceRegistryDiagnostics;
        mutable std::unordered_map<std::string, RGTextureHandle> m_TextureHandlesByName;
        mutable std::unordered_map<std::string, RGBufferHandle> m_BufferHandlesByName;
        mutable std::unordered_map<std::string, RGFramebufferHandle> m_FramebufferHandlesByName;

        struct HandleSlot
        {
            u32 Generation = 1;
            bool Alive = false;
            std::string Name;
        };
        mutable std::vector<HandleSlot> m_TextureHandleSlots;
        mutable std::vector<HandleSlot> m_BufferHandleSlots;
        mutable std::vector<HandleSlot> m_FramebufferHandleSlots;
        mutable std::vector<u32> m_FreeTextureHandleIndices;
        mutable std::vector<u32> m_FreeBufferHandleIndices;
        mutable std::vector<u32> m_FreeFramebufferHandleIndices;

        void RebuildExecutionCache();
        void EnsureResourceRegistryBuilt() const;
    };
} // namespace OloEngine
