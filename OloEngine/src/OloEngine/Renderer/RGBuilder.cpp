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

} // namespace OloEngine
