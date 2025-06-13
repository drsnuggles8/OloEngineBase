#include "OloEnginePCH.h"
#include "ShaderDebugger.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"

#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace OloEngine
{
    ShaderDebugger& ShaderDebugger::GetInstance()
    {
        static ShaderDebugger instance;
        return instance;
    }

    void ShaderDebugger::Initialize()
    {
        if (m_IsInitialized)
            return;

        OLO_CORE_INFO("Initializing Shader Debugger...");
        
        m_Shaders.clear();
        m_PendingCompilations.clear();
        
        m_TotalCompilationTime = 0.0;
        m_TotalCompilations = 0;
        m_FailedCompilations = 0;
        m_TotalReloads = 0;
        
        m_SelectedShaderID = 0;
        m_SelectedTab = 0;
        m_ShowOnlyActiveShaders = false;
        m_ShowOnlyErrorShaders = false;
        m_AutoSelectNewShaders = true;
        memset(m_SearchFilter, 0, sizeof(m_SearchFilter));

        m_IsInitialized = true;
        OLO_CORE_INFO("Shader Debugger initialized successfully");
    }

    void ShaderDebugger::Shutdown()
    {
        if (!m_IsInitialized)
            return;

        OLO_CORE_INFO("Shutting down Shader Debugger...");
        
        std::lock_guard<std::mutex> lock(m_ShaderMutex);
		m_Shaders.clear();
        m_PendingCompilations.clear();
        
        m_IsInitialized = false;
        OLO_CORE_INFO("Shader Debugger shutdown complete");
    }
    
    void ShaderDebugger::RegisterShader(const Ref<Shader>& shader)
    {
        if (!m_IsInitialized || !shader)
        {
            OLO_CORE_WARN("ShaderDebugger::RegisterShader - Not initialized or null shader");
            return;
        }

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        const u32 rendererID = shader->GetRendererID();
        const std::string& name = shader->GetName();
        
        OLO_CORE_INFO("ShaderDebugger: Registering shader '{0}' with ID {1}", name, rendererID);
        
        // Check if shader is already registered
        if (m_Shaders.find(rendererID) != m_Shaders.end())
        {
            OLO_CORE_WARN("Shader with ID {0} already registered", rendererID);
            return;
        }

        ShaderInfo info;
        info.m_RendererID = rendererID;
		info.m_Name = name;
        info.m_CreationTime = std::chrono::steady_clock::now();
          // Initialize compilation result with zero instruction count
        OLO_CORE_INFO("ShaderDebugger: RESET instruction count to 0 in OnCompilationStart (new shader): {0}", name);
        info.m_LastCompilation.m_InstructionCount = 0;
        info.m_LastCompilation.m_VertexGeometrySPIRVSize = 0;
        info.m_LastCompilation.m_FragmentComputeSPIRVSize = 0;
        
        m_Shaders[rendererID] = std::move(info);
          // Auto-select new shader if enabled
        if (m_AutoSelectNewShaders)
        {
            m_SelectedShaderID = rendererID;
        }
        
        OLO_CORE_TRACE("Registered shader: {0} (ID: {1})", name, rendererID);
    }
    
    void ShaderDebugger::RegisterShader(u32 rendererID, const std::string& name, const std::string& filePath)
    {
        if (!m_IsInitialized)
        {
            OLO_CORE_WARN("ShaderDebugger::RegisterShader - Not initialized");
            return;
        }

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        OLO_CORE_INFO("ShaderDebugger: Manually registering shader '{0}' with ID {1}", name, rendererID);
        
        // Check if shader is already registered
        if (m_Shaders.find(rendererID) != m_Shaders.end())
        {
            OLO_CORE_WARN("Shader with ID {0} already registered", rendererID);
            return;
        }

        ShaderInfo info;
        info.m_RendererID = rendererID;
        info.m_Name = name;
        info.m_FilePath = filePath;
        info.m_CreationTime = std::chrono::steady_clock::now();
          // Initialize compilation result with zero instruction count
        OLO_CORE_INFO("ShaderDebugger: RESET instruction count to 0 in OnCompilationStart (file-based shader): {0}", name);
        info.m_LastCompilation.m_InstructionCount = 0;
        info.m_LastCompilation.m_VertexGeometrySPIRVSize = 0;
        info.m_LastCompilation.m_FragmentComputeSPIRVSize = 0;
        
        m_Shaders[rendererID] = std::move(info);
        
        // Auto-select new shader if enabled
        if (m_AutoSelectNewShaders)
        {
            m_SelectedShaderID = rendererID;
        }
        
        OLO_CORE_TRACE("Manually registered shader: {0} (ID: {1})", name, rendererID);
    }

    void ShaderDebugger::UnregisterShader(u32 rendererID)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        auto it = m_Shaders.find(rendererID);
        if (it != m_Shaders.end())
        {
            UpdateActiveTime(it->second);
            
            OLO_CORE_TRACE("Unregistered shader: {0} (ID: {1})", it->second.m_Name, rendererID);
            
            // Clear selection if this shader was selected
            if (m_SelectedShaderID == rendererID)
            {
                m_SelectedShaderID = 0;
            }
            
            m_Shaders.erase(it);
        }
    }
	
	void ShaderDebugger::OnCompilationStart(const std::string& name, const std::string& filepath)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        PendingCompilation pending;
        pending.m_Name = name;
        pending.m_FilePath = filepath;
        pending.m_StartTime = std::chrono::steady_clock::now();
        
        m_PendingCompilations[name] = pending;
        
        // Find and reset instruction count for this shader if it already exists
        for (auto& [id, shaderInfo] : m_Shaders)
        {
            if (shaderInfo.m_Name == name)
            {
                shaderInfo.m_LastCompilation.m_InstructionCount = 0;
                shaderInfo.m_LastCompilation.m_VertexGeometrySPIRVSize = 0;
                shaderInfo.m_LastCompilation.m_FragmentComputeSPIRVSize = 0;
                break;
            }
        }
    }

    void ShaderDebugger::OnCompilationEnd(u32 rendererID, bool success, const std::string& errorMsg, f64 compileTimeMs)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        OLO_CORE_INFO("ShaderDebugger: Compilation ended for ID {0}, Success: {1}, Time: {2:.2f}ms", 
                      rendererID, success, compileTimeMs);
        
        auto shaderIt = m_Shaders.find(rendererID);
        if (shaderIt != m_Shaders.end())
        {
            ShaderInfo& info = shaderIt->second;
              // Update compilation result without resetting instruction count
            info.m_LastCompilation.m_Success = success;
            info.m_LastCompilation.m_ErrorMessage = errorMsg;
            info.m_LastCompilation.m_CompileTimeMs = compileTimeMs;
            info.m_LastCompilation.m_Timestamp = std::chrono::steady_clock::now();
            info.m_HasErrors = !success;
            
            OLO_CORE_INFO("ShaderDebugger: Final instruction count after compilation: {0}", 
                         info.m_LastCompilation.m_InstructionCount);
            
            // Note: Instruction count is NOT reset here - it should persist from reflection
            
            // Update global statistics
            m_TotalCompilationTime += compileTimeMs;
            m_TotalCompilations++;
            if (!success)
                m_FailedCompilations++;
            
            // Remove from pending compilations
            auto pendingIt = m_PendingCompilations.find(info.m_Name);
            if (pendingIt != m_PendingCompilations.end())
            {
                info.m_FilePath = pendingIt->second.m_FilePath;
                m_PendingCompilations.erase(pendingIt);
            }
            
            OLO_CORE_TRACE("Shader compilation ended: {0} (ID: {1}), Success: {2}, Time: {3:.2f}ms", 
                          info.m_Name, rendererID, success, compileTimeMs);
        }
        else
        {
            OLO_CORE_WARN("ShaderDebugger: OnCompilationEnd called for unregistered shader ID {0}", rendererID);
        }
    }

    void ShaderDebugger::OnReloadStart(u32 rendererID)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        auto it = m_Shaders.find(rendererID);
        if (it != m_Shaders.end())
        {
            it->second.m_IsReloading = true;
            OLO_CORE_TRACE("Shader reload started: {0} (ID: {1})", it->second.m_Name, rendererID);
        }
    }

    void ShaderDebugger::OnReloadEnd(u32 rendererID, bool success)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        auto it = m_Shaders.find(rendererID);
        if (it != m_Shaders.end())
        {
            ShaderInfo& info = it->second;
            info.m_IsReloading = false;
            
            // Add reload event to history
            ReloadEvent event;
            event.m_Timestamp = std::chrono::steady_clock::now();
            event.m_Success = success;
            event.m_Reason = "Manual Reload";
            info.m_ReloadHistory.push_back(event);
            
            // Keep only last 10 reload events
            if (info.m_ReloadHistory.size() > 10)
            {
                info.m_ReloadHistory.erase(info.m_ReloadHistory.begin());
            }
            
            m_TotalReloads++;
            
            OLO_CORE_TRACE("Shader reload ended: {0} (ID: {1}), Success: {2}", 
                          info.m_Name, rendererID, success);
        }
    }
	
	void ShaderDebugger::OnShaderBind(u32 rendererID)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        // First, update active time for all currently active shaders and mark them inactive
        for (auto& [id, shaderInfo] : m_Shaders)
        {
            if (shaderInfo.m_IsActive)
            {
                UpdateActiveTime(shaderInfo);
                shaderInfo.m_IsActive = false;
            }
        }
        
        // Now update and activate the newly bound shader
        auto it = m_Shaders.find(rendererID);
        if (it != m_Shaders.end())
        {
            ShaderInfo& info = it->second;
            
            info.m_BindCount++;
            info.m_LastBindTime = std::chrono::steady_clock::now();
            info.m_LastActivationTime = info.m_LastBindTime;
            info.m_IsActive = true;
        }
    }

    void ShaderDebugger::OnUniformSet(u32 rendererID, const std::string& name, UniformType type)
    {
        if (!m_IsInitialized)
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        auto it = m_Shaders.find(rendererID);
        if (it != m_Shaders.end())
        {
            ShaderInfo& info = it->second;
            
            // Find or create uniform info
            auto uniformIt = std::find_if(info.m_Uniforms.begin(), info.m_Uniforms.end(),
                [&name](const UniformInfo& uniform) { return uniform.m_Name == name; });
            
            if (uniformIt != info.m_Uniforms.end())
            {
                uniformIt->m_SetCount++;
                uniformIt->m_LastSetTime = std::chrono::steady_clock::now();
            }
            else
            {
                // Create new uniform info
                UniformInfo uniform;
                uniform.m_Name = name;
                uniform.m_Type = type;
                uniform.m_SetCount = 1;
                uniform.m_LastSetTime = std::chrono::steady_clock::now();
                info.m_Uniforms.push_back(uniform);
            }
        }
    }
	
	void ShaderDebugger::UpdateReflectionData(u32 rendererID, const std::vector<u32>& spirvData)
    {
        if (!m_IsInitialized || spirvData.empty())
            return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        auto it = m_Shaders.find(rendererID);
        if (it == m_Shaders.end())
        {
            OLO_CORE_WARN("ShaderDebugger::UpdateReflectionData - Shader ID {0} not found", rendererID);
            return;
        }

        ShaderInfo& info = it->second;
		try
        {
            spirv_cross::Compiler compiler(spirvData);
            const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

            info.m_UniformBuffers.clear();
            info.m_Samplers.clear();

            // Process uniform buffers
            for (const auto& resource : resources.uniform_buffers)
            {
                const auto& bufferType = compiler.get_type(resource.base_type_id);
                const sizet bufferSize = compiler.get_declared_struct_size(bufferType);
                const u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);

                UniformBufferInfo uboInfo;
                uboInfo.m_Name = resource.name;
                uboInfo.m_Binding = binding;
                uboInfo.m_Size = static_cast<u32>(bufferSize);

                // Get member names
                for (u32 i = 0; i < bufferType.member_types.size(); ++i)
                {
                    const std::string memberName = compiler.get_member_name(resource.base_type_id, i);
                    if (!memberName.empty())
                    {
                        uboInfo.m_Members.push_back(memberName);
                    }
                }

                info.m_UniformBuffers.push_back(uboInfo);
            }

            // Process sampled images (textures)
            for (const auto& resource : resources.sampled_images)
            {
                const u32 binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                const auto& type = compiler.get_type(resource.type_id);

                SamplerInfo samplerInfo;
                samplerInfo.m_Name = resource.name;
                samplerInfo.m_Binding = binding;
                samplerInfo.m_TextureUnit = binding; // Assuming binding == texture unit

                // Determine sampler type
                if (type.image.dim == spv::Dim2D)
                {
                    samplerInfo.m_Type = "sampler2D";
                }
                else if (type.image.dim == spv::DimCube)
                {
                    samplerInfo.m_Type = "samplerCube";
                }
                else
                {
                    samplerInfo.m_Type = "sampler";
                }

                info.m_Samplers.push_back(samplerInfo);
            }
              // Estimate instruction count
            u32 instructionCount = 0;
            AnalyzeSPIRVFromWords(spirvData, instructionCount);
            info.m_LastCompilation.m_InstructionCount += instructionCount;

        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to analyze SPIR-V for shader {0}: {1}", info.m_Name, e.what());
        }
    }

    void ShaderDebugger::SetShaderSource(u32 rendererID, ShaderStage stage, 
                                        const std::string& originalSource, 
                                        const std::string& generatedGLSL,
                                        const std::vector<u8>& spirvBinary)
    {
        if (!m_IsInitialized)
			return;

        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        auto it = m_Shaders.find(rendererID);
        if (it != m_Shaders.end())
        {
            ShaderInfo& info = it->second;
            
            info.m_OriginalSource[stage] = originalSource;
            if (!generatedGLSL.empty())
            {
                info.m_GeneratedGLSL[stage] = generatedGLSL;
            }
            if (!spirvBinary.empty())
            {
                info.m_SPIRVBinary[stage] = spirvBinary;
                // Update SPIR-V size in compilation result
                // Categorize by pipeline stage type: geometry (Vertex+Geometry) vs pixel/compute (Fragment+Compute)
                if (stage == ShaderStage::Vertex || stage == ShaderStage::Geometry)
                    info.m_LastCompilation.m_VertexGeometrySPIRVSize += spirvBinary.size();
                else if (stage == ShaderStage::Fragment || stage == ShaderStage::Compute)
                    info.m_LastCompilation.m_FragmentComputeSPIRVSize += spirvBinary.size();
                
                // Convert SPIR-V binary to u32 vector for analysis
                std::vector<u32> spirvWords;
                spirvWords.resize(spirvBinary.size() / sizeof(u32));
                std::memcpy(spirvWords.data(), spirvBinary.data(), spirvBinary.size());
                
                // Perform reflection analysis and update instruction count
                try
                {
                    u32 instructionCount = 0;
                    AnalyzeSPIRVFromWords(spirvWords, instructionCount);
                    info.m_LastCompilation.m_InstructionCount += instructionCount;
                }
                catch (const std::exception&)
                {
                    // Silently continue if SPIR-V analysis fails
                }
            }
        }
    }

    void ShaderDebugger::UpdateActiveTime(ShaderInfo& shaderInfo)
    {
        if (shaderInfo.m_IsActive)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                now - shaderInfo.m_LastActivationTime).count() / 1000.0;
            shaderInfo.m_TotalActiveTimeMs += duration;
        }
    }

    const ShaderDebugger::ShaderInfo* ShaderDebugger::GetShaderInfo(u32 rendererID) const
    {
        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        auto it = m_Shaders.find(rendererID);
        return (it != m_Shaders.end()) ? &it->second : nullptr;
    }

    std::string ShaderDebugger::GetUniformTypeString(UniformType type) const
    {
        switch (type)
        {
            case UniformType::Int:        return "int";
            case UniformType::IntArray:   return "int[]";
            case UniformType::Float:      return "float";
            case UniformType::Float2:     return "vec2";
            case UniformType::Float3:     return "vec3";
            case UniformType::Float4:     return "vec4";
            case UniformType::Mat3:       return "mat3";
            case UniformType::Mat4:       return "mat4";
            case UniformType::Sampler2D:  return "sampler2D";
            case UniformType::SamplerCube: return "samplerCube";
            default: return "unknown";
        }
    }

    std::string ShaderDebugger::GetShaderStageString(ShaderStage stage) const
    {
        switch (stage)
        {
            case ShaderStage::Vertex:   return "Vertex";
            case ShaderStage::Fragment: return "Fragment";
            case ShaderStage::Geometry: return "Geometry";
            case ShaderStage::Compute:  return "Compute";
            default: return "Unknown";
        }
    }

    ImVec4 ShaderDebugger::GetShaderStageColor(ShaderStage stage) const
    {
        switch (stage)
        {
            case ShaderStage::Vertex:   return ImVec4(0.3f, 0.8f, 0.3f, 1.0f); // Green
            case ShaderStage::Fragment: return ImVec4(0.8f, 0.3f, 0.3f, 1.0f); // Red
            case ShaderStage::Geometry: return ImVec4(0.3f, 0.3f, 0.8f, 1.0f); // Blue
            case ShaderStage::Compute:  return ImVec4(0.8f, 0.8f, 0.3f, 1.0f); // Yellow
            default: return ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Gray
        }
    }

    /**
     * @brief Analyzes SPIR-V binary data to count instructions
     * @param spirvData SPIR-V binary data
     * @param instructionCount Output parameter for instruction count
     * 
     * Parses SPIR-V binary format to count the number of instructions.
     * This provides a meaningful metric for shader complexity analysis.
     */
    void ShaderDebugger::AnalyzeSPIRV(const std::vector<u8>& spirvData, u32& instructionCount) const
    {
        instructionCount = 0;
        
        if (spirvData.size() < 20) // Minimum SPIR-V header size
            return;
        
        // Simple instruction counting - each SPIR-V instruction is at least 4 bytes
        // This is a rough estimate, actual parsing would be more complex
        const u32* data = reinterpret_cast<const u32*>(spirvData.data());
        const sizet wordCount = spirvData.size() / 4;
        
        // Skip header (5 words)
        sizet offset = 5;
        while (offset < wordCount)
        {
            if (offset >= wordCount) break;
            
            const u32 instruction = data[offset];
            const u32 length = instruction >> 16;
            
            if (length == 0) break; // Invalid instruction
            
            instructionCount++;
            offset += length;
        }
    } 
	
	/**
     * @brief Analyzes SPIR-V word data to count instructions
     * @param spirvWords SPIR-V data as 32-bit words
     * @param instructionCount Output parameter for instruction count
     * 
     * More efficient version that works with pre-converted 32-bit word data.
     * Used internally for instruction counting during shader compilation.
     */
    void ShaderDebugger::AnalyzeSPIRVFromWords(const std::vector<u32>& spirvWords, u32& instructionCount) const
    {
        instructionCount = 0;
		if (spirvWords.size() < 5) // Minimum SPIR-V header size (5 words)
        {
            return;
        }
        
        // SPIR-V instruction counting
        const u32* data = spirvWords.data();
        const sizet wordCount = spirvWords.size();
        
        // Skip header (5 words: magic, version, generator, bound, schema)
        sizet offset = 5;
        while (offset < wordCount)
        {
            if (offset >= wordCount) break;
            
            const u32 instruction = data[offset];
            const u32 length = instruction >> 16; // High 16 bits contain instruction length
            
            if (length == 0 || length > (wordCount - offset)) break; // Invalid instruction
            
            instructionCount++;
            offset += length;
        }
	}

    /**
     * @brief Exports a comprehensive shader debugging report to file
     * @param filePath Path to output file
     * @return True if export succeeded, false otherwise
     * 
     * Generates a detailed text report containing all shader information,
     * compilation statistics, performance metrics, and error logs.
     */
    bool ShaderDebugger::ExportReport(const std::string& filePath) const
    {
        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
                return false;

            file << "OloEngine Shader Debugger Report\n";
            file << "Generated: " << std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() << "\n\n";

            // Global statistics
            file << "=== Global Statistics ===\n";
            file << "Total Shaders: " << m_Shaders.size() << "\n";
            file << "Total Compilations: " << m_TotalCompilations << "\n";
            file << "Failed Compilations: " << m_FailedCompilations << "\n";
            file << "Total Reloads: " << m_TotalReloads << "\n";
            file << "Average Compilation Time: " << 
                (m_TotalCompilations > 0 ? m_TotalCompilationTime / m_TotalCompilations : 0.0) << "ms\n\n";

            // Per-shader details
            file << "=== Shader Details ===\n";
            for (const auto& [id, info] : m_Shaders)
            {
                file << "Shader: " << info.m_Name << " (ID: " << id << ")\n";
                file << "  File: " << info.m_FilePath << "\n";
                file << "  Bind Count: " << info.m_BindCount << "\n";
                file << "  Active Time: " << info.m_TotalActiveTimeMs << "ms\n";
                file << "  Last Compilation: " << (info.m_LastCompilation.m_Success ? "Success" : "Failed") << "\n";
                file << "  Compilation Time: " << info.m_LastCompilation.m_CompileTimeMs << "ms\n";
                file << "  Instruction Count: " << info.m_LastCompilation.m_InstructionCount << "\n";
                file << "  SPIR-V Size: " << (info.m_LastCompilation.m_VertexGeometrySPIRVSize + info.m_LastCompilation.m_FragmentComputeSPIRVSize) << " bytes\n";
                file << "  Uniforms: " << info.m_Uniforms.size() << "\n";
                file << "  Uniform Buffers: " << info.m_UniformBuffers.size() << "\n";
                file << "  Samplers: " << info.m_Samplers.size() << "\n";
                file << "  Reload Count: " << info.m_ReloadHistory.size() << "\n";
                if (info.m_HasErrors)
                {
                    file << "  Error: " << info.m_LastCompilation.m_ErrorMessage << "\n";
                }
                file << "\n";
            }

            file.close();
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export shader debugger report: {0}", e.what());
            return false;
        }
    }

    void ShaderDebugger::RenderDebugView(bool* open, const char* title)
    {
        if (!m_IsInitialized)
            return;

        if (!open || *open)
        {
            ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
            if (!ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar))
            {
                ImGui::End();
                return;
            }

            // Menu bar
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("Options"))
                {
                    ImGui::MenuItem("Show Only Active Shaders", nullptr, &m_ShowOnlyActiveShaders);
                    ImGui::MenuItem("Show Only Error Shaders", nullptr, &m_ShowOnlyErrorShaders);
                    ImGui::MenuItem("Auto-Select New Shaders", nullptr, &m_AutoSelectNewShaders);
                    
                    ImGui::Separator();
                    if (ImGui::Button("Export Report"))
                    {
                        ExportReport("shader_debugger_report.txt");
                    }
                    
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            // Main content area
            ImGui::Columns(2, "ShaderDebuggerColumns", true);
            ImGui::SetColumnWidth(0, 350.0f);

            // Left panel: Shader list
            RenderShaderList();

            ImGui::NextColumn();
			// Right panel: Shader details
            ShaderInfo shaderInfoCopy; // Local copy to render without holding the mutex
            bool hasShaderToRender = false;
            
            {
                std::lock_guard<std::mutex> lock(m_ShaderMutex);
                auto it = m_Shaders.find(m_SelectedShaderID);
                if (it != m_Shaders.end())
                {
                    shaderInfoCopy = it->second; // Copy the shader info
                    hasShaderToRender = true;
                }
            } // Mutex unlocked here
            
            if (hasShaderToRender)
            {
                RenderShaderDetails(shaderInfoCopy);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Select a shader to view details");
            }

            ImGui::Columns(1);
            ImGui::End();
        }
    }

    void ShaderDebugger::RenderShaderList()
    {
        ImGui::Text("Shaders (%zu)", m_Shaders.size());
        
        // Search filter
        ImGui::InputText("##Search", m_SearchFilter, sizeof(m_SearchFilter));
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            memset(m_SearchFilter, 0, sizeof(m_SearchFilter));
        }

        ImGui::Separator();

        // Global stats
        ImGui::Text("Compilations: %u (%u failed)", m_TotalCompilations, m_FailedCompilations);
        ImGui::Text("Reloads: %u", m_TotalReloads);
        if (m_TotalCompilations > 0)
        {
            ImGui::Text("Avg. Compile Time: %.2fms", m_TotalCompilationTime / m_TotalCompilations);
        }

        ImGui::Separator();

        // Shader list
        ImGui::BeginChild("ShaderList", ImVec2(0, -30), true);
        
        std::lock_guard<std::mutex> lock(m_ShaderMutex);
        for (const auto& [id, info] : m_Shaders)
        {
            // Apply filters
            if (m_ShowOnlyActiveShaders && !info.m_IsActive)
                continue;
            if (m_ShowOnlyErrorShaders && !info.m_HasErrors)
                continue;
            
            // Apply search filter
            if (strlen(m_SearchFilter) > 0)
            {
                const std::string searchLower = [this]() {
                    std::string s(m_SearchFilter);
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    return s;
                }();
                
                std::string nameLower = info.m_Name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                
                if (nameLower.find(searchLower) == std::string::npos)
                    continue;
            }

            // Render shader entry
            const bool isSelected = (m_SelectedShaderID == id);
            
            ImVec4 textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            if (info.m_HasErrors)
                textColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            else if (info.m_IsActive)
                textColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            else if (info.m_IsReloading)
                textColor = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Text, textColor);
            
            if (ImGui::Selectable(info.m_Name.c_str(), isSelected))
            {
                m_SelectedShaderID = id;
            }
            
            ImGui::PopStyleColor();
            
            // Tooltip with basic info
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("ID: %u", id);
                ImGui::Text("File: %s", info.m_FilePath.c_str());
                ImGui::Text("Bind Count: %u", info.m_BindCount);
                if (info.m_HasErrors)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Has Errors");
                }
                if (info.m_IsActive)
                {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Currently Active");
                }
                ImGui::EndTooltip();
            }
        }
        
        ImGui::EndChild();

        // Bottom controls
        if (ImGui::Button("Refresh All"))
        {
            // TODO: Trigger reload of all shaders
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Selection"))
        {
            m_SelectedShaderID = 0;
        }
    }

    void ShaderDebugger::RenderShaderDetails(const ShaderInfo& shaderInfo)
    {
        ImGui::Text("Shader: %s (ID: %u)", shaderInfo.m_Name.c_str(), shaderInfo.m_RendererID);
        
        // Status indicators
        ImGui::SameLine(0, 20);
        if (shaderInfo.m_IsActive)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[ACTIVE]");
        }
        if (shaderInfo.m_HasErrors)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ERROR]");
        }
        if (shaderInfo.m_IsReloading)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "[RELOADING]");
        }

        ImGui::Separator();

        // Tab bar for different views
        if (ImGui::BeginTabBar("ShaderDetailsTabs"))
        {
            if (ImGui::BeginTabItem("Overview"))
            {
                // Basic information
                ImGui::Text("File Path: %s", shaderInfo.m_FilePath.c_str());
                ImGui::Text("Bind Count: %u", shaderInfo.m_BindCount);
                ImGui::Text("Total Active Time: %s", DebugUtils::FormatDuration(shaderInfo.m_TotalActiveTimeMs).c_str());
                
                const auto now = std::chrono::steady_clock::now();
                const auto creationTime = std::chrono::duration_cast<std::chrono::seconds>(
                    now - shaderInfo.m_CreationTime).count();
                ImGui::Text("Age: %lld seconds", creationTime);

                if (shaderInfo.m_LastBindTime != std::chrono::steady_clock::time_point{})
                {
                    const auto lastBindTime = std::chrono::duration_cast<std::chrono::seconds>(
                        now - shaderInfo.m_LastBindTime).count();
                    ImGui::Text("Last Bind: %lld seconds ago", lastBindTime);
                }

                ImGui::Separator();

                // Compilation info
                ImGui::Text("Compilation Status: %s", 
                           shaderInfo.m_LastCompilation.m_Success ? "Success" : "Failed");
                ImGui::Text("Compile Time: %.2fms", shaderInfo.m_LastCompilation.m_CompileTimeMs);
                ImGui::Text("Instruction Count: %u", shaderInfo.m_LastCompilation.m_InstructionCount);
                  const sizet totalSPIRVSize = shaderInfo.m_LastCompilation.m_VertexGeometrySPIRVSize + 
                                             shaderInfo.m_LastCompilation.m_FragmentComputeSPIRVSize;
                ImGui::Text("SPIR-V Size: %s", DebugUtils::FormatMemorySize(totalSPIRVSize).c_str());

                ImGui::Separator();

                // Resource counts
                ImGui::Text("Uniforms: %zu", shaderInfo.m_Uniforms.size());
                ImGui::Text("Uniform Buffers: %zu", shaderInfo.m_UniformBuffers.size());
                ImGui::Text("Samplers: %zu", shaderInfo.m_Samplers.size());
                ImGui::Text("Reloads: %zu", shaderInfo.m_ReloadHistory.size());

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Source Code"))
            {
                RenderSourceCode(shaderInfo);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Uniforms"))
            {
                RenderUniforms(shaderInfo);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Performance"))
            {
                RenderPerformanceMetrics(shaderInfo);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Reload History"))
            {
                RenderReloadHistory(shaderInfo);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("SPIR-V Analysis"))
            {
                RenderSPIRVAnalysis(shaderInfo);
                ImGui::EndTabItem();
            }

            if (shaderInfo.m_HasErrors && ImGui::BeginTabItem("Errors"))
            {
                RenderCompilationErrors(shaderInfo);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    void ShaderDebugger::RenderSourceCode(const ShaderInfo& shaderInfo)
    {
        if (shaderInfo.m_OriginalSource.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No source code available");
            return;
        }

        // Stage selector
        static i32 selectedStage = 0;
        const char* stageNames[] = { "Vertex", "Fragment", "Geometry", "Compute" };
        ImGui::Combo("Stage", &selectedStage, stageNames, IM_ARRAYSIZE(stageNames));

        const ShaderStage stage = static_cast<ShaderStage>(selectedStage);
        
        ImGui::Separator();

        // Source type tabs
        if (ImGui::BeginTabBar("SourceTabs"))
        {
            // Original source
            auto originalIt = shaderInfo.m_OriginalSource.find(stage);
            if (originalIt != shaderInfo.m_OriginalSource.end() && ImGui::BeginTabItem("Original"))
            {
                // Use InputTextMultiline for selectable/copyable text
                const std::string& sourceText = originalIt->second;
                ImGui::InputTextMultiline("##OriginalSource", 
                                        const_cast<char*>(sourceText.c_str()), 
                                        sourceText.length() + 1, 
                                        ImVec2(-1, -1), 
                                        ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }

            // Generated GLSL
            auto generatedIt = shaderInfo.m_GeneratedGLSL.find(stage);
            if (generatedIt != shaderInfo.m_GeneratedGLSL.end() && ImGui::BeginTabItem("Generated GLSL"))
            {
                // Use InputTextMultiline for selectable/copyable text
                const std::string& sourceText = generatedIt->second;
                ImGui::InputTextMultiline("##GeneratedGLSL", 
                                        const_cast<char*>(sourceText.c_str()), 
                                        sourceText.length() + 1, 
                                        ImVec2(-1, -1), 
                                        ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }

            // SPIR-V hex dump
            auto spirvIt = shaderInfo.m_SPIRVBinary.find(stage);
            if (spirvIt != shaderInfo.m_SPIRVBinary.end() && ImGui::BeginTabItem("SPIR-V Binary"))
            {
                ImGui::Text("Size: %s", DebugUtils::FormatMemorySize(spirvIt->second.size()).c_str());
                ImGui::Separator();
                
                ImGui::BeginChild("SPIRVBinary", ImVec2(0, 0), true);
                
                // Display as hex dump
                const u8* data = spirvIt->second.data();
                const sizet size = spirvIt->second.size();
                
                for (sizet i = 0; i < size; i += 16)
                {
                    ImGui::Text("%08zX: ", i);
                    ImGui::SameLine();
                    
                    // Hex bytes
                    for (sizet j = 0; j < 16 && (i + j) < size; ++j)
                    {
                        ImGui::SameLine();
                        ImGui::Text("%02X", data[i + j]);
                        if (j == 7) { ImGui::SameLine(); ImGui::Text(" "); }
                    }
                    
                    // ASCII representation
                    ImGui::SameLine(0, 20);
                    for (sizet j = 0; j < 16 && (i + j) < size; ++j)
                    {
                        const u8 c = data[i + j];
                        ImGui::SameLine(0, 0);
                        ImGui::Text("%c", (c >= 32 && c < 127) ? c : '.');
                    }
                }
                
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    void ShaderDebugger::RenderUniforms(const ShaderInfo& shaderInfo)
    {
        // Uniforms table
        if (!shaderInfo.m_Uniforms.empty())
        {
            ImGui::Text("Uniforms (%zu):", shaderInfo.m_Uniforms.size());
            
            if (ImGui::BeginTable("UniformsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Location");
                ImGui::TableSetupColumn("Set Count");
                ImGui::TableSetupColumn("Last Set");
                ImGui::TableHeadersRow();

                for (const auto& uniform : shaderInfo.m_Uniforms)
                {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", uniform.m_Name.c_str());
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", GetUniformTypeString(uniform.m_Type).c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", uniform.m_Location);
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", uniform.m_SetCount);
                    
                    ImGui::TableSetColumnIndex(4);
                    if (uniform.m_LastSetTime != std::chrono::steady_clock::time_point{})
                    {
                        const auto now = std::chrono::steady_clock::now();
                        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - uniform.m_LastSetTime).count();
                        ImGui::Text("%lld s ago", elapsed);
                    }
                    else
                    {
                        ImGui::Text("Never");
                    }
                }
                
                ImGui::EndTable();
            }
        }

        ImGui::Separator();

        // Uniform Buffers
        if (!shaderInfo.m_UniformBuffers.empty())
        {
            ImGui::Text("Uniform Buffers (%zu):", shaderInfo.m_UniformBuffers.size());
            
            for (const auto& ubo : shaderInfo.m_UniformBuffers)
            {
                if (ImGui::TreeNode(ubo.m_Name.c_str()))
                {
                    ImGui::Text("Binding: %u", ubo.m_Binding);
                    ImGui::Text("Size: %s", DebugUtils::FormatMemorySize(ubo.m_Size).c_str());
                    ImGui::Text("Members (%zu):", ubo.m_Members.size());
                    
                    ImGui::Indent();
                    for (const auto& member : ubo.m_Members)
                    {
                        ImGui::BulletText("%s", member.c_str());
                    }
                    ImGui::Unindent();
                    
                    ImGui::TreePop();
                }
            }
        }

        ImGui::Separator();

        // Samplers
        if (!shaderInfo.m_Samplers.empty())
        {
            ImGui::Text("Samplers (%zu):", shaderInfo.m_Samplers.size());
            
            if (ImGui::BeginTable("SamplersTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Binding");
                ImGui::TableSetupColumn("Texture Unit");
                ImGui::TableHeadersRow();

                for (const auto& sampler : shaderInfo.m_Samplers)
                {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", sampler.m_Name.c_str());
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", sampler.m_Type.c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", sampler.m_Binding);
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", sampler.m_TextureUnit);
                }
                
                ImGui::EndTable();
            }
        }

        if (shaderInfo.m_Uniforms.empty() && shaderInfo.m_UniformBuffers.empty() && shaderInfo.m_Samplers.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No uniform information available");
        }
    }

    void ShaderDebugger::RenderPerformanceMetrics(const ShaderInfo& shaderInfo)
    {
        ImGui::Text("Performance Metrics");
        ImGui::Separator();

        // Bind statistics
        ImGui::Text("Bind Count: %u", shaderInfo.m_BindCount);
        ImGui::Text("Total Active Time: %s", DebugUtils::FormatDuration(shaderInfo.m_TotalActiveTimeMs).c_str());
        
        if (shaderInfo.m_BindCount > 0)
        {
            const f64 avgActiveTime = shaderInfo.m_TotalActiveTimeMs / shaderInfo.m_BindCount;
            ImGui::Text("Avg. Active Time per Bind: %s", DebugUtils::FormatDuration(avgActiveTime).c_str());
        }

        ImGui::Separator();
		
		// Compilation metrics
        ImGui::Text("Compilation Time: %s", DebugUtils::FormatDuration(shaderInfo.m_LastCompilation.m_CompileTimeMs).c_str());
        ImGui::Text("Instruction Count: %u", shaderInfo.m_LastCompilation.m_InstructionCount);        const sizet totalSPIRVSize = shaderInfo.m_LastCompilation.m_VertexGeometrySPIRVSize + shaderInfo.m_LastCompilation.m_FragmentComputeSPIRVSize;
        ImGui::Text("Total SPIR-V Size: %s", DebugUtils::FormatMemorySize(totalSPIRVSize).c_str());
        ImGui::Text("Vertex+Geometry SPIR-V: %s", DebugUtils::FormatMemorySize(shaderInfo.m_LastCompilation.m_VertexGeometrySPIRVSize).c_str());
        ImGui::Text("Fragment+Compute SPIR-V: %s", DebugUtils::FormatMemorySize(shaderInfo.m_LastCompilation.m_FragmentComputeSPIRVSize).c_str());

        ImGui::Separator();

        // Performance indicators
        ImGui::Text("Performance Indicators:");
          if (shaderInfo.m_LastCompilation.m_CompileTimeMs > 100.0)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "⚠ Slow compilation (>100ms)");
        }
        
        if (shaderInfo.m_LastCompilation.m_InstructionCount > 1000)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "⚠ High instruction count (>1000)");
        }
        
        if (totalSPIRVSize > 1024 * 50) // 50KB
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "⚠ Large SPIR-V binary (>50KB)");
        }
        
        if (shaderInfo.m_BindCount == 0)
        {
            ImGui::TextColored(DebugUtils::Colors::Disabled, "ℹ Shader never bound");
        }
        else if (shaderInfo.m_BindCount > 1000)
        {
            ImGui::TextColored(DebugUtils::Colors::Good, "✓ Frequently used shader");
        }
    }

    void ShaderDebugger::RenderReloadHistory(const ShaderInfo& shaderInfo)
    {
        ImGui::Text("Reload History (%zu events):", shaderInfo.m_ReloadHistory.size());
        ImGui::Separator();

        if (shaderInfo.m_ReloadHistory.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No reload events recorded");
            return;
        }

        if (ImGui::BeginTable("ReloadHistoryTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Result");
            ImGui::TableSetupColumn("Reason");
            ImGui::TableHeadersRow();

            // Show events in reverse chronological order (newest first)
            for (auto it = shaderInfo.m_ReloadHistory.rbegin(); it != shaderInfo.m_ReloadHistory.rend(); ++it)
            {
                const auto& event = *it;
                
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    event.m_Timestamp.time_since_epoch()).count();
                ImGui::Text("%lld", timestamp);
                
                ImGui::TableSetColumnIndex(1);
                if (event.m_Success)
                {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Success");
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed");
                }
                
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", event.m_Reason.c_str());
            }
            
            ImGui::EndTable();
        }
    }

    void ShaderDebugger::RenderSPIRVAnalysis(const ShaderInfo& shaderInfo)
    {
        ImGui::Text("SPIR-V Analysis");
        ImGui::Separator();

        if (shaderInfo.m_SPIRVBinary.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No SPIR-V data available");
            return;
        }

        // Overview
        ImGui::Text("Available Stages:");
        for (const auto& [stage, binary] : shaderInfo.m_SPIRVBinary)
        {
            ImGui::BulletText("%s: %s", 
                            GetShaderStageString(stage).c_str(), 
                            DebugUtils::FormatMemorySize(binary.size()).c_str());
        }

        ImGui::Separator();

        // Detailed analysis per stage
        static i32 selectedStageForAnalysis = 0;
        const char* stageNames[] = { "Vertex", "Fragment", "Geometry", "Compute" };
        ImGui::Combo("Analyze Stage", &selectedStageForAnalysis, stageNames, IM_ARRAYSIZE(stageNames));

        const ShaderStage analysisStage = static_cast<ShaderStage>(selectedStageForAnalysis);
        auto spirvIt = shaderInfo.m_SPIRVBinary.find(analysisStage);
        
        if (spirvIt != shaderInfo.m_SPIRVBinary.end())
        {
            const auto& binary = spirvIt->second;
            
            ImGui::Text("Stage: %s", GetShaderStageString(analysisStage).c_str());
            ImGui::Text("Binary Size: %s", DebugUtils::FormatMemorySize(binary.size()).c_str());
            
            // Estimate instruction count for this stage
            u32 stageInstructionCount = 0;
            AnalyzeSPIRV(binary, stageInstructionCount);
            ImGui::Text("Estimated Instructions: %u", stageInstructionCount);
            
            // Basic SPIR-V header info
            if (binary.size() >= 20)
            {
                const u32* header = reinterpret_cast<const u32*>(binary.data());
                ImGui::Text("Magic Number: 0x%08X", header[0]);
                ImGui::Text("Version: %u.%u", (header[1] >> 16) & 0xFF, (header[1] >> 8) & 0xFF);
                ImGui::Text("Generator: 0x%08X", header[2]);
                ImGui::Text("Bound: %u", header[3]);
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 
                             "No SPIR-V data for %s stage", GetShaderStageString(analysisStage).c_str());
        }
		ImGui::Separator();

        // Advanced SPIR-V analysis section
        if (spirvIt != shaderInfo.m_SPIRVBinary.end())
        {
            const auto& binary = spirvIt->second;
            
            // SPIR-V disassembly section
            static bool showDisassembly = false;
            static std::string disassemblyText;
            
            if (ImGui::Button("Generate SPIR-V Disassembly"))
            {
                disassemblyText = GenerateSPIRVDisassembly(binary);
                showDisassembly = true;
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Optimize Analysis"))
            {
                PerformOptimizationAnalysis(binary);
            }
            
            if (showDisassembly && !disassemblyText.empty())
            {
                ImGui::Separator();
                ImGui::Text("SPIR-V Disassembly:");
                
                if (ImGui::Button("Copy to Clipboard"))
                {
                    ImGui::SetClipboardText(disassemblyText.c_str());
                }
                
                ImGui::SameLine();
                if (ImGui::Button("Hide Disassembly"))
                {
                    showDisassembly = false;
                }
                
                // Make the disassembly text selectable and copyable
                ImGui::InputTextMultiline("##SPIRVDisassembly", 
                                        const_cast<char*>(disassemblyText.c_str()), 
                                        disassemblyText.length() + 1, 
                                        ImVec2(-1, 200), 
                                        ImGuiInputTextFlags_ReadOnly);
            }

            // Enhanced resource analysis using spirv-cross
            try
            {
                // Convert byte vector to u32 vector for spirv-cross
                std::vector<u32> spirvWords;
                spirvWords.resize(binary.size() / sizeof(u32));
                std::memcpy(spirvWords.data(), binary.data(), binary.size());
                
                spirv_cross::Compiler compiler(spirvWords);
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();

                ImGui::Separator();
                ImGui::Text("Resource Analysis:");
                ImGui::Indent();
                ImGui::Text("Uniform Buffers: %zu", resources.uniform_buffers.size());
                ImGui::Text("Storage Buffers: %zu", resources.storage_buffers.size());
                ImGui::Text("Sampled Images: %zu", resources.sampled_images.size());
                ImGui::Text("Storage Images: %zu", resources.storage_images.size());
                ImGui::Text("Input Attributes: %zu", resources.stage_inputs.size());
                ImGui::Text("Output Attributes: %zu", resources.stage_outputs.size());
                ImGui::Unindent();
                
                // Show entry point info
                const auto entryPoints = compiler.get_entry_points_and_stages();
                if (!entryPoints.empty())
                {
                    ImGui::Text("Entry Points:");
                    ImGui::Indent();
                    for (const auto& ep : entryPoints)
                    {
                        ImGui::Text("- %s (model: %d)", ep.name.c_str(), static_cast<i32>(ep.execution_model));
                    }
                    ImGui::Unindent();
                }
            }
            catch (const std::exception& e)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), 
                                 "SPIR-V analysis failed: %s", e.what());
            }
        }
    }

    void ShaderDebugger::RenderCompilationErrors(const ShaderInfo& shaderInfo)
    {
        ImGui::Text("Compilation Errors");
        ImGui::Separator();

        if (!shaderInfo.m_HasErrors)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "No compilation errors");
            return;
        }

        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Compilation Failed");
        ImGui::Text("Compile Time: %.2fms", shaderInfo.m_LastCompilation.m_CompileTimeMs);
        
        const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            shaderInfo.m_LastCompilation.m_Timestamp.time_since_epoch()).count();
        ImGui::Text("Error Time: %lld", timestamp);

        ImGui::Separator();

        ImGui::Text("Error Message:");
        ImGui::BeginChild("ErrorMessage", ImVec2(0, 0), true);
        ImGui::TextWrapped("%s", shaderInfo.m_LastCompilation.m_ErrorMessage.c_str());
        ImGui::EndChild();
    }

    /**
     * @brief Generates human-readable SPIR-V disassembly from binary data
     * @param spirvData SPIR-V binary data
     * @return GLSL representation of SPIR-V code for debugging
     * 
     * Uses spirv-cross to convert SPIR-V binary back to readable GLSL.
     * This helps developers understand the compiled shader structure.
     */
    std::string ShaderDebugger::GenerateSPIRVDisassembly(const std::vector<u8>& spirvData) const
    {
        if (spirvData.empty())
            return "No SPIR-V data available";
            
        try
        {
            // Convert byte vector to u32 vector for spirv-cross
            std::vector<u32> spirvWords;
            spirvWords.resize(spirvData.size() / sizeof(u32));
            std::memcpy(spirvWords.data(), spirvData.data(), spirvData.size());
            
            spirv_cross::Compiler compiler(spirvWords);
            
            // Generate GLSL output as a form of disassembly
            spirv_cross::CompilerGLSL glslCompiler(spirvWords);
            
            // Set options for more readable output
            spirv_cross::CompilerGLSL::Options options;
            options.version = 450;
            options.es = false;
            options.vulkan_semantics = false;
            options.enable_420pack_extension = true;
            glslCompiler.set_common_options(options);
            
            std::string disassembly = "=== SPIR-V to GLSL Disassembly ===\n\n";
            
            // Add basic information
            disassembly += "Original SPIR-V size: " + std::to_string(spirvData.size()) + " bytes\n";
            disassembly += "Word count: " + std::to_string(spirvWords.size()) + "\n\n";
            
            // Add the converted GLSL
            disassembly += "=== Generated GLSL ===\n";
            disassembly += glslCompiler.compile();
            
            // Add resource information
            const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            disassembly += "\n\n=== Resource Summary ===\n";
            disassembly += "Uniform buffers: " + std::to_string(resources.uniform_buffers.size()) + "\n";
            disassembly += "Storage buffers: " + std::to_string(resources.storage_buffers.size()) + "\n";
            disassembly += "Sampled images: " + std::to_string(resources.sampled_images.size()) + "\n";
            disassembly += "Storage images: " + std::to_string(resources.storage_images.size()) + "\n";
            disassembly += "Push constant buffers: " + std::to_string(resources.push_constant_buffers.size()) + "\n";
            
            return disassembly;
        }
        catch (const std::exception& e)
        {
            return std::string("SPIR-V disassembly failed: ") + e.what();
        }
	}

    /**
     * @brief Analyzes SPIR-V code for optimization opportunities
     * @param spirvData SPIR-V binary data
     * 
     * Examines shader resources and instruction count to suggest performance
     * optimizations such as reducing uniform buffer bindings or instruction count.
     */
    void ShaderDebugger::PerformOptimizationAnalysis(const std::vector<u8>& spirvData) const
    {
        if (spirvData.empty())
        {
            OLO_CORE_WARN("Cannot perform optimization analysis: No SPIR-V data");
            return;
        }
        
        try
        {
            // Convert byte vector to u32 vector for spirv-cross
            std::vector<u32> spirvWords;
            spirvWords.resize(spirvData.size() / sizeof(u32));
            std::memcpy(spirvWords.data(), spirvData.data(), spirvData.size());
            
            spirv_cross::Compiler compiler(spirvWords);
            const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
            
            // Analyze and log optimization opportunities
            OLO_CORE_INFO("=== Shader Optimization Analysis ===");
            
            // Check for excessive uniform buffers
            if (resources.uniform_buffers.size() > 8)
            {
                OLO_CORE_WARN("High uniform buffer count ({0}). Consider combining buffers.", 
                             resources.uniform_buffers.size());
            }
            
            // Check for excessive texture bindings
            if (resources.sampled_images.size() > 16)
            {
                OLO_CORE_WARN("High texture binding count ({0}). Consider texture arrays or atlasing.", 
                             resources.sampled_images.size());
            }
            
            // Check for storage buffers (might indicate complex compute operations)
            if (!resources.storage_buffers.empty())
            {
                OLO_CORE_INFO("Storage buffers detected ({0}). Ensure efficient memory access patterns.", 
                             resources.storage_buffers.size());
            }
            
            // Analyze instruction density
            u32 instructionCount = 0;
            AnalyzeSPIRV(spirvData, instructionCount);
            
            if (instructionCount > 1000)
            {
                OLO_CORE_WARN("High instruction count ({0}). Consider shader optimization.", instructionCount);
            }
            else if (instructionCount > 500)
            {
                OLO_CORE_INFO("Moderate instruction count ({0}). Monitor performance on low-end devices.", instructionCount);
            }
            else
            {
                OLO_CORE_INFO("Reasonable instruction count ({0}).", instructionCount);
            }
            
            // Check for entry points
            const auto entryPoints = compiler.get_entry_points_and_stages();
            if (entryPoints.size() > 1)
            {
                OLO_CORE_INFO("Multiple entry points detected ({0}). Ensure correct usage.", entryPoints.size());
            }
            
            OLO_CORE_INFO("=== End Optimization Analysis ===");
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Optimization analysis failed: {0}", e.what());
        }
    }
}
