#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/RenderState.h"
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
        DrawQuad,
        BindDefaultFramebuffer,
        BindTexture,
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
    };    // Higher-level commands combine multiple lower-level commands
	struct DrawMeshCommand
	{
		CommandHeader header;
		Ref<Mesh> mesh;              // Store the actual mesh reference
		Ref<VertexArray> vertexArray; // Store the actual vertex array
		u32 indexCount;
		glm::mat4 transform;
		// Material properties
		glm::vec3 ambient;
		glm::vec3 diffuse;
		glm::vec3 specular;
		f32 shininess;
		bool useTextureMaps;
		// Actual texture references instead of IDs
		Ref<Texture2D> diffuseMap;
		Ref<Texture2D> specularMap;
		// Actual shader instead of ID
		Ref<Shader> shader;
		// Per-draw-call render state
		Ref<RenderState> renderState;
		// Skinning support for animated meshes
		bool isSkinnedMesh = false;
		std::vector<glm::mat4> boneMatrices;  // Final bone matrices for GPU skinning
	};
	struct DrawMeshInstancedCommand
	{
		CommandHeader header;
		Ref<Mesh> mesh;              // Store the actual mesh reference
		Ref<VertexArray> vertexArray; // Store the actual vertex array
		u32 indexCount;
		u32 instanceCount;
		std::vector<glm::mat4> transforms; // Store the actual transform data
		// Material properties (same for all instances)
		glm::vec3 ambient;
		glm::vec3 diffuse;
		glm::vec3 specular;
		f32 shininess;
		bool useTextureMaps;
		// Actual texture references instead of IDs
		Ref<Texture2D> diffuseMap;
		Ref<Texture2D> specularMap;
		// Actual shader instead of ID
		Ref<Shader> shader;
		// Per-draw-call render state
		Ref<RenderState> renderState;
		// Skinning support for animated meshes
		bool isSkinnedMesh = false;
		// For instanced skinned meshes, each instance has its own set of bone matrices
		std::vector<std::vector<glm::mat4>> instanceBoneMatrices;
	};

	struct DrawQuadCommand
	{
		CommandHeader header;
		glm::mat4 transform;
		Ref<Texture2D> texture;   // Store the actual texture
		Ref<Shader> shader;       // Store the actual shader
		Ref<VertexArray> quadVA;  // Store the quad vertex array
		// Per-draw-call render state
		Ref<RenderState> renderState;
	};

	struct DrawSkinnedMeshCommand
	{
		CommandHeader header;
		Ref<VertexArray> vertexArray;
		u32 indexCount;
		glm::mat4 modelMatrix;
		// Material properties
		glm::vec3 ambient;
		glm::vec3 diffuse;
		glm::vec3 specular;
		f32 shininess;
		bool useTextureMaps;
		// Actual texture references
		Ref<Texture2D> diffuseMap;
		Ref<Texture2D> specularMap;
		// Actual shader for skinned rendering
		Ref<Shader> shader;
		// Per-draw-call render state
		Ref<RenderState> renderState;
		// Bone matrices for GPU skinning (up to 100 bones)
		std::vector<glm::mat4> boneMatrices;
	};    // Maximum command size for allocation purposes - increased for bone matrices
    constexpr sizet MAX_COMMAND_SIZE = 512;
}
