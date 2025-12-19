#pragma once

#include <vector>
#include <span>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/RenderState.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include <glad/gl.h>
#include <glm/glm.hpp>

/*
 * TODO: Asset Management System Integration
 *
 * This file was converted from Ref<T>-based commands to ID-based commands for POD compliance,
 * but the implementation is incomplete pending a proper asset management system.
 *
 * REQUIRED CHANGES once Asset Management System is implemented:
 *
 * 1. COMMAND CREATION (Renderer3D.cpp, Renderer2D.cpp, etc.):
 *    - Update all code that creates DrawMeshCommand, DrawMeshInstancedCommand, etc.
 *    - Convert from storing Ref<T> objects to storing their asset handles/IDs
 *    - Example: cmd->shaderID = shader->GetAssetHandle(); instead of cmd->shader = shader;
 *
 * 2. COMMAND DISPATCH (CommandDispatch.cpp):
 *    - Add asset resolution during dispatch phase
 *    - Example: auto shader = AssetManager::GetAsset<Shader>(cmd->shaderID);
 *    - Update all dispatch functions to resolve IDs back to objects
 *    - Ensure proper error handling for missing/invalid assets
 *
 * 3. ASSET LIFETIME MANAGEMENT:
 *    - Ensure assets remain alive from command creation until dispatch
 *    - Consider implementing asset reference counting at the renderer level
 *    - Handle cases where assets are deleted while commands are queued
 *
 * 4. BUFFER MANAGEMENT for non-POD data:
 *    - Implement external buffer system for transforms (transformsBufferID)
 *    - Implement external buffer system for bone matrices (boneMatricesBufferID)
 *    - These should be managed by CommandAllocator or separate buffer pools
 *
 * 5. PERFORMANCE CONSIDERATIONS:
 *    - Asset lookup during dispatch should be fast (hash maps, etc.)
 *    - Consider caching resolved assets to avoid repeated lookups
 *    - Batch asset resolution where possible
 *
 * CURRENT STATE: POD-compliant command structures but incomplete dispatch system
 * NEXT STEPS: Complete asset management system, then revisit this implementation
 */

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;
    // Command type enum for dispatching
    enum class CommandType : u8
    {
        Invalid = 0,
        Clear,
        ClearStencil,
        DrawArrays,
        DrawIndexed,
        DrawIndexedInstanced,
        DrawLines,
        DrawMesh,
        DrawMeshInstanced,
        DrawSkybox,
        DrawQuad,
        BindDefaultFramebuffer,
        BindTexture,
        SetShaderResource,
        SetViewport,
        SetClearColor,
        SetBlendState,
        SetBlendFunc,
        SetBlendEquation,
        SetDepthTest,
        SetDepthMask,
        SetDepthFunc,
        SetStencilTest,
        SetStencilFunc,
        SetStencilMask,
        SetStencilOp,
        SetCulling,
        SetCullFace,
        SetLineWidth,
        SetPolygonMode,
        SetPolygonOffset,
        SetScissorTest,
        SetScissorBox,
        SetColorMask,
        SetMultisampling
    };

    // Function pointer type for command dispatch
    using CommandDispatchFn = void (*)(const void* data, RendererAPI& api);

    // Base command header - included in every command
    struct CommandHeader
    {
        CommandType type = CommandType::Invalid;
        CommandDispatchFn dispatchFn = nullptr;
    };

    /*
     * Render state commands - POD structures
     */
    struct SetViewportCommand
    {
        CommandHeader header;
        u32 x;
        u32 y;
        u32 width;
        u32 height;
    };

    struct SetClearColorCommand
    {
        CommandHeader header;
        glm::vec4 color;
    };

    struct ClearCommand
    {
        CommandHeader header;
        bool clearColor;
        bool clearDepth;
    };

    struct ClearStencilCommand
    {
        CommandHeader header;
    };

    struct SetBlendStateCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetBlendFuncCommand
    {
        CommandHeader header;
        GLenum sourceFactor;
        GLenum destFactor;
    };

    struct SetBlendEquationCommand
    {
        CommandHeader header;
        GLenum mode;
    };

    struct SetDepthTestCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetDepthMaskCommand
    {
        CommandHeader header;
        bool writeMask;
    };

    struct SetDepthFuncCommand
    {
        CommandHeader header;
        GLenum function;
    };

    struct SetStencilTestCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetStencilFuncCommand
    {
        CommandHeader header;
        GLenum function;
        GLint reference;
        GLuint mask;
    };

    struct SetStencilMaskCommand
    {
        CommandHeader header;
        GLuint mask;
    };

    struct SetStencilOpCommand
    {
        CommandHeader header;
        GLenum stencilFail;
        GLenum depthFail;
        GLenum depthPass;
    };

    struct SetCullingCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetCullFaceCommand
    {
        CommandHeader header;
        GLenum face;
    };

    struct SetLineWidthCommand
    {
        CommandHeader header;
        f32 width;
    };

    struct SetPolygonModeCommand
    {
        CommandHeader header;
        GLenum face;
        GLenum mode;
    };

    struct SetPolygonOffsetCommand
    {
        CommandHeader header;
        f32 factor;
        f32 units;
        bool enabled;
    };

    struct SetScissorTestCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetScissorBoxCommand
    {
        CommandHeader header;
        GLint x;
        GLint y;
        GLsizei width;
        GLsizei height;
    };

    struct SetColorMaskCommand
    {
        CommandHeader header;
        bool red;
        bool green;
        bool blue;
        bool alpha;
    };

    struct SetMultisamplingCommand
    {
        CommandHeader header;
        bool enabled;
    };

    /*
     * Draw commands - POD structures
     */
    struct BindDefaultFramebufferCommand
    {
        CommandHeader header;
    };

    struct BindTextureCommand
    {
        CommandHeader header;
        u32 slot;
        u32 textureID;
    };

    struct SetShaderResourceCommand
    {
        CommandHeader header;
        u32 shaderID;
        const char* resourceName; // Changed from std::string to const char* for POD compliance
        ShaderResourceInput resourceInput;
    };

    struct DrawIndexedCommand
    {
        CommandHeader header;
        Ref<VertexArray> vertexArray;
        u32 indexCount;
        GLenum indexType;
    };

    struct DrawIndexedInstancedCommand
    {
        CommandHeader header;
        Ref<VertexArray> vertexArray; // Changed from rendererID to vertexArray
        u32 indexCount;
        u32 instanceCount;
        GLenum indexType;
    };

    struct DrawArraysCommand
    {
        CommandHeader header;
        Ref<VertexArray> vertexArray; // Changed from rendererID to vertexArray
        u32 vertexCount;
        GLenum primitiveType;
    };

    struct DrawLinesCommand
    {
        CommandHeader header;
        Ref<VertexArray> vertexArray; // Changed from rendererID to vertexArray
        u32 vertexCount;
    };

    // Higher-level commands combine multiple lower-level commands
    struct DrawMeshCommand
    {
        CommandHeader header;
        Ref<Mesh> mesh;               // Store the actual mesh reference
        Ref<VertexArray> vertexArray; // Store the actual vertex array
        u32 indexCount;
        glm::mat4 transform;

        // Legacy material properties (for backward compatibility)
        glm::vec3 ambient;
        glm::vec3 diffuse;
        glm::vec3 specular;
        f32 shininess;
        bool useTextureMaps;
        // Legacy texture references
        Ref<Texture2D> diffuseMap;
        Ref<Texture2D> specularMap;

        // PBR material properties
        bool enablePBR = false;
        glm::vec4 baseColorFactor = glm::vec4(1.0f);
        glm::vec4 emissiveFactor = glm::vec4(0.0f);
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        f32 normalScale = 1.0f;
        f32 occlusionStrength = 1.0f;
        bool enableIBL = false;

        // PBR texture references
        Ref<Texture2D> albedoMap;
        Ref<Texture2D> metallicRoughnessMap;
        Ref<Texture2D> normalMap;
        Ref<Texture2D> aoMap;
        Ref<Texture2D> emissiveMap;
        Ref<TextureCubemap> environmentMap;
        Ref<TextureCubemap> irradianceMap;
        Ref<TextureCubemap> prefilterMap;
        Ref<Texture2D> brdfLutMap;

        // Actual shader instead of ID
        Ref<Shader> shader;
        // Per-draw-call render state
        Ref<RenderState> renderState;
        // Animation support for animated meshes
        bool isAnimatedMesh = false;
        // TODO: Replace with pooled buffer reference to reduce command size and heap allocations
        // Bone matrices reference for GPU animation - avoid copying large data
        // WARNING: The referenced bone matrix storage MUST outlive this command instance!
        // The caller is responsible for ensuring the bone matrices remain valid until
        // after GPU consumption during command dispatch.
        std::span<const glm::mat4> boneMatrices; // Reference to external bone matrices data
    };

    struct DrawMeshInstancedCommand
    {
        CommandHeader header;
        Ref<Mesh> mesh;               // Store the actual mesh reference
        Ref<VertexArray> vertexArray; // Store the actual vertex array
        u32 indexCount;
        u32 instanceCount;
        std::vector<glm::mat4> transforms; // Store the actual transform data

        // Legacy material properties (same for all instances)
        glm::vec3 ambient;
        glm::vec3 diffuse;
        glm::vec3 specular;
        f32 shininess;
        bool useTextureMaps;
        // Legacy texture references
        Ref<Texture2D> diffuseMap;
        Ref<Texture2D> specularMap;

        // PBR material properties
        bool enablePBR = false;
        glm::vec4 baseColorFactor = glm::vec4(1.0f);
        glm::vec4 emissiveFactor = glm::vec4(0.0f);
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        f32 normalScale = 1.0f;
        f32 occlusionStrength = 1.0f;
        bool enableIBL = false;

        // PBR texture references
        Ref<Texture2D> albedoMap;
        Ref<Texture2D> metallicRoughnessMap;
        Ref<Texture2D> normalMap;
        Ref<Texture2D> aoMap;
        Ref<Texture2D> emissiveMap;
        Ref<TextureCubemap> environmentMap;
        Ref<TextureCubemap> irradianceMap;
        Ref<TextureCubemap> prefilterMap;
        Ref<Texture2D> brdfLutMap;

        // Actual shader instead of ID
        Ref<Shader> shader;
        // Per-draw-call render state
        Ref<RenderState> renderState;
        // Animation support for animated meshes
        bool isAnimatedMesh = false;
        // TODO: Replace nested vector with flat contiguous buffer or SSBO for better memory efficiency
        // Consider single flat buffer with instance offsets instead of vector<vector<glm::mat4>>
        std::vector<std::vector<glm::mat4>> instanceBoneMatrices;
    };

    struct DrawSkyboxCommand
    {
        CommandHeader header;
        Ref<Mesh> mesh;               // Skybox mesh (special cube)
        Ref<VertexArray> vertexArray; // Store the actual vertex array
        u32 indexCount;
        glm::mat4 transform;               // Usually identity matrix
        Ref<Shader> shader;                // Skybox shader
        Ref<TextureCubemap> skyboxTexture; // The skybox cubemap texture
        Ref<RenderState> renderState;      // Skybox-specific render state
    };

    struct DrawQuadCommand
    {
        CommandHeader header;
        glm::mat4 transform;
        Ref<Texture2D> texture;  // Store the actual texture
        Ref<Shader> shader;      // Store the actual shader
        Ref<VertexArray> quadVA; // Store the quad vertex array
        // Per-draw-call render state
        Ref<RenderState> renderState;
    };

    // Maximum command size for allocation purposes - increased for PBR and bone matrices
    constexpr sizet MAX_COMMAND_SIZE = 1024;
} // namespace OloEngine
