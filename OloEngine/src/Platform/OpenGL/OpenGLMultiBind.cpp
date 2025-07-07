#include "OloEnginePCH.h"
#include "OpenGLMultiBind.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <glad/gl.h>
#include <algorithm>

namespace OloEngine
{
    // Static member initialization
    bool OpenGLMultiBind::s_CapabilitiesQueried = false;
    bool OpenGLMultiBind::s_MultiBindSupported = false;
    bool OpenGLMultiBind::s_DSASupported = false;
    u32 OpenGLMultiBind::s_MaxTextureUnits = 0;
    u32 OpenGLMultiBind::s_MaxUniformBufferBindings = 0;
    u32 OpenGLMultiBind::s_MaxShaderStorageBufferBindings = 0;

    OpenGLMultiBind::OpenGLMultiBind()
    {
        QueryCapabilities();
        
        // Set up default configuration
        m_Config = MultiBind Config{};
        m_Config.EnableTextureBatching = s_MultiBindSupported;
        m_Config.EnableBufferBatching = s_MultiBindSupported;
        m_Config.UseDirectStateAccess = s_DSASupported;
        m_Config.MaxTexturesPerBatch = std::min(32u, s_MaxTextureUnits);
        m_Config.MaxBuffersPerBatch = std::min(32u, s_MaxUniformBufferBindings);
        
        // Initialize state cache
        m_StateCache.Invalidate();
    }

    OpenGLMultiBind::OpenGLMultiBind(const MultiBind Config& config)
        : m_Config(config)
    {
        QueryCapabilities();
        
        // Validate configuration against OpenGL capabilities
        if (!s_MultiBindSupported)
        {
            m_Config.EnableTextureBatching = false;
            m_Config.EnableBufferBatching = false;
            OLO_CORE_WARN("OpenGL multi-bind not supported, falling back to individual bindings");
        }
        
        if (!s_DSASupported)
        {
            m_Config.UseDirectStateAccess = false;
            OLO_CORE_WARN("OpenGL Direct State Access not supported, using traditional binding");
        }
        
        m_Config.MaxTexturesPerBatch = std::min(m_Config.MaxTexturesPerBatch, s_MaxTextureUnits);
        m_Config.MaxBuffersPerBatch = std::min(m_Config.MaxBuffersPerBatch, s_MaxUniformBufferBindings);
        
        // Initialize state cache
        m_StateCache.Invalidate();
    }

    void OpenGLMultiBind::AddTexture(u32 textureID, u32 bindingPoint, GLenum target, ShaderResourceType resourceType)
    {
        // Check cache first
        if (m_Config.EnableCaching && m_StateCache.IsTextureBound(bindingPoint, textureID, target))
        {
            m_Statistics.CacheHits++;
            return;
        }
        m_Statistics.CacheMisses++;

        // Check if we need to flush current batch
        if (m_CurrentTextureBatch.IsFull(m_Config.MaxTexturesPerBatch))
        {
            SubmitTextures();
        }

        // Add to current batch
        m_CurrentTextureBatch.TextureIDs.push_back(textureID);
        m_CurrentTextureBatch.BindingPoints.push_back(bindingPoint);
        m_CurrentTextureBatch.Targets.push_back(target);
        m_CurrentTextureBatch.ResourceType = resourceType;
        m_CurrentTextureBatch.Count++;

        // Update start binding if this is the first texture
        if (m_CurrentTextureBatch.Count == 1)
        {
            m_CurrentTextureBatch.StartBinding = bindingPoint;
        }
    }

    void OpenGLMultiBind::AddTextures(const u32* textureIDs, const u32* bindingPoints, const GLenum* targets, 
                                     u32 count, ShaderResourceType resourceType)
    {
        for (u32 i = 0; i < count; ++i)
        {
            AddTexture(textureIDs[i], bindingPoints[i], targets[i], resourceType);
        }
    }

    void OpenGLMultiBind::SubmitTextures()
    {
        if (m_CurrentTextureBatch.IsEmpty())
            return;

        if (m_Config.EnableValidation && !ValidateTextureBatch(m_CurrentTextureBatch))
        {
            OLO_CORE_ERROR("Texture batch validation failed");
            m_CurrentTextureBatch.Clear();
            return;
        }

        SubmitTextureBatch(m_CurrentTextureBatch);
        
        // Update statistics
        m_Statistics.TotalTextureBatches++;
        m_Statistics.TotalTextureBindings += m_CurrentTextureBatch.Count;
        
        // Update average batch size
        f32 totalBindings = static_cast<f32>(m_Statistics.TotalTextureBindings + m_Statistics.TotalBufferBindings);
        f32 totalBatches = static_cast<f32>(m_Statistics.TotalTextureBatches + m_Statistics.TotalBufferBatches);
        m_Statistics.AverageBatchSize = totalBatches > 0 ? totalBindings / totalBatches : 0.0f;

        m_CurrentTextureBatch.Clear();
    }

    void OpenGLMultiBind::AddBuffer(u32 bufferID, u32 bindingPoint, GLenum target, size_t offset, size_t size, 
                                   ShaderResourceType resourceType)
    {
        // Check cache first
        if (m_Config.EnableCaching && m_StateCache.IsBufferBound(bindingPoint, bufferID, target))
        {
            m_Statistics.CacheHits++;
            return;
        }
        m_Statistics.CacheMisses++;

        // Check if we need to flush current batch
        if (m_CurrentBufferBatch.IsFull(m_Config.MaxBuffersPerBatch))
        {
            SubmitBuffers();
        }

        // Add to current batch
        m_CurrentBufferBatch.BufferIDs.push_back(bufferID);
        m_CurrentBufferBatch.BindingPoints.push_back(bindingPoint);
        m_CurrentBufferBatch.Offsets.push_back(offset);
        m_CurrentBufferBatch.Sizes.push_back(size);
        m_CurrentBufferBatch.Target = target;
        m_CurrentBufferBatch.ResourceType = resourceType;
        m_CurrentBufferBatch.Count++;

        // Update start binding if this is the first buffer
        if (m_CurrentBufferBatch.Count == 1)
        {
            m_CurrentBufferBatch.StartBinding = bindingPoint;
        }
    }

    void OpenGLMultiBind::AddBuffers(const u32* bufferIDs, const u32* bindingPoints, const size_t* offsets, 
                                    const size_t* sizes, u32 count, GLenum target, ShaderResourceType resourceType)
    {
        for (u32 i = 0; i < count; ++i)
        {
            AddBuffer(bufferIDs[i], bindingPoints[i], offsets[i], sizes[i], target, resourceType);
        }
    }

    void OpenGLMultiBind::SubmitBuffers()
    {
        if (m_CurrentBufferBatch.IsEmpty())
            return;

        if (m_Config.EnableValidation && !ValidateBufferBatch(m_CurrentBufferBatch))
        {
            OLO_CORE_ERROR("Buffer batch validation failed");
            m_CurrentBufferBatch.Clear();
            return;
        }

        SubmitBufferBatch(m_CurrentBufferBatch);
        
        // Update statistics
        m_Statistics.TotalBufferBatches++;
        m_Statistics.TotalBufferBindings += m_CurrentBufferBatch.Count;
        
        // Update average batch size
        f32 totalBindings = static_cast<f32>(m_Statistics.TotalTextureBindings + m_Statistics.TotalBufferBindings);
        f32 totalBatches = static_cast<f32>(m_Statistics.TotalTextureBatches + m_Statistics.TotalBufferBatches);
        m_Statistics.AverageBatchSize = totalBatches > 0 ? totalBindings / totalBatches : 0.0f;

        m_CurrentBufferBatch.Clear();
    }

    void OpenGLMultiBind::SubmitAll()
    {
        SubmitTextures();
        SubmitBuffers();
    }

    void OpenGLMultiBind::Clear()
    {
        m_CurrentTextureBatch.Clear();
        m_CurrentBufferBatch.Clear();
    }

    bool OpenGLMultiBind::HasPendingBindings() const
    {
        return !m_CurrentTextureBatch.IsEmpty() || !m_CurrentBufferBatch.IsEmpty();
    }

    void OpenGLMultiBind::SubmitTextureBatch(const TextureBatch& batch)
    {
        RENDERER_PROFILE_SCOPE("OpenGLMultiBind::SubmitTextureBatch");

        if (m_Config.EnableTextureBatching && s_MultiBindSupported && batch.Count > 1)
        {
            // Use OpenGL 4.6 multi-bind for texture arrays
            glBindTextures(batch.StartBinding, batch.Count, batch.TextureIDs.data());
            
            // Update cache for all textures in batch
            if (m_Config.EnableCaching)
            {
                for (u32 i = 0; i < batch.Count; ++i)
                {
                    UpdateTextureCache(batch.BindingPoints[i], batch.TextureIDs[i], batch.Targets[i]);
                }
            }
            
            // Track profiling metrics
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TextureBinds, batch.Count);
        }
        else
        {
            // Fallback to individual bindings
            for (u32 i = 0; i < batch.Count; ++i)
            {
                u32 bindingPoint = batch.BindingPoints[i];
                u32 textureID = batch.TextureIDs[i];
                GLenum target = batch.Targets[i];

                if (m_Config.UseDirectStateAccess && s_DSASupported)
                {
                    // Use DSA binding
                    glBindTextureUnit(bindingPoint, textureID);
                }
                else
                {
                    // Traditional binding
                    glActiveTexture(GL_TEXTURE0 + bindingPoint);
                    glBindTexture(target, textureID);
                }

                // Update cache
                if (m_Config.EnableCaching)
                {
                    UpdateTextureCache(bindingPoint, textureID, target);
                }
            }
            
            // Track profiling metrics
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TextureBinds, batch.Count);
        }
    }

    void OpenGLMultiBind::SubmitBufferBatch(const BufferBatch& batch)
    {
        RENDERER_PROFILE_SCOPE("OpenGLMultiBind::SubmitBufferBatch");

        if (m_Config.EnableBufferBatching && s_MultiBindSupported && batch.Count > 1)
        {
            // Use OpenGL 4.6 multi-bind for buffer ranges
            if (batch.Target == GL_UNIFORM_BUFFER)
            {
                glBindBuffersRange(GL_UNIFORM_BUFFER, batch.StartBinding, batch.Count, 
                                 batch.BufferIDs.data(), 
                                 reinterpret_cast<const GLintptr*>(batch.Offsets.data()),
                                 reinterpret_cast<const GLsizeiptr*>(batch.Sizes.data()));
            }
            else if (batch.Target == GL_SHADER_STORAGE_BUFFER)
            {
                glBindBuffersRange(GL_SHADER_STORAGE_BUFFER, batch.StartBinding, batch.Count, 
                                 batch.BufferIDs.data(), 
                                 reinterpret_cast<const GLintptr*>(batch.Offsets.data()),
                                 reinterpret_cast<const GLsizeiptr*>(batch.Sizes.data()));
            }
            
            // Update cache for all buffers in batch
            if (m_Config.EnableCaching)
            {
                for (u32 i = 0; i < batch.Count; ++i)
                {
                    UpdateBufferCache(batch.BindingPoints[i], batch.BufferIDs[i], batch.Target);
                }
            }
            
            // Track profiling metrics
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, batch.Count);
        }
        else
        {
            // Fallback to individual bindings
            for (u32 i = 0; i < batch.Count; ++i)
            {
                u32 bindingPoint = batch.BindingPoints[i];
                u32 bufferID = batch.BufferIDs[i];
                size_t offset = batch.Offsets[i];
                size_t size = batch.Sizes[i];

                if (size > 0)
                {
                    glBindBufferRange(batch.Target, bindingPoint, bufferID, offset, size);
                }
                else
                {
                    glBindBufferBase(batch.Target, bindingPoint, bufferID);
                }

                // Update cache
                if (m_Config.EnableCaching)
                {
                    UpdateBufferCache(bindingPoint, bufferID, batch.Target);
                }
            }
            
            // Track profiling metrics
            RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, batch.Count);
        }
    }

    bool OpenGLMultiBind::ValidateTextureBatch(const TextureBatch& batch) const
    {
        if (batch.IsEmpty())
            return false;

        // Validate batch consistency
        if (batch.TextureIDs.size() != batch.BindingPoints.size() || 
            batch.TextureIDs.size() != batch.Targets.size() ||
            batch.TextureIDs.size() != batch.Count)
        {
            OLO_CORE_ERROR("Texture batch size mismatch");
            return false;
        }

        // Validate binding points are within range
        for (u32 bindingPoint : batch.BindingPoints)
        {
            if (bindingPoint >= s_MaxTextureUnits)
            {
                OLO_CORE_ERROR("Texture binding point {0} exceeds maximum {1}", bindingPoint, s_MaxTextureUnits);
                return false;
            }
        }

        return true;
    }

    bool OpenGLMultiBind::ValidateBufferBatch(const BufferBatch& batch) const
    {
        if (batch.IsEmpty())
            return false;

        // Validate batch consistency
        if (batch.BufferIDs.size() != batch.BindingPoints.size() || 
            batch.BufferIDs.size() != batch.Offsets.size() ||
            batch.BufferIDs.size() != batch.Sizes.size() ||
            batch.BufferIDs.size() != batch.Count)
        {
            OLO_CORE_ERROR("Buffer batch size mismatch");
            return false;
        }

        // Validate binding points are within range
        u32 maxBindings = (batch.Target == GL_UNIFORM_BUFFER) ? s_MaxUniformBufferBindings : s_MaxShaderStorageBufferBindings;
        for (u32 bindingPoint : batch.BindingPoints)
        {
            if (bindingPoint >= maxBindings)
            {
                OLO_CORE_ERROR("Buffer binding point {0} exceeds maximum {1} for target {2}", 
                              bindingPoint, maxBindings, batch.Target);
                return false;
            }
        }

        return true;
    }

    void OpenGLMultiBind::UpdateTextureCache(u32 binding, u32 textureID, GLenum target)
    {
        if (binding < m_StateCache.BoundTextures.size())
        {
            m_StateCache.BoundTextures[binding] = textureID;
            m_StateCache.TextureTargets[binding] = target;
            m_StateCache.IsValid = true;
        }
    }

    void OpenGLMultiBind::UpdateBufferCache(u32 binding, u32 bufferID, GLenum target)
    {
        if (binding < m_StateCache.BoundBuffers.size())
        {
            m_StateCache.BoundBuffers[binding] = bufferID;
            m_StateCache.BufferTargets[binding] = target;
            m_StateCache.IsValid = true;
        }
    }

    void OpenGLMultiBind::FlushTextureBatchIfNeeded()
    {
        if (m_CurrentTextureBatch.IsFull(m_Config.MaxTexturesPerBatch))
        {
            SubmitTextures();
        }
    }

    void OpenGLMultiBind::FlushBufferBatchIfNeeded()
    {
        if (m_CurrentBufferBatch.IsFull(m_Config.MaxBuffersPerBatch))
        {
            SubmitBuffers();
        }
    }

    bool OpenGLMultiBind::IsMultiBindSupported()
    {
        QueryCapabilities();
        return s_MultiBindSupported;
    }

    bool OpenGLMultiBind::IsDSASupported()
    {
        QueryCapabilities();
        return s_DSASupported;
    }

    u32 OpenGLMultiBind::GetMaxTextureUnits()
    {
        QueryCapabilities();
        return s_MaxTextureUnits;
    }

    u32 OpenGLMultiBind::GetMaxUniformBufferBindings()
    {
        QueryCapabilities();
        return s_MaxUniformBufferBindings;
    }

    u32 OpenGLMultiBind::GetMaxShaderStorageBufferBindings()
    {
        QueryCapabilities();
        return s_MaxShaderStorageBufferBindings;
    }

    void OpenGLMultiBind::QueryCapabilities()
    {
        if (s_CapabilitiesQueried)
            return;

        // Check OpenGL version and extensions
        GLint majorVersion, minorVersion;
        glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
        glGetIntegerv(GL_MINOR_VERSION, &minorVersion);

        // Multi-bind requires OpenGL 4.4+
        s_MultiBindSupported = (majorVersion > 4) || (majorVersion == 4 && minorVersion >= 4);
        
        // DSA requires OpenGL 4.5+
        s_DSASupported = (majorVersion > 4) || (majorVersion == 4 && minorVersion >= 5);

        // Query maximum binding points
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, reinterpret_cast<GLint*>(&s_MaxTextureUnits));
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, reinterpret_cast<GLint*>(&s_MaxUniformBufferBindings));
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, reinterpret_cast<GLint*>(&s_MaxShaderStorageBufferBindings));

        s_CapabilitiesQueried = true;

        OLO_CORE_INFO("OpenGL Multi-Bind Capabilities:");
        OLO_CORE_INFO("  OpenGL Version: {0}.{1}", majorVersion, minorVersion);
        OLO_CORE_INFO("  Multi-Bind Supported: {0}", s_MultiBindSupported ? "Yes" : "No");
        OLO_CORE_INFO("  DSA Supported: {0}", s_DSASupported ? "Yes" : "No");
        OLO_CORE_INFO("  Max Texture Units: {0}", s_MaxTextureUnits);
        OLO_CORE_INFO("  Max Uniform Buffer Bindings: {0}", s_MaxUniformBufferBindings);
        OLO_CORE_INFO("  Max Shader Storage Buffer Bindings: {0}", s_MaxShaderStorageBufferBindings);
    }
}
