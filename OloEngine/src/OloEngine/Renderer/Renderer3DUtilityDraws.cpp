#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace OloEngine
{
    CommandPacket* Renderer3D::DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: ScenePass is null!");
            return nullptr;
        }
        if (!texture)
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: No texture provided!");
            return nullptr;
        }
        if (!s_Data.QuadShader)
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: Quad shader is not loaded!");
            return nullptr;
        }
        if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawQuad: Quad mesh or its vertex array is invalid!");
            s_Data.QuadMesh = MeshPrimitives::CreatePlane(1.0f, 1.0f);
            if (!s_Data.QuadMesh || !s_Data.QuadMesh->GetVertexArray())
                return nullptr;
        }

        // Create POD command
        CommandPacket* packet = CreateDrawCall<DrawQuadCommand>();
        auto* cmd = packet->GetCommandData<DrawQuadCommand>();
        cmd->header.type = CommandType::DrawQuad;
        cmd->transform = glm::mat4(modelMatrix);
        cmd->textureID = texture->GetRendererID();
        cmd->shaderHandle = s_Data.QuadShader->GetHandle();
        cmd->shaderRendererID = s_Data.QuadShader->GetRendererID();
        cmd->quadVAID = s_Data.QuadMesh->GetVertexArray()->GetRendererID();
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreateDefaultPODRenderState());

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for quad commands
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = s_Data.QuadShader->GetRendererID() & 0xFFFF;
        u32 materialID = texture ? (texture->GetRendererID() & 0xFFFF) : 0;
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::TwoD, shaderID, materialID, depth);
        packet->SetMetadata(metadata);

        return packet;
    }

    CommandPacket* Renderer3D::DrawLightCube(const glm::mat4& modelMatrix)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawLightCube: ScenePass is null!");
            return nullptr;
        }

        // In Deferred mode route into the ScenePass (which binds the 4-RT
        // G-Buffer) and substitute the LightCube_GBuffer variant shader so
        // the cube writes a full MRT payload with `emissive.a = 1.0` (unlit
        // flag). `ComputeDeferredLit` then short-circuits and outputs the
        // raw emissive colour — matching the forward debug look. Falls back
        // to the ForwardOverlayPass if the G-Buffer variant failed to load.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = deferredActive && s_Data.LightCubeGBufferShader;
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.LightCubeGBufferShader : s_Data.LightCubeShader;

        // Create POD command on the appropriate bucket.
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshCommand>()
                                    : CreateDrawCall<DrawMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        const u32 vertexArrayID = s_Data.CubeMesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = activeShader ? activeShader->GetRendererID() : 0u;
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawLightCube", vertexArrayID, shaderRendererID))
            return nullptr;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = s_Data.CubeMesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = s_Data.CubeMesh->GetIndexCount();
        cmd->transform = modelMatrix;
        cmd->prevTransform = modelMatrix; // debug viz — no motion history
        cmd->shaderHandle = activeShader->GetHandle();

        // Light cube material data — simple default material
        {
            PODMaterialData matData{};
            matData.shaderRendererID = shaderRendererID;
            matData.ambient = glm::vec3(1.0f);
            matData.diffuse = glm::vec3(1.0f);
            matData.specular = glm::vec3(1.0f);
            matData.shininess = 32.0f;
            cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(matData);
        }

        // Render state via table
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreateDefaultPODRenderState());

        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for light cube
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    CommandPacket* Renderer3D::DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic)
    {
        return DrawMesh(s_Data.CubeMesh, modelMatrix, material, isStatic);
    }

    CommandPacket* Renderer3D::DrawSkybox(const Ref<TextureCubemap>& skyboxTexture)
    {
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkybox: ScenePass is null!");
            return nullptr;
        }

        if (!skyboxTexture)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkybox: Skybox texture is null!");
            return nullptr;
        }

        if (!s_Data.SkyboxMesh || !s_Data.SkyboxShader)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkybox: Skybox mesh or shader not initialized!");
            return nullptr;
        }

        // In Deferred mode, swap to the Skybox_GBuffer variant and submit the
        // packet to ScenePass (which binds the 4-RT G-Buffer). The shader
        // writes the cubemap sample into RT2.rgb with `emissive.a = 1.0`
        // (unlit flag) so `ComputeDeferredLit` short-circuits and passes the
        // colour through unshaded. Falls back to ForwardOverlayPass when the
        // variant failed to load.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = deferredActive && s_Data.SkyboxGBufferShader;
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.SkyboxGBufferShader : s_Data.SkyboxShader;

        // Create POD command on the appropriate bucket
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawSkyboxCommand>()
                                    : CreateDrawCall<DrawSkyboxCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawSkyboxCommand>();
        cmd->header.type = CommandType::DrawSkybox;

        // Store asset handles and renderer IDs (POD)
        cmd->meshHandle = s_Data.SkyboxMesh->GetHandle();
        cmd->vertexArrayID = s_Data.SkyboxMesh->GetVertexArray()->GetRendererID();
        cmd->indexCount = s_Data.SkyboxMesh->GetIndexCount();
        cmd->transform = glm::mat4(1.0f); // Identity matrix for skybox
        cmd->shaderHandle = activeShader->GetHandle();
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->skyboxTextureID = skyboxTexture->GetRendererID();

        // Skybox-specific POD render state
        {
            PODRenderState skyboxState = CreateDefaultPODRenderState();
            skyboxState.depthTestEnabled = true;
            skyboxState.depthFunction = GL_LEQUAL;
            skyboxState.depthWriteMask = false;
            skyboxState.cullingEnabled = false;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(skyboxState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for skybox (rendered last in skybox layer with max depth)
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        // Skybox always renders at maximum depth (far plane)
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::Skybox, shaderID, 0, 0xFFFFFF);
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            // Submit directly to the overlay bucket; return nullptr so the
            // caller's follow-up SubmitPacket(packet) is a safe no-op.
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    CommandPacket* Renderer3D::DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color, f32 thickness)
    {
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawLine: ScenePass is null!");
            return nullptr;
        }

        // Use a highly emissive material for skeleton visualization
        Material material{};
        material.SetType(MaterialType::PBR);
        material.SetBaseColorFactor(glm::vec4(color, 1.0f));
        material.SetMetallicFactor(0.0f);
        material.SetRoughnessFactor(1.0f);
        material.SetEmissiveFactor(glm::vec4(color * 5.0f, 1.0f)); // Very bright emissive for visibility through surfaces

        if (!s_Data.LineQuadMesh)
        {
            OLO_CORE_WARN("Renderer3D::DrawLine: LineQuadMesh not initialized");
            return nullptr;
        }

        // Build transform: translate to start, rotate to align +X with (end-start), scale X to length and Y to thickness
        const glm::vec3 seg = end - start;
        const float length = glm::length(seg);
        if (length <= 0.0001f)
            return nullptr;

        // Convert UI thickness to world thickness
        const float worldThickness = thickness * 0.005f; // matches previous visual scale

        // Compute rotation from +X axis to segment direction
        const glm::vec3 dir = seg / length;
        const glm::vec3 xAxis = glm::vec3(1, 0, 0);
        glm::mat4 rot(1.0f);
        const float dot = glm::clamp(glm::dot(xAxis, dir), -1.0f, 1.0f);
        if (dot < 0.9999f)
        {
            if (dot < -0.9999f) // Direction is opposite to +X axis (antiparallel)
            {
                // Use Y axis for 180-degree rotation to avoid zero cross product
                rot = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0));
            }
            else
            {
                glm::vec3 axis = glm::normalize(glm::cross(xAxis, dir));
                float angle = acosf(dot);
                rot = glm::rotate(glm::mat4(1.0f), angle, axis);
            }
        }
        // Scale: X=length, Y=worldThickness, Z=1
        glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(length, worldThickness, 1.0f));
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), start) * rot * scale;

        auto* packet = DrawMesh(s_Data.LineQuadMesh, transform, material);

        // Modify render state and sort key to ensure skeleton visibility through geometry
        if (packet)
        {
            auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
            if (drawCmd)
            {
                // Depth off so lines always pass depth test
                PODRenderState skelState = FrameDataBufferManager::Get().GetRenderState(drawCmd->renderStateIndex);
                skelState.depthTestEnabled = false;
                // Only write to color attachment (0); skip entity-ID (1) and normals (2)
                skelState.colorAttachmentWriteMask = 0x01;
                drawCmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(skelState);
            }

            // Move to UI layer so these draw AFTER all 3D geometry
            PacketMetadata meta = packet->GetMetadata();
            meta.m_SortKey.SetViewLayer(ViewLayerType::UI);
            packet->SetMetadata(meta);
        }

        return packet;
    }

    CommandPacket* Renderer3D::DrawSphere(const glm::vec3& position, f32 radius, const glm::vec3& color)
    {
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSphere: ScenePass is null!");
            return nullptr;
        }

        // Create transform matrix for the sphere
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(radius));

        // Use a highly emissive material for skeleton joints
        Material material{};
        material.SetType(MaterialType::PBR);
        material.SetBaseColorFactor(glm::vec4(color, 1.0f));
        material.SetMetallicFactor(0.0f);
        material.SetRoughnessFactor(0.8f);
        material.SetEmissiveFactor(glm::vec4(color * 3.0f, 1.0f)); // Very bright emission for visibility through surfaces

        CommandPacket* packet = nullptr;

        if (s_Data.SphereMesh)
        {
            packet = DrawMesh(s_Data.SphereMesh, transform, material);
        }
        else
        {
            OLO_CORE_WARN("Renderer3D::DrawSphere: No sphere mesh available; returning nullptr");
            return nullptr;
        }

        // Modify render state and sort key to ensure joint visibility through geometry
        if (packet)
        {
            auto* drawCmd = packet->GetCommandData<DrawMeshCommand>();
            if (drawCmd)
            {
                // Depth off so joints always pass depth test
                PODRenderState jointState = FrameDataBufferManager::Get().GetRenderState(drawCmd->renderStateIndex);
                jointState.depthTestEnabled = false;
                // Only write to color attachment (0); skip entity-ID (1) and normals (2)
                jointState.colorAttachmentWriteMask = 0x01;
                drawCmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(jointState);
            }

            // Move to UI layer so these draw AFTER all 3D geometry
            PacketMetadata meta = packet->GetMetadata();
            meta.m_SortKey.SetViewLayer(ViewLayerType::UI);
            packet->SetMetadata(meta);
        }

        return packet;
    }

    void Renderer3D::DrawCameraFrustum(const glm::mat4& cameraTransform,
                                       f32 fov, f32 aspectRatio,
                                       f32 nearClip, f32 farClip,
                                       const glm::vec3& color,
                                       bool isPerspective,
                                       f32 orthoSize)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawCameraFrustum: ScenePass is null!");
            return;
        }

        // Clamp far plane for visualization (avoid frustums too large to be useful)
        const f32 visualFar = glm::min(farClip, 50.0f);
        const f32 lineThickness = 1.5f;

        // Extract camera position and orientation from transform
        glm::vec3 position = glm::vec3(cameraTransform[3]);
        glm::vec3 forward = -glm::normalize(glm::vec3(cameraTransform[2])); // -Z is forward
        glm::vec3 right = glm::normalize(glm::vec3(cameraTransform[0]));
        glm::vec3 up = glm::normalize(glm::vec3(cameraTransform[1]));

        // Calculate frustum corners
        std::array<glm::vec3, 8> corners;

        if (isPerspective)
        {
            // Perspective frustum
            const f32 tanHalfFov = std::tan(fov * 0.5f);

            // Near plane dimensions
            const f32 nearHeight = 2.0f * tanHalfFov * nearClip;
            const f32 nearWidth = nearHeight * aspectRatio;

            // Far plane dimensions (clamped for visualization)
            const f32 farHeight = 2.0f * tanHalfFov * visualFar;
            const f32 farWidth = farHeight * aspectRatio;

            // Near plane center
            glm::vec3 nearCenter = position + forward * nearClip;
            // Far plane center
            glm::vec3 farCenter = position + forward * visualFar;

            // Near plane corners (top-left, top-right, bottom-right, bottom-left)
            corners[0] = nearCenter + up * (nearHeight * 0.5f) - right * (nearWidth * 0.5f); // Near top-left
            corners[1] = nearCenter + up * (nearHeight * 0.5f) + right * (nearWidth * 0.5f); // Near top-right
            corners[2] = nearCenter - up * (nearHeight * 0.5f) + right * (nearWidth * 0.5f); // Near bottom-right
            corners[3] = nearCenter - up * (nearHeight * 0.5f) - right * (nearWidth * 0.5f); // Near bottom-left

            // Far plane corners
            corners[4] = farCenter + up * (farHeight * 0.5f) - right * (farWidth * 0.5f); // Far top-left
            corners[5] = farCenter + up * (farHeight * 0.5f) + right * (farWidth * 0.5f); // Far top-right
            corners[6] = farCenter - up * (farHeight * 0.5f) + right * (farWidth * 0.5f); // Far bottom-right
            corners[7] = farCenter - up * (farHeight * 0.5f) - right * (farWidth * 0.5f); // Far bottom-left
        }
        else
        {
            // Orthographic frustum
            const f32 halfHeight = orthoSize;
            const f32 halfWidth = orthoSize * aspectRatio;

            glm::vec3 nearCenter = position + forward * nearClip;
            glm::vec3 farCenter = position + forward * visualFar;

            // Near plane corners
            corners[0] = nearCenter + up * halfHeight - right * halfWidth;
            corners[1] = nearCenter + up * halfHeight + right * halfWidth;
            corners[2] = nearCenter - up * halfHeight + right * halfWidth;
            corners[3] = nearCenter - up * halfHeight - right * halfWidth;

            // Far plane corners
            corners[4] = farCenter + up * halfHeight - right * halfWidth;
            corners[5] = farCenter + up * halfHeight + right * halfWidth;
            corners[6] = farCenter - up * halfHeight + right * halfWidth;
            corners[7] = farCenter - up * halfHeight - right * halfWidth;
        }

        // Draw near plane (quad)
        auto* p0 = DrawLine(corners[0], corners[1], color, lineThickness);
        auto* p1 = DrawLine(corners[1], corners[2], color, lineThickness);
        auto* p2 = DrawLine(corners[2], corners[3], color, lineThickness);
        auto* p3 = DrawLine(corners[3], corners[0], color, lineThickness);
        if (p0)
            SubmitPacket(p0);
        if (p1)
            SubmitPacket(p1);
        if (p2)
            SubmitPacket(p2);
        if (p3)
            SubmitPacket(p3);

        // Draw far plane (quad)
        auto* p4 = DrawLine(corners[4], corners[5], color, lineThickness);
        auto* p5 = DrawLine(corners[5], corners[6], color, lineThickness);
        auto* p6 = DrawLine(corners[6], corners[7], color, lineThickness);
        auto* p7 = DrawLine(corners[7], corners[4], color, lineThickness);
        if (p4)
            SubmitPacket(p4);
        if (p5)
            SubmitPacket(p5);
        if (p6)
            SubmitPacket(p6);
        if (p7)
            SubmitPacket(p7);

        // Draw connecting edges (near to far)
        auto* p8 = DrawLine(corners[0], corners[4], color, lineThickness);
        auto* p9 = DrawLine(corners[1], corners[5], color, lineThickness);
        auto* p10 = DrawLine(corners[2], corners[6], color, lineThickness);
        auto* p11 = DrawLine(corners[3], corners[7], color, lineThickness);
        if (p8)
            SubmitPacket(p8);
        if (p9)
            SubmitPacket(p9);
        if (p10)
            SubmitPacket(p10);
        if (p11)
            SubmitPacket(p11);

        // Draw camera direction indicator (small arrow from camera position)
        const f32 arrowLength = 0.5f;
        const f32 arrowHeadSize = 0.15f;
        glm::vec3 arrowTip = position + forward * arrowLength;
        glm::vec3 arrowColor = glm::vec3(0.2f, 0.8f, 0.2f); // Green for direction

        auto* arrowLine = DrawLine(position, arrowTip, arrowColor, lineThickness * 1.5f);
        if (arrowLine)
            SubmitPacket(arrowLine);

        // Arrow head (two lines from tip going back)
        glm::vec3 arrowBack = position + forward * (arrowLength - arrowHeadSize);
        glm::vec3 arrowHead1 = arrowBack + up * arrowHeadSize * 0.5f;
        glm::vec3 arrowHead2 = arrowBack - up * arrowHeadSize * 0.5f;
        glm::vec3 arrowHead3 = arrowBack + right * arrowHeadSize * 0.5f;
        glm::vec3 arrowHead4 = arrowBack - right * arrowHeadSize * 0.5f;

        auto* ah1 = DrawLine(arrowTip, arrowHead1, arrowColor, lineThickness);
        auto* ah2 = DrawLine(arrowTip, arrowHead2, arrowColor, lineThickness);
        auto* ah3 = DrawLine(arrowTip, arrowHead3, arrowColor, lineThickness);
        auto* ah4 = DrawLine(arrowTip, arrowHead4, arrowColor, lineThickness);
        if (ah1)
            SubmitPacket(ah1);
        if (ah2)
            SubmitPacket(ah2);
        if (ah3)
            SubmitPacket(ah3);
        if (ah4)
            SubmitPacket(ah4);
    }

    CommandPacket* Renderer3D::DrawInfiniteGrid(f32 gridScale)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawInfiniteGrid: ScenePass is null!");
            return nullptr;
        }

        if (!s_Data.InfiniteGridShader)
        {
            OLO_CORE_ERROR("Renderer3D::DrawInfiniteGrid: InfiniteGrid shader not loaded!");
            return nullptr;
        }

        if (!s_Data.FullscreenQuadVAO)
        {
            OLO_CORE_ERROR("Renderer3D::DrawInfiniteGrid: FullscreenQuadVAO not initialized!");
            return nullptr;
        }

        // Deferred path: the G-Buffer variant used to be preferred so the
        // grid participates in deferred lighting via emissive+`gl_FragDepth`.
        // But grid depth in the scene FB breaks water's SSR — the ray-march
        // finds a "hit" at the infinite plane and samples the refraction
        // texture there (which contains the grid itself), making water
        // appear to reflect/refract the grid and look transparent.
        // Route the grid through the forward overlay pass instead; its
        // `depthWriteMask=false` state keeps the grid out of the scene
        // depth buffer so downstream SSR behaves like Forward+.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        const bool useGBufferVariant = false;
        const bool overlayRoute = deferredActive && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay;
        Ref<Shader> activeShader = useGBufferVariant ? s_Data.InfiniteGridGBufferShader : s_Data.InfiniteGridShader;

        // Create POD command packet
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawInfiniteGridCommand>()
                                    : CreateDrawCall<DrawInfiniteGridCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawInfiniteGridCommand>();
        cmd->header.type = CommandType::DrawInfiniteGrid;

        // Store renderer IDs (POD)
        cmd->shaderHandle = activeShader->GetHandle();
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->quadVAOID = s_Data.FullscreenQuadVAO->GetRendererID();
        cmd->gridScale = gridScale;

        // Grid-specific render state. The G-Buffer variant writes gl_FragDepth
        // and premultiplies alpha into emissive.rgb, so blending is disabled
        // (destination G-Buffer is opaque). Forward path still alpha-blends.
        {
            PODRenderState gridState = CreateDefaultPODRenderState();
            if (useGBufferVariant)
            {
                gridState.blendEnabled = false;
                gridState.depthTestEnabled = true;
                gridState.depthWriteMask = true; // gl_FragDepth path
            }
            else
            {
                gridState.blendEnabled = true;
                gridState.blendSrcFactor = GL_SRC_ALPHA;
                gridState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
                gridState.depthTestEnabled = true;
                gridState.depthWriteMask = false;
            }
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(gridState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: G-Buffer variant sits with opaques (depth-write on); forward
        // variant stays in the transparent layer.
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        if (useGBufferVariant)
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, 0x800000);
        else
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, 0x800000);
        packet->SetMetadata(metadata);

        // Submit packet to the appropriate bucket. Despite both `CreateForwardOverlayDrawCall`
        // / `CreateDrawCall` being named "CreateDrawCall", they ONLY allocate via
        // `CommandBucket::CreateDrawCall<T>()` (which does not push into the bucket's
        // packet list). The actual submission happens here — exactly once. We return
        // nullptr afterwards to match the other overlay helpers and prevent callers
        // from accidentally double-submitting the packet.
        if (overlayRoute)
            SubmitForwardOverlayPacket(packet);
        else
            SubmitPacket(packet);
        return nullptr;
    }

    CommandPacket* Renderer3D::DrawTerrainPatch(
        RendererID vaoID, u32 indexCount, u32 patchVertexCount,
        const Ref<Shader>& shader,
        RendererID heightmapID, RendererID splatmapID, RendererID splatmap1ID,
        RendererID albedoArrayID, RendererID normalArrayID, RendererID armArrayID,
        const glm::mat4& transform,
        const ShaderBindingLayout::TerrainUBO& terrainUBO,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawTerrainPatch: ScenePass is null!");
            return nullptr;
        }

        if (vaoID == 0 || !shader)
        {
            return nullptr;
        }

        // In the deferred path, substitute the forward terrain shader with the
        // G-Buffer variant so terrain participates in the deferred composite
        // (full PBR evaluated once in DeferredLightingPass).
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        Ref<Shader> activeShader = shader;
        if (deferredActive)
        {
            if (shader == s_Data.TerrainPBRShader && s_Data.TerrainGBufferShader)
                activeShader = s_Data.TerrainGBufferShader;
            else if (shader == s_Data.VoxelPBRShader && s_Data.VoxelGBufferShader)
                activeShader = s_Data.VoxelGBufferShader;
        }
        const bool useGBufferVariant = deferredActive && (activeShader != shader);
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay;

        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawTerrainPatchCommand>()
                                    : CreateDrawCall<DrawTerrainPatchCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawTerrainPatchCommand>();
        cmd->header.type = CommandType::DrawTerrainPatch;

        cmd->vertexArrayID = vaoID;
        cmd->indexCount = indexCount;
        cmd->patchVertexCount = patchVertexCount;
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->heightmapTextureID = heightmapID;
        cmd->splatmapTextureID = splatmapID;
        cmd->splatmap1TextureID = splatmap1ID;
        cmd->albedoArrayTextureID = albedoArrayID;
        cmd->normalArrayTextureID = normalArrayID;
        cmd->armArrayTextureID = armArrayID;
        cmd->transform = transform;
        cmd->entityID = entityID;
        cmd->terrainUBOData = terrainUBO;

        // Terrain is opaque — depth test on, no blending, culling on
        {
            PODRenderState terrainState = CreateDefaultPODRenderState();
            terrainState.blendEnabled = false;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(terrainState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Sort key: group by shader for state efficiency
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(transform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = true;
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    CommandPacket* Renderer3D::DrawVoxelMesh(
        RendererID vaoID, u32 indexCount,
        const Ref<Shader>& shader,
        RendererID albedoArrayID, RendererID normalArrayID, RendererID armArrayID,
        const glm::mat4& transform,
        i32 entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawVoxelMesh: ScenePass is null!");
            return nullptr;
        }

        if (vaoID == 0 || !shader)
        {
            return nullptr;
        }

        // Substitute voxel G-Buffer shader in deferred path.
        const bool deferredActive = (s_Data.Settings.Path == RenderingPath::Deferred);
        Ref<Shader> activeShader = shader;
        if (deferredActive && shader == s_Data.VoxelPBRShader && s_Data.VoxelGBufferShader)
            activeShader = s_Data.VoxelGBufferShader;
        const bool useGBufferVariant = deferredActive && (activeShader != shader);
        const bool overlayRoute = deferredActive && !useGBufferVariant && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay;

        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawVoxelMeshCommand>()
                                    : CreateDrawCall<DrawVoxelMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawVoxelMeshCommand>();
        cmd->header.type = CommandType::DrawVoxelMesh;

        cmd->vertexArrayID = vaoID;
        cmd->indexCount = indexCount;
        cmd->shaderRendererID = activeShader->GetRendererID();
        cmd->albedoArrayTextureID = albedoArrayID;
        cmd->normalArrayTextureID = normalArrayID;
        cmd->armArrayTextureID = armArrayID;
        cmd->transform = transform;
        cmd->entityID = entityID;

        {
            PODRenderState voxelState = CreateDefaultPODRenderState();
            voxelState.blendEnabled = false;
            cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(voxelState);
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = activeShader->GetRendererID() & 0xFFFF;
        u32 depth = ComputeDepthForSortKey(transform);
        metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, 0, depth);
        metadata.m_IsStatic = true;
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    void Renderer3D::DrawDirectionalLightGizmo(const glm::vec3& position,
                                               const glm::vec3& direction,
                                               const glm::vec3& color,
                                               f32 intensity)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 2.0f;
        const f32 arrowLength = 1.5f;
        const f32 arrowHeadSize = 0.2f;

        // Normalize direction
        glm::vec3 dir = glm::normalize(direction);
        glm::vec3 arrowEnd = position + dir * arrowLength;

        // Main arrow line
        auto* mainLine = DrawLine(position, arrowEnd, color * intensity, lineThickness);
        if (mainLine)
            SubmitPacket(mainLine);

        // Create arrow head - find perpendicular vectors
        glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0, 1, 0))) < 0.99f
                           ? glm::vec3(0, 1, 0)
                           : glm::vec3(1, 0, 0);
        glm::vec3 right = glm::normalize(glm::cross(dir, up));
        up = glm::normalize(glm::cross(right, dir));

        glm::vec3 arrowBack = arrowEnd - dir * arrowHeadSize;

        // Arrow head lines (4 lines forming a pyramid)
        auto* ah1 = DrawLine(arrowEnd, arrowBack + up * arrowHeadSize, color * intensity, lineThickness);
        auto* ah2 = DrawLine(arrowEnd, arrowBack - up * arrowHeadSize, color * intensity, lineThickness);
        auto* ah3 = DrawLine(arrowEnd, arrowBack + right * arrowHeadSize, color * intensity, lineThickness);
        auto* ah4 = DrawLine(arrowEnd, arrowBack - right * arrowHeadSize, color * intensity, lineThickness);
        if (ah1)
            SubmitPacket(ah1);
        if (ah2)
            SubmitPacket(ah2);
        if (ah3)
            SubmitPacket(ah3);
        if (ah4)
            SubmitPacket(ah4);

        // Draw a small sun-like icon at position (circle with rays)
        const f32 sunRadius = 0.2f;
        const i32 segments = 8;
        for (i32 i = 0; i < segments; ++i)
        {
            f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

            glm::vec3 p1 = position + right * std::cos(angle1) * sunRadius + up * std::sin(angle1) * sunRadius;
            glm::vec3 p2 = position + right * std::cos(angle2) * sunRadius + up * std::sin(angle2) * sunRadius;

            auto* seg = DrawLine(p1, p2, color * intensity, lineThickness * 0.8f);
            if (seg)
                SubmitPacket(seg);
        }

        // Rays emanating from the sun
        for (i32 i = 0; i < 8; ++i)
        {
            f32 angle = (f32(i) / 8.0f) * glm::two_pi<f32>();
            glm::vec3 rayDir = right * std::cos(angle) + up * std::sin(angle);
            glm::vec3 rayStart = position + rayDir * (sunRadius + 0.05f);
            glm::vec3 rayEnd = position + rayDir * (sunRadius + 0.15f);

            auto* ray = DrawLine(rayStart, rayEnd, color * intensity, lineThickness * 0.6f);
            if (ray)
                SubmitPacket(ray);
        }
    }

    void Renderer3D::DrawPointLightGizmo(const glm::vec3& position,
                                         f32 range,
                                         const glm::vec3& color,
                                         bool showRangeSphere)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 1.5f;

        // Draw a light bulb icon at position
        const f32 bulbRadius = 0.15f;
        const i32 segments = 12;

        // Draw three orthogonal circles to represent a sphere
        for (i32 axis = 0; axis < 3; ++axis)
        {
            for (i32 i = 0; i < segments; ++i)
            {
                f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                glm::vec3 offset1, offset2;
                if (axis == 0) // XY plane
                {
                    offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * bulbRadius;
                    offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * bulbRadius;
                }
                else if (axis == 1) // XZ plane
                {
                    offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * bulbRadius;
                    offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * bulbRadius;
                }
                else // YZ plane
                {
                    offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * bulbRadius;
                    offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * bulbRadius;
                }

                auto* seg = DrawLine(position + offset1, position + offset2, color, lineThickness);
                if (seg)
                    SubmitPacket(seg);
            }
        }

        // Draw range sphere if enabled (larger circle to show falloff range)
        if (showRangeSphere && range > 0.0f)
        {
            // Clamp range for visualization
            f32 visualRange = glm::min(range, 20.0f);
            const i32 rangeSegments = 24;
            glm::vec3 dimColor = color * 0.3f;

            // Draw range circles on three planes
            for (i32 axis = 0; axis < 3; ++axis)
            {
                for (i32 i = 0; i < rangeSegments; ++i)
                {
                    f32 angle1 = (f32(i) / f32(rangeSegments)) * glm::two_pi<f32>();
                    f32 angle2 = (f32(i + 1) / f32(rangeSegments)) * glm::two_pi<f32>();

                    glm::vec3 offset1, offset2;
                    if (axis == 0)
                    {
                        offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * visualRange;
                        offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * visualRange;
                    }
                    else if (axis == 1)
                    {
                        offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * visualRange;
                        offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * visualRange;
                    }
                    else
                    {
                        offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * visualRange;
                        offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * visualRange;
                    }

                    auto* seg = DrawLine(position + offset1, position + offset2, dimColor, lineThickness * 0.5f);
                    if (seg)
                        SubmitPacket(seg);
                }
            }
        }
    }

    void Renderer3D::DrawSpotLightGizmo(const glm::vec3& position,
                                        const glm::vec3& direction,
                                        f32 range,
                                        f32 innerCutoff,
                                        f32 outerCutoff,
                                        const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 1.5f;

        // Normalize direction
        glm::vec3 dir = glm::normalize(direction);

        // Clamp range for visualization
        f32 visualRange = glm::min(range, 15.0f);

        // Calculate outer cone radius at the end of the range
        f32 outerAngleRad = glm::radians(outerCutoff);
        f32 innerAngleRad = glm::radians(innerCutoff);
        f32 outerRadius = visualRange * std::tan(outerAngleRad);
        f32 innerRadius = visualRange * std::tan(innerAngleRad);

        // Create coordinate system aligned with direction
        glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0, 1, 0))) < 0.99f
                           ? glm::vec3(0, 1, 0)
                           : glm::vec3(1, 0, 0);
        glm::vec3 right = glm::normalize(glm::cross(dir, up));
        up = glm::normalize(glm::cross(right, dir));

        // Cone tip to base
        glm::vec3 coneEnd = position + dir * visualRange;

        // Draw central direction line
        auto* centerLine = DrawLine(position, coneEnd, color, lineThickness);
        if (centerLine)
            SubmitPacket(centerLine);

        // Draw outer cone edges (4 lines from tip to base)
        const i32 coneEdges = 8;
        for (i32 i = 0; i < coneEdges; ++i)
        {
            f32 angle = (f32(i) / f32(coneEdges)) * glm::two_pi<f32>();
            glm::vec3 offset = right * std::cos(angle) * outerRadius + up * std::sin(angle) * outerRadius;
            glm::vec3 edgeEnd = coneEnd + offset;

            auto* edge = DrawLine(position, edgeEnd, color * 0.6f, lineThickness * 0.8f);
            if (edge)
                SubmitPacket(edge);
        }

        // Draw outer cone base circle
        const i32 circleSegments = 16;
        for (i32 i = 0; i < circleSegments; ++i)
        {
            f32 angle1 = (f32(i) / f32(circleSegments)) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / f32(circleSegments)) * glm::two_pi<f32>();

            glm::vec3 p1 = coneEnd + right * std::cos(angle1) * outerRadius + up * std::sin(angle1) * outerRadius;
            glm::vec3 p2 = coneEnd + right * std::cos(angle2) * outerRadius + up * std::sin(angle2) * outerRadius;

            auto* seg = DrawLine(p1, p2, color * 0.5f, lineThickness * 0.7f);
            if (seg)
                SubmitPacket(seg);
        }

        // Draw inner cone base circle (brighter, showing hotspot)
        if (innerRadius > 0.01f && innerRadius < outerRadius)
        {
            for (i32 i = 0; i < circleSegments; ++i)
            {
                f32 angle1 = (f32(i) / f32(circleSegments)) * glm::two_pi<f32>();
                f32 angle2 = (f32(i + 1) / f32(circleSegments)) * glm::two_pi<f32>();

                glm::vec3 p1 = coneEnd + right * std::cos(angle1) * innerRadius + up * std::sin(angle1) * innerRadius;
                glm::vec3 p2 = coneEnd + right * std::cos(angle2) * innerRadius + up * std::sin(angle2) * innerRadius;

                auto* seg = DrawLine(p1, p2, color, lineThickness);
                if (seg)
                    SubmitPacket(seg);
            }
        }

        // Draw a small light bulb icon at position
        const f32 bulbRadius = 0.1f;
        for (i32 i = 0; i < 8; ++i)
        {
            f32 angle1 = (f32(i) / 8.0f) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / 8.0f) * glm::two_pi<f32>();

            glm::vec3 p1 = position + right * std::cos(angle1) * bulbRadius + up * std::sin(angle1) * bulbRadius;
            glm::vec3 p2 = position + right * std::cos(angle2) * bulbRadius + up * std::sin(angle2) * bulbRadius;

            auto* seg = DrawLine(p1, p2, color, lineThickness);
            if (seg)
                SubmitPacket(seg);
        }
    }

    void Renderer3D::DrawAudioSourceGizmo(const glm::vec3& position,
                                          f32 minDistance,
                                          f32 maxDistance,
                                          const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 1.5f;
        const i32 segments = 16;

        // Clamp distances for visualization
        f32 visualMin = glm::min(minDistance, 10.0f);
        f32 visualMax = glm::min(maxDistance, 30.0f);

        // Draw speaker icon at position (small)
        const f32 iconSize = 0.15f;
        glm::vec3 iconColor = color * 1.2f;

        // Speaker body (rectangle)
        auto* l1 = DrawLine(position + glm::vec3(-iconSize, -iconSize * 0.5f, 0),
                            position + glm::vec3(-iconSize, iconSize * 0.5f, 0), iconColor, lineThickness);
        auto* l2 = DrawLine(position + glm::vec3(-iconSize, iconSize * 0.5f, 0),
                            position + glm::vec3(0, iconSize * 0.5f, 0), iconColor, lineThickness);
        auto* l3 = DrawLine(position + glm::vec3(0, iconSize * 0.5f, 0),
                            position + glm::vec3(iconSize, iconSize, 0), iconColor, lineThickness);
        auto* l4 = DrawLine(position + glm::vec3(iconSize, iconSize, 0),
                            position + glm::vec3(iconSize, -iconSize, 0), iconColor, lineThickness);
        auto* l5 = DrawLine(position + glm::vec3(iconSize, -iconSize, 0),
                            position + glm::vec3(0, -iconSize * 0.5f, 0), iconColor, lineThickness);
        auto* l6 = DrawLine(position + glm::vec3(0, -iconSize * 0.5f, 0),
                            position + glm::vec3(-iconSize, -iconSize * 0.5f, 0), iconColor, lineThickness);
        if (l1)
            SubmitPacket(l1);
        if (l2)
            SubmitPacket(l2);
        if (l3)
            SubmitPacket(l3);
        if (l4)
            SubmitPacket(l4);
        if (l5)
            SubmitPacket(l5);
        if (l6)
            SubmitPacket(l6);

        // Draw min distance sphere (bright, full volume zone)
        if (visualMin > 0.01f)
        {
            for (i32 axis = 0; axis < 3; ++axis)
            {
                for (i32 i = 0; i < segments; ++i)
                {
                    f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                    f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                    glm::vec3 offset1, offset2;
                    if (axis == 0)
                    {
                        offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * visualMin;
                        offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * visualMin;
                    }
                    else if (axis == 1)
                    {
                        offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * visualMin;
                        offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * visualMin;
                    }
                    else
                    {
                        offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * visualMin;
                        offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * visualMin;
                    }

                    auto* seg = DrawLine(position + offset1, position + offset2, color, lineThickness);
                    if (seg)
                        SubmitPacket(seg);
                }
            }
        }

        // Draw max distance sphere (dimmer, falloff boundary)
        if (visualMax > visualMin)
        {
            glm::vec3 dimColor = color * 0.3f;
            for (i32 axis = 0; axis < 3; ++axis)
            {
                for (i32 i = 0; i < segments; ++i)
                {
                    f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                    f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                    glm::vec3 offset1, offset2;
                    if (axis == 0)
                    {
                        offset1 = glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * visualMax;
                        offset2 = glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * visualMax;
                    }
                    else if (axis == 1)
                    {
                        offset1 = glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * visualMax;
                        offset2 = glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * visualMax;
                    }
                    else
                    {
                        offset1 = glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * visualMax;
                        offset2 = glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * visualMax;
                    }

                    auto* seg = DrawLine(position + offset1, position + offset2, dimColor, lineThickness * 0.5f);
                    if (seg)
                        SubmitPacket(seg);
                }
            }
        }
    }

    void Renderer3D::DrawWorldAxisHelper(f32 axisLength)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 2.5f;
        const glm::vec3 origin(0.0f);

        // X axis - Red
        auto* xAxis = DrawLine(origin, glm::vec3(axisLength, 0.0f, 0.0f), glm::vec3(1.0f, 0.2f, 0.2f), lineThickness);
        if (xAxis)
            SubmitPacket(xAxis);

        // X axis arrow head
        auto* xArrow1 = DrawLine(glm::vec3(axisLength, 0, 0), glm::vec3(axisLength - 0.2f, 0.1f, 0), glm::vec3(1.0f, 0.2f, 0.2f), lineThickness);
        auto* xArrow2 = DrawLine(glm::vec3(axisLength, 0, 0), glm::vec3(axisLength - 0.2f, -0.1f, 0), glm::vec3(1.0f, 0.2f, 0.2f), lineThickness);
        if (xArrow1)
            SubmitPacket(xArrow1);
        if (xArrow2)
            SubmitPacket(xArrow2);

        // Y axis - Green
        auto* yAxis = DrawLine(origin, glm::vec3(0.0f, axisLength, 0.0f), glm::vec3(0.2f, 1.0f, 0.2f), lineThickness);
        if (yAxis)
            SubmitPacket(yAxis);

        // Y axis arrow head
        auto* yArrow1 = DrawLine(glm::vec3(0, axisLength, 0), glm::vec3(0.1f, axisLength - 0.2f, 0), glm::vec3(0.2f, 1.0f, 0.2f), lineThickness);
        auto* yArrow2 = DrawLine(glm::vec3(0, axisLength, 0), glm::vec3(-0.1f, axisLength - 0.2f, 0), glm::vec3(0.2f, 1.0f, 0.2f), lineThickness);
        if (yArrow1)
            SubmitPacket(yArrow1);
        if (yArrow2)
            SubmitPacket(yArrow2);

        // Z axis - Blue
        auto* zAxis = DrawLine(origin, glm::vec3(0.0f, 0.0f, axisLength), glm::vec3(0.2f, 0.2f, 1.0f), lineThickness);
        if (zAxis)
            SubmitPacket(zAxis);

        // Z axis arrow head
        auto* zArrow1 = DrawLine(glm::vec3(0, 0, axisLength), glm::vec3(0, 0.1f, axisLength - 0.2f), glm::vec3(0.2f, 0.2f, 1.0f), lineThickness);
        auto* zArrow2 = DrawLine(glm::vec3(0, 0, axisLength), glm::vec3(0, -0.1f, axisLength - 0.2f), glm::vec3(0.2f, 0.2f, 1.0f), lineThickness);
        if (zArrow1)
            SubmitPacket(zArrow1);
        if (zArrow2)
            SubmitPacket(zArrow2);

        // Draw small negative axis indicators (dashed appearance using short segments)
        const f32 negLength = axisLength * 0.3f;
        const f32 dashLen = 0.1f;

        // Negative X (dimmer red)
        for (f32 t = 0; t < negLength; t += dashLen * 2)
        {
            auto* dash = DrawLine(glm::vec3(-t, 0, 0), glm::vec3(-t - dashLen, 0, 0), glm::vec3(0.5f, 0.1f, 0.1f), lineThickness * 0.5f);
            if (dash)
                SubmitPacket(dash);
        }

        // Negative Y (dimmer green)
        for (f32 t = 0; t < negLength; t += dashLen * 2)
        {
            auto* dash = DrawLine(glm::vec3(0, -t, 0), glm::vec3(0, -t - dashLen, 0), glm::vec3(0.1f, 0.5f, 0.1f), lineThickness * 0.5f);
            if (dash)
                SubmitPacket(dash);
        }

        // Negative Z (dimmer blue)
        for (f32 t = 0; t < negLength; t += dashLen * 2)
        {
            auto* dash = DrawLine(glm::vec3(0, 0, -t), glm::vec3(0, 0, -t - dashLen), glm::vec3(0.1f, 0.1f, 0.5f), lineThickness * 0.5f);
            if (dash)
                SubmitPacket(dash);
        }
    }

    void Renderer3D::DrawBoxColliderGizmo(const glm::vec3& position,
                                          const glm::vec3& halfExtents,
                                          const glm::quat& rotation,
                                          const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 2.0f;

        // Create rotation matrix
        glm::mat3 rotMat = glm::mat3_cast(rotation);

        // Local corners of the box
        glm::vec3 corners[8] = {
            glm::vec3(-halfExtents.x, -halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x, -halfExtents.y, -halfExtents.z),
            glm::vec3(halfExtents.x, halfExtents.y, -halfExtents.z),
            glm::vec3(-halfExtents.x, halfExtents.y, -halfExtents.z),
            glm::vec3(-halfExtents.x, -halfExtents.y, halfExtents.z),
            glm::vec3(halfExtents.x, -halfExtents.y, halfExtents.z),
            glm::vec3(halfExtents.x, halfExtents.y, halfExtents.z),
            glm::vec3(-halfExtents.x, halfExtents.y, halfExtents.z)
        };

        // Transform corners to world space
        for (auto& corner : corners)
        {
            corner = position + rotMat * corner;
        }

        // Draw edges - bottom face
        auto* e1 = DrawLine(corners[0], corners[1], color, lineThickness);
        auto* e2 = DrawLine(corners[1], corners[2], color, lineThickness);
        auto* e3 = DrawLine(corners[2], corners[3], color, lineThickness);
        auto* e4 = DrawLine(corners[3], corners[0], color, lineThickness);
        if (e1)
            SubmitPacket(e1);
        if (e2)
            SubmitPacket(e2);
        if (e3)
            SubmitPacket(e3);
        if (e4)
            SubmitPacket(e4);

        // Top face
        auto* e5 = DrawLine(corners[4], corners[5], color, lineThickness);
        auto* e6 = DrawLine(corners[5], corners[6], color, lineThickness);
        auto* e7 = DrawLine(corners[6], corners[7], color, lineThickness);
        auto* e8 = DrawLine(corners[7], corners[4], color, lineThickness);
        if (e5)
            SubmitPacket(e5);
        if (e6)
            SubmitPacket(e6);
        if (e7)
            SubmitPacket(e7);
        if (e8)
            SubmitPacket(e8);

        // Vertical edges
        auto* e9 = DrawLine(corners[0], corners[4], color, lineThickness);
        auto* e10 = DrawLine(corners[1], corners[5], color, lineThickness);
        auto* e11 = DrawLine(corners[2], corners[6], color, lineThickness);
        auto* e12 = DrawLine(corners[3], corners[7], color, lineThickness);
        if (e9)
            SubmitPacket(e9);
        if (e10)
            SubmitPacket(e10);
        if (e11)
            SubmitPacket(e11);
        if (e12)
            SubmitPacket(e12);
    }

    void Renderer3D::DrawSphereColliderGizmo(const glm::vec3& position,
                                             f32 radius,
                                             const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 1.5f;
        const i32 segments = 24;

        // Draw three circles for each axis
        for (i32 axis = 0; axis < 3; ++axis)
        {
            for (i32 i = 0; i < segments; ++i)
            {
                f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
                f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

                glm::vec3 p1, p2;
                if (axis == 0) // XY plane
                {
                    p1 = position + glm::vec3(std::cos(angle1), std::sin(angle1), 0.0f) * radius;
                    p2 = position + glm::vec3(std::cos(angle2), std::sin(angle2), 0.0f) * radius;
                }
                else if (axis == 1) // XZ plane
                {
                    p1 = position + glm::vec3(std::cos(angle1), 0.0f, std::sin(angle1)) * radius;
                    p2 = position + glm::vec3(std::cos(angle2), 0.0f, std::sin(angle2)) * radius;
                }
                else // YZ plane
                {
                    p1 = position + glm::vec3(0.0f, std::cos(angle1), std::sin(angle1)) * radius;
                    p2 = position + glm::vec3(0.0f, std::cos(angle2), std::sin(angle2)) * radius;
                }

                auto* seg = DrawLine(p1, p2, color, lineThickness);
                if (seg)
                    SubmitPacket(seg);
            }
        }
    }

    void Renderer3D::DrawCapsuleColliderGizmo(const glm::vec3& position,
                                              f32 radius,
                                              f32 halfHeight,
                                              const glm::quat& rotation,
                                              const glm::vec3& color)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            return;
        }

        const f32 lineThickness = 1.5f;
        const i32 segments = 16;
        const i32 hemisphereSegments = 8;

        // Create rotation matrix
        glm::mat3 rotMat = glm::mat3_cast(rotation);

        // Local up vector (capsule aligned with Y axis locally)
        glm::vec3 localUp = rotMat * glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 localRight = rotMat * glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 localForward = rotMat * glm::vec3(0.0f, 0.0f, 1.0f);

        glm::vec3 topCenter = position + localUp * halfHeight;
        glm::vec3 bottomCenter = position - localUp * halfHeight;

        // Draw cylindrical body circles at top and bottom
        for (i32 i = 0; i < segments; ++i)
        {
            f32 angle1 = (f32(i) / f32(segments)) * glm::two_pi<f32>();
            f32 angle2 = (f32(i + 1) / f32(segments)) * glm::two_pi<f32>();

            glm::vec3 offset1 = localRight * std::cos(angle1) * radius + localForward * std::sin(angle1) * radius;
            glm::vec3 offset2 = localRight * std::cos(angle2) * radius + localForward * std::sin(angle2) * radius;

            // Top circle
            auto* topSeg = DrawLine(topCenter + offset1, topCenter + offset2, color, lineThickness);
            if (topSeg)
                SubmitPacket(topSeg);

            // Bottom circle
            auto* bottomSeg = DrawLine(bottomCenter + offset1, bottomCenter + offset2, color, lineThickness);
            if (bottomSeg)
                SubmitPacket(bottomSeg);
        }

        // Draw vertical lines connecting top and bottom
        for (i32 i = 0; i < 4; ++i)
        {
            f32 angle = (f32(i) / 4.0f) * glm::two_pi<f32>();
            glm::vec3 offset = localRight * std::cos(angle) * radius + localForward * std::sin(angle) * radius;

            auto* vertLine = DrawLine(topCenter + offset, bottomCenter + offset, color, lineThickness);
            if (vertLine)
                SubmitPacket(vertLine);
        }

        // Draw top hemisphere
        for (i32 i = 0; i < hemisphereSegments; ++i)
        {
            f32 phi1 = (f32(i) / f32(hemisphereSegments)) * glm::half_pi<f32>();
            f32 phi2 = (f32(i + 1) / f32(hemisphereSegments)) * glm::half_pi<f32>();

            f32 r1 = radius * std::cos(phi1);
            f32 r2 = radius * std::cos(phi2);
            f32 y1 = radius * std::sin(phi1);
            f32 y2 = radius * std::sin(phi2);

            // Draw arc segments
            for (i32 j = 0; j < segments; ++j)
            {
                f32 theta = (f32(j) / f32(segments)) * glm::two_pi<f32>();
                glm::vec3 offset1 = localRight * std::cos(theta) * r1 + localForward * std::sin(theta) * r1;
                glm::vec3 offset2 = localRight * std::cos(theta) * r2 + localForward * std::sin(theta) * r2;

                auto* arcSeg = DrawLine(topCenter + offset1 + localUp * y1,
                                        topCenter + offset2 + localUp * y2, color, lineThickness * 0.5f);
                if (arcSeg)
                    SubmitPacket(arcSeg);
            }
        }

        // Draw bottom hemisphere
        for (i32 i = 0; i < hemisphereSegments; ++i)
        {
            f32 phi1 = (f32(i) / f32(hemisphereSegments)) * glm::half_pi<f32>();
            f32 phi2 = (f32(i + 1) / f32(hemisphereSegments)) * glm::half_pi<f32>();

            f32 r1 = radius * std::cos(phi1);
            f32 r2 = radius * std::cos(phi2);
            f32 y1 = radius * std::sin(phi1);
            f32 y2 = radius * std::sin(phi2);

            // Draw arc segments
            for (i32 j = 0; j < segments; ++j)
            {
                f32 theta = (f32(j) / f32(segments)) * glm::two_pi<f32>();
                glm::vec3 offset1 = localRight * std::cos(theta) * r1 + localForward * std::sin(theta) * r1;
                glm::vec3 offset2 = localRight * std::cos(theta) * r2 + localForward * std::sin(theta) * r2;

                auto* arcSeg = DrawLine(bottomCenter + offset1 - localUp * y1,
                                        bottomCenter + offset2 - localUp * y2, color, lineThickness * 0.5f);
                if (arcSeg)
                    SubmitPacket(arcSeg);
            }
        }
    }

    void Renderer3D::DrawSkeleton(const Skeleton& skeleton, const glm::mat4& modelMatrix,
                                  bool showBones, bool showJoints, f32 jointSize, f32 boneThickness)
    {
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkeleton: ScenePass is null!");
            return;
        }

        if (skeleton.m_GlobalTransforms.empty() || skeleton.m_ParentIndices.empty())
        {
            OLO_CORE_WARN("Renderer3D::DrawSkeleton: Empty skeleton data");
            return;
        }

        if (skeleton.m_GlobalTransforms.size() != skeleton.m_ParentIndices.size())
        {
            OLO_CORE_ERROR("Renderer3D::DrawSkeleton: Skeleton transforms and parents size mismatch");
            return;
        }

        // Compute world-space joint positions
        std::vector<glm::vec3> worldPositions(skeleton.m_GlobalTransforms.size());
        for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
        {
            worldPositions[i] = glm::vec3(modelMatrix * skeleton.m_GlobalTransforms[i] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        // Compute average bone length to auto-scale visualization.
        // jointSize and boneThickness are treated as fractions of this extent
        // so the skeleton is visible regardless of the model's unit system.
        f32 totalBoneLength = 0.0f;
        i32 boneCount = 0;
        for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
        {
            i32 parentIndex = skeleton.m_ParentIndices[i];
            if (parentIndex >= 0 && parentIndex < static_cast<i32>(skeleton.m_GlobalTransforms.size()))
            {
                f32 len = glm::length(worldPositions[i] - worldPositions[parentIndex]);
                if (len > 0.001f)
                {
                    totalBoneLength += len;
                    ++boneCount;
                }
            }
        }
        f32 avgBoneLength = (boneCount > 0) ? (totalBoneLength / static_cast<f32>(boneCount)) : 1.0f;

        // Scale factors: jointSize=1.0 means joint radius = 10% of average bone length
        f32 scaledJointRadius = jointSize * avgBoneLength * 0.1f;
        // DrawLine internally multiplies thickness by 0.005, so divide to compensate
        f32 scaledBoneThickness = (boneThickness * avgBoneLength * 0.02f) / 0.005f;

        // Colors for visualization
        const glm::vec3 boneColor(1.0f, 0.5f, 0.0f);  // Bright orange for bones
        const glm::vec3 jointColor(0.0f, 1.0f, 0.0f); // Bright green for joints

        // Draw joints
        if (showJoints)
        {
            for (sizet i = 0; i < worldPositions.size(); ++i)
            {
                auto* spherePacket = DrawSphere(worldPositions[i], scaledJointRadius, jointColor);
                if (spherePacket)
                {
                    SubmitPacket(spherePacket);
                }
            }
        }

        // Draw bones (connections between joints)
        if (showBones)
        {
            for (sizet i = 0; i < skeleton.m_GlobalTransforms.size(); ++i)
            {
                i32 parentIndex = skeleton.m_ParentIndices[i];
                if (parentIndex >= 0 && parentIndex < static_cast<i32>(skeleton.m_GlobalTransforms.size()))
                {
                    f32 boneLength = glm::length(worldPositions[i] - worldPositions[parentIndex]);
                    if (boneLength > 0.001f)
                    {
                        auto* linePacket = DrawLine(worldPositions[parentIndex], worldPositions[i], boneColor, scaledBoneThickness);
                        if (linePacket)
                        {
                            SubmitPacket(linePacket);
                        }
                    }
                }
            }
        }
    }
} // namespace OloEngine
