#pragma once

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Helper function to compute depth from camera space for sort key.
    // Returns a quantized depth value in range [0, 0xFFFFFF] for 24-bit depth.
    // @param modelMatrix The model transformation matrix.
    // @param boundingSphereCenter Optional world-space bounding sphere center. If provided,
    //        uses this for more accurate depth sorting for off-center meshes. If nullptr,
    //        falls back to using modelMatrix[3] (the origin of the transformed object).
    inline auto ComputeDepthForSortKeyWithView(const glm::mat4& modelMatrix,
                                               const glm::mat4& viewMatrix,
                                               const glm::vec3* boundingSphereCenter = nullptr) -> u32
    {
        const glm::vec4 worldPos = boundingSphereCenter
                                       ? glm::vec4(*boundingSphereCenter, 1.0f)
                                       : modelMatrix[3];
        const glm::vec4 viewPos = viewMatrix * worldPos;

        // Use negative Z since camera looks down -Z axis.
        f32 depth = -viewPos.z;

        // Clamp depth to reasonable range [0, 1000] and quantize to 24 bits.
        constexpr f32 MIN_DEPTH = 0.1f;
        constexpr f32 MAX_DEPTH = 1000.0f;
        depth = glm::clamp(depth, MIN_DEPTH, MAX_DEPTH);
        const f32 normalizedDepth = (depth - MIN_DEPTH) / (MAX_DEPTH - MIN_DEPTH);
        return static_cast<u32>(normalizedDepth * 0xFFFFFF);
    }

    inline auto ComputeDepthForSortKey(const glm::mat4& modelMatrix,
                                       const glm::vec3* boundingSphereCenter = nullptr) -> u32
    {
        return ComputeDepthForSortKeyWithView(modelMatrix, CommandDispatch::GetViewMatrix(), boundingSphereCenter);
    }

    // Helper to generate material ID hash for sort key.
    inline auto ComputeMaterialID(const Material& material) -> u32
    {
        u64 hash = 0;

        if (material.GetType() == MaterialType::PBR)
        {
            const u64 albedoID = material.GetAlbedoMap() ? RHI::HashKey(material.GetAlbedoMap()->GetRHIHandle()) : 0ULL;
            const u64 metallicID = material.GetMetallicRoughnessMap() ? RHI::HashKey(material.GetMetallicRoughnessMap()->GetRHIHandle()) : 0ULL;
            const u64 normalID = material.GetNormalMap() ? RHI::HashKey(material.GetNormalMap()->GetRHIHandle()) : 0ULL;

            hash = albedoID;
            hash ^= metallicID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
            hash ^= normalID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
        }
        else
        {
            const u64 diffuseID = material.GetDiffuseMap() ? RHI::HashKey(material.GetDiffuseMap()->GetRHIHandle()) : 0ULL;
            const u64 specularID = material.GetSpecularMap() ? RHI::HashKey(material.GetSpecularMap()->GetRHIHandle()) : 0ULL;

            hash = diffuseID;
            hash ^= specularID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
        }

        // Fold 64-bit hash to 16-bit material ID (as defined in DrawKey).
        return static_cast<u32>((hash ^ (hash >> 32)) & 0xFFFF);
    }

    // Helper to create default POD render state.
    inline auto CreateDefaultPODRenderState() -> PODRenderState
    {
        PODRenderState state{};
        // All fields are initialized to sensible defaults by the struct itself.
        return state;
    }

    // The decal draw's render state, in ONE place.
    //
    // Renderer3D::DrawDecal stamps this onto every decal packet and
    // CommandDispatch::DrawDecal applies it through ApplyPODRenderState. It is a
    // shared function rather than an inline block because the decal tenant has
    // to apply the SAME state the production draw applies -- a tenant that
    // hand-copies it pins its own copy, which is how issue #853's channel-mask
    // defect survived a green mode-matrix test.
    //
    // @param mode         the decal's G-Buffer mode (already collapsed to Albedo
    //                     on the forward path by the caller).
    // @param deferredPath true when this decal writes into the G-Buffer
    //                     pre-lighting. ONLY then does the per-attachment
    //                     channel mask apply: on the forward path the decal
    //                     draws into scene colour (or the WB-OIT accum/revealage
    //                     MRT), where the G-Buffer channel routing is meaningless
    //                     and disabling channels would break OIT compositing.
    inline auto CreateDecalPODRenderState(DrawDecalCommand::DecalMode mode, bool deferredPath) -> PODRenderState
    {
        PODRenderState state = CreateDefaultPODRenderState();

        // Blend on for albedo (soft edges), off for normal/RMA/emissive (hard
        // discard threshold -- see the shader comments). Depth read-only,
        // front-face culling in all cases (the decal is a closed cube; its far
        // faces are what survives the depth test).
        const bool blendForThisMode = (mode == DrawDecalCommand::DecalMode::Albedo);
        state.blendEnabled = blendForThisMode;
        state.blendSrcFactor = RHI::BlendFactor::SrcAlpha;
        state.blendDstFactor = RHI::BlendFactor::OneMinusSrcAlpha;
        state.depthTestEnabled = true;
        // GreaterOrEqual, not LessOrEqual, and the pairing with cullFace below is
        // the whole point: front-face culling keeps the projection box's BACK
        // faces, and those are BEHIND the surface the decal projects onto. A
        // LessOrEqual test rejects every one of them, so a decal straddling its
        // receiving surface produced NO fragments at all -- on the deferred and
        // the forward path alike, since both read this one state. That was true
        // for as long as the decal existed (it predates the RHI refactor as
        // GL_LEQUAL + GL_FRONT) and no test caught it, because the pass tenant
        // substitutes a proxy QUAD placed in front of the surface for the
        // production cube. Back-face decal rendering wants "the box's far side
        // is at or behind the geometry", which is GreaterOrEqual; the shader's
        // own in-box test then rejects surfaces that are merely far away.
        // Measured: with LessOrEqual the G-Buffer is a uniform floor colour with
        // no decal anywhere; with GreaterOrEqual an Albedo decal's authored
        // (1, 0.25, 0.15) lands in GBufferAlbedo as (255, 64, 38).
        state.depthFunction = RHI::CompareOp::GreaterOrEqual;
        state.depthWriteMask = false;
        state.cullingEnabled = true;
        state.cullFace = RHI::CullMode::Front;

        if (deferredPath)
        {
            // The mode matrix's channel routing, carried on the command so
            // ApplyPODRenderState re-asserts it after its global SetColorMask
            // (issue #853). Without this the pass's per-attachment masks are
            // wiped between the pass setting them and the draw consuming them,
            // and every mode writes every channel of the attachments its draw
            // map selects.
            state.colorAttachmentChannelMask = DecalGBufferChannelMask(mode);
        }

        return state;
    }

    // Helper to populate POD render state from material properties.
    // Maps MaterialFlag to PODRenderState for proper render state setup.
    inline auto CreatePODRenderStateForMaterial(const Material& material) -> PODRenderState
    {
        PODRenderState state{};

        // Depth test - most materials want this enabled.
        state.depthTestEnabled = material.GetFlag(MaterialFlag::DepthTest);
        state.depthWriteMask = true; // Write to depth buffer for opaque materials.
        state.depthFunction = RHI::CompareOp::Less;

        // Blend state - for transparent materials.
        if (material.GetFlag(MaterialFlag::Blend))
        {
            state.blendEnabled = true;
            state.blendSrcFactor = RHI::BlendFactor::SrcAlpha;
            state.blendDstFactor = RHI::BlendFactor::OneMinusSrcAlpha;
            state.blendEquation = RHI::BlendOp::Add;
            // Transparent objects typically don't write to depth buffer.
            state.depthWriteMask = false;
        }
        else
        {
            state.blendEnabled = false;
        }

        // Culling - controlled by TwoSided flag.
        if (material.GetFlag(MaterialFlag::TwoSided))
        {
            // Double-sided materials don't cull any faces.
            state.cullingEnabled = false;
        }
        else
        {
            state.cullingEnabled = true;
            state.cullFace = RHI::CullMode::Back;
        }

        return state;
    }
} // namespace OloEngine
