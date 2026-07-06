#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <algorithm>

namespace OloEngine
{
    void RGBuilder::BeginPass(std::string_view passName)
    {
        m_CurrentPassName = std::string(passName);
        m_DeclaredReads.clear();
        m_DeclaredWrites.clear();
        m_DeclaredAccesses.clear();
        m_DeclaredFeedbacks.clear();
        m_DeclaredPassDependencies.clear();
        m_DeclaredLifetimeExtensions.clear();
        m_NextVersionOrdinalByResource.clear();
    }

    std::string RGBuilder::BuildVersionedResourceName(std::string_view resourceName,
                                                      std::string_view versionTag)
    {
        auto stableTag = !versionTag.empty() ? std::string(versionTag) : m_CurrentPassName;
        if (stableTag.empty())
            stableTag = "Version";

        auto& nextOrdinal = m_NextVersionOrdinalByResource[std::string(resourceName)];
        ++nextOrdinal;

        auto derivedName = std::string(resourceName) + "@" + stableTag;
        if (nextOrdinal > 1u)
            derivedName += "_" + std::to_string(nextOrdinal);

        return derivedName;
    }

    void RGBuilder::RecordFeedback(std::string_view resourceName, const RGSubresourceRange& range)
    {
        if (resourceName.empty())
            return;

        const auto sameRange = [&range](const RGFeedbackDeclaration& declaration)
        {
            return declaration.Range.BaseMip == range.BaseMip &&
                   declaration.Range.MipCount == range.MipCount &&
                   declaration.Range.BaseLayer == range.BaseLayer &&
                   declaration.Range.LayerCount == range.LayerCount &&
                   declaration.Range.BaseSlice == range.BaseSlice &&
                   declaration.Range.SliceCount == range.SliceCount;
        };

        if (const auto existingIt = std::ranges::find_if(m_DeclaredFeedbacks,
                                                         [resourceName, &sameRange](const RGFeedbackDeclaration& declaration)
                                                         {
                                                             return declaration.ResourceName == resourceName &&
                                                                    sameRange(declaration);
                                                         });
            existingIt != m_DeclaredFeedbacks.end())
        {
            return;
        }

        m_DeclaredFeedbacks.push_back(RGFeedbackDeclaration{
            .ResourceName = std::string(resourceName),
            .Range = range,
        });
    }

    void RGBuilder::DependsOnPass(std::string_view passName)
    {
        if (passName.empty())
            return;

        if (const auto alreadyDeclared = std::ranges::find(m_DeclaredPassDependencies, passName);
            alreadyDeclared == m_DeclaredPassDependencies.end())
        {
            m_DeclaredPassDependencies.emplace_back(passName);
        }
    }

    void RGBuilder::DependsOnPreviousWriter(std::string_view resourceName)
    {
        if (resourceName.empty())
            return;

        const auto& previousWriter = m_Graph.GetLastWriterPassName(resourceName);
        if (previousWriter.empty() || previousWriter == m_CurrentPassName)
            return;

        DependsOnPass(previousWriter);
    }

    void RGBuilder::RecordRead(std::string_view resourceName, const RGReadUsage usage, const RGSubresourceRange& range)
    {
        // Defensive: validate the builder references are sane
        OLO_CORE_ASSERT(&m_Graph != nullptr, "RGBuilder::RecordRead: m_Graph reference is null");
        OLO_CORE_ASSERT(&m_Blackboard != nullptr, "RGBuilder::RecordRead: m_Blackboard reference is null");

        if (resourceName.empty())
            return;

        if (const auto alreadyDeclared = std::ranges::find(m_DeclaredReads, resourceName); alreadyDeclared == m_DeclaredReads.end())
            m_DeclaredReads.emplace_back(resourceName);

        m_DeclaredAccesses.push_back(RGAccessDeclaration{
            .ResourceName = std::string(resourceName),
            .IsWrite = false,
            .ReadUsage = usage,
            .WriteUsage = RGWriteUsage::RenderTarget,
            .Range = range,
        });
    }

    void RGBuilder::RecordWrite(std::string_view resourceName, const RGWriteUsage usage, const RGSubresourceRange& range)
    {
        // Defensive: validate the builder references are sane
        OLO_CORE_ASSERT(&m_Graph != nullptr, "RGBuilder::RecordWrite: m_Graph reference is null");
        OLO_CORE_ASSERT(&m_Blackboard != nullptr, "RGBuilder::RecordWrite: m_Blackboard reference is null");

        if (resourceName.empty())
            return;

        if (const auto alreadyDeclared = std::ranges::find(m_DeclaredWrites, resourceName); alreadyDeclared == m_DeclaredWrites.end())
            m_DeclaredWrites.emplace_back(resourceName);

        m_DeclaredAccesses.push_back(RGAccessDeclaration{
            .ResourceName = std::string(resourceName),
            .IsWrite = true,
            .ReadUsage = RGReadUsage::ShaderSample,
            .WriteUsage = usage,
            .Range = range,
        });
    }

    void RGBuilder::RecordLifetimeExtension(std::string_view resourceName)
    {
        if (resourceName.empty())
            return;

        if (const auto alreadyDeclared = std::ranges::find(m_DeclaredLifetimeExtensions, resourceName);
            alreadyDeclared == m_DeclaredLifetimeExtensions.end())
        {
            m_DeclaredLifetimeExtensions.emplace_back(resourceName);
        }
    }

    // -------------------------------------------------------------------
    // Read operations
    // -------------------------------------------------------------------

    RGTextureHandle RGBuilder::Read(
        RGTextureHandle handle,
        RGReadUsage usage,
        const RGSubresourceRange& range)
    {
        // Phase C stub: record read declaration to graph for validation
        // and dependency tracking. For now, just return the handle;
        // full implementation will populate m_Graph's dependency DAG.
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot read an invalid texture handle");

        if (const auto resourceName = m_Graph.GetResourceName(handle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Read: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordRead(resourceName, usage, range);

            // When the read targets a framebuffer attachment view, also record
            // the access against the parent framebuffer. The transient planner
            // tracks lifetimes per registered transient resource, and an
            // attachment-view name lives in a separate registry from the
            // parent framebuffer. Without this propagation the parent's
            // lifetime ends at its writer, letting the transient allocator
            // alias the parent's storage onto another transient (e.g. the
            // next pass's output) — producing a same-FB feedback loop when
            // the reading pass also writes the aliased downstream resource.
            if (auto parent = m_Graph.FindAttachmentViewParent(resourceName);
                !parent.empty() && parent != resourceName)
            {
                RecordRead(parent, usage, range);
            }
        }

        (void)usage;
        (void)range;
        return handle;
    }

    RGFramebufferHandle RGBuilder::Read(
        RGFramebufferHandle handle,
        RGReadUsage usage)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot read an invalid framebuffer handle");

        if (const auto resourceName = m_Graph.GetResourceName(handle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Read: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordRead(resourceName, usage, RGSubresourceRange::Full());
        }

        (void)usage;
        return handle;
    }

    RGBufferHandle RGBuilder::Read(
        RGBufferHandle handle,
        RGReadUsage usage)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot read an invalid buffer handle");

        if (const auto resourceName = m_Graph.GetResourceName(handle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Read: pass='{}' buffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordRead(resourceName, usage, RGSubresourceRange::Full());
        }

        (void)usage;
        return handle;
    }

    // -------------------------------------------------------------------
    // Write operations
    // -------------------------------------------------------------------

    void RGBuilder::Write(
        RGTextureHandle handle,
        RGWriteUsage usage,
        const RGSubresourceRange& range)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot write an invalid texture handle");

        if (const auto resourceName = m_Graph.GetResourceName(handle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Write: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordWrite(resourceName, usage, range);

            // When the write targets a framebuffer attachment view, also
            // extend the parent framebuffer's transient lifetime back to
            // this pass. Without this, a pass that seeds an MRT purely
            // through attachment-view writes (e.g. OITPreparePass writing
            // OITAccum/OITRevealage/OITDepthAttachment but never the parent
            // OITBuffer directly) doesn't count as a writer of the parent
            // framebuffer, so the transient planner's FirstPassIndex for the
            // parent starts at the first *reader* instead of this earlier
            // write — under-reporting the parent's lifetime and letting the
            // transient allocator alias its storage while the attachment is
            // still logically live.
            //
            // This deliberately does NOT mirror Read's propagation by
            // calling RecordWrite(parent, ...): that would declare a real
            // write access under the parent's name, which (a) collides with
            // this same pass's propagated Read(parent) whenever it also
            // reads a *different* attachment view of the same framebuffer
            // (a legitimate RMW pattern, e.g. DecalRenderPass/ParticleRender
            // Pass sampling one OIT attachment while rewriting another) —
            // producing a false same-pass feedback hazard on the parent's
            // name even though the individual views involved never overlap
            // and are correctly declared via AllowSamePassReadWrite; and (b)
            // gets expanded by expandTextureViewAccesses back down onto
            // *every* sibling attachment view of the parent, falsely
            // implying this pass wrote attachments it never touched (e.g.
            // marking OITDepthAttachment written just because OITAccum was).
            // RecordLifetimeExtension sidesteps both by feeding only the
            // transient planner, not hazard validation or the view-expansion
            // pass.
            if (auto parent = m_Graph.FindAttachmentViewParent(resourceName);
                !parent.empty() && parent != resourceName)
            {
                RecordLifetimeExtension(parent);
            }
        }

        (void)usage;
        (void)range;
        // Phase C stub: record write for dependency tracking
    }

    void RGBuilder::Write(
        RGFramebufferHandle handle,
        RGWriteUsage usage)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot write an invalid framebuffer handle");

        if (const auto resourceName = m_Graph.GetResourceName(handle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Write: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordWrite(resourceName, usage, RGSubresourceRange::Full());
        }

        (void)usage;
    }

    void RGBuilder::Write(
        RGBufferHandle handle,
        RGWriteUsage usage)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot write an invalid buffer handle");

        if (const auto resourceName = m_Graph.GetResourceName(handle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Write: pass='{}' buffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordWrite(resourceName, usage, RGSubresourceRange::Full());
        }

        (void)usage;
    }

    RGTextureHandle RGBuilder::WriteNewVersion(
        const RGTextureHandle sourceHandle,
        const RGWriteUsage usage,
        std::string_view versionTag,
        const RGSubresourceRange& range)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot create a new version from an invalid texture handle");

        const auto sourceResource = m_Graph.GetResourceName(sourceHandle);
        if (sourceResource.empty())
        {
            OLO_CORE_ERROR("RGBuilder::WriteNewVersion: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return {};
        }

        const auto versionedName = BuildVersionedResourceName(sourceResource, versionTag);
        auto versionHandle = m_Graph.CreateVersionedTextureHandle(sourceHandle, versionedName, m_CurrentPassName);
        if (!versionHandle.IsValid())
        {
            OLO_CORE_ERROR("RGBuilder::WriteNewVersion: pass='{}' failed to create texture version '{}' from '{}'",
                           m_CurrentPassName, versionedName, sourceResource);
            return {};
        }

        Write(versionHandle, usage, range);
        return versionHandle;
    }

    RGFramebufferHandle RGBuilder::WriteNewVersion(
        const RGFramebufferHandle sourceHandle,
        const RGWriteUsage usage,
        std::string_view versionTag)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot create a new version from an invalid framebuffer handle");

        const auto sourceResource = m_Graph.GetResourceName(sourceHandle);
        if (sourceResource.empty())
        {
            OLO_CORE_ERROR("RGBuilder::WriteNewVersion: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return {};
        }

        const auto versionedName = BuildVersionedResourceName(sourceResource, versionTag);
        auto versionHandle = m_Graph.CreateVersionedFramebufferHandle(sourceHandle, versionedName, m_CurrentPassName);
        if (!versionHandle.IsValid())
        {
            OLO_CORE_ERROR("RGBuilder::WriteNewVersion: pass='{}' failed to create framebuffer version '{}' from '{}'",
                           m_CurrentPassName, versionedName, sourceResource);
            return {};
        }

        Write(versionHandle, usage);
        return versionHandle;
    }

    RGBufferHandle RGBuilder::WriteNewVersion(
        const RGBufferHandle sourceHandle,
        const RGWriteUsage usage,
        std::string_view versionTag)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot create a new version from an invalid buffer handle");

        const auto sourceResource = m_Graph.GetResourceName(sourceHandle);
        if (sourceResource.empty())
        {
            OLO_CORE_ERROR("RGBuilder::WriteNewVersion: pass='{}' buffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return {};
        }

        const auto versionedName = BuildVersionedResourceName(sourceResource, versionTag);
        auto versionHandle = m_Graph.CreateVersionedBufferHandle(sourceHandle, versionedName, m_CurrentPassName);
        if (!versionHandle.IsValid())
        {
            OLO_CORE_ERROR("RGBuilder::WriteNewVersion: pass='{}' failed to create buffer version '{}' from '{}'",
                           m_CurrentPassName, versionedName, sourceResource);
            return {};
        }

        Write(versionHandle, usage);
        return versionHandle;
    }

    RGTextureHandle RGBuilder::CreateFramebufferAttachmentView(
        std::string_view name,
        const RGFramebufferHandle framebufferHandle,
        const u32 colorAttachmentIndex)
    {
        return m_Graph.CreateFramebufferAttachmentView(name, framebufferHandle, colorAttachmentIndex);
    }

    RGTextureHandle RGBuilder::CreateFramebufferDepthAttachmentView(
        std::string_view name,
        const RGFramebufferHandle framebufferHandle)
    {
        return m_Graph.CreateFramebufferDepthAttachmentView(name, framebufferHandle);
    }

    void RGBuilder::AllowSamePassReadWrite(
        const RGTextureHandle handle,
        const RGSubresourceRange& range)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot declare same-pass read/write for an invalid texture handle");

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::AllowSamePassReadWrite: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
            return;
        }

        RecordFeedback(resourceName, range);
    }

    void RGBuilder::AllowSamePassReadWrite(
        const RGFramebufferHandle handle)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot declare same-pass read/write for an invalid framebuffer handle");

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::AllowSamePassReadWrite: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
            return;
        }

        RecordFeedback(resourceName, RGSubresourceRange::Full());
    }

    void RGBuilder::AllowSamePassReadWrite(
        const RGBufferHandle handle,
        const RGSubresourceRange& range)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot declare same-pass read/write for an invalid buffer handle");

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::AllowSamePassReadWrite: pass='{}' buffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
            return;
        }

        RecordFeedback(resourceName, range);
    }

    // -------------------------------------------------------------------
    // Create operations
    // -------------------------------------------------------------------

    RGTextureHandle RGBuilder::CreateTexture(
        std::string_view name,
        const RGResourceDesc& desc)
    {
        return m_Graph.AllocateTransientTextureHandle(name, desc);
    }

    RGFramebufferHandle RGBuilder::CreateFramebuffer(
        std::string_view name,
        const RGResourceDesc& desc)
    {
        return m_Graph.AllocateTransientFramebufferHandle(name, desc);
    }

    RGBufferHandle RGBuilder::CreateBuffer(
        std::string_view name,
        const RGResourceDesc& desc)
    {
        return m_Graph.AllocateTransientBufferHandle(name, desc);
    }

    // -------------------------------------------------------------------
    // Import operations
    // -------------------------------------------------------------------

    RGTextureHandle RGBuilder::ImportTexture(
        std::string_view name,
        u32 textureID,
        const RGResourceDesc& desc)
    {
        return m_Graph.ImportTexture(name, textureID, desc);
    }

    RGFramebufferHandle RGBuilder::ImportFramebuffer(
        std::string_view name,
        const Ref<Framebuffer>& fb,
        const RGResourceDesc& desc)
    {
        return m_Graph.ImportFramebuffer(name, fb, desc);
    }

    RGBufferHandle RGBuilder::ImportBuffer(
        std::string_view name,
        u32 bufferID,
        const RGResourceDesc& desc)
    {
        return m_Graph.ImportBuffer(name, bufferID, desc);
    }

    // -------------------------------------------------------------------
    // Extract operations
    // -------------------------------------------------------------------

    void RGBuilder::ExtractTexture(
        RGTextureHandle handle,
        std::function<void(u32)> callback)
    {
        m_Graph.ExtractTexture(handle, callback);
    }

    void RGBuilder::ExtractFramebuffer(
        RGFramebufferHandle handle,
        std::function<void(Ref<Framebuffer>)> callback)
    {
        m_Graph.ExtractFramebuffer(handle, callback);
    }

    void RGBuilder::RegisterExternalTextureSink(
        const RGTextureHandle sourceHandle,
        const u32 textureID,
        const u32 width,
        const u32 height,
        bool* const validFlag)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot register an external sink from an invalid texture handle");

        if (const auto resourceName = m_Graph.GetResourceName(sourceHandle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::RegisterExternalTextureSink: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return;
        }

        m_Graph.RegisterExternalTextureSink(sourceHandle, textureID, width, height, validFlag);
    }

    void RGBuilder::RegisterExternalTextureSink(
        const RGFramebufferHandle sourceHandle,
        const u32 textureID,
        const u32 width,
        const u32 height,
        const u32 colorAttachmentIndex,
        bool* const validFlag)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot register an external sink from an invalid framebuffer handle");

        if (const auto resourceName = m_Graph.GetResourceName(sourceHandle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::RegisterExternalTextureSink: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return;
        }

        m_Graph.RegisterExternalTextureSink(sourceHandle, textureID, width, height, colorAttachmentIndex, validFlag);
    }

    void RGBuilder::ExtractHistoryTexture(
        std::string_view historyResource,
        const RGTextureHandle sourceHandle)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot extract history from an invalid texture handle");

        if (const auto resourceName = m_Graph.GetResourceName(sourceHandle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::ExtractHistoryTexture: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return;
        }

        m_Graph.ExtractHistoryTexture(historyResource, sourceHandle);
    }

    void RGBuilder::ExtractHistoryTexture(
        std::string_view historyResource,
        const RGFramebufferHandle sourceHandle,
        const u32 colorAttachmentIndex)
    {
        OLO_CORE_ASSERT(sourceHandle.IsValid(), "Cannot extract history from an invalid framebuffer handle");

        if (const auto resourceName = m_Graph.GetResourceName(sourceHandle); resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::ExtractHistoryTexture: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return;
        }

        m_Graph.ExtractHistoryTexture(historyResource, sourceHandle, colorAttachmentIndex);
    }

} // namespace OloEngine
