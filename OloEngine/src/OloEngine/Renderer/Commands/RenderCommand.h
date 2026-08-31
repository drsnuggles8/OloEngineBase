#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/TerrainVTBindings.h"
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
 * - Use RHI::ResourceHandle for GPU resource identities (VAO, textures, etc.)
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
    // `using RendererID = u32` lived here and is GONE (issue #691,
    // slice 6). Every GPU-object field below is an RHI::ResourceHandle now:
    // the command layer's redundant-bind cache keys on these values, and a
    // driver name cannot key it safely — GL reissues names, so a deleted
    // object and a newly created one could compare equal and the cache would
    // skip a real bind. See CommandDispatch's InvalidateTextureSlot comment
    // for the visual bug that actually shipped from exactly that.

    // Sentinel value for uninitialized render state index
    static constexpr u16 INVALID_RENDER_STATE_INDEX = UINT16_MAX;

    // Sentinel value for uninitialized material data index
    static constexpr u16 INVALID_MATERIAL_DATA_INDEX = UINT16_MAX;

    // ---- Per-attachment CHANNEL masks (issue #853) -------------------------
    //
    // One nibble per colour attachment, packed into a u32: attachment N owns
    // bits 4N..4N+3, low bit = red, then green, blue, alpha. Eight attachments
    // fit exactly, which is also the width of the attachment-level
    // `PODRenderState::colorAttachmentWriteMask` this refines.
    static constexpr u32 COLOR_CHANNEL_MASK_ALL = 0xFFFFFFFFu;
    static constexpr u32 MAX_MASKED_COLOR_ATTACHMENTS = 8u;

    [[nodiscard]] constexpr u8 MakeColorChannelMask(bool red, bool green, bool blue, bool alpha) noexcept
    {
        return static_cast<u8>((red ? 0x1u : 0x0u) | (green ? 0x2u : 0x0u) | (blue ? 0x4u : 0x0u) |
                               (alpha ? 0x8u : 0x0u));
    }

    [[nodiscard]] constexpr u8 GetColorChannelMask(u32 packed, u32 attachment) noexcept
    {
        return static_cast<u8>((packed >> (attachment * 4u)) & 0xFu);
    }

    [[nodiscard]] constexpr u32 WithColorChannelMask(u32 packed, u32 attachment, u8 channels) noexcept
    {
        const u32 shift = attachment * 4u;
        return (packed & ~(0xFu << shift)) | ((static_cast<u32>(channels) & 0xFu) << shift);
    }

    // Inlined POD render state for commands (replaces Ref<RenderState>)
    struct PODRenderState
    {
        // Blend state
        bool blendEnabled = false;
        RHI::BlendFactor blendSrcFactor = RHI::BlendFactor::SrcAlpha;
        RHI::BlendFactor blendDstFactor = RHI::BlendFactor::OneMinusSrcAlpha;
        RHI::BlendOp blendEquation = RHI::BlendOp::Add;

        // Depth state
        bool depthTestEnabled = true;
        bool depthWriteMask = true;
        RHI::CompareOp depthFunction = RHI::CompareOp::Less;

        // Stencil state
        bool stencilEnabled = false;
        RHI::CompareOp stencilFunction = RHI::CompareOp::Always;
        i32 stencilReference = 0;
        u32 stencilReadMask = 0xFF;
        u32 stencilWriteMask = 0xFF;
        RHI::StencilOp stencilFail = RHI::StencilOp::Keep;
        RHI::StencilOp stencilDepthFail = RHI::StencilOp::Keep;
        RHI::StencilOp stencilDepthPass = RHI::StencilOp::Keep;

        // Culling state
        bool cullingEnabled = false;
        RHI::CullMode cullFace = RHI::CullMode::Back;

        // Polygon mode. No face member: core-profile glPolygonMode accepts only
        // GL_FRONT_AND_BACK, and Vulkan's polygonMode has no face either.
        RHI::PolygonMode polygonMode = RHI::PolygonMode::Fill;

        // Polygon offset
        bool polygonOffsetEnabled = false;
        f32 polygonOffsetFactor = 0.0f;
        f32 polygonOffsetUnits = 0.0f;

        // Scissor
        bool scissorEnabled = false;
        i32 scissorX = 0;
        i32 scissorY = 0;
        u32 scissorWidth = 0;
        u32 scissorHeight = 0;

        // Color mask
        bool colorMaskR = true;
        bool colorMaskG = true;
        bool colorMaskB = true;
        bool colorMaskA = true;

        // Per-attachment color write mask (bit N = attachment N writable, default: all enabled)
        u8 colorAttachmentWriteMask = 0xFF;

        // Per-attachment CHANNEL write mask -- nibble N holds attachment N's
        // R,G,B,A enables (see MakeColorChannelMask above). Default: every
        // channel of every attachment writable.
        //
        // Why this exists (issue #853): `colorAttachmentWriteMask` is one BIT
        // per attachment, so it can say "do not write RT1" but never "write
        // only RT1.xy". A pass that installs channel-level masks with
        // SetColorMaskForAttachment cannot make them survive the next draw:
        // ApplyPODRenderState issues the GLOBAL SetColorMask, which is defined
        // as the indexed call for EVERY draw buffer
        // (docs/agent-rules/gl-global-setter-resets-indexed-state.md), and the
        // narrowing loop that follows could only re-DISABLE whole attachments.
        // DecalRenderPass's decal mode matrix was flattened exactly that way,
        // on both backends, before every decal draw.
        //
        // Carrying the refinement on the COMMAND -- rather than as a
        // pass-scoped "these masks are mine for the next N draws" override --
        // keeps the queue stateless and replay-safe, the same reason
        // DrawDecalCommand carries its OIT program override instead of reading
        // a global. It also cannot leak: a pass-scoped override whose owner
        // forgets to clear it is precisely the process-permanent indexed-state
        // leak issue #823 was about.
        //
        // Composition rule, applied in ApplyPODRenderState: a channel is
        // written iff the global colorMask* allows it AND this nibble allows
        // it AND colorAttachmentWriteMask names the attachment. AND, never
        // widen -- which makes the old attachment-level-only behaviour a
        // strict special case (nibble 0xF everywhere).
        u32 colorAttachmentChannelMask = COLOR_CHANNEL_MASK_ALL;

        // Multisampling
        bool multisamplingEnabled = true;

        // Line width
        f32 lineWidth = 1.0f;

        // Field-wise equality (safe against struct padding, unlike memcmp)
        bool operator==(const PODRenderState& o) const
        {
            return blendEnabled == o.blendEnabled && blendSrcFactor == o.blendSrcFactor && blendDstFactor == o.blendDstFactor && blendEquation == o.blendEquation && depthTestEnabled == o.depthTestEnabled && depthWriteMask == o.depthWriteMask && depthFunction == o.depthFunction && stencilEnabled == o.stencilEnabled && stencilFunction == o.stencilFunction && stencilReference == o.stencilReference && stencilReadMask == o.stencilReadMask && stencilWriteMask == o.stencilWriteMask && stencilFail == o.stencilFail && stencilDepthFail == o.stencilDepthFail && stencilDepthPass == o.stencilDepthPass && cullingEnabled == o.cullingEnabled && cullFace == o.cullFace && polygonMode == o.polygonMode && polygonOffsetEnabled == o.polygonOffsetEnabled && polygonOffsetFactor == o.polygonOffsetFactor && polygonOffsetUnits == o.polygonOffsetUnits && scissorEnabled == o.scissorEnabled && scissorX == o.scissorX && scissorY == o.scissorY && scissorWidth == o.scissorWidth && scissorHeight == o.scissorHeight && colorMaskR == o.colorMaskR && colorMaskG == o.colorMaskG && colorMaskB == o.colorMaskB && colorMaskA == o.colorMaskA && colorAttachmentWriteMask == o.colorAttachmentWriteMask && colorAttachmentChannelMask == o.colorAttachmentChannelMask && multisamplingEnabled == o.multisamplingEnabled && lineWidth == o.lineWidth;
        }
    };

    // Static assertion to ensure PODRenderState is trivially copyable
    static_assert(std::is_trivially_copyable_v<PODRenderState>, "PODRenderState must be trivially copyable");

    // Inlined POD material data for commands — stored in FrameDataBuffer table,
    // referenced by u16 index from DrawMeshCommand / DrawMeshInstancedCommand.
    struct PODMaterialData
    {
        // Shader
        RHI::ResourceHandle shaderRendererID{};

        // Legacy material properties
        glm::vec3 ambient = glm::vec3(0.1f);
        glm::vec3 diffuse = glm::vec3(0.8f);
        glm::vec3 specular = glm::vec3(1.0f);
        f32 shininess = 32.0f;
        bool useTextureMaps = false;
        RHI::ResourceHandle diffuseMapID{};
        RHI::ResourceHandle specularMapID{};

        // PBR material properties
        bool enablePBR = false;
        glm::vec4 baseColorFactor = glm::vec4(1.0f);
        glm::vec4 emissiveFactor = glm::vec4(0.0f);
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        f32 normalScale = 1.0f;
        f32 occlusionStrength = 1.0f;
        bool enableIBL = false;
        f32 iblIntensity = 1.0f;
        // glTF-style alpha mode: 0=Opaque, 1=Mask, 2=Blend (matches AlphaMode enum).
        i32 alphaMode = 0;
        f32 alphaCutoff = 0.5f;

        // PBR texture identities (an invalid handle means no map for that slot;
        // test with .IsValid(), never against a literal 0)
        RHI::ResourceHandle albedoMapID{};
        RHI::ResourceHandle metallicRoughnessMapID{};
        RHI::ResourceHandle normalMapID{};
        RHI::ResourceHandle aoMapID{};
        RHI::ResourceHandle emissiveMapID{};
        RHI::ResourceHandle environmentMapID{};
        RHI::ResourceHandle irradianceMapID{};
        RHI::ResourceHandle prefilterMapID{};
        RHI::ResourceHandle brdfLutMapID{};

        // Field-wise equality (safe against struct padding, unlike memcmp)
        bool operator==(const PODMaterialData& o) const
        {
            return shaderRendererID == o.shaderRendererID && ambient == o.ambient && diffuse == o.diffuse && specular == o.specular && shininess == o.shininess && useTextureMaps == o.useTextureMaps && diffuseMapID == o.diffuseMapID && specularMapID == o.specularMapID && enablePBR == o.enablePBR && baseColorFactor == o.baseColorFactor && emissiveFactor == o.emissiveFactor && metallicFactor == o.metallicFactor && roughnessFactor == o.roughnessFactor && normalScale == o.normalScale && occlusionStrength == o.occlusionStrength && enableIBL == o.enableIBL && iblIntensity == o.iblIntensity && alphaMode == o.alphaMode && alphaCutoff == o.alphaCutoff && albedoMapID == o.albedoMapID && metallicRoughnessMapID == o.metallicRoughnessMapID && normalMapID == o.normalMapID && aoMapID == o.aoMapID && emissiveMapID == o.emissiveMapID && environmentMapID == o.environmentMapID && irradianceMapID == o.irradianceMapID && prefilterMapID == o.prefilterMapID && brdfLutMapID == o.brdfLutMapID;
        }
    };

    static_assert(std::is_trivially_copyable_v<PODMaterialData>, "PODMaterialData must be trivially copyable");

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
        DrawInfiniteGrid,
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
        SetMultisampling,

        // Terrain/Voxel commands
        DrawTerrainPatch,
        DrawVoxelMesh,

        // Decal commands
        DrawDecal,

        // Foliage commands
        DrawFoliageLayer,

        // Water commands
        DrawWater,

        // Sentinel — always keep last for dispatch table sizing
        COUNT
    };

    // Single source-of-truth for CommandType -> string conversion
    inline const char* CommandTypeToString(CommandType type)
    {
        switch (type)
        {
            case CommandType::Invalid:
                return "Invalid";
            case CommandType::Clear:
                return "Clear";
            case CommandType::ClearStencil:
                return "ClearStencil";
            case CommandType::DrawArrays:
                return "DrawArrays";
            case CommandType::DrawIndexed:
                return "DrawIndexed";
            case CommandType::DrawIndexedInstanced:
                return "DrawIndexedInstanced";
            case CommandType::DrawLines:
                return "DrawLines";
            case CommandType::DrawMesh:
                return "DrawMesh";
            case CommandType::DrawMeshInstanced:
                return "DrawMeshInstanced";
            case CommandType::DrawSkybox:
                return "DrawSkybox";
            case CommandType::DrawInfiniteGrid:
                return "DrawInfiniteGrid";
            case CommandType::DrawQuad:
                return "DrawQuad";
            case CommandType::BindDefaultFramebuffer:
                return "BindDefaultFramebuffer";
            case CommandType::BindTexture:
                return "BindTexture";
            case CommandType::SetShaderResource:
                return "SetShaderResource";
            case CommandType::SetViewport:
                return "SetViewport";
            case CommandType::SetClearColor:
                return "SetClearColor";
            case CommandType::SetBlendState:
                return "SetBlendState";
            case CommandType::SetBlendFunc:
                return "SetBlendFunc";
            case CommandType::SetBlendEquation:
                return "SetBlendEquation";
            case CommandType::SetDepthTest:
                return "SetDepthTest";
            case CommandType::SetDepthMask:
                return "SetDepthMask";
            case CommandType::SetDepthFunc:
                return "SetDepthFunc";
            case CommandType::SetStencilTest:
                return "SetStencilTest";
            case CommandType::SetStencilFunc:
                return "SetStencilFunc";
            case CommandType::SetStencilMask:
                return "SetStencilMask";
            case CommandType::SetStencilOp:
                return "SetStencilOp";
            case CommandType::SetCulling:
                return "SetCulling";
            case CommandType::SetCullFace:
                return "SetCullFace";
            case CommandType::SetLineWidth:
                return "SetLineWidth";
            case CommandType::SetPolygonMode:
                return "SetPolygonMode";
            case CommandType::SetPolygonOffset:
                return "SetPolygonOffset";
            case CommandType::SetScissorTest:
                return "SetScissorTest";
            case CommandType::SetScissorBox:
                return "SetScissorBox";
            case CommandType::SetColorMask:
                return "SetColorMask";
            case CommandType::SetMultisampling:
                return "SetMultisampling";
            case CommandType::DrawTerrainPatch:
                return "DrawTerrainPatch";
            case CommandType::DrawVoxelMesh:
                return "DrawVoxelMesh";
            case CommandType::DrawDecal:
                return "DrawDecal";
            case CommandType::DrawFoliageLayer:
                return "DrawFoliageLayer";
            case CommandType::DrawWater:
                return "DrawWater";
            case CommandType::COUNT:
                return "COUNT";
            default:
                return "Unknown";
        }
    }

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
        RHI::BlendFactor sourceFactor;
        RHI::BlendFactor destFactor;
    };

    struct SetBlendEquationCommand
    {
        CommandHeader header;
        RHI::BlendOp mode;
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
        RHI::CompareOp function;
    };

    struct SetStencilTestCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetStencilFuncCommand
    {
        CommandHeader header;
        RHI::CompareOp function;
        i32 reference;
        u32 mask;
    };

    struct SetStencilMaskCommand
    {
        CommandHeader header;
        u32 mask;
    };

    struct SetStencilOpCommand
    {
        CommandHeader header;
        RHI::StencilOp stencilFail;
        RHI::StencilOp depthFail;
        RHI::StencilOp depthPass;
    };

    struct SetCullingCommand
    {
        CommandHeader header;
        bool enabled;
    };

    struct SetCullFaceCommand
    {
        CommandHeader header;
        RHI::CullMode face;
    };

    struct SetLineWidthCommand
    {
        CommandHeader header;
        f32 width;
    };

    struct SetPolygonModeCommand
    {
        CommandHeader header;
        RHI::PolygonMode mode;
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
        i32 x;
        i32 y;
        u32 width;
        u32 height;
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
        RHI::ResourceHandle textureID;
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
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 indexCount;
        RHI::IndexType indexType;
    };

    struct DrawIndexedInstancedCommand
    {
        CommandHeader header;
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 indexCount;
        u32 instanceCount;
        RHI::IndexType indexType;
    };

    struct DrawArraysCommand
    {
        CommandHeader header;
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 vertexCount;
        RHI::PrimitiveTopology primitiveType;
    };

    struct DrawLinesCommand
    {
        CommandHeader header;
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 vertexCount;
    };

    // Higher-level commands combine multiple lower-level commands
    // All use POD types for radix sort compatibility
    struct DrawMeshCommand
    {
        CommandHeader header;

        // Mesh data (POD identifiers)
        AssetHandle meshHandle;              // Mesh asset handle for resolution
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 indexCount;
        u32 baseIndex = 0; // Starting index offset in shared index buffer (for multi-submesh MeshSources)
        glm::mat4 transform;
        // Previous-frame world transform for per-object motion-vector
        // generation in the G-Buffer path. Renderer3D fills this from a
        // per-entity cache on submission; equals `transform` for static /
        // first-frame objects so velocity is zero.
        glm::mat4 prevTransform;

        // Entity ID for picking (editor support)
        i32 entityID = -1;

        // Shader handle (for asset tracking — shaderRendererID lives in PODMaterialData)
        AssetHandle shaderHandle;

        // Material data index (into FrameDataBuffer::MaterialDataTable)
        u16 materialDataIndex = INVALID_MATERIAL_DATA_INDEX;

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;

        // Animation support
        bool isAnimatedMesh = false;
        u32 boneBufferOffset = 0;              // Offset into FrameDataBuffer for bone matrices
        u32 prevBoneBufferOffset = UINT32_MAX; // Offset into FrameDataBuffer for previous-frame bone matrices; UINT32_MAX = alias current (no motion)
        u32 boneCount = 0;                     // Number of bone matrices
        u8 workerIndex = 0;                    // Worker index for parallel submission (used to remap local bone offset to global)
        bool needsBoneOffsetRemap = false;     // True if boneBufferOffset is worker-local and needs remapping

        // Occlusion culling: query index for conditional rendering (UINT32_MAX = no query)
        u32 occlusionQueryIndex = UINT32_MAX;

        // Per-source tint and free float. Survive CommandBucket auto-batching
        // via FrameDataBuffer Colors / Customs streams; the InstanceData the
        // dispatcher writes for this source ends up with these values rather
        // than the (1,1,1,1) / 0.0f defaults from InstanceData. Callers that
        // need per-entity tinting (scripts, future MaterialComponent override
        // path) set these directly; non-setters get identity behaviour for
        // free.
        glm::vec4 color = glm::vec4(1.0f);
        f32 custom = 0.0f;

        // Lightmap atlas region for this draw (issue #439): uv2 * xy + zw
        // addresses the source entity's charts in the scene lightmap atlas.
        // All zeros (the default) = no lightmap; the shader's ambient ladder
        // falls through to probes/IBL. Survives CommandBucket auto-batching
        // the same way color does — via a vec4 FrameDataBuffer stream entry
        // per source command.
        glm::vec4 lightmapScaleOffset = glm::vec4(0.0f);
    };

    // Static assertion to verify DrawMeshCommand is trivially copyable (POD)
    static_assert(std::is_trivially_copyable_v<DrawMeshCommand>, "DrawMeshCommand must be trivially copyable for radix sort");

    struct DrawMeshInstancedCommand
    {
        CommandHeader header;

        // Mesh data (POD identifiers)
        AssetHandle meshHandle;              // Mesh asset handle
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 indexCount;
        u32 baseIndex = 0; // Starting index offset in shared index buffer (for multi-submesh MeshSources)
        u32 instanceCount;
        u32 transformBufferOffset = 0;              // Offset into FrameDataBuffer for instance transforms
        u32 prevTransformBufferOffset = UINT32_MAX; // Offset into FrameDataBuffer for previous-frame per-instance transforms; UINT32_MAX = alias current (no motion)
        u32 transformCount = 0;                     // Number of instance transforms

        // Shader handle (for asset tracking — shaderRendererID lives in PODMaterialData)
        AssetHandle shaderHandle;

        // Material data index (into FrameDataBuffer::MaterialDataTable)
        u16 materialDataIndex = INVALID_MATERIAL_DATA_INDEX;

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;

        // Animation support for instanced animated meshes
        bool isAnimatedMesh = false;
        u32 boneBufferOffset = 0;     // Offset into FrameDataBuffer for all instance bone matrices
        u32 boneCountPerInstance = 0; // Number of bones per instance

        // Per-instance i32 entity IDs in FrameDataBuffer::EntityIDs. UINT32_MAX
        // = no stream (dispatcher writes -1 to InstanceData.EntityID, breaking
        // editor picking on the batched instances). Auto-batching always
        // populates this so per-source picking survives the N-into-1 collapse.
        u32 entityIDBufferOffset = UINT32_MAX;

        // Per-instance vec4 Color and f32 Custom in FrameDataBuffer::Colors /
        // FrameDataBuffer::Customs. UINT32_MAX leaves InstanceData defaults
        // ((1,1,1,1) tint, 0 free float). Populated by the InstanceData
        // overload of Renderer3D::DrawMeshInstanced — auto-batching of plain
        // DrawMeshCommand sources leaves them at UINT32_MAX since
        // DrawMeshCommand has no per-source color/custom slot.
        u32 colorBufferOffset = UINT32_MAX;
        u32 customBufferOffset = UINT32_MAX;

        // Per-instance vec4 lightmap atlas regions (issue #439), riding the
        // same generic vec4 stream as Colors under their own offset. UINT32_MAX
        // leaves InstanceData::LightmapScaleOffset at the all-zero "no
        // lightmap" sentinel. Populated by CommandBucket::BatchCommands when
        // any batched source carries a non-zero region.
        u32 lightmapRegionBufferOffset = UINT32_MAX;

        // GPU frustum-cull path. When `cullIndirectBufferID` is non-zero, the
        // dispatcher SKIPS the FrameDataBuffer-driven InstanceData upload
        // (the cull compute already wrote compacted survivors to
        // `cullOutputInstanceBufferID`), rebinds that buffer at
        // SSBO_INSTANCE_DATA = 15, and uses `glDrawElementsIndirect` with
        // `cullIndirectBufferID` so the surviving instance count comes from
        // the GPU without a CPU readback. Set by
        // Renderer3D::DrawMeshInstanced for submissions above the GPU-cull
        // threshold; left at 0 for the regular CPU-cull / upload path.
        RHI::ResourceHandle cullOutputInstanceBufferID{};
        RHI::ResourceHandle cullIndirectBufferID{};
        // ADR 0011 §4.2: the frustum compute wrote the binding-15 address at
        // this reflected offset. The dispatcher completes the shader-specific
        // root struct around that field and hands the GPU buffer to the
        // matching indirect draw. Invalid handle = ordinary CPU root assembly.
        RHI::ResourceHandle cullRootDataBufferID{};
        u32 cullRootDataAddressOffsetBytes = 0;
    };

    // Static assertion to verify DrawMeshInstancedCommand is trivially copyable
    static_assert(std::is_trivially_copyable_v<DrawMeshInstancedCommand>, "DrawMeshInstancedCommand must be trivially copyable for radix sort");

    struct DrawSkyboxCommand
    {
        CommandHeader header;
        AssetHandle meshHandle;              // Skybox mesh handle
        RHI::ResourceHandle vertexArrayID{}; // VAO identity (invalid = no VAO)
        u32 indexCount;
        glm::mat4 transform;                               // Usually identity matrix
        AssetHandle shaderHandle;                          // Skybox shader handle (for asset tracking)
        RHI::ResourceHandle shaderRendererID{};            // Shader program identity
        RHI::ResourceHandle skyboxTextureID{};             // Cubemap texture identity
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX; // Render state index
    };

    // Static assertion for DrawSkyboxCommand
    static_assert(std::is_trivially_copyable_v<DrawSkyboxCommand>, "DrawSkyboxCommand must be trivially copyable for radix sort");

    struct DrawInfiniteGridCommand
    {
        CommandHeader header;
        AssetHandle shaderHandle;                          // Grid shader handle (for asset tracking)
        RHI::ResourceHandle shaderRendererID{};            // Shader program identity
        RHI::ResourceHandle quadVAOID{};                   // Fullscreen quad VAO identity
        f32 gridScale;                                     // Grid spacing scale factor
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX; // Render state index
    };

    // Static assertion for DrawInfiniteGridCommand
    static_assert(std::is_trivially_copyable_v<DrawInfiniteGridCommand>, "DrawInfiniteGridCommand must be trivially copyable for radix sort");

    struct DrawQuadCommand
    {
        CommandHeader header;
        glm::mat4 transform;
        RHI::ResourceHandle textureID{};                   // Texture identity
        AssetHandle shaderHandle;                          // Shader asset handle (for asset tracking)
        RHI::ResourceHandle shaderRendererID{};            // Shader program identity
        RHI::ResourceHandle quadVAID{};                    // Quad vertex array identity
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX; // Render state index
    };

    // Static assertion for DrawQuadCommand
    static_assert(std::is_trivially_copyable_v<DrawQuadCommand>, "DrawQuadCommand must be trivially copyable for radix sort");

    // Terrain patch command — uses GL_PATCHES with tessellation shaders
    struct DrawTerrainPatchCommand
    {
        CommandHeader header;

        // Mesh data
        RHI::ResourceHandle vertexArrayID{};
        u32 indexCount = 0;
        u32 patchVertexCount = 3; // Tessellation patch vertex count

        // Shader
        RHI::ResourceHandle shaderRendererID{};

        // Terrain textures
        RHI::ResourceHandle heightmapTextureID{};
        RHI::ResourceHandle splatmapTextureID{};
        RHI::ResourceHandle splatmap1TextureID{};
        RHI::ResourceHandle albedoArrayTextureID{};
        RHI::ResourceHandle normalArrayTextureID{};
        RHI::ResourceHandle armArrayTextureID{};

        // Transform
        glm::mat4 transform = glm::mat4(1.0f);
        i32 entityID = -1;

        // GPU-driven LOD (issue #714). When both are valid the draw sources its
        // instance count from `terrainIndirectArgsID` via glDrawElementsIndirect
        // and the vertex stage reads its per-node rect + seam deltas from
        // `terrainVisibleNodesID` at SSBO_TERRAIN_VISIBLE_NODES; `indexCount` is
        // then only a fallback for a backend that cannot honour the indirect
        // path. Both stay null for the chunk-geometry draws (shadow casters, the
        // non-tessellated path), which keep drawing exactly as before.
        RHI::ResourceHandle terrainIndirectArgsID{};
        RHI::ResourceHandle terrainVisibleNodesID{};

        // Terrain virtual texturing (issue #715). All three null on a draw whose
        // terrain has VT off, or whose VT has not converged yet; the UBO's
        // VTParams2.x carries the matching enable flag, so the shader never
        // samples an unbound cache.
        TerrainVTBindings virtualTexture{};

        // Terrain UBO data (inlined per-chunk — tess factors vary per chunk)
        ShaderBindingLayout::TerrainUBO terrainUBOData{};

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;
    };

    static_assert(std::is_trivially_copyable_v<DrawTerrainPatchCommand>, "DrawTerrainPatchCommand must be trivially copyable for radix sort");

    // Voxel mesh command — standard GL_TRIANGLES
    struct DrawVoxelMeshCommand
    {
        CommandHeader header;

        // Mesh data
        RHI::ResourceHandle vertexArrayID{};
        u32 indexCount = 0;

        // Packed-quad (binary greedy meshing, issue #727) instance count. Zero
        // means the marching-cubes path: a plain indexed draw of a triangle
        // soup. Non-zero routes the same command through
        // DrawBoundIndexedInstanced with the shared 6-index unit quad, one
        // instance per merged quad. One command type covers both because the
        // two paths differ only in the draw verb — same shader slot, same
        // texture bindings, same sort key.
        u32 instanceCount = 0;

        // Shader
        RHI::ResourceHandle shaderRendererID{};

        // Textures for triplanar sampling
        RHI::ResourceHandle albedoArrayTextureID{};
        RHI::ResourceHandle normalArrayTextureID{};
        RHI::ResourceHandle armArrayTextureID{};

        // Transform
        glm::mat4 transform = glm::mat4(1.0f);
        i32 entityID = -1;

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;
    };

    static_assert(std::is_trivially_copyable_v<DrawVoxelMeshCommand>, "DrawVoxelMeshCommand must be trivially copyable for radix sort");

    // Decal projection command — deferred projected decals
    struct DrawDecalCommand
    {
        CommandHeader header;

        // Mesh data (decal projection cube)
        RHI::ResourceHandle vertexArrayID{};
        u32 indexCount = 0;

        // Shader
        RHI::ResourceHandle shaderRendererID{};

        // Decal transform
        glm::mat4 decalTransform = glm::mat4(1.0f);        // Scaled transform for geometry
        glm::mat4 inverseDecalTransform = glm::mat4(1.0f); // For world->decal-space projection
        glm::mat4 inverseViewProjection = glm::mat4(1.0f); // Precomputed per-frame

        // Decal appearance
        glm::vec4 decalColor = glm::vec4(1.0f);
        glm::vec4 decalParams = glm::vec4(0.0f); // x = fadeDistance, y = normalAngleThreshold, z/w = unused
        RHI::ResourceHandle albedoTextureID{};
        RHI::ResourceHandle normalTextureID{}; // Bound at ShaderBindingLayout::TEX_USER_1 for Normal-mode decals (see CommandDispatch::DrawDecal)
        RHI::ResourceHandle rmaTextureID{};    // Bound at ShaderBindingLayout::TEX_USER_2 for RMA-mode decals (R=roughness, G=metal, B=AO)
        // Inserting fields between members above is safe: every Renderer3D::DrawDecal
        // call site assigns members by name (`cmd->normalTextureID = …`) rather than
        // positional brace initialization, and the same convention applies to
        // DrawMeshCommand / DrawMeshInstancedCommand.

        // Decal mode: matches Scene::DecalMode enum. DecalRenderPass::
        // ExecuteOnGBuffer uses this to pick draw-buffer + colour mask.
        enum class DecalMode : u8
        {
            Albedo = 0,
            Normal = 1,
            RMA = 2,
            Emissive = 3
        };
        DecalMode mode = DecalMode::Albedo;

        // Transparency override. When non-zero, this decal must be routed
        // through the forward (WB-OIT or blended) pipeline instead of the
        // deferred G-Buffer overlay path, regardless of the active
        // RenderingPath. Used by DecalRenderPass to decide which drain
        // phase owns the packet: ExecuteOnGBuffer skips `transparent == 1`
        // entries so that the graph-scheduled Execute() (which runs after
        // DeferredLightingPass in the Deferred path) can render them with
        // the forward shader over the already-lit scene colour.
        u8 transparent = 0;

        // Entity ID for picking
        i32 entityID = -1;

        // OIT program override. When non-zero, CommandDispatch::DrawDecal
        // substitutes this program ID for `shaderRendererID` so decal
        // commands composite via the WB-OIT layout without resubmission.
        // DecalRenderPass populates this on the command itself (not a
        // global) so the queue stays stateless and replay-safe.
        RHI::ResourceHandle oitProgramOverride{};

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;
    };

    static_assert(sizeof(DrawDecalCommand::DecalMode) == 1, "DecalMode must be 1 byte to preserve POD layout");
    static_assert(std::is_trivially_copyable_v<DrawDecalCommand>, "DrawDecalCommand must be trivially copyable for radix sort");

    // The G-Buffer CHANNEL routing of each decal mode, as one packed
    // per-attachment channel mask (see PODRenderState::colorAttachmentChannelMask).
    //
    // SINGLE SOURCE, deliberately: DecalRenderPass::ExecuteOnGBuffer drives its
    // per-attachment SetColorMaskForAttachment calls from this table, and
    // Renderer3D::DrawDecal stamps the same value onto the packet's POD state so
    // ApplyPODRenderState re-asserts it after the global SetColorMask has
    // flattened everything (issue #853). Two copies of this table is exactly the
    // drift the bug lived in -- the pass was setting the right masks and the
    // draw was throwing them away.
    //
    // Attachments 4..7 keep the fully-writable nibble on purpose. No decal
    // mode's draw-attachment map attaches them (RT4 is the R32I entity id, and
    // a decal must never stamp its pickability over the underlying mesh), so
    // masking them would be state this pass has no business owning -- and a
    // fully-writable nibble costs no indexed call at all, since it is what the
    // preceding global SetColorMask already left there.
    [[nodiscard]] constexpr u32 DecalGBufferChannelMask(DrawDecalCommand::DecalMode mode) noexcept
    {
        // Nibbles 0..3 cleared (this mode writes nothing there unless it says
        // otherwise below); 4..7 left fully writable.
        u32 mask = COLOR_CHANNEL_MASK_ALL & ~0x0000FFFFu;
        switch (mode)
        {
            case DrawDecalCommand::DecalMode::Normal:
                // RT1.xy is the oct-encoded normal; RT1.zw (roughness, AO) is
                // the underlying surface's and must survive.
                mask = WithColorChannelMask(mask, 1u, MakeColorChannelMask(true, true, false, false));
                break;
            case DrawDecalCommand::DecalMode::RMA:
                // RT0.a is metallic, RT1.zw is roughness + AO; RT0.rgb (albedo)
                // and RT1.xy (the normal) are the surface's.
                mask = WithColorChannelMask(mask, 0u, MakeColorChannelMask(false, false, false, true));
                mask = WithColorChannelMask(mask, 1u, MakeColorChannelMask(false, false, true, true));
                break;
            case DrawDecalCommand::DecalMode::Emissive:
                // RT2.rgb accumulates additively; RT2.a is the deferred UNLIT
                // flag and flipping it would take the surface out of lighting.
                mask = WithColorChannelMask(mask, 2u, MakeColorChannelMask(true, true, true, false));
                break;
            case DrawDecalCommand::DecalMode::Albedo:
            default:
                // RT0.rgb is albedo; RT0.a doubles as metallic in the G-Buffer
                // layout and the decal's own alpha is a blend weight, not a
                // material value.
                mask = WithColorChannelMask(mask, 0u, MakeColorChannelMask(true, true, true, false));
                break;
        }
        return mask;
    }

    // Foliage instanced layer command — one command per foliage layer
    struct DrawFoliageLayerCommand
    {
        CommandHeader header;

        // Mesh data (instanced quad)
        RHI::ResourceHandle vertexArrayID{};
        u32 indexCount = 0;
        u32 instanceCount = 0;

        // Shader
        RHI::ResourceHandle shaderRendererID{};

        // Model transform (parent terrain entity)
        glm::mat4 modelTransform = glm::mat4(1.0f);
        glm::mat4 normalMatrix = glm::mat4(1.0f);

        // Per-layer foliage parameters (inlined, fully POD)
        f32 time = 0.0f;
        f32 windStrength = 0.3f;
        f32 windSpeed = 1.0f;
        f32 viewDistance = 100.0f;
        f32 fadeStart = 80.0f;
        f32 alphaCutoff = 0.5f;
        f32 prevTime = 0.0f; // Previous-frame time for wind velocity reprojection
        f32 Pad1 = 0.0f;
        glm::vec4 baseColor = glm::vec4(1.0f); // xyz = color, w = unused

        // Albedo texture (0 = no texture). On the impostor path this is the
        // octahedral albedo atlas (rgb + coverage).
        RHI::ResourceHandle albedoTextureID{};

        // Octahedral impostor atlas (issue #433): normal+depth atlas + params.
        // impostorEnabled == 0 for the flat-billboard path (fields ignored).
        RHI::ResourceHandle impostorNormalDepthTextureID{};
        f32 impostorEnabled = 0.0f;
        f32 impostorFramesPerAxis = 8.0f;
        f32 impostorHemi = 1.0f;
        f32 impostorStartDistance = 40.0f;
        f32 impostorBand = 15.0f;
        f32 impostorRadius = 1.0f;
        f32 impostorParallaxScale = 0.5f;

        // Entity ID for picking
        i32 entityID = -1;

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;
    };

    static_assert(std::is_trivially_copyable_v<DrawFoliageLayerCommand>, "DrawFoliageLayerCommand must be trivially copyable for radix sort");

    // Water surface draw command
    struct DrawWaterCommand
    {
        CommandHeader header;

        // Mesh data
        RHI::ResourceHandle vertexArrayID{};
        u32 indexCount = 0;

        // Shader
        RHI::ResourceHandle shaderRendererID{};

        // Transform
        glm::mat4 modelTransform = glm::mat4(1.0f);
        glm::mat4 normalMatrix = glm::mat4(1.0f);

        // Water UBO data (inlined, fully POD)
        glm::vec4 waveParams = glm::vec4(0.0f);            // Time, WaveSpeed, WaveAmplitude, WaveFrequency
        glm::vec4 waveDir0 = glm::vec4(0.0f);              // xy = dir, z = steepness, w = wavelength
        glm::vec4 waveDir1 = glm::vec4(0.0f);              // xy = dir, z = steepness, w = wavelength
        glm::vec4 waterColor = glm::vec4(0.0f);            // rgb = shallow, a = transparency
        glm::vec4 waterDeepColor = glm::vec4(0.0f);        // rgb = deep,    a = reflectivity
        glm::vec4 visualParams = glm::vec4(0.0f);          // FresnelPower, SpecularIntensity, NormalMapTiling, NoiseIntensity
        glm::vec4 normalMapScroll = glm::vec4(0.0f);       // xy = scroll0 offset, zw = scroll1 offset
        glm::vec4 normalMapSpeed = glm::vec4(0.0f);        // x = speed0, y = speed1, z/w = unused
        glm::vec4 lightDirection = glm::vec4(0.0f);        // xyz = directional light dir, w = unused
        glm::vec4 depthRefractionParams = glm::vec4(0.0f); // depthSoftening, refrDistortion, refrHeightFactor
        glm::vec4 refractionColor = glm::vec4(0.0f);       // rgb = underwater tint
        glm::vec4 foamParams = glm::vec4(0.0f);            // foamHeightStart, foamFadeDistance, foamTiling, foamBrightness
        glm::vec4 foamParams2 = glm::vec4(0.0f);           // foamAngleExponent, shorelineFoamPower, sssIntensity
        glm::vec4 sssColor = glm::vec4(0.0f);              // rgb = SSS color
        glm::vec4 ssrParams = glm::vec4(0.0f);             // maxSteps, stepSize, maxDistance, thickness
        glm::vec4 tessParams = glm::vec4(0.0f);            // tessellationFactor, minDist, maxDist, frustumCullEnable
        glm::vec4 fftParams = glm::vec4(0.0f);             // useFFT (0/1), 1/patchSize, heightScale, horizontalScale

        // Normal map / noise texture IDs
        RHI::ResourceHandle normalMap0ID{};
        RHI::ResourceHandle normalMap1ID{};
        RHI::ResourceHandle noiseTextureID{};
        RHI::ResourceHandle foamTextureID{};
        // FFT ocean cascade textures (WATER_FUTURE_IMPROVEMENTS.md §1)
        RHI::ResourceHandle fftDisplacementID{}; // rgb = (dx, height, dz), a = foam
        RHI::ResourceHandle fftDerivativesID{};  // rgb = normal, a = jacobian

        // Feature toggles
        bool refractionEnabled = true;
        bool ssrEnabled = true;

        // Entity ID for picking
        i32 entityID = -1;

        // Render state index (into FrameDataBuffer::RenderStateTable)
        u16 renderStateIndex = INVALID_RENDER_STATE_INDEX;
    };

    static_assert(std::is_trivially_copyable_v<DrawWaterCommand>, "DrawWaterCommand must be trivially copyable for radix sort");

    // Maximum command size for allocation purposes. 1024 (PBR + bone
    // matrices) until issue #715 grew DrawTerrainPatchCommand's
    // inlined TerrainUBO by the adaptive sector table. Commands are packed at
    // their exact size, so this is a sanity bound, not a per-command cost.
    // Mirrored in Commands/CommandAllocator.h — keep the two identical.
    constexpr sizet MAX_COMMAND_SIZE = 4096;
} // namespace OloEngine
