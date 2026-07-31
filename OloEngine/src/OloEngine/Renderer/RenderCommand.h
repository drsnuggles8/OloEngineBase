#pragma once

#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine
{
    class RenderCommand
    {
      public:
        static void Init()
        {
            s_RendererAPI->Init();
        }

        static void SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
        {
            s_RendererAPI->SetViewport(x, y, width, height);
        }

        static void SetClearColor(const glm::vec4& color)
        {
            s_RendererAPI->SetClearColor(color);
        }

        static void Clear()
        {
            s_RendererAPI->Clear();
        }

        static void ClearDepthOnly()
        {
            s_RendererAPI->ClearDepthOnly();
        }

        static void ClearColorAndDepth()
        {
            s_RendererAPI->ClearColorAndDepth();
        }

        static Viewport GetViewport()
        {
            return s_RendererAPI->GetViewport();
        }

        static void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
        {
            s_RendererAPI->DrawArrays(vertexArray, vertexCount);
        }

        static void DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0)
        {
            s_RendererAPI->DrawIndexed(vertexArray, indexCount);
        }

        static void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0, const u32 instanceCount = 1)
        {
            s_RendererAPI->DrawIndexedInstanced(vertexArray, indexCount, instanceCount);
        }

        static void DrawLines(const Ref<VertexArray>& vertexArray, const u32 vertexCount)
        {
            s_RendererAPI->DrawLines(vertexArray, vertexCount);
        }

        static void DrawIndexedPatches(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0, const u32 patchVertices = 4)
        {
            s_RendererAPI->DrawIndexedPatches(vertexArray, indexCount, patchVertices);
        }

        // Raw VAO ID overloads for POD shadow casters
        static void DrawIndexedRaw(u32 vaoID, u32 indexCount)
        {
            s_RendererAPI->DrawIndexedRaw(vaoID, indexCount);
        }

        // baseIndex variant — required for submeshes that share a combined IBO
        // with their siblings (Sponza-style multi-submesh static models).
        static void DrawIndexedRaw(u32 vaoID, u32 indexCount, u32 baseIndex)
        {
            s_RendererAPI->DrawIndexedRaw(vaoID, indexCount, baseIndex);
        }

        // Instanced raw variant — used by shadow caster auto-batching to collapse
        // N casters sharing (vao, indexCount, baseIndex) into one GPU draw.
        static void DrawIndexedInstancedRaw(u32 vaoID, u32 indexCount, u32 baseIndex, u32 instanceCount)
        {
            s_RendererAPI->DrawIndexedInstancedRaw(vaoID, indexCount, baseIndex, instanceCount);
        }

        static void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVertices)
        {
            s_RendererAPI->DrawIndexedPatchesRaw(vaoID, indexCount, patchVertices);
        }

        static void SetLineWidth(const f32 width)
        {
            s_RendererAPI->SetLineWidth(width);
        }

        static void EnableCulling()
        {
            s_RendererAPI->EnableCulling();
        }

        static void DisableCulling()
        {
            s_RendererAPI->DisableCulling();
        }

        static void FrontCull()
        {
            s_RendererAPI->FrontCull();
        }

        static void BackCull()
        {
            s_RendererAPI->BackCull();
        }

        static void SetCullFace(RHI::CullMode face)
        {
            s_RendererAPI->SetCullFace(face);
        }

        // Depth
        static void SetDepthMask(bool value)
        {
            s_RendererAPI->SetDepthMask(value);
        }

        static void SetDepthTest(bool value)
        {
            s_RendererAPI->SetDepthTest(value);
        }

        static void SetDepthFunc(RHI::CompareOp func)
        {
            s_RendererAPI->SetDepthFunc(func);
        }

        // Blending
        static void EnableBlending()
        {
            s_RendererAPI->SetBlendState(true);
        }

        static void DisableBlending()
        {
            s_RendererAPI->SetBlendState(false);
        }

        static void SetBlendState(bool value)
        {
            s_RendererAPI->SetBlendState(value);
        }

        static void SetBlendFunc(RHI::BlendFactor sfactor, RHI::BlendFactor dfactor)
        {
            s_RendererAPI->SetBlendFunc(sfactor, dfactor);
        }

        static void SetBlendEquation(RHI::BlendOp mode)
        {
            s_RendererAPI->SetBlendEquation(mode);
        }

        // Stencil
        static void EnableStencilTest()
        {
            s_RendererAPI->EnableStencilTest();
        }

        static void DisableStencilTest()
        {
            s_RendererAPI->DisableStencilTest();
        }

        static bool IsStencilTestEnabled()
        {
            return s_RendererAPI->IsStencilTestEnabled();
        }

        static void SetStencilFunc(RHI::CompareOp func, i32 ref, u32 mask)
        {
            s_RendererAPI->SetStencilFunc(func, ref, mask);
        }

        static void SetStencilOp(RHI::StencilOp sfail, RHI::StencilOp dpfail, RHI::StencilOp dppass)
        {
            s_RendererAPI->SetStencilOp(sfail, dpfail, dppass);
        }

        static void SetStencilMask(u32 mask)
        {
            s_RendererAPI->SetStencilMask(mask);
        }

        static void ClearStencil()
        {
            s_RendererAPI->ClearStencil();
        }

        static void SetPolygonMode(RHI::PolygonMode mode)
        {
            s_RendererAPI->SetPolygonMode(mode);
        }

        static void EnableScissorTest()
        {
            s_RendererAPI->EnableScissorTest();
        }

        static void DisableScissorTest()
        {
            s_RendererAPI->DisableScissorTest();
        }

        static void SetScissorBox(i32 x, i32 y, u32 width, u32 height)
        {
            s_RendererAPI->SetScissorBox(x, y, width, height);
        }

        static void BindDefaultFramebuffer()
        {
            s_RendererAPI->BindDefaultFramebuffer();
        }

        static void BlitFramebufferToDefault(u32 srcFboID, u32 width, u32 height)
        {
            s_RendererAPI->BlitFramebufferToDefault(srcFboID, width, height);
        }

        static void BindTexture(u32 slot, u32 textureID)
        {
            s_RendererAPI->BindTexture(slot, textureID);
        }

        static void BindImageTexture(u32 unit, u32 textureID, u32 mipLevel, bool layered, u32 layer,
                                     RHI::Access access, RHI::Format format)
        {
            s_RendererAPI->BindImageTexture(unit, textureID, mipLevel, layered, layer, access, format);
        }

        static void SetPolygonOffset(f32 factor, f32 units)
        {
            s_RendererAPI->SetPolygonOffset(factor, units);
        }

        static void EnableMultisampling()
        {
            s_RendererAPI->EnableMultisampling();
        }

        static void DisableMultisampling()
        {
            s_RendererAPI->DisableMultisampling();
        }

        static void SetColorMask(bool red, bool green, bool blue, bool alpha)
        {
            s_RendererAPI->SetColorMask(red, green, blue, alpha);
        }

        static void SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha)
        {
            s_RendererAPI->SetColorMaskForAttachment(attachment, red, green, blue, alpha);
        }

        // Indirect draw calls (GPU-driven rendering)
        static void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID)
        {
            s_RendererAPI->DrawElementsIndirect(vertexArray, indirectBufferID);
        }

        static void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, u32 indirectBufferID)
        {
            s_RendererAPI->DrawArraysIndirect(vertexArray, indirectBufferID);
        }

        // Raw-VAO variant used by the GPU frustum-cull path that only has a
        // RendererID (no Ref<VertexArray> on hand inside the dispatcher).
        static void DrawElementsIndirectRaw(u32 vaoID, u32 indirectBufferID)
        {
            s_RendererAPI->DrawElementsIndirectRaw(vaoID, indirectBufferID);
        }

        // Multi-draw indirect with a GPU-sourced draw count (core GL 4.6, issue #629).
        static void MultiDrawElementsIndirectCountRaw(u32 vaoID, u32 indirectBufferID, u32 indirectOffsetBytes,
                                                      u32 parameterBufferID, u32 parameterOffsetBytes,
                                                      u32 maxDrawCount, u32 strideBytes)
        {
            s_RendererAPI->MultiDrawElementsIndirectCountRaw(vaoID, indirectBufferID, indirectOffsetBytes,
                                                             parameterBufferID, parameterOffsetBytes,
                                                             maxDrawCount, strideBytes);
        }

        // Compute shader dispatch
        static void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ)
        {
            s_RendererAPI->DispatchCompute(groupsX, groupsY, groupsZ);
        }

        static void MemoryBarrier(MemoryBarrierFlags flags)
        {
            s_RendererAPI->MemoryBarrier(flags);
        }

        // Per-attachment blend control
        static void SetBlendStateForAttachment(u32 attachment, bool enabled)
        {
            s_RendererAPI->SetBlendStateForAttachment(attachment, enabled);
        }

        static void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst)
        {
            s_RendererAPI->SetBlendFuncForAttachment(attachment, src, dst);
        }

        // GPU-side image copy
        static void CopyImageSubData(u32 srcID, RendererAPI::TextureTargetType srcTarget, u32 dstID, RendererAPI::TextureTargetType dstTarget,
                                     u32 width, u32 height)
        {
            s_RendererAPI->CopyImageSubData(srcID, srcTarget, dstID, dstTarget, width, height);
        }

        // Full image copy with source/dest z offsets (cubemap face copies)
        static void CopyImageSubDataFull(u32 srcID, RendererAPI::TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                         u32 dstID, RendererAPI::TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                         u32 width, u32 height)
        {
            s_RendererAPI->CopyImageSubDataFull(srcID, srcTarget, srcLevel, srcZ,
                                                dstID, dstTarget, dstLevel, dstZ,
                                                width, height);
        }

        // Copy from currently-bound READ framebuffer to a named texture
        static void CopyFramebufferToTexture(u32 textureID, u32 width, u32 height)
        {
            s_RendererAPI->CopyFramebufferToTexture(textureID, width, height);
        }

        // Draw buffer control
        static void SetDrawBuffers(std::span<const u32> attachments)
        {
            s_RendererAPI->SetDrawBuffers(attachments);
        }

        static void RestoreAllDrawBuffers(u32 colorAttachmentCount)
        {
            s_RendererAPI->RestoreAllDrawBuffers(colorAttachmentCount);
        }

        // Texture lifecycle
        static u32 CreateTexture2D(u32 width, u32 height, RHI::Format internalFormat)
        {
            return s_RendererAPI->CreateTexture2D(width, height, internalFormat);
        }

        static u32 CreateTextureCubemap(u32 width, u32 height, RHI::Format internalFormat)
        {
            return s_RendererAPI->CreateTextureCubemap(width, height, internalFormat);
        }

        static u32 CreateDepthArrayCompareOffView(u32 srcTextureID, u32 numLayers)
        {
            return s_RendererAPI->CreateDepthArrayCompareOffView(srcTextureID, numLayers);
        }

        static void SetTextureFilter(u32 textureID, RHI::Filter minFilter, RHI::Filter magFilter)
        {
            s_RendererAPI->SetTextureFilter(textureID, minFilter, magFilter);
        }

        static void SetTextureWrap(u32 textureID, RHI::AddressMode wrap)
        {
            s_RendererAPI->SetTextureWrap(textureID, wrap);
        }

        static void UploadTextureSubImage2D(u32 textureID, u32 width, u32 height,
                                            RHI::Format sourceFormat, const void* data)
        {
            s_RendererAPI->UploadTextureSubImage2D(textureID, width, height, sourceFormat, data);
        }

        static void DeleteTexture(u32 textureID)
        {
            s_RendererAPI->DeleteTexture(textureID);
        }

        // Conditional rendering (occlusion query driven)
        static void BeginConditionalRender(u32 queryID)
        {
            s_RendererAPI->BeginConditionalRender(queryID);
        }

        static void EndConditionalRender()
        {
            s_RendererAPI->EndConditionalRender();
        }

        // Total by design — safe to call before (or entirely without) renderer
        // bring-up. RendererAPI::Create() returns null for API::None, and the
        // asset paths that ask this question run in harnesses that never
        // initialise a renderer at all.
        [[nodiscard("Store this!")]] static bool IsDeviceAvailable()
        {
            return s_RendererAPI && s_RendererAPI->IsDeviceAvailable();
        }

        [[nodiscard("Store this!")]] static u32 GetMaxUniformBlockSize()
        {
            return s_RendererAPI->GetMaxUniformBlockSize();
        }

        [[nodiscard("Store this!")]] static bool SupportsInt64ShaderAtomics()
        {
            return s_RendererAPI->SupportsInt64ShaderAtomics();
        }

        // =====================================================================
        // Phase 2 step 2 additions (issue #691). One-line forwarders, same as
        // everything above — see RendererAPI.h for the shape rationale and
        // ADR 0011's "Amendments from Phase 2 step 2" for the design.
        // =====================================================================

        static void BindUniformBuffer(u32 bindingPoint, u32 bufferID)
        {
            s_RendererAPI->BindUniformBuffer(bindingPoint, bufferID);
        }

        static void BindStorageBuffer(u32 bindingPoint, u32 bufferID)
        {
            s_RendererAPI->BindStorageBuffer(bindingPoint, bufferID);
        }

        static void BindShaderProgram(u32 programID)
        {
            s_RendererAPI->BindShaderProgram(programID);
        }

        static void BindVertexArrayRaw(u32 vaoID)
        {
            s_RendererAPI->BindVertexArrayRaw(vaoID);
        }

        static void BindFramebuffer(u32 framebufferID)
        {
            s_RendererAPI->BindFramebuffer(framebufferID);
        }

        static void DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount,
                                     RHI::IndexType indexType = RHI::IndexType::UInt32, u32 baseIndex = 0)
        {
            s_RendererAPI->DrawBoundIndexed(topology, indexCount, indexType, baseIndex);
        }

        static void DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount,
                                              RHI::IndexType indexType, u32 baseIndex, u32 instanceCount)
        {
            s_RendererAPI->DrawBoundIndexedInstanced(topology, indexCount, indexType, baseIndex, instanceCount);
        }

        static void DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount)
        {
            s_RendererAPI->DrawBoundArrays(topology, firstVertex, vertexCount);
        }

        static void SetPatchVertexCount(u32 patchVertices)
        {
            s_RendererAPI->SetPatchVertexCount(patchVertices);
        }

        static void SetFrontFace(RHI::FrontFace face)
        {
            s_RendererAPI->SetFrontFace(face);
        }

        static void SetBlendFuncSeparate(RHI::BlendFactor srcRGB, RHI::BlendFactor dstRGB,
                                         RHI::BlendFactor srcAlpha, RHI::BlendFactor dstAlpha)
        {
            s_RendererAPI->SetBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
        }

        static void SetClearDepth(f32 depth)
        {
            s_RendererAPI->SetClearDepth(depth);
        }

        // Named framebuffers
        static u32 CreateFramebuffer()
        {
            return s_RendererAPI->CreateFramebuffer();
        }

        static void DeleteFramebuffer(u32 framebufferID)
        {
            s_RendererAPI->DeleteFramebuffer(framebufferID);
        }

        static void AttachFramebufferColorTexture(u32 framebufferID, u32 attachmentIndex,
                                                  u32 textureID, u32 mipLevel = 0)
        {
            s_RendererAPI->AttachFramebufferColorTexture(framebufferID, attachmentIndex, textureID, mipLevel);
        }

        static void AttachFramebufferDepthTexture(u32 framebufferID, u32 textureID, u32 mipLevel = 0)
        {
            s_RendererAPI->AttachFramebufferDepthTexture(framebufferID, textureID, mipLevel);
        }

        [[nodiscard("Store this!")]] static bool IsFramebufferComplete(u32 framebufferID)
        {
            return s_RendererAPI->IsFramebufferComplete(framebufferID);
        }

        static void SetFramebufferDrawAttachments(u32 framebufferID, std::span<const u32> attachmentIndices)
        {
            s_RendererAPI->SetFramebufferDrawAttachments(framebufferID, attachmentIndices);
        }

        static void RestoreAllFramebufferDrawAttachments(u32 framebufferID, u32 colorAttachmentCount)
        {
            s_RendererAPI->RestoreAllFramebufferDrawAttachments(framebufferID, colorAttachmentCount);
        }

        static void SetFramebufferReadAttachment(u32 framebufferID, u32 attachmentIndex)
        {
            s_RendererAPI->SetFramebufferReadAttachment(framebufferID, attachmentIndex);
        }

        static void ClearFramebufferColorAttachment(u32 framebufferID, u32 attachmentIndex, const glm::vec4& color)
        {
            s_RendererAPI->ClearFramebufferColorAttachment(framebufferID, attachmentIndex, color);
        }

        static void ClearFramebufferDepth(u32 framebufferID, f32 depth)
        {
            s_RendererAPI->ClearFramebufferDepth(framebufferID, depth);
        }

        static void BlitFramebuffer(u32 srcFramebufferID, u32 dstFramebufferID,
                                    i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                                    i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                                    RHI::BlitAspect aspect, RHI::Filter filter = RHI::Filter::Nearest)
        {
            s_RendererAPI->BlitFramebuffer(srcFramebufferID, dstFramebufferID,
                                           srcX0, srcY0, srcX1, srcY1,
                                           dstX0, dstY0, dstX1, dstY1, aspect, filter);
        }

        // Raw buffers
        static u32 CreateBuffer()
        {
            return s_RendererAPI->CreateBuffer();
        }

        static void DeleteBuffer(u32 bufferID)
        {
            s_RendererAPI->DeleteBuffer(bufferID);
        }

        static void AllocateBufferStorage(u32 bufferID, u64 sizeBytes, RHI::MemoryResidency residency)
        {
            s_RendererAPI->AllocateBufferStorage(bufferID, sizeBytes, residency);
        }

        static void* AllocatePersistentUploadStorage(u32 bufferID, u64 sizeBytes)
        {
            return s_RendererAPI->AllocatePersistentUploadStorage(bufferID, sizeBytes);
        }

        static void UnmapBuffer(u32 bufferID)
        {
            s_RendererAPI->UnmapBuffer(bufferID);
        }

        static void UploadBufferSubData(u32 bufferID, u64 offsetBytes, u64 sizeBytes, const void* data)
        {
            s_RendererAPI->UploadBufferSubData(bufferID, offsetBytes, sizeBytes, data);
        }

        static void ReadBufferSubData(u32 bufferID, u64 offsetBytes, u64 sizeBytes, void* dest)
        {
            s_RendererAPI->ReadBufferSubData(bufferID, offsetBytes, sizeBytes, dest);
        }

        static void CopyBufferSubData(u32 srcBufferID, u32 dstBufferID,
                                      u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes)
        {
            s_RendererAPI->CopyBufferSubData(srcBufferID, dstBufferID, srcOffsetBytes, dstOffsetBytes, sizeBytes);
        }

        static void ClearBufferUInt(u32 bufferID, u32 value)
        {
            s_RendererAPI->ClearBufferUInt(bufferID, value);
        }

        static void ClearBufferFloat(u32 bufferID, f32 value)
        {
            s_RendererAPI->ClearBufferFloat(bufferID, value);
        }

        // Vertex arrays
        static u32 CreateVertexArray()
        {
            return s_RendererAPI->CreateVertexArray();
        }

        static void SetVertexArrayIndexBuffer(u32 vaoID, u32 bufferID)
        {
            s_RendererAPI->SetVertexArrayIndexBuffer(vaoID, bufferID);
        }

        static void DeleteVertexArray(u32 vaoID)
        {
            s_RendererAPI->DeleteVertexArray(vaoID);
        }

        // Texture clear / upload / readback
        static void ClearTextureFloat(u32 textureID, u32 mipLevel, const glm::vec4& color)
        {
            s_RendererAPI->ClearTextureFloat(textureID, mipLevel, color);
        }

        static void ClearTextureUInt(u32 textureID, u32 mipLevel, u32 value)
        {
            s_RendererAPI->ClearTextureUInt(textureID, mipLevel, value);
        }

        static void UploadTextureSubImage2D(u32 textureID, i32 xOffset, i32 yOffset,
                                            u32 width, u32 height,
                                            RHI::Format sourceFormat, const void* data)
        {
            s_RendererAPI->UploadTextureSubImage2D(textureID, xOffset, yOffset, width, height, sourceFormat, data);
        }

        static void UploadTextureSubImage3D(u32 textureID, i32 xOffset, i32 yOffset, i32 zOffset,
                                            u32 width, u32 height, u32 depth,
                                            RHI::Format sourceFormat, const void* data)
        {
            s_RendererAPI->UploadTextureSubImage3D(textureID, xOffset, yOffset, zOffset,
                                                   width, height, depth, sourceFormat, data);
        }

        [[nodiscard("Store this!")]] static bool ReadTextureImage(u32 textureID, u32 mipLevel,
                                                                  RHI::Format destFormat,
                                                                  sizet destSizeBytes, void* dest)
        {
            return s_RendererAPI->ReadTextureImage(textureID, mipLevel, destFormat, destSizeBytes, dest);
        }

        [[nodiscard("Store this!")]] static bool ReadTextureSubImage(u32 textureID, u32 mipLevel,
                                                                     i32 x, i32 y, i32 z,
                                                                     u32 width, u32 height, u32 depth,
                                                                     RHI::Format destFormat,
                                                                     sizet destSizeBytes, void* dest)
        {
            return s_RendererAPI->ReadTextureSubImage(textureID, mipLevel, x, y, z, width, height, depth,
                                                      destFormat, destSizeBytes, dest);
        }

        static void GetTextureDimensions(u32 textureID, u32 mipLevel, u32& outWidth, u32& outHeight)
        {
            s_RendererAPI->GetTextureDimensions(textureID, mipLevel, outWidth, outHeight);
        }

        static void TextureBarrier()
        {
            s_RendererAPI->TextureBarrier();
        }

        // Queries
        static void CreateQueries(RHI::QueryType type, std::span<u32> outQueryIDs)
        {
            s_RendererAPI->CreateQueries(type, outQueryIDs);
        }

        static void DeleteQueries(std::span<const u32> queryIDs)
        {
            s_RendererAPI->DeleteQueries(queryIDs);
        }

        static void BeginQuery(RHI::QueryType type, u32 queryID)
        {
            s_RendererAPI->BeginQuery(type, queryID);
        }

        static void EndQuery(RHI::QueryType type)
        {
            s_RendererAPI->EndQuery(type);
        }

        [[nodiscard("Store this!")]] static bool IsQueryResultAvailable(u32 queryID)
        {
            return s_RendererAPI->IsQueryResultAvailable(queryID);
        }

        [[nodiscard("Store this!")]] static u32 GetQueryResultU32(u32 queryID)
        {
            return s_RendererAPI->GetQueryResultU32(queryID);
        }

        [[nodiscard("Store this!")]] static u64 GetQueryResultU64(u32 queryID)
        {
            return s_RendererAPI->GetQueryResultU64(queryID);
        }

        // Fences
        [[nodiscard("Store this!")]] static u64 CreateFence()
        {
            return s_RendererAPI->CreateFence();
        }

        [[nodiscard("Store this!")]] static RHI::FenceStatus ClientWaitFence(u64 fence, u64 timeoutNanoseconds)
        {
            return s_RendererAPI->ClientWaitFence(fence, timeoutNanoseconds);
        }

        [[nodiscard("Store this!")]] static bool IsFenceSignaled(u64 fence)
        {
            return s_RendererAPI->IsFenceSignaled(fence);
        }

        static void DestroyFence(u64 fence)
        {
            s_RendererAPI->DestroyFence(fence);
        }

        // Debug markers
        static void PushDebugGroup(u32 id, std::string_view label)
        {
            s_RendererAPI->PushDebugGroup(id, label);
        }

        static void PopDebugGroup()
        {
            s_RendererAPI->PopDebugGroup();
        }

        static void WaitForDeviceIdle()
        {
            s_RendererAPI->WaitForDeviceIdle();
        }

        [[nodiscard("Store this!")]] static u32 GetMaxFramebufferSamples()
        {
            return s_RendererAPI->GetMaxFramebufferSamples();
        }

        [[nodiscard("Store this!")]] static u32 GetMaxColorTextureSamples()
        {
            return s_RendererAPI->GetMaxColorTextureSamples();
        }

        [[nodiscard("Store this!")]] static u32 GetMaxDepthTextureSamples()
        {
            return s_RendererAPI->GetMaxDepthTextureSamples();
        }

        // See RendererAPI.h: the one virtual with no faithful Vulkan lowering.
        // Phase 6 deletes it by moving u_GridScale into a UBO.
        static void SetProgramUniformFloat(u32 programID, std::string_view name, f32 value)
        {
            s_RendererAPI->SetProgramUniformFloat(programID, name, value);
        }

        static RendererAPI& GetRendererAPI()
        {
            return *s_RendererAPI;
        }

      private:
        static Scope<RendererAPI> s_RendererAPI;
    };
} // namespace OloEngine
