#include "OloEnginePCH.h"
#include "ShaderEditorPanel.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>

#include <fstream>

namespace OloEngine
{
    void ShaderEditorPanel::OnImGuiRender(bool* p_open)
    {
        if (!ImGui::Begin("Shader Editor", p_open, ImGuiWindowFlags_MenuBar))
        {
            ImGui::End();
            return;
        }

        // Menu bar
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Save", "Ctrl+S", false, m_Dirty))
                {
                    Save();
                }
                if (ImGui::MenuItem("Reload from Disk", nullptr, false, m_FileLoaded))
                {
                    LoadFileContents(m_CurrentFilePath);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Shader"))
            {
                if (ImGui::MenuItem("Recompile", nullptr, false, m_FileLoaded))
                {
                    if (m_Dirty)
                    {
                        if (!Save())
                        {
                            ImGui::EndMenu();
                            ImGui::EndMenuBar();
                            ImGui::End();
                            return;
                        }
                    }
                    auto shaderName = m_CurrentFilePath.stem().string();

                    bool reloaded = false;

                    // Reload in Renderer3D's library
                    auto& library3D = Renderer3D::GetShaderLibrary();
                    if (library3D.Exists(shaderName))
                    {
                        library3D.Get(shaderName)->Reload();
                        reloaded = true;
                    }

                    // Reload in Renderer2D's library
                    auto& library2D = Renderer2D::GetShaderLibrary();
                    if (library2D.Exists(shaderName))
                    {
                        library2D.Get(shaderName)->Reload();
                        reloaded = true;
                    }

                    // Reload matching post-process shader
                    if (auto ppPass = Renderer3D::GetPostProcessPass(); ppPass)
                    {
                        ppPass->ReloadShader(shaderName);
                    }

                    if (reloaded)
                    {
                        m_CompileOutput = "Recompiled '" + shaderName + "' successfully.";
                    }
                    else
                    {
                        m_CompileOutput = "Shader '" + shaderName + "' not found in any library.";
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // File path display
        if (m_FileLoaded)
        {
            ImGui::TextWrapped("File: %s%s", m_CurrentFilePath.string().c_str(), m_Dirty ? " *" : "");
            ImGui::Separator();
        }
        else
        {
            ImGui::TextDisabled("No shader file open. Double-click a .glsl file in the Content Browser.");
            ImGui::End();
            return;
        }

        // Compile output
        if (!m_CompileOutput.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
            ImGui::TextWrapped("%s", m_CompileOutput.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        // Text editor area — use InputTextMultiline for in-place editing
        ImVec2 const availSize = ImGui::GetContentRegionAvail();
        if (ImGui::InputTextMultiline(
                "##ShaderSource",
                m_SourceCode.data(),
                m_SourceCode.capacity() + 1,
                availSize,
                ImGuiInputTextFlags_CallbackResize,
                [](ImGuiInputTextCallbackData* data) -> int
                {
                    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
                    {
                        auto* str = static_cast<std::string*>(data->UserData);
                        str->resize(static_cast<size_t>(data->BufTextLen));
                        data->Buf = str->data();
                    }
                    return 0;
                },
                &m_SourceCode))
        {
            m_Dirty = true;
        }

        ImGui::End();
    }

    void ShaderEditorPanel::OpenFile(const std::filesystem::path& filepath)
    {
        if (m_FileLoaded && m_CurrentFilePath == filepath)
        {
            return; // Already open
        }

        // Guard against clobbering unsaved edits when switching files
        if (m_Dirty && m_FileLoaded)
        {
            // Auto-save before switching to a different file
            if (!Save())
            {
                return; // Save failed — keep the current file open
            }
        }

        LoadFileContents(filepath);
    }

    bool ShaderEditorPanel::Save()
    {
        if (!m_FileLoaded || !m_Dirty)
        {
            return true;
        }

        std::ofstream out(m_CurrentFilePath, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            m_CompileOutput = "ERROR: Could not write to " + m_CurrentFilePath.string();
            return false;
        }
        out << m_SourceCode;
        out.close();
        m_Dirty = false;
        m_CompileOutput = "Saved " + m_CurrentFilePath.filename().string();
        return true;
    }

    void ShaderEditorPanel::LoadFileContents(const std::filesystem::path& filepath)
    {
        std::ifstream in(filepath, std::ios::in | std::ios::ate);
        if (!in.is_open())
        {
            m_CompileOutput = "ERROR: Could not open " + filepath.string();
            m_FileLoaded = false;
            return;
        }

        auto const size = in.tellg();
        in.seekg(0);
        m_SourceCode.resize(static_cast<size_t>(size));
        in.read(m_SourceCode.data(), size);
        in.close();

        // Reserve extra capacity for editing
        m_SourceCode.reserve(m_SourceCode.size() + 4096);

        m_CurrentFilePath = filepath;
        m_Dirty = false;
        m_FileLoaded = true;
        m_CompileOutput.clear();
    }
} // namespace OloEngine
