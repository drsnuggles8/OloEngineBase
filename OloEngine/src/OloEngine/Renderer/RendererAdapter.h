#pragma once

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/StatelessRenderer3D.h"
#include "OloEngine/Core/Application.h"

namespace OloEngine {

    // This class adapts calls to the correct renderer implementation based on application settings
    class RendererAdapter
    {
    public:
        static void BeginScene(const PerspectiveCamera& camera)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::BeginScene(camera);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::BeginScene(camera);
                    break;
            }
        }
        
        static void EndScene()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::EndScene();
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::EndScene();
                    break;
            }
        }
        
        static void SetLight(const Light& light)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetLight(light);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::SetLight(light);
                    break;
            }
        }
        
        static void SetViewPosition(const glm::vec3& position)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetViewPosition(position);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::SetViewPosition(position);
                    break;
            }
        }
        
        static void EnableFrustumCulling(bool enable)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::EnableFrustumCulling(enable);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::EnableFrustumCulling(enable);
                    break;
            }
        }
        
        static bool IsFrustumCullingEnabled()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    return StatelessRenderer3D::IsFrustumCullingEnabled();
                case RendererType::Renderer3D:
                default:
                    return Renderer3D::IsFrustumCullingEnabled();
            }
        }
        
        static void EnableDynamicCulling(bool enable)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::EnableDynamicCulling(enable);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::EnableDynamicCulling(enable);
                    break;
            }
        }
        
        static bool IsDynamicCullingEnabled()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    return StatelessRenderer3D::IsDynamicCullingEnabled();
                case RendererType::Renderer3D:
                default:
                    return Renderer3D::IsDynamicCullingEnabled();
            }
        }
        
        static void DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = false)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::DrawMesh(mesh, modelMatrix, material, isStatic);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::DrawMesh(mesh, modelMatrix, material, isStatic);
                    break;
            }
        }
        
        static void DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::DrawQuad(modelMatrix, texture);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::DrawQuad(modelMatrix, texture);
                    break;
            }
        }
        
        static void DrawLightCube(const glm::mat4& modelMatrix)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::DrawLightCube(modelMatrix);
                    break;
                case RendererType::Renderer3D:
                default:
                    Renderer3D::DrawLightCube(modelMatrix);
                    break;
            }
        }
        
        struct Statistics
        {
            u32 TotalMeshes = 0;
            u32 CulledMeshes = 0;
        };
        
        static Statistics GetStats()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                {
                    auto stats = StatelessRenderer3D::GetStats();
                    Statistics result;
                    result.TotalMeshes = stats.TotalMeshes;
                    result.CulledMeshes = stats.CulledMeshes;
                    return result;
                }
                case RendererType::Renderer3D:
                default:
                {
                    auto stats = Renderer3D::GetStats();
                    Statistics result;
                    result.TotalMeshes = stats.TotalMeshes;
                    result.CulledMeshes = stats.CulledMeshes;
                    return result;
                }
            }
        }

        static Ref<RenderGraph> GetRenderGraph()
        {
            // Assuming both renderers have a similar way to access the render graph
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    return StatelessRenderer3D::GetRenderGraph();
                case RendererType::Renderer3D:
                default:
                    return Renderer3D::GetRenderGraph();
            }
        }

        // State management methods needed for state tests
        static void SetPolygonMode(unsigned int face, unsigned int mode)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetPolygonMode(face, mode);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetPolygonMode(face, mode);
                    break;
            }
        }

        static void SetLineWidth(float width)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetLineWidth(width);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetLineWidth(width);
                    break;
            }
        }

        // Blending state functions
        static void EnableBlending()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::EnableBlending();
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::EnableBlending();
                    break;
            }
        }
        
        static void DisableBlending()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::DisableBlending();
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::DisableBlending();
                    break;
            }
        }
        
        static void SetBlendFunc(uint32_t src, uint32_t dst)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetBlendFunc(src, dst);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetBlendFunc(src, dst);
                    break;
            }
        }

        static void SetBlendEquation(uint32_t mode)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetBlendEquation(mode);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetBlendEquation(mode);
                    break;
            }
        }

        static void SetColorMask(bool red, bool green, bool blue, bool alpha)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetColorMask(red, green, blue, alpha);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetColorMask(red, green, blue, alpha);
                    break;
            }
        }

        static void SetDepthMask(bool enabled)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetDepthMask(enabled);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetDepthMask(enabled);
                    break;
            }
        }

        // Stencil test methods
        static void EnableStencilTest()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::EnableStencilTest();
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::EnableStencilTest();
                    break;
            }
        }

        static void DisableStencilTest()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::DisableStencilTest();
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::DisableStencilTest();
                    break;
            }
        }

        static void SetStencilFunc(unsigned int func, int ref, unsigned int mask)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetStencilFunc(func, ref, mask);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetStencilFunc(func, ref, mask);
                    break;
            }
        }

        static void SetStencilMask(unsigned int mask)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetStencilMask(mask);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetStencilMask(mask);
                    break;
            }
        }

        static void SetStencilOp(unsigned int sfail, unsigned int dpfail, unsigned int dppass)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetStencilOp(sfail, dpfail, dppass);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetStencilOp(sfail, dpfail, dppass);
                    break;
            }
        }

        static void ClearStencil()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::ClearStencil();
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::ClearStencil();
                    break;
            }
        }

        // Polygon offset method
        static void SetPolygonOffset(float factor, float units)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetPolygonOffset(factor, units);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetPolygonOffset(factor, units);
                    break;
            }
        }
        
        // Basic rendering methods
        static void SetClearColor(const glm::vec4& color)
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::SetClearColor(color);
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::SetClearColor(color);
                    break;
            }
        }
        
        static void Clear()
        {
            switch (GetActiveRendererType())
            {
                case RendererType::StatelessRenderer3D:
                    StatelessRenderer3D::Clear();
                    break;
                case RendererType::Renderer3D:
                default:
                    RenderCommand::Clear();
                    break;
            }
        }
        
    private:
        static RendererType GetActiveRendererType()
        {
            return Application::Get().GetSpecification().PreferredRenderer;
        }
    };

} // namespace OloEngine
