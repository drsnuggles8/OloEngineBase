#include "OloEnginePCH.h"
#include "OpenGLDSABindingManager.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Core/Log.h"
#include <chrono>
#include <algorithm>

namespace OloEngine
{
    DSABindingManager::DSABindingManager()
    {
        m_BatchBuffer.reserve(64); // Reserve space for batch operations
    }

    DSABindingManager::~DSABindingManager()
    {
        if (m_IsInitialized)
        {
            Shutdown();
        }
    }

    bool DSABindingManager::Initialize()
    {
        if (m_IsInitialized)
        {
            OLO_CORE_WARN("DSABindingManager already initialized");
            return true;
        }

        // Get OpenGL version information
        const char* versionStr = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* rendererStr = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        
        if (versionStr) m_VersionInfo.Version = versionStr;
        if (rendererStr) m_VersionInfo.Renderer = rendererStr;

        glGetIntegerv(GL_MAJOR_VERSION, &m_VersionInfo.Major);
        glGetIntegerv(GL_MINOR_VERSION, &m_VersionInfo.Minor);

        // Check for minimum DSA support (OpenGL 4.5+)
        m_VersionInfo.HasDSA = (m_VersionInfo.Major > 4) || 
                               (m_VersionInfo.Major == 4 && m_VersionInfo.Minor >= 5);
        
        // Check for multi-bind support (OpenGL 4.4+)
        m_VersionInfo.HasMultiBind = (m_VersionInfo.Major > 4) || 
                                     (m_VersionInfo.Major == 4 && m_VersionInfo.Minor >= 4);
        
        // Check for buffer storage (OpenGL 4.4+)
        m_VersionInfo.HasBufferStorage = (m_VersionInfo.Major > 4) || 
                                         (m_VersionInfo.Major == 4 && m_VersionInfo.Minor >= 4);

        if (!m_VersionInfo.HasDSA)
        {
            OLO_CORE_WARN("DSA requires OpenGL 4.5+, current version: {}.{}", 
                         m_VersionInfo.Major, m_VersionInfo.Minor);
            return false;
        }

        // Detect available DSA features
        DetectDSAFeatures();

        m_IsInitialized = true;
        
        OLO_CORE_INFO("DSABindingManager initialized with OpenGL {}.{}", 
                     m_VersionInfo.Major, m_VersionInfo.Minor);
        OLO_CORE_INFO("Renderer: {}", m_VersionInfo.Renderer);
        OLO_CORE_INFO("DSA Features: 0x{:X}", m_SupportedFeatures);
        
        return true;
    }

    void DSABindingManager::Shutdown()
    {
        if (!m_IsInitialized)
            return;

        ClearBindingState();
        m_IsInitialized = false;
        
        OLO_CORE_INFO("DSABindingManager shutdown complete");
    }

    void DSABindingManager::DetectDSAFeatures()
    {
        m_SupportedFeatures = 0;

        // Check for each DSA feature
        if (m_VersionInfo.HasDSA)
        {
            // OpenGL 4.5+ features
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::NamedBufferStorage);
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::NamedBufferSubData);
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::TextureStorage);
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::TextureSubImage);
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::BindTextureUnit);
        }

        if (m_VersionInfo.HasMultiBind)
        {
            // OpenGL 4.4+ features
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::BindBuffersRange);
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::MultiBindBuffers);
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::BindImageTextures);
        }

        // Basic features available in earlier versions
        m_SupportedFeatures |= static_cast<u32>(DSAFeature::BindBufferRange);
        
        if (m_VersionInfo.Major > 4 || (m_VersionInfo.Major == 4 && m_VersionInfo.Minor >= 3))
        {
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::InvalidateBufferData);
        }

        if (m_VersionInfo.Major > 4 || (m_VersionInfo.Major == 4 && m_VersionInfo.Minor >= 1))
        {
            m_SupportedFeatures |= static_cast<u32>(DSAFeature::ProgramUniform);
        }
    }

    bool DSABindingManager::IsFeatureSupported(DSAFeature features) const
    {
        return (m_SupportedFeatures & static_cast<u32>(features)) == static_cast<u32>(features);
    }

    u64 DSABindingManager::GenerateBindingKey(GLenum target, u32 bindingPoint) const
    {
        return (static_cast<u64>(target) << 32) | static_cast<u64>(bindingPoint);
    }

    bool DSABindingManager::IsBindingRedundant(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                                             GLintptr offset, GLsizeiptr size) const
    {
        if (!m_RedundancyCheckingEnabled)
            return false;

        u64 key = GenerateBindingKey(target, bindingPoint);
        auto it = m_BindingStates.find(key);
        
        if (it == m_BindingStates.end())
            return false; // Not bound yet

        const auto& state = it->second;
        return state.BufferHandle == bufferHandle && 
               state.Offset == offset && 
               state.Range == size &&
               state.IsActive;
    }

    void DSABindingManager::UpdateBindingState(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                                             GLintptr offset, GLsizeiptr size)
    {
        u64 key = GenerateBindingKey(target, bindingPoint);
        
        DSABindingState& state = m_BindingStates[key];
        state.BufferHandle = bufferHandle;
        state.BindingPoint = bindingPoint;
        state.Target = target;
        state.Offset = offset;
        state.Range = size;
        state.LastBoundFrame = m_CurrentFrame;
        state.IsDirty = false;
        state.IsActive = (bufferHandle != 0);
        
        // Get buffer size if not provided
        if (size == 0 && bufferHandle != 0)
        {
            state.Size = GetBufferSize(bufferHandle);
            state.Range = state.Size;
        }
        else
        {
            state.Size = size;
        }
    }

    bool DSABindingManager::ValidateBindingParameters(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                                                    GLintptr offset, GLsizeiptr size) const
    {
        // Check for valid buffer handle
        if (bufferHandle == 0)
            return false;

        // Validate target
        switch (target)
        {
            case GL_UNIFORM_BUFFER:
            case GL_SHADER_STORAGE_BUFFER:
            case GL_ATOMIC_COUNTER_BUFFER:
            case GL_TRANSFORM_FEEDBACK_BUFFER:
                break;
            default:
                OLO_CORE_ERROR("DSA: Invalid buffer target: 0x{:X}", target);
                return false;
        }

        // Validate offset and size alignment for uniform buffers
        if (target == GL_UNIFORM_BUFFER)
        {
            GLint alignment;
            glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
            
            if (offset % alignment != 0)
            {
                OLO_CORE_ERROR("DSA: Uniform buffer offset {} is not aligned to {}", offset, alignment);
                return false;
            }
        }

        return true;
    }

    size_t DSABindingManager::GetBufferSize(u32 bufferHandle) const
    {
        if (bufferHandle == 0)
            return 0;

        GLint size = 0;
        glGetNamedBufferParameteriv(bufferHandle, GL_BUFFER_SIZE, &size);
        
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            OLO_CORE_WARN("DSA: Failed to get buffer size for handle {}: 0x{:X}", bufferHandle, error);
            return 0;
        }
        
        return static_cast<size_t>(size);
    }

    bool DSABindingManager::PerformBinding(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                                         GLintptr offset, GLsizeiptr size)
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        bool success = false;
        
        if (size == 0)
        {
            // Bind entire buffer
            glBindBufferBase(target, bindingPoint, bufferHandle);
            success = (glGetError() == GL_NO_ERROR);
        }
        else
        {
            // Bind buffer range
            glBindBufferRange(target, bindingPoint, bufferHandle, offset, size);
            success = (glGetError() == GL_NO_ERROR);
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        f32 bindTime = std::chrono::duration<f32, std::milli>(endTime - startTime).count();
        
        if (success)
        {
            UpdateBindingState(target, bindingPoint, bufferHandle, offset, size);
        }
        else
        {
            OLO_CORE_ERROR("DSA: Failed to bind buffer {} to point {}", bufferHandle, bindingPoint);
        }
        
        UpdateStatistics(false, false, bindTime);
        
        return success;
    }

    bool DSABindingManager::BindUniformBuffer(u32 bindingPoint, u32 bufferHandle, GLintptr offset, GLsizeiptr size)
    {
        if (!m_IsInitialized)
        {
            OLO_CORE_ERROR("DSA: BindingManager not initialized");
            return false;
        }

        if (!ValidateBindingParameters(GL_UNIFORM_BUFFER, bindingPoint, bufferHandle, offset, size))
            return false;

        // Check for redundant binding
        if (IsBindingRedundant(GL_UNIFORM_BUFFER, bindingPoint, bufferHandle, offset, size))
        {
            UpdateStatistics(true, false, 0.0f);
            return true;
        }

        return PerformBinding(GL_UNIFORM_BUFFER, bindingPoint, bufferHandle, offset, size);
    }

    bool DSABindingManager::BindStorageBuffer(u32 bindingPoint, u32 bufferHandle, GLintptr offset, GLsizeiptr size)
    {
        if (!m_IsInitialized)
        {
            OLO_CORE_ERROR("DSA: BindingManager not initialized");
            return false;
        }

        if (!ValidateBindingParameters(GL_SHADER_STORAGE_BUFFER, bindingPoint, bufferHandle, offset, size))
            return false;

        // Check for redundant binding
        if (IsBindingRedundant(GL_SHADER_STORAGE_BUFFER, bindingPoint, bufferHandle, offset, size))
        {
            UpdateStatistics(true, false, 0.0f);
            return true;
        }

        return PerformBinding(GL_SHADER_STORAGE_BUFFER, bindingPoint, bufferHandle, offset, size);
    }

    bool DSABindingManager::BindTexture(u32 textureUnit, u32 textureHandle)
    {
        if (!m_IsInitialized)
        {
            OLO_CORE_ERROR("DSA: BindingManager not initialized");
            return false;
        }

        auto startTime = std::chrono::high_resolution_clock::now();
        
        bool success = false;
        
        if (IsFeatureSupported(DSAFeature::BindTextureUnit))
        {
            // Use DSA function (OpenGL 4.5+)
            glBindTextureUnit(textureUnit, textureHandle);
            success = (glGetError() == GL_NO_ERROR);
        }
        else
        {
            // Fallback to traditional method
            glActiveTexture(GL_TEXTURE0 + textureUnit);
            glBindTexture(GL_TEXTURE_2D, textureHandle); // Assuming 2D texture
            success = (glGetError() == GL_NO_ERROR);
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        f32 bindTime = std::chrono::duration<f32, std::milli>(endTime - startTime).count();
        
        if (success)
        {
            // Update texture binding state
            u64 key = GenerateBindingKey(GL_TEXTURE_2D, textureUnit);
            DSABindingState& state = m_BindingStates[key];
            state.BufferHandle = textureHandle;
            state.BindingPoint = textureUnit;
            state.Target = GL_TEXTURE_2D;
            state.LastBoundFrame = m_CurrentFrame;
            state.IsDirty = false;
            state.IsActive = (textureHandle != 0);
        }
        
        UpdateStatistics(false, false, bindTime);
        
        return success;
    }

    u32 DSABindingManager::BindUniformBuffers(u32 firstBinding, const std::vector<DSABindingState>& bindings)
    {
        if (!m_IsInitialized || bindings.empty())
            return 0;

        if (!IsFeatureSupported(DSAFeature::MultiBindBuffers) || !m_BatchingEnabled)
        {
            // Fallback to individual bindings
            u32 count = 0;
            for (const auto& binding : bindings)
            {
                if (BindUniformBuffer(binding.BindingPoint, binding.BufferHandle, binding.Offset, binding.Range))
                    count++;
            }
            return count;
        }

        return PerformBatchBinding(GL_UNIFORM_BUFFER, firstBinding, bindings);
    }

    u32 DSABindingManager::BindStorageBuffers(u32 firstBinding, const std::vector<DSABindingState>& bindings)
    {
        if (!m_IsInitialized || bindings.empty())
            return 0;

        if (!IsFeatureSupported(DSAFeature::MultiBindBuffers) || !m_BatchingEnabled)
        {
            // Fallback to individual bindings
            u32 count = 0;
            for (const auto& binding : bindings)
            {
                if (BindStorageBuffer(binding.BindingPoint, binding.BufferHandle, binding.Offset, binding.Range))
                    count++;
            }
            return count;
        }

        return PerformBatchBinding(GL_SHADER_STORAGE_BUFFER, firstBinding, bindings);
    }

    u32 DSABindingManager::PerformBatchBinding(GLenum target, u32 firstBinding, const std::vector<DSABindingState>& bindings)
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        u32 batchSize = std::min(static_cast<u32>(bindings.size()), 
                                m_MaxBatchSize > 0 ? m_MaxBatchSize : static_cast<u32>(bindings.size()));
        
        std::vector<GLuint> buffers(batchSize);
        std::vector<GLintptr> offsets(batchSize);
        std::vector<GLsizeiptr> sizes(batchSize);
        
        for (u32 i = 0; i < batchSize; ++i)
        {
            buffers[i] = bindings[i].BufferHandle;
            offsets[i] = bindings[i].Offset;
            sizes[i] = bindings[i].Range;
        }
        
        // Perform batch binding
        if (target == GL_UNIFORM_BUFFER)
        {
            glBindBuffersRange(GL_UNIFORM_BUFFER, firstBinding, batchSize, buffers.data(), offsets.data(), sizes.data());
        }
        else if (target == GL_SHADER_STORAGE_BUFFER)
        {
            glBindBuffersRange(GL_SHADER_STORAGE_BUFFER, firstBinding, batchSize, buffers.data(), offsets.data(), sizes.data());
        }
        
        bool success = (glGetError() == GL_NO_ERROR);
        u32 boundCount = success ? batchSize : 0;
        
        if (success)
        {
            // Update binding states
            for (u32 i = 0; i < batchSize; ++i)
            {
                UpdateBindingState(target, firstBinding + i, buffers[i], offsets[i], sizes[i]);
            }
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        f32 bindTime = std::chrono::duration<f32, std::milli>(endTime - startTime).count();
        
        UpdateStatistics(false, true, bindTime);
        
        return boundCount;
    }

    u32 DSABindingManager::ApplyRegistryBindings(const UniformBufferRegistry& registry, bool enableBatching)
    {
        if (!m_IsInitialized)
            return 0;

        const auto& bindings = registry.GetResourceBindings();
        u32 boundCount = 0;

        // Separate bindings by type for potential batching
        std::vector<DSABindingState> uniformBuffers;
        std::vector<DSABindingState> storageBuffers;
        std::vector<u32> textures;

        for (const auto& [name, binding] : bindings)
        {
            if (!registry.IsResourceBound(name))
                continue;

            u32 handle = binding.GetOpenGLHandle();
            if (handle == 0)
                continue;

            switch (binding.Type)
            {
                case ShaderResourceType::UniformBuffer:
                    if (enableBatching)
                    {
                        uniformBuffers.emplace_back(handle, binding.BindingPoint, GL_UNIFORM_BUFFER, binding.Size);
                    }
                    else
                    {
                        if (BindUniformBuffer(binding.BindingPoint, handle))
                            boundCount++;
                    }
                    break;

                case ShaderResourceType::StorageBuffer:
                    if (enableBatching)
                    {
                        storageBuffers.emplace_back(handle, binding.BindingPoint, GL_SHADER_STORAGE_BUFFER, binding.Size);
                    }
                    else
                    {
                        if (BindStorageBuffer(binding.BindingPoint, handle))
                            boundCount++;
                    }
                    break;

                case ShaderResourceType::Texture2D:
                case ShaderResourceType::TextureCube:
                    if (BindTexture(binding.BindingPoint, handle))
                        boundCount++;
                    break;

                default:
                    break;
            }
        }

        // Apply batched bindings
        if (enableBatching)
        {
            if (!uniformBuffers.empty())
            {
                boundCount += BindUniformBuffers(0, uniformBuffers);
            }
            
            if (!storageBuffers.empty())
            {
                boundCount += BindStorageBuffers(0, storageBuffers);
            }
        }

        return boundCount;
    }

    void DSABindingManager::UnbindResource(u32 bindingPoint, GLenum target)
    {
        u64 key = GenerateBindingKey(target, bindingPoint);
        auto it = m_BindingStates.find(key);
        
        if (it != m_BindingStates.end())
        {
            glBindBufferBase(target, bindingPoint, 0);
            it->second.IsActive = false;
            it->second.BufferHandle = 0;
        }
    }

    void DSABindingManager::UnbindAllResources(GLenum target)
    {
        for (auto& [key, state] : m_BindingStates)
        {
            if (state.Target == target && state.IsActive)
            {
                glBindBufferBase(target, state.BindingPoint, 0);
                state.IsActive = false;
                state.BufferHandle = 0;
            }
        }
    }

    void DSABindingManager::ClearBindingState()
    {
        m_BindingStates.clear();
        m_BatchBuffer.clear();
    }

    void DSABindingManager::InvalidateAllBindings()
    {
        for (auto& [key, state] : m_BindingStates)
        {
            state.IsDirty = true;
        }
    }

    void DSABindingManager::InvalidateBinding(u32 bindingPoint, GLenum target)
    {
        u64 key = GenerateBindingKey(target, bindingPoint);
        auto it = m_BindingStates.find(key);
        
        if (it != m_BindingStates.end())
        {
            it->second.IsDirty = true;
        }
    }

    bool DSABindingManager::IsResourceBound(u32 bindingPoint, GLenum target) const
    {
        u64 key = GenerateBindingKey(target, bindingPoint);
        auto it = m_BindingStates.find(key);
        
        return (it != m_BindingStates.end()) && it->second.IsActive;
    }

    const DSABindingState* DSABindingManager::GetBindingState(u32 bindingPoint, GLenum target) const
    {
        u64 key = GenerateBindingKey(target, bindingPoint);
        auto it = m_BindingStates.find(key);
        
        return (it != m_BindingStates.end()) ? &it->second : nullptr;
    }

    void DSABindingManager::UpdateStatistics(bool wasRedundant, bool wasBatched, f32 bindTime) const
    {
        m_Statistics.TotalBindings++;
        
        if (wasRedundant)
        {
            m_Statistics.SkippedRedundant++;
        }
        else
        {
            m_Statistics.StateChanges++;
            m_Statistics.TotalBindTime += bindTime;
        }
        
        if (wasBatched)
        {
            m_Statistics.BatchedBindings++;
        }
        
        if (m_Statistics.TotalBindings > 0)
        {
            m_Statistics.AverageBindTime = m_Statistics.TotalBindTime / 
                                          static_cast<f32>(m_Statistics.StateChanges > 0 ? m_Statistics.StateChanges : 1);
        }
        
        m_Statistics.UpdateEfficiency();
    }

    // Utility functions
    bool IsDSASupported()
    {
        GLint major, minor;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        
        return (major > 4) || (major == 4 && minor >= 5);
    }

    const char* GetDSAFeatureName(DSABindingManager::DSAFeature feature)
    {
        switch (feature)
        {
            case DSABindingManager::DSAFeature::NamedBufferStorage: return "NamedBufferStorage";
            case DSABindingManager::DSAFeature::NamedBufferSubData: return "NamedBufferSubData";
            case DSABindingManager::DSAFeature::BindBufferRange: return "BindBufferRange";
            case DSABindingManager::DSAFeature::BindBuffersRange: return "BindBuffersRange";
            case DSABindingManager::DSAFeature::MultiBindBuffers: return "MultiBindBuffers";
            case DSABindingManager::DSAFeature::TextureStorage: return "TextureStorage";
            case DSABindingManager::DSAFeature::TextureSubImage: return "TextureSubImage";
            case DSABindingManager::DSAFeature::BindTextureUnit: return "BindTextureUnit";
            case DSABindingManager::DSAFeature::BindImageTextures: return "BindImageTextures";
            case DSABindingManager::DSAFeature::ProgramUniform: return "ProgramUniform";
            case DSABindingManager::DSAFeature::InvalidateBufferData: return "InvalidateBufferData";
            default: return "Unknown";
        }
    }
}
