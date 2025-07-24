#include "GPUResourceInspector.h"
#include "DebugUtils.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace OloEngine
{
    GPUResourceInspector::GPUResourceInspector()
    {
        m_ResourceCounts.fill(0);
        m_MemoryUsageByType.fill(0);
    }
    
    GPUResourceInspector::~GPUResourceInspector()
    {
        if (m_IsInitialized)
        {
            Shutdown();
        }
    }

    GPUResourceInspector& GPUResourceInspector::GetInstance()
    {
        static GPUResourceInspector instance;
        return instance;
    }
    
    void GPUResourceInspector::Initialize()
    {
        if (m_IsInitialized)
            return;

        OLO_CORE_INFO("Initializing GPU Resource Inspector");
        m_IsInitialized = true;
    }

    void GPUResourceInspector::Shutdown()
    {
        if (!m_IsInitialized)
            return;

        OLO_CORE_INFO("Shutting down GPU Resource Inspector");
          // Clean up any pending texture downloads
        for (auto& download : m_TextureDownloads)
        {
            if (download.m_PBO != 0)
            {
                glDeleteBuffers(1, &download.m_PBO);
            }
            if (download.m_Fence != nullptr)
            {
                glDeleteSync(download.m_Fence);
            }
        }
        m_TextureDownloads.clear();

        // Clean up resources
        {
            std::lock_guard<std::mutex> lock(m_ResourceMutex);
            m_Resources.clear();
        }

        m_IsInitialized = false;
    }
    
    void GPUResourceInspector::RegisterTexture(u32 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto textureInfo = CreateScope<TextureInfo>();
        textureInfo->m_RendererID = rendererID;
		textureInfo->m_Type = ResourceType::Texture2D;
        textureInfo->m_Name = name;
        textureInfo->m_DebugName = debugName.empty() ? name : debugName;
        textureInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();

          // Query texture properties immediately
        QueryTextureInfo(*textureInfo);
        
        sizet memoryUsage = textureInfo->m_MemoryUsage;
        
        // Check for duplicate registration
        auto existingIt = m_Resources.find(rendererID);
        bool isDuplicate = (existingIt != m_Resources.end());
        
        m_Resources[rendererID] = std::move(textureInfo);
          // Handle resource counting for new registrations and type changes
        if (!isDuplicate)
        {
            m_ResourceCounts[static_cast<sizet>(ResourceType::Texture2D)]++;
            m_MemoryUsageByType[static_cast<sizet>(ResourceType::Texture2D)] += memoryUsage;
        }
        else
        {
            // For duplicates, update memory tracking and handle type changes
            ResourceType oldType = existingIt->second->m_Type;
            sizet oldMemory = existingIt->second->m_MemoryUsage;
            m_MemoryUsageByType[static_cast<sizet>(oldType)] -= oldMemory;
            m_MemoryUsageByType[static_cast<sizet>(ResourceType::Texture2D)] += memoryUsage;
            
            // Handle resource count changes when type differs
            if (oldType != ResourceType::Texture2D)
            {
                m_ResourceCounts[static_cast<sizet>(oldType)]--;
                m_ResourceCounts[static_cast<sizet>(ResourceType::Texture2D)]++;
            }
            OLO_CORE_WARN("Registered DUPLICATE texture: {} (ID: {}) - replacing existing", name, rendererID);
        }
    }

    void GPUResourceInspector::RegisterTextureCubemap(u32 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto textureInfo = CreateScope<TextureInfo>();
        textureInfo->m_RendererID = rendererID;
		textureInfo->m_Type = ResourceType::TextureCubemap;
        textureInfo->m_Name = name;
        textureInfo->m_DebugName = debugName.empty() ? name : debugName;
        textureInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();
          // Query cubemap properties
        QueryTextureCubemapInfo(*textureInfo);
        
        sizet memoryUsage = textureInfo->m_MemoryUsage;
        
        // Check for duplicate registration
        auto existingIt = m_Resources.find(rendererID);
        bool isDuplicate = (existingIt != m_Resources.end());
        
        m_Resources[rendererID] = std::move(textureInfo);
          // Handle resource counting for new registrations and type changes
        if (!isDuplicate)
        {
            m_ResourceCounts[static_cast<sizet>(ResourceType::TextureCubemap)]++;
            m_MemoryUsageByType[static_cast<sizet>(ResourceType::TextureCubemap)] += memoryUsage;
        }
        else
        {
            // For duplicates, update memory tracking and handle type changes
            ResourceType oldType = existingIt->second->m_Type;
            sizet oldMemory = existingIt->second->m_MemoryUsage;
            m_MemoryUsageByType[static_cast<sizet>(oldType)] -= oldMemory;
            m_MemoryUsageByType[static_cast<sizet>(ResourceType::TextureCubemap)] += memoryUsage;
            
            // Handle resource count changes when type differs
            if (oldType != ResourceType::TextureCubemap)
            {
                m_ResourceCounts[static_cast<sizet>(oldType)]--;
                m_ResourceCounts[static_cast<sizet>(ResourceType::TextureCubemap)]++;
            }
            OLO_CORE_WARN("Registered DUPLICATE cubemap: {} (ID: {}) - replacing existing", name, rendererID);
        }
        
        OLO_CORE_TRACE("Registered texture cubemap: {} (ID: {})", name, rendererID);
    }
    
    void GPUResourceInspector::RegisterBuffer(u32 rendererID, GLenum target, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto bufferInfo = CreateScope<BufferInfo>();
        bufferInfo->m_RendererID = rendererID;
		bufferInfo->m_Target = target;
        bufferInfo->m_Name = name;
        bufferInfo->m_DebugName = debugName.empty() ? name : debugName;
        bufferInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();
        
        // Determine resource type based on target
        switch (target)
        {
            case GL_ARRAY_BUFFER:
                bufferInfo->m_Type = ResourceType::VertexBuffer;
                break;
            case GL_ELEMENT_ARRAY_BUFFER:
                bufferInfo->m_Type = ResourceType::IndexBuffer;
                break;
            case GL_UNIFORM_BUFFER:
                bufferInfo->m_Type = ResourceType::UniformBuffer;
                break;
            default:
                bufferInfo->m_Type = ResourceType::VertexBuffer; // Default fallback
                break;
        }
		
		// Query buffer properties immediately
        QueryBufferInfo(*bufferInfo);
        
        ResourceType bufferType = bufferInfo->m_Type;
        sizet memoryUsage = bufferInfo->m_MemoryUsage;
          // Check for duplicate registration
        auto existingIt = m_Resources.find(rendererID);
        bool isDuplicate = (existingIt != m_Resources.end());
        
        m_Resources[rendererID] = std::move(bufferInfo);
          // Handle resource counting for new registrations and type changes
        if (!isDuplicate)
        {
            m_ResourceCounts[static_cast<sizet>(bufferType)]++;
            m_MemoryUsageByType[static_cast<sizet>(bufferType)] += memoryUsage;
        }
        else
        {
            // For duplicates, update memory tracking and handle type changes
            ResourceType oldType = existingIt->second->m_Type;
            sizet oldMemory = existingIt->second->m_MemoryUsage;
            m_MemoryUsageByType[static_cast<sizet>(oldType)] -= oldMemory;
            m_MemoryUsageByType[static_cast<sizet>(bufferType)] += memoryUsage;
            
            // Handle resource count changes when type differs
            if (oldType != bufferType)
            {
                m_ResourceCounts[static_cast<sizet>(oldType)]--;
                m_ResourceCounts[static_cast<sizet>(bufferType)]++;
            }
            OLO_CORE_WARN("Registered DUPLICATE buffer: {} (ID: {}, Target: 0x{:X}) - replacing existing", name, rendererID, target);
        }
    }

    void GPUResourceInspector::RegisterFramebuffer(u32 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto framebufferInfo = CreateScope<FramebufferInfo>();
        framebufferInfo->m_RendererID = rendererID;
		framebufferInfo->m_Type = ResourceType::Framebuffer;
        framebufferInfo->m_Name = name;
        framebufferInfo->m_DebugName = debugName.empty() ? name : debugName;
        framebufferInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();
          // Query framebuffer properties
        QueryFramebufferInfo(*framebufferInfo);
        
        sizet memoryUsage = framebufferInfo->m_MemoryUsage;
        
        // Check for duplicate registration
        auto existingIt = m_Resources.find(rendererID);
        bool isDuplicate = (existingIt != m_Resources.end());
        
        m_Resources[rendererID] = std::move(framebufferInfo);
          // Handle resource counting for new registrations and type changes
        if (!isDuplicate)
        {
            m_ResourceCounts[static_cast<sizet>(ResourceType::Framebuffer)]++;
            m_MemoryUsageByType[static_cast<sizet>(ResourceType::Framebuffer)] += memoryUsage;
        }
        else
        {
            // For duplicates, update memory tracking and handle type changes
            ResourceType oldType = existingIt->second->m_Type;
            sizet oldMemory = existingIt->second->m_MemoryUsage;
            m_MemoryUsageByType[static_cast<sizet>(oldType)] -= oldMemory;
            m_MemoryUsageByType[static_cast<sizet>(ResourceType::Framebuffer)] += memoryUsage;
            
            // Handle resource count changes when type differs
            if (oldType != ResourceType::Framebuffer)
            {
                m_ResourceCounts[static_cast<sizet>(oldType)]--;
                m_ResourceCounts[static_cast<sizet>(ResourceType::Framebuffer)]++;
            }
            OLO_CORE_WARN("Registered DUPLICATE framebuffer: {} (ID: {}) - replacing existing", name, rendererID);
        }
    }

    void GPUResourceInspector::UnregisterResource(u32 rendererID)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            ResourceType type = it->second->m_Type;
            m_ResourceCounts[static_cast<sizet>(type)]--;
            m_MemoryUsageByType[static_cast<sizet>(type)] -= it->second->m_MemoryUsage;
            m_Resources.erase(it);
        }
    }

    void GPUResourceInspector::UpdateBindingStates()
    {
        if (!m_IsInitialized)
            return;

        // This would be called by the renderer to update binding states
        // For now, we'll implement basic texture binding detection
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        for (auto& [id, resource] : m_Resources)
        {
            resource->m_IsBound = false; // Reset binding state
            
            if (resource->m_Type == ResourceType::Texture2D)
            {
                // Check if this texture is bound to any texture unit
                // This is a simplified check - in practice, we'd need to track all texture units
                GLint currentTexture;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);
                if (static_cast<u32>(currentTexture) == resource->m_RendererID)
                {
                    resource->m_IsBound = true;
                    resource->m_BindingSlot = 0; // Assume texture unit 0 for simplicity
                }
            }
        }
    }

    void GPUResourceInspector::UpdateResourceActiveState(u32 rendererID, bool isActive)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            it->second->m_IsActive = isActive;
        }
    }

    void GPUResourceInspector::UpdateResourceBinding(u32 rendererID, bool isBound, u32 bindingSlot)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            it->second->m_IsBound = isBound;
            it->second->m_BindingSlot = bindingSlot;
        }
    }

    void GPUResourceInspector::QueryTextureInfo(TextureInfo& info)
    {
        // Modern OpenGL 4.5+ DSA approach - no texture binding required
        GLint width, height, internalFormat;
        glGetTextureLevelParameteriv(info.m_RendererID, 0, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(info.m_RendererID, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(info.m_RendererID, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        
        info.m_Width = static_cast<u32>(width);
        info.m_Height = static_cast<u32>(height);
        info.m_InternalFormat = static_cast<GLenum>(internalFormat);
        // Determine format and type based on internal format
        switch (internalFormat)
        {
            case GL_RGBA8:
            case GL_SRGB8_ALPHA8:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGB8:
            case GL_SRGB8:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RG8:
                info.m_Format = GL_RG;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_R8:
                info.m_Format = GL_RED;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGBA16F:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_RGB16F:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_RG16F:
                info.m_Format = GL_RG;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_R16F:
                info.m_Format = GL_RED;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_RGBA32F:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_RGB32F:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_RG32F:
                info.m_Format = GL_RG;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_R32F:
                info.m_Format = GL_RED;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_DEPTH_COMPONENT16:
            case GL_DEPTH_COMPONENT24:
            case GL_DEPTH_COMPONENT32:
                info.m_Format = GL_DEPTH_COMPONENT;
                info.m_DataType = GL_UNSIGNED_INT;
                break;
            case GL_DEPTH_COMPONENT32F:
                info.m_Format = GL_DEPTH_COMPONENT;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_DEPTH24_STENCIL8:
                info.m_Format = GL_DEPTH_STENCIL;
                info.m_DataType = GL_UNSIGNED_INT_24_8;
                break;
            case GL_DEPTH32F_STENCIL8:
                info.m_Format = GL_DEPTH_STENCIL;
                info.m_DataType = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
                break;
            default:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
        }
        
        // bytesPerPixel and switch-case removed (now unused)
        
        // Check for mip levels using DSA
        GLint maxLevel;
        glGetTextureParameteriv(info.m_RendererID, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        info.m_MipLevels = static_cast<u32>(maxLevel + 1);
        info.m_HasMips = maxLevel > 0;
        
        // Calculate accurate memory usage including compression and mip levels
        info.m_MemoryUsage = CalculateAccurateTextureMemoryUsage(info.m_RendererID, GL_TEXTURE_2D, 
                                                               info.m_InternalFormat, 
                                                               info.m_Width, info.m_Height, 
                                                               info.m_MipLevels);
    }
    
    void GPUResourceInspector::QueryTextureCubemapInfo(TextureInfo& info)
    {
        // Modern OpenGL 4.5+ DSA approach - no texture binding required
        GLint width, height, internalFormat;
        // For cubemaps, query the positive X face (they're all the same size)
        glGetTextureLevelParameteriv(info.m_RendererID, 0, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(info.m_RendererID, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(info.m_RendererID, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        
        info.m_Width = static_cast<u32>(width);
        info.m_Height = static_cast<u32>(height);
        info.m_InternalFormat = static_cast<GLenum>(internalFormat);
          // Determine format and type based on internal format (same as texture 2D)
        switch (internalFormat)
        {
            case GL_RGBA8:
            case GL_SRGB8_ALPHA8:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGB8:
            case GL_SRGB8:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RG8:
                info.m_Format = GL_RG;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_R8:
                info.m_Format = GL_RED;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGBA16F:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_RGB16F:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_RG16F:
                info.m_Format = GL_RG;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_R16F:
                info.m_Format = GL_RED;
                info.m_DataType = GL_HALF_FLOAT;
                break;
            case GL_RGBA32F:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_RGB32F:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_RG32F:
                info.m_Format = GL_RG;
                info.m_DataType = GL_FLOAT;
                break;
            case GL_R32F:
                info.m_Format = GL_RED;
                info.m_DataType = GL_FLOAT;
                break;
            default:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
        }
        
        // bytesPerPixel and switch-case removed (now unused)
        
        // Check for mip levels using DSA
        GLint maxLevel;
        glGetTextureParameteriv(info.m_RendererID, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        info.m_MipLevels = static_cast<u32>(maxLevel + 1);
        info.m_HasMips = maxLevel > 0;
          // Calculate accurate memory usage for cubemap (6 faces) including compression and mip levels
        info.m_MemoryUsage = CalculateAccurateTextureMemoryUsage(info.m_RendererID, GL_TEXTURE_CUBE_MAP, 
                                                               info.m_InternalFormat, 
                                                               info.m_Width, info.m_Height, 
                                                               info.m_MipLevels);
    }

    GLenum GPUResourceInspector::GetBufferBindingQuery(GLenum target)
    {
        switch (target)
        {
            case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
            case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
            case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
            case GL_SHADER_STORAGE_BUFFER: return GL_SHADER_STORAGE_BUFFER_BINDING;
            case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
            case GL_ATOMIC_COUNTER_BUFFER: return GL_ATOMIC_COUNTER_BUFFER_BINDING;
            case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER_BINDING;
            case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
            case GL_DISPATCH_INDIRECT_BUFFER: return GL_DISPATCH_INDIRECT_BUFFER_BINDING;
            case GL_DRAW_INDIRECT_BUFFER: return GL_DRAW_INDIRECT_BUFFER_BINDING;
            case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
            case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
            case GL_QUERY_BUFFER: return GL_QUERY_BUFFER_BINDING;
            case GL_TEXTURE_BUFFER: return GL_TEXTURE_BUFFER_BINDING;
            default:
                OLO_CORE_WARN("Unknown buffer target 0x{0:X}, falling back to GL_ARRAY_BUFFER_BINDING", target);
                return GL_ARRAY_BUFFER_BINDING;
        }
    }

    void GPUResourceInspector::QueryBufferInfo(BufferInfo& info)
    {
        // Save current buffer binding for this target
        GLint previousBinding = 0;
        GLenum bindingQuery = GPUResourceInspector::GetBufferBindingQuery(info.m_Target);
        glGetIntegerv(bindingQuery, &previousBinding);

        // Bind the buffer temporarily to query its properties
        glBindBuffer(info.m_Target, info.m_RendererID);

        GLint size, usage;
        glGetBufferParameteriv(info.m_Target, GL_BUFFER_SIZE, &size);
        glGetBufferParameteriv(info.m_Target, GL_BUFFER_USAGE, &usage);

        info.m_Size = static_cast<u32>(size);
        info.m_Usage = static_cast<GLenum>(usage);
        info.m_MemoryUsage = static_cast<sizet>(size);

        // Restore previous buffer binding
        glBindBuffer(info.m_Target, previousBinding);
    }

    void GPUResourceInspector::QueryFramebufferInfo(FramebufferInfo& info)
    {
        // Save current framebuffer binding
        GLint previousBinding;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousBinding);
        
        // Bind the framebuffer temporarily to query its properties
        glBindFramebuffer(GL_FRAMEBUFFER, info.m_RendererID);
        
        // Check framebuffer status
        info.m_Status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        
        // Query color attachments
        info.m_ColorAttachmentCount = 0;
        info.m_ColorAttachmentFormats.clear();
        
        for (u32 i = 0; i < 8; ++i) 
        {
            GLint attachmentType;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, 
                                                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachmentType);
            
            if (attachmentType != GL_NONE)
            {
                info.m_ColorAttachmentCount++;
                
                GLint internalFormat;
                glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, 
                                                    GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &internalFormat);
                info.m_ColorAttachmentFormats.push_back(static_cast<GLenum>(internalFormat));
            }
        }
        
        // Check depth attachment
        GLint depthAttachmentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthAttachmentType);
        info.m_HasDepthAttachment = (depthAttachmentType != GL_NONE);
        
        if (info.m_HasDepthAttachment)
        {
            GLint depthFormat;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 
                                                GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &depthFormat);
            info.m_DepthAttachmentFormat = static_cast<GLenum>(depthFormat);
        }
        
        // Check stencil attachment
        GLint stencilAttachmentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, 
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilAttachmentType);
        info.m_HasStencilAttachment = (stencilAttachmentType != GL_NONE);
        
        if (info.m_HasStencilAttachment)
        {
            GLint stencilFormat;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, 
                                                GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &stencilFormat);
            info.m_StencilAttachmentFormat = static_cast<GLenum>(stencilFormat);
        }
        
        // Estimate memory usage (simplified)
        if (info.m_ColorAttachmentCount > 0)
        {
            // Get dimensions from first color attachment if available
            GLint textureID;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                                                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &textureID);
              if (textureID != 0)
            {
                GLint width, height;
                glGetTextureLevelParameteriv(textureID, 0, GL_TEXTURE_WIDTH, &width);
                glGetTextureLevelParameteriv(textureID, 0, GL_TEXTURE_HEIGHT, &height);
                
                info.m_Width = static_cast<u32>(width);
                info.m_Height = static_cast<u32>(height);
                
                // Estimate memory usage (simplified calculation)
                info.m_MemoryUsage = static_cast<sizet>(width * height * 4 * info.m_ColorAttachmentCount);
                if (info.m_HasDepthAttachment) info.m_MemoryUsage += static_cast<sizet>(width * height * 4);
                if (info.m_HasStencilAttachment) info.m_MemoryUsage += static_cast<sizet>(width * height);
            }
        }
        
        // Restore previous framebuffer binding
        glBindFramebuffer(GL_FRAMEBUFFER, previousBinding);
    }
        void GPUResourceInspector::ProcessTextureDownloads()
    {
        // Process async texture downloads and check for completion using modern sync objects
        auto it = m_TextureDownloads.begin();
        while (it != m_TextureDownloads.end())
        {
            if (it->m_InProgress)
            {
                bool downloadComplete = false;
                  // Modern OpenGL 4.5+ approach: Use sync objects for non-blocking completion detection
                if (it->m_Fence != nullptr)
                {
                    // Check fence status without blocking
                    GLenum result = glClientWaitSync(it->m_Fence, 0, 0); // 0 timeout = non-blocking
                    
                    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED)
                    {
                        // Download is complete!
                        downloadComplete = true;
                        OLO_CORE_TRACE("Texture download completed for texture {} (sync object signaled)", it->m_TextureID);
                    }
                    else if (result == GL_WAIT_FAILED)
                    {
                        // Sync object failed - this shouldn't happen but handle gracefully
                        OLO_CORE_WARN("Sync object wait failed for texture {}", it->m_TextureID);
                        downloadComplete = true; // Force completion to avoid hanging
                    }
                    // GL_TIMEOUT_EXPIRED means not ready yet - continue to next frame
                }
                else
                {
                    // No sync object available - this shouldn't happen with modern approach
                    OLO_CORE_WARN("No sync fence available for texture download {}", it->m_TextureID);
                    downloadComplete = true; // Force completion to avoid hanging
                }
                
                if (downloadComplete)
                {
                    // Find the corresponding texture and complete the download
                    auto resourceIt = m_Resources.find(it->m_TextureID);
                    if (resourceIt != m_Resources.end() && resourceIt->second->m_Type == ResourceType::Texture2D)
                    {
                        auto* texInfo = static_cast<TextureInfo*>(resourceIt->second.get());
                        CompleteTextureDownload(*texInfo, *it);
                    }
                    
                    // Clean up resources
                    if (it->m_Fence != nullptr)
                    {
                        glDeleteSync(it->m_Fence);
                    }
                    glDeleteBuffers(1, &it->m_PBO);
                    
                    // Remove completed download from queue
                    it = m_TextureDownloads.erase(it);
                }
                else
                {
                    // Check for timeout (5 seconds)
                    f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
                    if (currentTime - it->m_RequestTime > 5.0)
                    {
                        OLO_CORE_WARN("Texture download timeout for texture {}, mip level {}", it->m_TextureID, it->m_MipLevel);
                        
                        // Clean up resources
                        if (it->m_Fence != nullptr)
                        {
                            glDeleteSync(it->m_Fence);
                        }
                        glDeleteBuffers(1, &it->m_PBO);
                        it = m_TextureDownloads.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            else
            {
                ++it;
            }
        }
    }

    void GPUResourceInspector::RequestTextureDownload(TextureInfo& info, u32 mipLevel)
    {
        // Check if there's already a pending download for this texture
        for (const auto& download : m_TextureDownloads)
        {
            if (download.m_TextureID == info.m_RendererID && download.m_MipLevel == mipLevel)
                return;
        }
        
        GLuint pbo;
        glGenBuffers(1, &pbo);
        
        if (pbo == 0)
        {
            OLO_CORE_WARN("Failed to create PBO for texture download");
            return;
        }
          // Calculate data size for this mip level - use RGBA format for consistency
        u32 width = std::max(1u, info.m_Width >> mipLevel);
        u32 height = std::max(1u, info.m_Height >> mipLevel);
        u32 bytesPerPixel = 4; // Always use RGBA format for downloads
        sizet dataSize = width * height * bytesPerPixel;
        // Modern OpenGL 4.5+ approach: Use immutable buffer storage + DSA
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        
        // Use modern immutable buffer storage (OpenGL 4.4+)
        glBufferStorage(GL_PIXEL_PACK_BUFFER, dataSize, nullptr, GL_MAP_READ_BIT | GL_DYNAMIC_STORAGE_BIT);

        // Modern OpenGL 4.5+ DSA: Direct texture access without state changes
        // Use DSA - no texture binding required!
        glGetTextureSubImage(info.m_RendererID, mipLevel, 0, 0, 0, width, height, 1, 
                           GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(dataSize), nullptr);
        
        // Unbind PBO
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        
        // Create modern sync object for better async completion detection (OpenGL 3.2+)
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence == nullptr)
        {
            OLO_CORE_WARN("Failed to create sync fence for texture download");
            glDeleteBuffers(1, &pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return;
        }
        
        // Restore PBO binding
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        
        // Add to download queue
        TextureDownloadRequest request;
        request.m_TextureID = info.m_RendererID;
        request.m_MipLevel = mipLevel;
        request.m_PBO = pbo;
        request.m_Fence = fence;
        request.m_InProgress = true;
        request.m_RequestTime = DebugUtils::GetCurrentTimeSeconds();
        
        m_TextureDownloads.push_back(request);
        
        OLO_CORE_TRACE("Requested async texture download for texture {} mip level {}", info.m_RendererID, mipLevel);
    }
        void GPUResourceInspector::UpdateTexturePreview(TextureInfo& info)
    {
        if (info.m_PreviewDataValid)
            return;

        // Check if there's already a pending download for this texture and mip level
        for (const auto& download : m_TextureDownloads)
        {
            if (download.m_TextureID == info.m_RendererID && download.m_MipLevel == info.m_SelectedMipLevel)
            {
                // Download already in progress, just wait
                return;
            }
        }        // Check if texture is valid using modern OpenGL 4.5+ DSA
        GLint width, height;
        glGetTextureLevelParameteriv(info.m_RendererID, info.m_SelectedMipLevel, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(info.m_RendererID, info.m_SelectedMipLevel, GL_TEXTURE_HEIGHT, &height);
        
        if (width <= 0 || height <= 0)
        {
            // Invalid mip level or texture
            return;
        }

        // Start async download instead of blocking
        RequestTextureDownload(info, info.m_SelectedMipLevel);
    }
    
    void GPUResourceInspector::UpdateBufferPreview(BufferInfo& info)
    {
        if (info.m_ContentPreviewValid)
            return;

        // Save current buffer binding for this target
        GLint previousBinding = 0;
        GLenum bindingQuery = GPUResourceInspector::GetBufferBindingQuery(info.m_Target);
        glGetIntegerv(bindingQuery, &previousBinding);

        // Bind buffer and map data
        glBindBuffer(info.m_Target, info.m_RendererID);

        u32 previewSize = std::min(info.m_Size, info.m_PreviewSize);
        info.m_ContentPreview.resize(previewSize);

        // Map buffer and copy data
        void* data = glMapBuffer(info.m_Target, GL_READ_ONLY);
        if (data)
        {
            memcpy(info.m_ContentPreview.data(), static_cast<const u8*>(data) + info.m_PreviewOffset, previewSize);
            glUnmapBuffer(info.m_Target);
            info.m_ContentPreviewValid = true;
        }
		else
		{
			OLO_CORE_WARN("Failed to map buffer for preview: ID {}", info.m_RendererID);
			info.m_ContentPreviewValid = false;
		}

        // Restore previous buffer binding
        glBindBuffer(info.m_Target, previousBinding);
    }

    sizet GPUResourceInspector::GetMemoryUsage(ResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        return m_MemoryUsageByType[static_cast<sizet>(type)];
    }

    sizet GPUResourceInspector::GetTotalMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        sizet total = 0;
        for (const auto& [id, resource] : m_Resources)
        {
            total += resource->m_MemoryUsage;
        }
        return total;
    }    void GPUResourceInspector::RenderDebugView(bool* open, const char* title)
    {
        if (!m_IsInitialized)
            return;

        // Process any pending texture downloads to prevent stalls
        ProcessTextureDownloads();

        if (!ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar))
        {
            ImGui::End();
            return;
        }

        // Menu bar
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("View"))
            {
                ImGui::Checkbox("Show Inactive Resources", &m_ShowInactiveResources);
                ImGui::Checkbox("Auto Update Previews", &m_AutoUpdatePreviews);
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Export"))
            {
                if (ImGui::MenuItem("Export to CSV"))
                {
                    ExportToCSV("gpu_resources.csv");
                }
                ImGui::EndMenu();
            }
            
            ImGui::EndMenuBar();
        }

        // Statistics section
        RenderResourceStatistics();
        
        ImGui::Separator();
        
        // Filter controls
        ImGui::Text("Filters:");
        ImGui::SameLine();
        
        const char* typeNames[] = { "All", "Textures", "Cubemaps", "Vertex Buffers", "Index Buffers", "Uniform Buffers", "Framebuffers" };
        int currentFilter = static_cast<int>(m_FilterType) + 1;
        if (ImGui::Combo("Type", &currentFilter, typeNames, IM_ARRAYSIZE(typeNames)))
        {
            m_FilterType = (currentFilter == 0) ? ResourceType::COUNT : static_cast<ResourceType>(currentFilter - 1);
        }
        
        ImGui::SameLine();        // Create a buffer for InputText (ImGui needs a char buffer)
        static char searchBuffer[256] = "";
        if (ImGui::InputText("Search", searchBuffer, sizeof(searchBuffer)))
        {
            m_SearchFilter = std::string(searchBuffer);
        }
          ImGui::Separator();
        
        // Split view: resource tree on left, details on right
        static float leftPaneWidth = 300.0f;
        
        // Resource tree pane
        ImGui::BeginChild("ResourceTree", ImVec2(leftPaneWidth, -1), true);
        RenderResourceTree();
        ImGui::EndChild();
        
        // Splitter
        ImGui::SameLine();
        ImGui::Button("##splitter", ImVec2(8.0f, -1));
        
        if (ImGui::IsItemActive())
        {
            leftPaneWidth += ImGui::GetIO().MouseDelta.x;
            leftPaneWidth = std::clamp(leftPaneWidth, 100.0f, ImGui::GetContentRegionAvail().x - 100.0f);
        }

        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        
        // Resource details pane
        ImGui::SameLine();
        ImGui::BeginChild("ResourceDetails", ImVec2(-1, -1), true);
        RenderResourceDetails();
        ImGui::EndChild();

        ImGui::End();
    }

    void GPUResourceInspector::RenderResourceTree()
    {
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        ImGui::Text("Resources (%u)", GetResourceCount());
        ImGui::Separator();
        
        // Debug: Show filter state and actual resource counts
        u32 totalResources = static_cast<u32>(m_Resources.size());
        u32 activeResources = 0;
        u32 inactiveResources = 0;
        
        for (const auto& [id, resource] : m_Resources)
        {
            if (resource->m_IsActive)
                activeResources++;
            else
                inactiveResources++;
        }
        
        ImGui::Text("Total: %u, Active: %u, Inactive: %u", totalResources, activeResources, inactiveResources);
        if (!m_ShowInactiveResources && inactiveResources > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(%u hidden)", inactiveResources);
        }
        ImGui::Separator();
        
        // Group resources by type
        std::unordered_map<ResourceType, std::vector<ResourceInfo*>> groupedResources;
        
        for (const auto& [id, resource] : m_Resources)
        {
            // Apply filters
            if (m_FilterType != ResourceType::COUNT && resource->m_Type != m_FilterType)
                continue;
                
            if (!m_SearchFilter.empty())
            {
                std::string searchLower = m_SearchFilter;
                std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
                
                std::string nameLower = resource->m_Name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                
                if (nameLower.find(searchLower) == std::string::npos)
                    continue;
            }
            
            if (!m_ShowInactiveResources && !resource->m_IsActive)
                continue;
            
            groupedResources[resource->m_Type].push_back(resource.get());
        }
        
        // Render tree nodes by type
        for (int i = 0; i < static_cast<int>(ResourceType::COUNT); ++i)
        {
            ResourceType type = static_cast<ResourceType>(i);
            const auto& resources = groupedResources[type];
            
            if (resources.empty())
                continue;
                
            if (ImGui::TreeNode(GetResourceTypeName(type)))
            {
                for (const auto* resource : resources)
                {
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (resource->m_RendererID == m_SelectedResourceID)
                        flags |= ImGuiTreeNodeFlags_Selected;
                    
                    std::string label = resource->m_DebugName.empty() ? resource->m_Name : resource->m_DebugName;
                    if (label.empty())
                        label = "Unnamed Resource";
                    
                    // Add memory usage to label
                    label += " (" + FormatMemorySize(resource->m_MemoryUsage) + ")";
                      if (resource->m_IsBound)
                        label += " [BOUND]";
                    
                    // Create unique ID for this tree node using resource ID
                    std::string uniqueID = label + "##" + std::to_string(resource->m_RendererID);
                    ImGui::TreeNodeEx(uniqueID.c_str(), flags);
                    
                    if (ImGui::IsItemClicked())
                    {
                        m_SelectedResourceID = resource->m_RendererID;
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    void GPUResourceInspector::RenderResourceDetails()
    {
        if (m_SelectedResourceID == 0)
        {
            ImGui::Text("Select a resource to view details");
            return;
        }
        
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto it = m_Resources.find(m_SelectedResourceID);
        if (it == m_Resources.end())
        {
            ImGui::Text("Selected resource not found");
            return;
        }
        
        ResourceInfo* resource = it->second.get();
        
        ImGui::Text("Resource Details");
        ImGui::Separator();
        
        ImGui::Text("ID: %u", resource->m_RendererID);
        ImGui::Text("Type: %s", GetResourceTypeName(resource->m_Type));
        ImGui::Text("Name: %s", resource->m_Name.c_str());
        if (!resource->m_DebugName.empty() && resource->m_DebugName != resource->m_Name)
        {
            ImGui::Text("Debug Name: %s", resource->m_DebugName.c_str());
        }
        ImGui::Text("Memory Usage: %s", FormatMemorySize(resource->m_MemoryUsage).c_str());
        ImGui::Text("Active: %s", resource->m_IsActive ? "Yes" : "No");
        ImGui::Text("Bound: %s", resource->m_IsBound ? "Yes" : "No");
        if (resource->m_IsBound)
            ImGui::Text("Binding Slot: %u", resource->m_BindingSlot);
        
        ImGui::Separator();
        
        // Type-specific details
        if (resource->m_Type == ResourceType::Texture2D || resource->m_Type == ResourceType::TextureCubemap)
        {
            RenderTexturePreview(*static_cast<TextureInfo*>(resource));
        }
        else if (resource->m_Type == ResourceType::VertexBuffer || 
                 resource->m_Type == ResourceType::IndexBuffer || 
                 resource->m_Type == ResourceType::UniformBuffer)
        {
            RenderBufferContent(*static_cast<BufferInfo*>(resource));
        }
        else if (resource->m_Type == ResourceType::Framebuffer)
        {
            RenderFramebufferDetails(*static_cast<FramebufferInfo*>(resource));
        }
    }
    
    void GPUResourceInspector::RenderTexturePreview(TextureInfo& info)
    {
        ImGui::Text("Texture Properties");
        ImGui::Text("Dimensions: %u x %u", info.m_Width, info.m_Height);
        ImGui::Text("Internal Format: %s", FormatTextureFormat(info.m_InternalFormat).c_str());
        ImGui::Text("Mip Levels: %u", info.m_MipLevels);
        ImGui::Text("Has Mipmaps: %s", info.m_HasMips ? "Yes" : "No");
        
        // Special handling for cubemaps
        if (info.m_Type == ResourceType::TextureCubemap)
        {
            ImGui::Text("Cubemap Faces: 6");
            
            // Face selection for cubemaps
            const char* faceNames[] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
            static int selectedFace = 0;
            if (ImGui::Combo("Face", &selectedFace, faceNames, 6))
            {
                info.m_PreviewDataValid = false; // Force refresh for new face
                // Store selected face in a custom field (could extend TextureInfo for this)
            }
        }
        
        if (info.m_HasMips)
        {
            ImGui::SliderInt("Mip Level", reinterpret_cast<int*>(&info.m_SelectedMipLevel), 0, static_cast<int>(info.m_MipLevels - 1));
            if (ImGui::IsItemEdited())
            {
                info.m_PreviewDataValid = false; // Force refresh
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("Refresh Preview"))
        {
            info.m_PreviewDataValid = false;
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Save to File"))
        {
            // TODO: Implement texture save functionality
            OLO_CORE_INFO("Texture save functionality not yet implemented");
        }
          if (m_AutoUpdatePreviews || ImGui::IsItemClicked())
        {
            // Only try to update preview if we have valid dimensions
            if (info.m_Width > 0 && info.m_Height > 0 && info.m_SelectedMipLevel < info.m_MipLevels)
            {
                UpdateTexturePreview(info);
            }
        }
        
        if (info.m_PreviewDataValid && !info.m_PreviewData.empty())
        {
            // Create ImGui texture if not already created
            if (info.m_ImGuiTextureID == 0)
            {
                // This is simplified - in practice, we'd create a proper ImGui texture
                info.m_ImGuiTextureID = static_cast<ImTextureID>(static_cast<uintptr_t>(info.m_RendererID));
            }
            
            static float zoom = 1.0f;
            ImGui::SliderFloat("Zoom", &zoom, 0.1f, 4.0f);
            
            ImVec2 imageSize(256 * zoom, 256 * zoom);
            
            ImVec2 availableSize = ImGui::GetContentRegionAvail();
            if (imageSize.x > availableSize.x)
            {
                float scale = availableSize.x / imageSize.x;
                imageSize.x *= scale;
                imageSize.y *= scale;
            }
            if (imageSize.y > availableSize.y - 60) // Leave some space for controls
            {
                float scale = (availableSize.y - 60) / imageSize.y;
                imageSize.x *= scale;
                imageSize.y *= scale;
            }
            
            ImGui::Image(info.m_ImGuiTextureID, imageSize);
            
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Texture Preview\nSize: %u x %u\nFormat: %s\nClick to view full size", 
                                 info.m_Width, info.m_Height, FormatTextureFormat(info.m_InternalFormat).c_str());
            }
            
            // Show texture statistics
            ImGui::Separator();
            ImGui::Text("Preview Info");
            ImGui::Text("Displayed Size: %.0f x %.0f", imageSize.x, imageSize.y);
            ImGui::Text("Memory Usage: %s", FormatMemorySize(info.m_MemoryUsage).c_str());
        }
        else
        {
            ImGui::Text("Preview not available");
            if (info.m_Width == 0 || info.m_Height == 0)
            {
                ImGui::Text("(Invalid texture dimensions)");
            }
            else if (info.m_SelectedMipLevel >= info.m_MipLevels)
            {
                ImGui::Text("(Invalid mip level selected)");
            }
            else
            {
                ImGui::Text("(Texture may be too large, compressed, or use unsupported format)");
            }
            
            if (ImGui::Button("Try Download Preview"))
            {
                info.m_PreviewDataValid = false;
                if (info.m_Width > 0 && info.m_Height > 0 && info.m_SelectedMipLevel < info.m_MipLevels)
                {
                    UpdateTexturePreview(info);
                }
            }
        }
    }
    
    void GPUResourceInspector::RenderBufferContent(BufferInfo& info)
    {
        ImGui::Text("Buffer Properties");
        ImGui::Text("Target: 0x%X (%s)", info.m_Target, GetBufferTargetName(info.m_Target));
        ImGui::Text("Usage: %s", FormatBufferUsage(info.m_Usage).c_str());
        ImGui::Text("Size: %s", FormatMemorySize(info.m_Size).c_str());
        
        if (info.m_Type == ResourceType::VertexBuffer)
        {
            ImGui::Separator();
            ImGui::Text("Vertex Buffer Layout");
            if (ImGui::InputInt("Stride (bytes)", reinterpret_cast<int*>(&info.m_Stride)))
            {
                info.m_Stride = std::max(1u, info.m_Stride); // Ensure stride is at least 1
            }
            
            if (info.m_Stride > 0 && info.m_ContentPreviewValid && !info.m_ContentPreview.empty())
            {
                ImGui::Text("Vertex Count (estimated): %u", info.m_Size / info.m_Stride);
                
                // Show structured vertex data if stride is set
                ImGui::Separator();
                ImGui::Text("Vertex Data (first 10 vertices):");
                
                const u8* data = info.m_ContentPreview.data();
                sizet size = info.m_ContentPreview.size();
                u32 vertexCount = std::min(10u, static_cast<u32>(size / info.m_Stride));
                
                if (ImGui::BeginTable("VertexData", std::min(info.m_Stride / 4 + 2, 8u), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Vertex");
                    for (u32 i = 0; i < std::min(info.m_Stride / 4, 7u); ++i)
                    {
                        ImGui::TableSetupColumn(("Float" + std::to_string(i)).c_str());
                    }
                    ImGui::TableHeadersRow();
                    
                    for (u32 v = 0; v < vertexCount; ++v)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%u", v);
                        
                        const f32* vertexData = reinterpret_cast<const f32*>(data + v * info.m_Stride);
                        u32 floatCount = std::min(info.m_Stride / 4, 7u);
                        
                        for (u32 f = 0; f < floatCount; ++f)
                        {
                            ImGui::TableSetColumnIndex(f + 1);
                            ImGui::Text("%.3f", vertexData[f]);
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        else if (info.m_Type == ResourceType::IndexBuffer)
        {
            ImGui::Separator();
            ImGui::Text("Index Buffer");
            
            if (info.m_ContentPreviewValid && !info.m_ContentPreview.empty())
            {
                // Assume 32-bit indices for now (could be improved to detect 16-bit vs 32-bit)
                u32 indexCount = info.m_Size / sizeof(u32);
                ImGui::Text("Index Count (estimated): %u", indexCount);
                
                // Show first few indices
                ImGui::Text("Indices (first 20):");
                const u32* indices = reinterpret_cast<const u32*>(info.m_ContentPreview.data());
                sizet previewIndices = std::min(20u, static_cast<u32>(info.m_ContentPreview.size() / sizeof(u32)));
                
                std::string indexString;
                for (sizet i = 0; i < previewIndices; ++i)
                {
                    if (i > 0) indexString += ", ";
                    indexString += std::to_string(indices[i]);
                }
                ImGui::Text("%s", indexString.c_str());
            }
        }
        
        ImGui::Separator();
        
        ImGui::InputInt("Preview Offset", reinterpret_cast<int*>(&info.m_PreviewOffset));
        ImGui::InputInt("Preview Size", reinterpret_cast<int*>(&info.m_PreviewSize));
        
        if (ImGui::Button("Refresh Content"))
        {
            info.m_ContentPreviewValid = false;
        }
        
        if (m_AutoUpdatePreviews || ImGui::IsItemClicked())
        {
            UpdateBufferPreview(info);
        }
        
        if (info.m_ContentPreviewValid && !info.m_ContentPreview.empty())
        {
            ImGui::Separator();
            ImGui::Text("Raw Content Preview (Hex Dump):");
            
            // Hex dump display
            const u8* data = info.m_ContentPreview.data();
            sizet size = info.m_ContentPreview.size();
            
            for (sizet i = 0; i < size; i += 16)
            {
                // Address
                ImGui::Text("%08X: ", static_cast<u32>(info.m_PreviewOffset + i));
                ImGui::SameLine();
                
                // Hex bytes
                for (sizet j = 0; j < 16 && (i + j) < size; ++j)
                {
                    ImGui::SameLine();
                    ImGui::Text("%02X", data[i + j]);
                }
                
                // ASCII representation
                ImGui::SameLine();
                ImGui::Text("  ");
                for (sizet j = 0; j < 16 && (i + j) < size; ++j)
                {
                    ImGui::SameLine();
                    char c = static_cast<char>(data[i + j]);
                    ImGui::Text("%c", (c >= 32 && c <= 126) ? c : '.');
                }
            }
        }
        else
        {
            ImGui::Text("Content preview not available");
        }
    }

    void GPUResourceInspector::RenderFramebufferDetails(FramebufferInfo& info)
    {
        ImGui::Text("Framebuffer Properties");
        ImGui::Text("Dimensions: %u x %u", info.m_Width, info.m_Height);
        
        // Framebuffer status
        const char* statusText = "Unknown";
        ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        
        switch (info.m_Status)
        {
            case GL_FRAMEBUFFER_COMPLETE:
                statusText = "Complete";
                statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
                break;
            case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
                statusText = "Incomplete Attachment";
                statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red
                break;
            case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
                statusText = "Missing Attachment";
                statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red
                break;
            case GL_FRAMEBUFFER_UNSUPPORTED:
                statusText = "Unsupported";
                statusColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); // Orange
                break;
        }
        
        ImGui::Text("Status: ");
        ImGui::SameLine();
        ImGui::TextColored(statusColor, "%s", statusText);
        
        ImGui::Separator();
        
        // Color attachments
        ImGui::Text("Color Attachments: %u", info.m_ColorAttachmentCount);
        for (u32 i = 0; i < info.m_ColorAttachmentCount; ++i)
        {
            if (i < info.m_ColorAttachmentFormats.size())
            {
                ImGui::Text("  Attachment %u: Format 0x%X", i, info.m_ColorAttachmentFormats[i]);
            }
            else
            {
                ImGui::Text("  Attachment %u: Unknown format", i);
            }
        }
        
        // Depth attachment
        if (info.m_HasDepthAttachment)
        {
            ImGui::Text("Depth Attachment: Format 0x%X", info.m_DepthAttachmentFormat);
        }
        else
        {
            ImGui::Text("Depth Attachment: None");
        }
        
        // Stencil attachment
        if (info.m_HasStencilAttachment)
        {
            ImGui::Text("Stencil Attachment: Format 0x%X", info.m_StencilAttachmentFormat);
        }
        else
        {
            ImGui::Text("Stencil Attachment: None");
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("Refresh"))
        {
            // Force refresh of framebuffer info
            QueryFramebufferInfo(info);
        }
    }
    
    void GPUResourceInspector::RenderResourceStatistics()
    {
        ImGui::Text("Statistics");
        ImGui::Separator();
        
        // Count actual resources in map by type and calculate memory usage
        std::array<u32, static_cast<sizet>(ResourceType::COUNT)> actualCounts = {};
        std::array<sizet, static_cast<sizet>(ResourceType::COUNT)> actualMemoryUsage = {};
        sizet totalMemory = 0;
        
        {
            std::lock_guard<std::mutex> lock(m_ResourceMutex);
            for (const auto& [id, resource] : m_Resources)
            {
                sizet typeIndex = static_cast<sizet>(resource->m_Type);
                actualCounts[typeIndex]++;
                actualMemoryUsage[typeIndex] += resource->m_MemoryUsage;
                totalMemory += resource->m_MemoryUsage;
            }
        }
        
        ImGui::Text("Total Resources: %u", GetResourceCount());
        ImGui::Text("Total Memory: %s", FormatMemorySize(totalMemory).c_str());
        
        // Memory usage by type (only show types that have resources)
        for (int i = 0; i < static_cast<int>(ResourceType::COUNT); ++i)
        {
            ResourceType type = static_cast<ResourceType>(i);
            u32 count = actualCounts[i];
            if (count > 0)
            {
                sizet memory = actualMemoryUsage[i];
                ImGui::Text("%s: %u (%s)", GetResourceTypeName(type), count, FormatMemorySize(memory).c_str());
            }
        }
    }

    void GPUResourceInspector::ExportToCSV(const std::string& filename)
    {
        std::ofstream file(filename);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("Failed to open file for export: {}", filename);
            return;
        }
        
        // CSV header
        file << "ID,Type,Name,DebugName,MemoryUsage,Active,Bound,CreationTime\n";
        
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        for (const auto& [id, resource] : m_Resources)
        {
            file << resource->m_RendererID << ","
                 << GetResourceTypeName(resource->m_Type) << ","
                 << "\"" << resource->m_Name << "\","
                 << "\"" << resource->m_DebugName << "\","
                 << resource->m_MemoryUsage << ","
                 << (resource->m_IsActive ? "true" : "false") << ","
                 << (resource->m_IsBound ? "true" : "false") << ","
                 << resource->m_CreationTime << "\n";
        }
        
        file.close();
        OLO_CORE_INFO("Exported GPU resource information to: {}", filename);
    }
    
    std::string GPUResourceInspector::FormatTextureFormat(GLenum format) const
    {
        switch (format)
        {
            // 8-bit formats
            case GL_RGBA8: return "RGBA8";
            case GL_RGB8: return "RGB8";
            case GL_RG8: return "RG8";
            case GL_R8: return "R8";
            case GL_RGBA8_SNORM: return "RGBA8_SNORM";
            case GL_RGB8_SNORM: return "RGB8_SNORM";
            case GL_RG8_SNORM: return "RG8_SNORM";
            case GL_R8_SNORM: return "R8_SNORM";
            
            // 16-bit formats
            case GL_RGBA16: return "RGBA16";
            case GL_RGB16: return "RGB16";
            case GL_RG16: return "RG16";
            case GL_R16: return "R16";
            case GL_RGBA16_SNORM: return "RGBA16_SNORM";
            case GL_RGB16_SNORM: return "RGB16_SNORM";
            case GL_RG16_SNORM: return "RG16_SNORM";
            case GL_R16_SNORM: return "R16_SNORM";
            
            // 32-bit float formats
            case GL_RGBA32F: return "RGBA32F";
            case GL_RGB32F: return "RGB32F";
            case GL_RG32F: return "RG32F";
            case GL_R32F: return "R32F";
            
            // 16-bit float formats
            case GL_RGBA16F: return "RGBA16F";
            case GL_RGB16F: return "RGB16F";
            case GL_RG16F: return "RG16F";
            case GL_R16F: return "R16F";
            
            // Integer formats
            case GL_RGBA32I: return "RGBA32I";
            case GL_RGB32I: return "RGB32I";
            case GL_RG32I: return "RG32I";
            case GL_R32I: return "R32I";
            case GL_RGBA16I: return "RGBA16I";
            case GL_RGB16I: return "RGB16I";
            case GL_RG16I: return "RG16I";
            case GL_R16I: return "R16I";
            case GL_RGBA8I: return "RGBA8I";
            case GL_RGB8I: return "RGB8I";
            case GL_RG8I: return "RG8I";
            case GL_R8I: return "R8I";
            
            // Unsigned integer formats
            case GL_RGBA32UI: return "RGBA32UI";
            case GL_RGB32UI: return "RGB32UI";
            case GL_RG32UI: return "RG32UI";
            case GL_R32UI: return "R32UI";
            case GL_RGBA16UI: return "RGBA16UI";
            case GL_RGB16UI: return "RGB16UI";
            case GL_RG16UI: return "RG16UI";
            case GL_R16UI: return "R16UI";
            case GL_RGBA8UI: return "RGBA8UI";
            case GL_RGB8UI: return "RGB8UI";
            case GL_RG8UI: return "RG8UI";
            case GL_R8UI: return "R8UI";
            
            // Depth/stencil formats
            case GL_DEPTH_COMPONENT16: return "DEPTH16";
            case GL_DEPTH_COMPONENT24: return "DEPTH24";
            case GL_DEPTH_COMPONENT32: return "DEPTH32";
            case GL_DEPTH_COMPONENT32F: return "DEPTH32F";
            case GL_DEPTH24_STENCIL8: return "DEPTH24_STENCIL8";
            case GL_DEPTH32F_STENCIL8: return "DEPTH32F_STENCIL8";
            case GL_STENCIL_INDEX8: return "STENCIL8";
            
            // Compressed formats
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: return "DXT1_RGB";
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return "DXT1_RGBA";
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return "DXT3";
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return "DXT5";
            
            // sRGB formats
            case GL_SRGB8: return "sRGB8";
            case GL_SRGB8_ALPHA8: return "sRGBA8";
            
            default: 
                {
                std::stringstream ss;
                ss << "Unknown (0x" << std::uppercase << std::hex << format << ")";
                return ss.str();
                }
        }
    }

    std::string GPUResourceInspector::FormatBufferUsage(GLenum usage) const
    {
        switch (usage)
        {
            case GL_STATIC_DRAW: return "STATIC_DRAW";
            case GL_DYNAMIC_DRAW: return "DYNAMIC_DRAW";
            case GL_STREAM_DRAW: return "STREAM_DRAW";
            case GL_STATIC_READ: return "STATIC_READ";
            case GL_DYNAMIC_READ: return "DYNAMIC_READ";
            case GL_STREAM_READ: return "STREAM_READ";
            case GL_STATIC_COPY: return "STATIC_COPY";
            case GL_DYNAMIC_COPY: return "DYNAMIC_COPY";
            case GL_STREAM_COPY: return "STREAM_COPY";
            default:
            {
                std::stringstream ss;
                ss << "Unknown (0x" << std::hex << usage << ")";
                return ss.str();
            }
        }
    }

    std::string GPUResourceInspector::FormatMemorySize(sizet bytes) const
    {
        const char* units[] = { "B", "KB", "MB", "GB" };
        int unit = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024.0 && unit < 3)
        {
            size /= 1024.0;
            unit++;
        }
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    const char* GPUResourceInspector::GetResourceTypeName(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::Texture2D: return "Texture2D";
            case ResourceType::TextureCubemap: return "TextureCubemap";
            case ResourceType::VertexBuffer: return "Vertex Buffer";
            case ResourceType::IndexBuffer: return "Index Buffer";
            case ResourceType::UniformBuffer: return "Uniform Buffer";
            case ResourceType::Framebuffer: return "Framebuffer";
            default: return "Unknown";
        }
    }

    const char* GPUResourceInspector::GetBufferTargetName(GLenum target) const
    {
        switch (target)
        {
            case GL_ARRAY_BUFFER: return "Array Buffer";
            case GL_ELEMENT_ARRAY_BUFFER: return "Element Array Buffer";
            case GL_UNIFORM_BUFFER: return "Uniform Buffer";
            case GL_SHADER_STORAGE_BUFFER: return "Shader Storage Buffer";
            case GL_TRANSFORM_FEEDBACK_BUFFER: return "Transform Feedback Buffer";
            case GL_COPY_READ_BUFFER: return "Copy Read Buffer";
            case GL_COPY_WRITE_BUFFER: return "Copy Write Buffer";
            case GL_PIXEL_PACK_BUFFER: return "Pixel Pack Buffer";
            case GL_PIXEL_UNPACK_BUFFER: return "Pixel Unpack Buffer";
            case GL_TEXTURE_BUFFER: return "Texture Buffer";
            case GL_DRAW_INDIRECT_BUFFER: return "Draw Indirect Buffer";
            case GL_DISPATCH_INDIRECT_BUFFER: return "Dispatch Indirect Buffer";
            default: return "Unknown";
        }
    }
        void GPUResourceInspector::CompleteTextureDownload(TextureInfo& info, const TextureDownloadRequest& request)
    {
        OLO_CORE_TRACE("Completing texture download for texture {} mip level {}", info.m_RendererID, request.m_MipLevel);
        
        // Map the PBO to get the downloaded data
        glBindBuffer(GL_PIXEL_PACK_BUFFER, request.m_PBO);
          // Calculate data size for this mip level - using RGBA format consistently
        u32 width = std::max(1u, info.m_Width >> request.m_MipLevel);
        u32 height = std::max(1u, info.m_Height >> request.m_MipLevel);
        u32 bytesPerPixel = 4; // RGBA format
        sizet dataSize = width * height * bytesPerPixel;
        
        // Map the buffer to read the data
        void* data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, dataSize, GL_MAP_READ_BIT);
        
        if (data != nullptr)
        {
            // Calculate preview size (limit to reasonable size for UI)
            u32 previewWidth = std::min(width, 256u);
            u32 previewHeight = std::min(height, 256u);
            
            // Allocate preview buffer
            info.m_PreviewData.resize(previewWidth * previewHeight * bytesPerPixel);
            
            if (previewWidth == width && previewHeight == height)
            {
                // Direct copy if no scaling needed
                std::memcpy(info.m_PreviewData.data(), data, dataSize);
            }
            else
            {
                // Simple nearest-neighbor downscaling for preview
                const u8* srcData = static_cast<const u8*>(data);
                
                for (u32 y = 0; y < previewHeight; ++y)
                {
                    for (u32 x = 0; x < previewWidth; ++x)
                    {
                        u32 srcX = (x * width) / previewWidth;
                        u32 srcY = (y * height) / previewHeight;
                        u32 srcIndex = (srcY * width + srcX) * bytesPerPixel;
                        u32 dstIndex = (y * previewWidth + x) * bytesPerPixel;
                        
                        for (u32 c = 0; c < bytesPerPixel; ++c)
                        {
                            info.m_PreviewData[dstIndex + c] = srcData[srcIndex + c];
                        }
                    }
                }
			}
            
            // Mark preview as valid
            info.m_PreviewDataValid = true;
            
            OLO_CORE_TRACE("Completed async texture download for texture {} mip level {}", request.m_TextureID, request.m_MipLevel);
        }
        else
        {
            OLO_CORE_ERROR("Failed to map PBO data for texture {}", request.m_TextureID);
        }        
        // Unmap and clean up
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }    sizet GPUResourceInspector::CalculateAccurateTextureMemoryUsage(u32 textureId, GLenum target, 
                                                                   GLenum internalFormat, u32 width, 
                                                                   u32 height, u32 mipLevels) const
    {
        sizet totalMemory = 0;
        
        // Check if format is compressed
        GLint isCompressed = GL_FALSE;
        glGetInternalformativ(target, internalFormat, GL_TEXTURE_COMPRESSED, 1, &isCompressed);
        
        if (isCompressed == GL_TRUE)
        {
            // Handle compressed textures - calculate based on block sizes
            totalMemory = CalculateCompressedTextureMemory(textureId, target, internalFormat, width, height, mipLevels);
        }
        else
        {
            // Handle uncompressed textures - calculate based on bytes per pixel
            u32 bytesPerPixel = GetUncompressedBytesPerPixel(internalFormat);
            totalMemory = CalculateUncompressedTextureMemory(width, height, bytesPerPixel, mipLevels);
            
            // For cubemaps, multiply by 6 faces
            if (target == GL_TEXTURE_CUBE_MAP)
            {
                totalMemory *= 6;
            }
        }
        
        return totalMemory;
    }

    sizet GPUResourceInspector::CalculateCompressedTextureMemory(u32 textureId, GLenum target, 
                                                               GLenum internalFormat, u32 /*width*/, 
                                                               u32 /*height*/, u32 mipLevels) const
    {
        sizet totalMemory = 0;
        u32 blockSize = GetCompressedBlockSize(internalFormat);
        
        // Determine number of faces
        u32 faceCount = (target == GL_TEXTURE_CUBE_MAP) ? 6 : 1;
        
        for (u32 face = 0; face < faceCount; ++face)
        {
            for (u32 level = 0; level < mipLevels; ++level)
            {
                // Get actual dimensions for this mip level and face
                GLint levelWidth, levelHeight, compressedSize;
                
                if (target == GL_TEXTURE_CUBE_MAP)
                {
                    // Query each face of the cubemap (GL_TEXTURE_CUBE_MAP_POSITIVE_X + face)
                    // (faceTarget variable removed as it was unused)
                    glGetTextureLevelParameteriv(textureId, level, GL_TEXTURE_WIDTH, &levelWidth);
                    glGetTextureLevelParameteriv(textureId, level, GL_TEXTURE_HEIGHT, &levelHeight);
                    glGetTextureLevelParameteriv(textureId, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compressedSize);
                }
                else
                {
                    glGetTextureLevelParameteriv(textureId, level, GL_TEXTURE_WIDTH, &levelWidth);
                    glGetTextureLevelParameteriv(textureId, level, GL_TEXTURE_HEIGHT, &levelHeight);
                    glGetTextureLevelParameteriv(textureId, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compressedSize);
                }
                
                if (levelWidth > 0 && levelHeight > 0)
                {
                    // Use actual compressed size if available, otherwise calculate
                    if (compressedSize > 0)
                    {
                        totalMemory += static_cast<sizet>(compressedSize);
                    }
                    else
                    {
                        // Calculate based on block compression
                        u32 blocksX = (static_cast<u32>(levelWidth) + 3) / 4;
                        u32 blocksY = (static_cast<u32>(levelHeight) + 3) / 4;
                        totalMemory += static_cast<sizet>(blocksX * blocksY * blockSize);
                    }
                }
            }
        }
        
        return totalMemory;
    }

    sizet GPUResourceInspector::CalculateUncompressedTextureMemory(u32 width, u32 height, 
                                                                  u32 bytesPerPixel, u32 mipLevels) const
    {
        sizet totalMemory = 0;
        u32 currentWidth = width;
        u32 currentHeight = height;
        
        for (u32 level = 0; level < mipLevels; ++level)
        {
            totalMemory += static_cast<sizet>(currentWidth * currentHeight * bytesPerPixel);
            
            // Calculate next mip level dimensions
            currentWidth = std::max(1u, currentWidth / 2);
            currentHeight = std::max(1u, currentHeight / 2);
        }
        
        return totalMemory;
    }

    u32 GPUResourceInspector::GetUncompressedBytesPerPixel(GLenum internalFormat) const
    {
        switch (internalFormat)
        {
            // 8-bit single channel
            case GL_R8:
            case GL_R8_SNORM:
            case GL_R8I:
            case GL_R8UI:
                return 1;
                
            // 16-bit single channel or 8-bit dual channel
            case GL_RG8:
            case GL_RG8_SNORM:
            case GL_RG8I:
            case GL_RG8UI:
            case GL_R16:
            case GL_R16F:
            case GL_R16I:
            case GL_R16UI:
            case GL_DEPTH_COMPONENT16:
                return 2;
                
            // 24-bit RGB
            case GL_RGB8:
            case GL_RGB8_SNORM:
            case GL_RGB8I:
            case GL_RGB8UI:
            case GL_SRGB8:
            case GL_DEPTH_COMPONENT24:
                return 3;
                
            // 32-bit formats (RGBA8, RG16, R32, depth32)
            case GL_RGBA8:
            case GL_RGBA8_SNORM:
            case GL_RGBA8I:
            case GL_RGBA8UI:
            case GL_SRGB8_ALPHA8:
            case GL_RG16:
            case GL_RG16F:
            case GL_RG16I:
            case GL_RG16UI:
            case GL_R32F:
            case GL_R32I:
            case GL_R32UI:
            case GL_DEPTH_COMPONENT32:
            case GL_DEPTH_COMPONENT32F:
            case GL_DEPTH24_STENCIL8:
                return 4;
                
            // 48-bit RGB16
            case GL_RGB16:
            case GL_RGB16F:
            case GL_RGB16I:
            case GL_RGB16UI:
                return 6;
                
            // 64-bit formats (RGBA16, RG32, depth32f+stencil8)
            case GL_RGBA16:
            case GL_RGBA16F:
            case GL_RGBA16I:
            case GL_RGBA16UI:
            case GL_RG32F:
            case GL_RG32I:
            case GL_RG32UI:
            case GL_DEPTH32F_STENCIL8:
                return 8;
                
            // 96-bit RGB32
            case GL_RGB32F:
            case GL_RGB32I:
            case GL_RGB32UI:
                return 12;
                
            // 128-bit RGBA32
            case GL_RGBA32F:
            case GL_RGBA32I:
            case GL_RGBA32UI:
                return 16;
                
            default:
                OLO_CORE_WARN("GPUResourceInspector: Unknown texture format 0x{:X}, assuming 4 bytes per pixel", internalFormat);
                return 4;
        }
    }

    u32 GPUResourceInspector::GetCompressedBlockSize(GLenum internalFormat) const
    {
        switch (internalFormat)
        {
            // DXT1/BC1 - 4x4 blocks, 8 bytes per block (RGB or RGBA with 1-bit alpha)
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
                return 8;
                
            // DXT3/BC2 - 4x4 blocks, 16 bytes per block (RGBA with explicit alpha)
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
                return 16;
                
            // DXT5/BC3 - 4x4 blocks, 16 bytes per block (RGBA with interpolated alpha)
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
                return 16;
                
            // BC4/ATI1 - 4x4 blocks, 8 bytes per block (single channel)
            case GL_COMPRESSED_RED_RGTC1:
            case GL_COMPRESSED_SIGNED_RED_RGTC1:
                return 8;
                
            // BC5/ATI2 - 4x4 blocks, 16 bytes per block (dual channel)
            case GL_COMPRESSED_RG_RGTC2:
            case GL_COMPRESSED_SIGNED_RG_RGTC2:
                return 16;
                
            // BC6H - 4x4 blocks, 16 bytes per block (HDR RGB)
            case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT:
            case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT:
                return 16;
                
            // BC7 - 4x4 blocks, 16 bytes per block (high quality RGBA)
            case GL_COMPRESSED_RGBA_BPTC_UNORM:
            case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
                return 16;
                
            // ETC2 formats - 4x4 blocks
            case GL_COMPRESSED_RGB8_ETC2:
            case GL_COMPRESSED_SRGB8_ETC2:
            case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
            case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
                return 8;
                
            case GL_COMPRESSED_RGBA8_ETC2_EAC:
            case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
                return 16;
                
            // EAC formats - 4x4 blocks
            case GL_COMPRESSED_R11_EAC:
            case GL_COMPRESSED_SIGNED_R11_EAC:
                return 8;
                
            case GL_COMPRESSED_RG11_EAC:
            case GL_COMPRESSED_SIGNED_RG11_EAC:
                return 16;
                
            // ASTC formats - variable block sizes (using 4x4 as most common)
            case GL_COMPRESSED_RGBA_ASTC_4x4_KHR:
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:
                return 16;
                
            default:
                OLO_CORE_WARN("GPUResourceInspector: Unknown compressed format 0x{:X}, assuming 16 bytes per block", internalFormat);
                return 16;
        }
    }
}
