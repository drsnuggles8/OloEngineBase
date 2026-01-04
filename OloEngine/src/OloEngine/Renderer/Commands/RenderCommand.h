#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <type_traits>

/*
 * POD Render Commands
 *
 * All commands in this file are designed to be POD (Plain Old Data) to enable:
 * - Fast radix sorting by 64-bit DrawKey
 * - Efficient memcpy-based command buffer operations
 * - Cache-friendly linear memory layout
 *
 * Design principles:
 * - Use AssetHandle (u64) instead of Ref<T> for asset references
 * - Use RendererID (u32) for GPU resource identifiers (VAO, textures, etc.)
 * - Use offset+count into FrameDataBuffer for variable-length data (bone matrices, transforms)
 * - Inline render state as POD flags instead of Ref<RenderState>
 *
 * Asset resolution happens at dispatch time in CommandDispatch.cpp via AssetManager::GetAsset<T>()
 */

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;

    // Type aliases for POD command fields
    using AssetHandle = UUID; // u64 asset identifier
    using RendererID = u32;   // OpenGL resource ID

    // Inlined POD render state for commands (replaces Ref<RenderState>)
    struct PODRenderState
    {
        // Blend state
        bool blendEnabled = false;
        GLenum blendSrcFactor = GL_SRC_ALPHA;
        GLenum blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
        GLenum blendEquation = GL_FUNC_ADD;

        // Depth state
        bool depthTestEnabled = true;
        bool depthWriteMask = true;
        GLenum depthFunction = GL_LESS;

        // Stencil state
        bool stencilEnabled = false;
        GLenum stencilFunction = GL_ALWAYS;
        GLint stencilReference = 0;
        GLuint stencilReadMask = 0xFF;
        GLuint stencilWriteMask = 0xFF;
        GLenum stencilFail = GL_KEEP;
        GLenum stencilDepthFail = GL_KEEP;
        GLenum stencilDepthPass = GL_KEEP;

        // Culling state
        bool cullingEnabled = false;
        GLenum cullFace = GL_BACK;

        // Polygon mode
        GLenum polygonFace = GL_FRONT_AND_BACK;
        GLenum polygonMode = GL_FILL;

        // Polygon offset
        bool polygonOffsetEnabled = false;
        f32 polygonOffsetFactor = 0.0f;
        f32 polygonOffsetUnits = 0.0f;

        // Scissor
        bool scissorEnabled = false;
        GLint scissorX = 0;
        GLint scissorY = 0;
        GLsizei scissorWidth = 0;
        GLsizei scissorHeight = 0;

        // Color mask
        bool colorMaskR = true;
        bool colorMaskG = true;
        bool colorMaskB = true;
        bool colorMaskA = true;

        // Multisampling
        bool multisamplingEnabled = true;

        // Line width
        f32 lineWidth = 1.0f;
    };

    // Static assertion to ensure PODRenderState is trivially copyable
    static_assert(std::is_trivially_copyable_v<PODRenderState>, "PODRenderState must be trivially copyable");

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
        RendererID vertexArrayID; // VAO renderer ID
        u32 indexCount;
        GLenum indexType;
    };

    struct DrawIndexedInstancedCommand
    {
        CommandHeader header;
        RendererID vertexArrayID; // VAO renderer ID
        u32 indexCount;
        u32 instanceCount;
        GLenum indexType;
    };

    struct DrawArraysCommand
    {
        CommandHeader header;
        RendererID vertexArrayID; // VAO renderer ID
        u32 vertexCount;
        GLenum primitiveType;
    };

    struct DrawLinesCommand
    {
        CommandHeader header;
        RendererID vertexArrayID; // VAO renderer ID
        u32 vertexCount;
    };

    // Higher-level commands combine multiple lower-level commands
    // All use POD types for radix sort compatibility
    struct DrawMeshCommand
    {
        CommandHeader header;

        // Mesh data (POD identifiers)
        AssetHandle meshHandle;   // Mesh asset handle for resolution
        RendererID vertexArrayID; // VAO renderer ID
        u32 indexCount;
        glm::mat4 transform;

        // Shader (store both handle and renderer ID for POD dispatch)
        AssetHandle shaderHandle;    // Shader asset handle (for asset tracking)
        RendererID shaderRendererID; // Shader program ID for glUseProgram

        // Legacy material properties (POD)
        glm::vec3 ambient;
        glm::vec3 diffuse;
        glm::vec3 specular;
        f32 shininess;
        bool useTextureMaps;
        RendererID diffuseMapID;  // Texture renderer ID (0 = none)
        RendererID specularMapID; // Texture renderer ID (0 = none)

        // PBR material properties (POD)
        bool enablePBR = false;
        glm::vec4 baseColorFactor = glm::vec4(1.0f);
        glm::vec4 emissiveFactor = glm::vec4(0.0f);
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        f32 normalScale = 1.0f;
        f32 occlusionStrength = 1.0f;
        bool enableIBL = false;

        // PBR texture IDs (renderer IDs, 0 = none)
        RendererID albedoMapID = 0;
        RendererID metallicRoughnessMapID = 0;
        RendererID normalMapID = 0;
        RendererID aoMapID = 0;
        RendererID emissiveMapID = 0;
        RendererID environmentMapID = 0;
        RendererID irradianceMapID = 0;
        RendererID prefilterMapID = 0;
        RendererID brdfLutMapID = 0;

        // Inlined render state (POD)
        PODRenderState renderState;

        // Animation support
        bool isAnimatedMesh = false;
        u32 boneBufferOffset = 0;          // Offset into FrameDataBuffer for bone matrices
        u32 boneCount = 0;                 // Number of bone matrices
        u8 workerIndex = 0;                // Worker index for parallel submission (used to remap local bone offset to global)
        bool needsBoneOffsetRemap = false; // True if boneBufferOffset is worker-local and needs remapping
    };

    // Static assertion to verify DrawMeshCommand is trivially copyable (POD)
    static_assert(std::is_trivially_copyable_v<DrawMeshCommand>, "DrawMeshCommand must be trivially copyable for radix sort");

    struct DrawMeshInstancedCommand
    {
        CommandHeader header;

        // Mesh data (POD identifiers)
        AssetHandle meshHandle;   // Mesh asset handle
        RendererID vertexArrayID; // VAO renderer ID
        u32 indexCount;
        u32 instanceCount;
        u32 transformBufferOffset = 0; // Offset into FrameDataBuffer for instance transforms
        u32 transformCount = 0;        // Number of instance transforms

        // Shader (store both handle and renderer ID for POD dispatch)
        AssetHandle shaderHandle;    // Shader asset handle (for asset tracking)
        RendererID shaderRendererID; // Shader program ID for glUseProgram

        // Legacy material properties (POD)
        glm::vec3 ambient;
        glm::vec3 diffuse;
        glm::vec3 specular;
        f32 shininess;
        bool useTextureMaps;
        RendererID diffuseMapID = 0;
        RendererID specularMapID = 0;

        // PBR material properties (POD)
        bool enablePBR = false;
        glm::vec4 baseColorFactor = glm::vec4(1.0f);
        glm::vec4 emissiveFactor = glm::vec4(0.0f);
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        f32 normalScale = 1.0f;
        f32 occlusionStrength = 1.0f;
        bool enableIBL = false;

        // PBR texture IDs
        RendererID albedoMapID = 0;
        RendererID metallicRoughnessMapID = 0;
        RendererID normalMapID = 0;
        RendererID aoMapID = 0;
        RendererID emissiveMapID = 0;
        RendererID environmentMapID = 0;
        RendererID irradianceMapID = 0;
        RendererID prefilterMapID = 0;
        RendererID brdfLutMapID = 0;

        // Inlined render state (POD)
        PODRenderState renderState;

        // Animation support for instanced animated meshes
        bool isAnimatedMesh = false;
        u32 boneBufferOffset = 0;     // Offset into FrameDataBuffer for all instance bone matrices
        u32 boneCountPerInstance = 0; // Number of bones per instance
    };

    // Static assertion to verify DrawMeshInstancedCommand is trivially copyable
    static_assert(std::is_trivially_copyable_v<DrawMeshInstancedCommand>, "DrawMeshInstancedCommand must be trivially copyable for radix sort");

    struct DrawSkyboxCommand
    {
        CommandHeader header;
        AssetHandle meshHandle;   // Skybox mesh handle
        RendererID vertexArrayID; // VAO renderer ID
        u32 indexCount;
        glm::mat4 transform;         // Usually identity matrix
        AssetHandle shaderHandle;    // Skybox shader handle (for asset tracking)
        RendererID shaderRendererID; // Shader program ID for glUseProgram
        RendererID skyboxTextureID;  // Cubemap texture renderer ID
        PODRenderState renderState;  // Inlined render state
    };

    // Static assertion for DrawSkyboxCommand
    static_assert(std::is_trivially_copyable_v<DrawSkyboxCommand>, "DrawSkyboxCommand must be trivially copyable for radix sort");

    struct DrawQuadCommand
    {
        CommandHeader header;
        glm::mat4 transform;
        RendererID textureID;        // Texture renderer ID
        AssetHandle shaderHandle;    // Shader asset handle (for asset tracking)
        RendererID shaderRendererID; // Shader program ID for glUseProgram
        RendererID quadVAID;         // Quad vertex array renderer ID
        PODRenderState renderState;  // Inlined render state
    };

    // Static assertion for DrawQuadCommand
    static_assert(std::is_trivially_copyable_v<DrawQuadCommand>, "DrawQuadCommand must be trivially copyable for radix sort");

    // Maximum command size for allocation purposes - increased for PBR and bone matrices
    constexpr sizet MAX_COMMAND_SIZE = 1024;
} // namespace OloEngine
