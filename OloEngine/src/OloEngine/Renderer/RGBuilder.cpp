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

        if (const auto existingIt = std::find_if(m_DeclaredFeedbacks.begin(),
                                                 m_DeclaredFeedbacks.end(),
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

        if (const auto alreadyDeclared = std::find(m_DeclaredPassDependencies.begin(),
                                                   m_DeclaredPassDependencies.end(),
                                                   passName);
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

        const auto alreadyDeclared = std::find(m_DeclaredReads.begin(), m_DeclaredReads.end(), resourceName);
        if (alreadyDeclared == m_DeclaredReads.end())
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

        const auto alreadyDeclared = std::find(m_DeclaredWrites.begin(), m_DeclaredWrites.end(), resourceName);
        if (alreadyDeclared == m_DeclaredWrites.end())
            m_DeclaredWrites.emplace_back(resourceName);

        m_DeclaredAccesses.push_back(RGAccessDeclaration{
            .ResourceName = std::string(resourceName),
            .IsWrite = true,
            .ReadUsage = RGReadUsage::ShaderSample,
            .WriteUsage = usage,
            .Range = range,
        });
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

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Read: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordRead(resourceName, usage, range);
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

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
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

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
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

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::Write: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
        }
        else
        {
            RecordWrite(resourceName, usage, range);
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

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
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

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
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

    void RGBuilder::AllowFeedback(
        const RGTextureHandle handle,
        const RGSubresourceRange& range)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot declare feedback for an invalid texture handle");

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::AllowFeedback: pass='{}' texture handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
            return;
        }

        RecordFeedback(resourceName, range);
    }

    void RGBuilder::AllowFeedback(
        const RGFramebufferHandle handle)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot declare feedback for an invalid framebuffer handle");

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::AllowFeedback: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, handle.Index, handle.Generation);
            return;
        }

        RecordFeedback(resourceName, RGSubresourceRange::Full());
    }

    void RGBuilder::AllowFeedback(
        const RGBufferHandle handle,
        const RGSubresourceRange& range)
    {
        OLO_CORE_ASSERT(handle.IsValid(), "Cannot declare feedback for an invalid buffer handle");

        const auto resourceName = m_Graph.GetResourceName(handle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::AllowFeedback: pass='{}' buffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
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

        const auto resourceName = m_Graph.GetResourceName(sourceHandle);
        if (resourceName.empty())
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

        const auto resourceName = m_Graph.GetResourceName(sourceHandle);
        if (resourceName.empty())
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

        const auto resourceName = m_Graph.GetResourceName(sourceHandle);
        if (resourceName.empty())
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

        const auto resourceName = m_Graph.GetResourceName(sourceHandle);
        if (resourceName.empty())
        {
            OLO_CORE_ERROR("RGBuilder::ExtractHistoryTexture: pass='{}' framebuffer handle (idx={}, gen={}) resolved to empty resource name — handle is stale or resource was never imported",
                           m_CurrentPassName, sourceHandle.Index, sourceHandle.Generation);
            return;
        }

        m_Graph.ExtractHistoryTexture(historyResource, sourceHandle, colorAttachmentIndex);
    }

} // namespace OloEngine
