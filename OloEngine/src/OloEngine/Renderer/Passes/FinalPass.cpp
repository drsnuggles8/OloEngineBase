#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalPass.h"

// Add OpenGL includes
#include <glad/gl.h>
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    FinalPass::FinalPass(const Ref<Framebuffer>& inputFramebuffer)
        : m_InputFramebuffer(inputFramebuffer)
    {
        OLO_PROFILE_FUNCTION();
        m_Name = "FinalPass";
        m_Shader = Shader::Create("assets/shaders/FullscreenTriangle.glsl");
        InitFullscreenTriangle();
    }

    FinalPass::~FinalPass()
    {
        OLO_PROFILE_FUNCTION();
        glDeleteVertexArrays(1, &m_FullscreenTriangleVAO);
        glDeleteBuffers(1, &m_FullscreenTriangleVBO);
    }

    std::vector<Ref<Framebuffer>> FinalPass::GetDependencies() const
    {
        return { m_InputFramebuffer };
    }

    void FinalPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Bind default framebuffer (0) for final output
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        
        // Clear default framebuffer
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Bind shader and set uniforms
        m_Shader->Bind();
        
        // Bind the input texture from previous pass
        u32 colorAttachment = m_InputFramebuffer->GetColorAttachmentRendererID(0);
        glBindTextureUnit(0, colorAttachment);
        m_Shader->SetInt("u_Texture", 0);

        // Render fullscreen triangle
        glBindVertexArray(m_FullscreenTriangleVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        
        // Cleanup
        glBindVertexArray(0);
        m_Shader->Unbind();
        glEnable(GL_DEPTH_TEST);
    }

    const std::string& FinalPass::GetName() const
    {
        return m_Name;
    }

    void FinalPass::InitFullscreenTriangle()
    {
        OLO_PROFILE_FUNCTION();
        
        // Create a single fullscreen triangle that covers the entire screen
        // This is more efficient than using a quad (2 triangles)
        // The triangle is positioned so it covers the entire NDC space without a vertex beyond -1 to 1
        // Vertex positions     // Texture coords
        float vertices[] = {
            // First triangle (covers full screen with clever tex coords)
            -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, // bottom left
             3.0f, -1.0f, 0.0f, 2.0f, 0.0f, // bottom right (way off screen)
            -1.0f,  3.0f, 0.0f, 0.0f, 2.0f  // top left (way off screen)
        };

        // Create and setup vertex array and buffer objects
        glCreateVertexArrays(1, &m_FullscreenTriangleVAO);
        glCreateBuffers(1, &m_FullscreenTriangleVBO);
        
        glBindVertexArray(m_FullscreenTriangleVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_FullscreenTriangleVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        
        // Position
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        
        // Texture coordinates
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        
        glBindVertexArray(0);
    }
}