#include "OloEnginePCH.h"
#include "CommandFinalRenderPass.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"

namespace OloEngine
{
    CommandFinalRenderPass::CommandFinalRenderPass()
    {
        SetName("CommandFinalRenderPass");
        OLO_CORE_INFO("Creating CommandFinalRenderPass");
    }

    void CommandFinalRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        
        m_FramebufferSpec = spec;
        
        // We don't create a framebuffer for this pass since we render to the default framebuffer
        m_Target = nullptr;
        
        // Create the fullscreen triangle
        CreateFullscreenTriangle();
        
        // Load or create the blit shader
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");
        
        if (!m_BlitShader)
        {
            OLO_CORE_ERROR("CommandFinalRenderPass::Init: Failed to load blit shader!");
            
            // Try to load from Renderer3D's shader library as fallback
            m_BlitShader = Renderer3D::GetShaderLibrary().Get("FullscreenBlit");
            
            if (!m_BlitShader)
            {
                OLO_CORE_ERROR("CommandFinalRenderPass::Init: Failed to find blit shader in library!");
            }
        }
        
        OLO_CORE_INFO("CommandFinalRenderPass: Initialized with viewport dimensions {}x{}", 
                     spec.Width, spec.Height);
    }

    void CommandFinalRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input)
    {
        OLO_PROFILE_FUNCTION();
        
        m_InputFramebuffer = input;
    }

    void CommandFinalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("CommandFinalRenderPass::SetupFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        // Store dimensions for viewport setup
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void CommandFinalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("CommandFinalRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }
        
        // Update stored dimensions
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        
        OLO_CORE_INFO("CommandFinalRenderPass: Resized viewport to {}x{}", width, height);
    }

    void CommandFinalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        
        // Recreate the fullscreen triangle
        CreateFullscreenTriangle();
        
        // Reload the shader
        if (m_BlitShader)
            m_BlitShader->Reload();
    }

    void CommandFinalRenderPass::CreateFullscreenTriangle()
    {
        OLO_PROFILE_FUNCTION();
        
        // Create a single triangle that covers the entire screen
        // We use a large triangle (larger than NDC space) to ensure full screen coverage
        
        // Vertex layout:
        // position (vec3) + texcoord (vec2)
        struct FullscreenVertex
        {
            glm::vec3 Position;
            glm::vec2 TexCoord;
        };
        
        FullscreenVertex vertices[3] = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } }, // Bottom left
            { {  3.0f, -1.0f, 0.0f }, { 2.0f, 0.0f } }, // Bottom right (extended)
            { { -1.0f,  3.0f, 0.0f }, { 0.0f, 2.0f } }  // Top left (extended)
        };
        
        u32 indices[3] = { 0, 1, 2 }; // Triangle
        
        // Create vertex array
        m_FullscreenTriangleVA = VertexArray::Create();
        
        // Create vertex buffer with the correct data type
        // Cast the vertices to float* as required by the VertexBuffer::Create method
        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(
            reinterpret_cast<f32*>(vertices), 
            static_cast<u32>(sizeof(vertices))
        );
        
        vertexBuffer->SetLayout({
            { ShaderDataType::Float3, "a_Position" },
            { ShaderDataType::Float2, "a_TexCoord" }
        });
        
        // Create index buffer
        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, 3);
        
        // Add buffers to vertex array
        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);
        
        OLO_CORE_INFO("CommandFinalRenderPass: Created fullscreen triangle");
    }

    void CommandFinalRenderPass::BeginRender()
    {
        OLO_PROFILE_FUNCTION();
        
        // We explicitly don't call the base class implementation here
        // because we want to render to the default framebuffer
        
        // Set viewport to match the stored dimensions
        SetViewportCommand viewportCmd;
        viewportCmd.x = 0;
        viewportCmd.y = 0;
        viewportCmd.width = m_FramebufferSpec.Width;
        viewportCmd.height = m_FramebufferSpec.Height;
        
        if (m_Allocator)
        {
            CommandPacket* packet = m_Allocator->CreateCommandPacket(viewportCmd);
            if (packet)
            {
                packet->Execute(RenderCommand::GetRendererAPI());
            }
        }
        
        // Clear the default framebuffer
        ClearCommand clearCmd;
        clearCmd.clearColor = true;
        clearCmd.clearDepth = true;
        
        if (m_Allocator)
        {
            CommandPacket* packet = m_Allocator->CreateCommandPacket(clearCmd);
            if (packet)
            {
                packet->Execute(RenderCommand::GetRendererAPI());
            }
        }
    }

    void CommandFinalRenderPass::EndRender()
    {
        OLO_PROFILE_FUNCTION();
        
        // We explicitly don't call the base class implementation here
        // as we're rendering to the default framebuffer
    }

    void CommandFinalRenderPass::BuildCommandBucket(CommandBucket& bucket)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_InputFramebuffer)
        {
            OLO_CORE_WARN("CommandFinalRenderPass::BuildCommandBucket: No input framebuffer set");
            return;
        }
        
        if (!m_FullscreenTriangleVA || !m_BlitShader)
        {
            OLO_CORE_WARN("CommandFinalRenderPass::BuildCommandBucket: Missing resources for rendering");
            return;
        }
        
        // Bind the shader for fullscreen rendering
        m_BlitShader->Bind();
        m_BlitShader->SetInt("u_Texture", 0); // Texture slot 0
        
        // Create bind texture command for the input framebuffer's color attachment
        BindTextureCommand bindTexCmd;
        bindTexCmd.slot = 0;
        bindTexCmd.textureID = m_InputFramebuffer->GetColorAttachmentRendererID(0);  // Use index 0
        
        // Set metadata for sorting
        PacketMetadata bindTexMetadata;
        bindTexMetadata.executionOrder = 0; // Make this execute first
        
        // Submit the command to the bucket
        bucket.Submit(bindTexCmd, bindTexMetadata, m_Allocator);
        
        // Create depth test command (disable for fullscreen rendering)
        SetDepthTestCommand depthCmd;
        depthCmd.enabled = false;
        
        PacketMetadata depthMetadata;
        depthMetadata.executionOrder = 1; // Execute second
        
        bucket.Submit(depthCmd, depthMetadata, m_Allocator);
        
        // Create draw indexed command for the fullscreen triangle
        // Since we can't directly access the renderer ID, we'll bind the vertex array first
        m_FullscreenTriangleVA->Bind();
        
        DrawArraysCommand drawCmd;
        // Using a DrawArrays command which doesn't need a renderer ID
        // since we've already bound the vertex array
        drawCmd.vertexCount = 3; // Fullscreen triangle uses 3 vertices
        drawCmd.primitiveType = GL_TRIANGLES;
        
        // We're not setting the rendererID field since we've already bound the vertex array
        
        PacketMetadata drawMetadata;
        drawMetadata.executionOrder = 2; // Make this render last
        
        // Submit the command to the bucket
        bucket.Submit(drawCmd, drawMetadata, m_Allocator);
    }
}