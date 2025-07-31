#pragma once

#include "OloEngine/Core/Base.h"
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

namespace OloEngine
{
    // Forward declarations
    class RendererAPI;    // Command type enum for dispatching
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
        DrawSkinnedMesh,
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
        const char* resourceName;  // Changed from std::string to const char* for POD compliance
        ShaderResourceInput resourceInput;
    };

    struct DrawIndexedCommand
    {
        CommandHeader header;
        AssetRef<VertexArray> vertexArray;
        u32 indexCount;
        GLenum indexType;
    };

    struct DrawIndexedInstancedCommand
    {
        CommandHeader header;
        AssetRef<VertexArray> vertexArray; // Changed from rendererID to vertexArray
        u32 indexCount;
        u32 instanceCount;
        GLenum indexType;
    };

    struct DrawArraysCommand
    {
        CommandHeader header;
        AssetRef<VertexArray> vertexArray; // Changed from rendererID to vertexArray
        u32 vertexCount;
        GLenum primitiveType;
    };

    struct DrawLinesCommand
    {
        CommandHeader header;
        AssetRef<VertexArray> vertexArray; // Changed from rendererID to vertexArray
        u32 vertexCount;
    };    // Higher-level commands combine multiple lower-level commands
	struct DrawMeshCommand
	{
		CommandHeader header;
		AssetRef<Mesh> mesh;              // Store the actual mesh reference
		AssetRef<VertexArray> vertexArray; // Store the actual vertex array
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
		// Skinning support for animated meshes
		bool isSkinnedMesh = false;
		u32 boneMatricesBufferID = 0;  // ID to external bone matrices buffer instead of vector
	};
	struct DrawMeshInstancedCommand
	{
		CommandHeader header;
		AssetRef<Mesh> mesh;              // Store the actual mesh reference
		AssetRef<VertexArray> vertexArray; // Store the actual vertex array
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
		// Skinning support for animated meshes
		bool isSkinnedMesh = false;
		// For instanced skinned meshes, each instance has its own set of bone matrices
		std::vector<std::vector<glm::mat4>> instanceBoneMatrices;
	};

	struct DrawSkyboxCommand
	{
		CommandHeader header;
		Ref<Mesh> mesh;              // Skybox mesh (special cube)
		AssetRef<VertexArray> vertexArray; // Store the actual vertex array
		u32 indexCount;
		glm::mat4 transform;         // Usually identity matrix
		Ref<Shader> shader;          // Skybox shader
		Ref<TextureCubemap> skyboxTexture; // The skybox cubemap texture
		Ref<RenderState> renderState; // Skybox-specific render state
	};

	struct DrawQuadCommand
	{
		CommandHeader header;
		glm::mat4 transform;
		Ref<Texture2D> texture;   // Store the actual texture
		Ref<Shader> shader;       // Store the actual shader
		AssetRef<VertexArray> quadVA;  // Store the quad vertex array
		// Per-draw-call render state
		Ref<RenderState> renderState;
	};

	struct DrawSkinnedMeshCommand
	{
		CommandHeader header;
		AssetRef<VertexArray> vertexArray;
		u32 indexCount;
		glm::mat4 modelMatrix;
		
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
		
		// Actual shader for skinned rendering
		Ref<Shader> shader;
		// Per-draw-call render state
		Ref<RenderState> renderState;
		// Bone matrices for GPU skinning (up to 100 bones)
		std::vector<glm::mat4> boneMatrices;
	};    // Maximum command size for allocation purposes - increased for PBR and bone matrices
    constexpr sizet MAX_COMMAND_SIZE = 1024;
}
