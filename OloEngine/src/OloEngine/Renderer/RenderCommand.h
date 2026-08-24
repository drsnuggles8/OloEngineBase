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

        // ADR 0011 amendment (39). `s_RendererAPI` is constructed at
        // STATIC INIT (RenderCommand.cpp), before `--rhi=` parses — so the
        // static-init instance is always the OpenGL default regardless of the
        // selected backend. Application's constructor calls this immediately
        // after `RendererAPI::SetAPI` (and before `Window::Create`) so the
        // facade matches the selection the moment anything can route through
        // it. Must never be called once a window/context exists or any code
        // has captured `GetRendererAPI()` by reference.
        static void RecreateForSelectedBackend()
        {
            s_RendererAPI = RendererAPI::Create();
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

        static void BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height)
        {
            s_RendererAPI->BlitFramebufferToDefault(srcFramebuffer, width, height);
        }

        // Release renderer-owned GPU state while the context is still current.
        // See RendererAPI::ShutdownGpuResources for why this is a virtual with a
        // default no-op body rather than a cast to the concrete backend.
        static void ShutdownGpuResources()
        {
            s_RendererAPI->ShutdownGpuResources();
        }

        static void BindTexture(u32 slot, RHI::ResourceHandle texture)
        {
            s_RendererAPI->BindTexture(slot, texture);
        }

        // Explicit-sampler form (#691) — see RendererAPI::BindTexture.
        static void BindTexture(u32 slot, RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler)
        {
            s_RendererAPI->BindTexture(slot, texture, sampler);
        }

        static void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered,
                                     u32 layer, RHI::Access access, RHI::Format format)
        {
            s_RendererAPI->BindImageTexture(unit, texture, mipLevel, layered, layer, access, format);
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
        static void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer)
        {
            s_RendererAPI->DrawElementsIndirect(vertexArray, indirectBuffer);
        }

        static void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer)
        {
            s_RendererAPI->DrawArraysIndirect(vertexArray, indirectBuffer);
        }

        static void DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 patchVertices)
        {
            s_RendererAPI->DrawIndexedPatchesRaw(vertexArray, indexCount, patchVertices);
        }

        static void DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex,
                                            u32 instanceCount)
        {
            s_RendererAPI->DrawIndexedInstancedRaw(vertexArray, indexCount, baseIndex, instanceCount);
        }

        static void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount)
        {
            s_RendererAPI->DrawIndexedRaw(vertexArray, indexCount);
        }

        static void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex)
        {
            s_RendererAPI->DrawIndexedRaw(vertexArray, indexCount, baseIndex);
        }

        // Multi-draw indirect with a GPU-sourced draw count (core GL 4.6, issue #629).
        static void MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray,
                                                      RHI::ResourceHandle indirectBuffer, u32 indirectOffsetBytes,
                                                      RHI::ResourceHandle parameterBuffer, u32 parameterOffsetBytes,
                                                      u32 maxDrawCount, u32 strideBytes)
        {
            s_RendererAPI->MultiDrawElementsIndirectCountRaw(vertexArray, indirectBuffer, indirectOffsetBytes,
                                                             parameterBuffer, parameterOffsetBytes,
                                                             maxDrawCount, strideBytes);
        }

        // Compute shader dispatch
        static void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ)
        {
            s_RendererAPI->DispatchCompute(groupsX, groupsY, groupsZ);
        }

        // GPU-sourced dispatch dimensions (issue #714). `offsetBytes` must be
        // 4-byte aligned and address a uvec3 group count.
        static void DispatchComputeIndirect(RHI::ResourceHandle argsBuffer, u32 offsetBytes)
        {
            s_RendererAPI->DispatchComputeIndirect(argsBuffer, offsetBytes);
        }

        // Task/mesh-pipeline dispatch (issue #813) — gate on
        // SupportsMeshShaders(); see RendererAPI::DrawMeshTasks.
        static void DrawMeshTasks(u32 groupsX, u32 groupsY, u32 groupsZ)
        {
            s_RendererAPI->DrawMeshTasks(groupsX, groupsY, groupsZ);
        }

        // Draws from the ALREADY-BOUND vertex array — used by the GPU
        // frustum-cull path (Triangles) and the GPU-driven terrain path
        // (PatchList), whose callers have just run BindVAOIfNeeded.
        static void DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer,
                                              RHI::PrimitiveTopology topology)
        {
            s_RendererAPI->DrawBoundElementsIndirect(indirectBuffer, topology);
        }

        static void MemoryBarrier(MemoryBarrierFlags flags)
        {
            s_RendererAPI->MemoryBarrier(flags);
        }

        // ADR 0011 §1.5: the render graph's pre-pass barrier batch
        // carrying both currencies — GL executes `flags`, explicit-barrier
        // backends lower `barriers`.
        static void IssueBarrierBatch(MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers)
        {
            s_RendererAPI->IssueBarrierBatch(flags, barriers);
        }

        // Per-attachment blend control. Tri-state: see the declaration in
        // RendererAPI.h -- an attachment a pass turned on or off keeps that
        // opinion until ResetBlendStateForAttachment withdraws it, so every
        // pass that states one has to withdraw it before returning.
        static void SetBlendStateForAttachment(u32 attachment, bool enabled)
        {
            s_RendererAPI->SetBlendStateForAttachment(attachment, enabled);
        }

        static void ResetBlendStateForAttachment(u32 attachment)
        {
            s_RendererAPI->ResetBlendStateForAttachment(attachment);
        }

        static void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst)
        {
            s_RendererAPI->SetBlendFuncForAttachment(attachment, src, dst);
        }

        // GPU-side image copy — both operands together (issue #691).
        static void CopyImageSubData(RHI::ResourceHandle src, RendererAPI::TextureTargetType srcTarget,
                                     RHI::ResourceHandle dst, RendererAPI::TextureTargetType dstTarget,
                                     u32 width, u32 height)
        {
            s_RendererAPI->CopyImageSubData(src, srcTarget, dst, dstTarget, width, height);
        }

        // Full image copy with source/dest z offsets (cubemap face copies).
        // Both operands together (issue #691).
        static void CopyImageSubDataFull(RHI::ResourceHandle src, RendererAPI::TextureTargetType srcTarget,
                                         i32 srcLevel, i32 srcZ,
                                         RHI::ResourceHandle dst, RendererAPI::TextureTargetType dstTarget,
                                         i32 dstLevel, i32 dstZ,
                                         u32 width, u32 height)
        {
            s_RendererAPI->CopyImageSubDataFull(src, srcTarget, srcLevel, srcZ,
                                                dst, dstTarget, dstLevel, dstZ,
                                                width, height);
        }

        // Region copy with full (x, y, z) offsets on both operands (terrain VT
        // tile stage, issue #715). Block-copy contract on the facade
        // declaration: width/height are SOURCE texels, dst offsets must be
        // block-aligned when the dest is block-compressed.
        static void CopyImageSubDataRegion(RHI::ResourceHandle src, RendererAPI::TextureTargetType srcTarget,
                                           i32 srcLevel, i32 srcX, i32 srcY, i32 srcZ,
                                           RHI::ResourceHandle dst, RendererAPI::TextureTargetType dstTarget,
                                           i32 dstLevel, i32 dstX, i32 dstY, i32 dstZ,
                                           u32 width, u32 height)
        {
            s_RendererAPI->CopyImageSubDataRegion(src, srcTarget, srcLevel, srcX, srcY, srcZ,
                                                  dst, dstTarget, dstLevel, dstX, dstY, dstZ,
                                                  width, height);
        }

        // Copy from currently-bound READ framebuffer to a named texture
        static void CopyFramebufferToTexture(RHI::ResourceHandle texture, u32 width, u32 height)
        {
            s_RendererAPI->CopyFramebufferToTexture(texture, width, height);
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
        [[nodiscard]] static RHI::ResourceHandle CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture,
                                                                                      u32 numLayers)
        {
            return s_RendererAPI->CreateDepthArrayCompareOffViewHandle(srcTexture, numLayers);
        }

        static void SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter)
        {
            s_RendererAPI->SetTextureFilter(texture, minFilter, magFilter);
        }
        static void SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap)
        {
            s_RendererAPI->SetTextureWrap(texture, wrap);
        }
        static void UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height,
                                            RHI::Format sourceFormat, const void* data)
        {
            s_RendererAPI->UploadTextureSubImage2D(texture, width, height, sourceFormat, data);
        }

        // Conditional rendering (occlusion query driven)
        static void BeginConditionalRender(RHI::ResourceHandle query)
        {
            s_RendererAPI->BeginConditionalRender(query);
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

        // The DrawMeshTasks capability gate (issue #813) — see
        // RendererAPI::SupportsMeshShaders.
        [[nodiscard("Store this!")]] static bool SupportsMeshShaders()
        {
            return s_RendererAPI->SupportsMeshShaders();
        }

        // =====================================================================
        // Call-site sweep additions (issue #691). One-line forwarders, same as
        // everything above — see RendererAPI.h for the shape rationale and
        // ADR 0011's "Amendments from the call-site sweep" for the design.
        // =====================================================================

        static void BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
        {
            s_RendererAPI->BindUniformBuffer(bindingPoint, buffer);
        }

        static void BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer)
        {
            s_RendererAPI->BindStorageBuffer(bindingPoint, buffer);
        }

        static void BindShaderProgram(RHI::ResourceHandle program)
        {
            s_RendererAPI->BindShaderProgram(program);
        }

        static void BindVertexArrayRaw(RHI::ResourceHandle vertexArray)
        {
            s_RendererAPI->BindVertexArrayRaw(vertexArray);
        }

        static void BindFramebuffer(RHI::ResourceHandle framebuffer)
        {
            s_RendererAPI->BindFramebuffer(framebuffer);
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
        static void AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                                  RHI::ResourceHandle texture, u32 mipLevel = 0)
        {
            s_RendererAPI->AttachFramebufferColorTexture(framebuffer, attachmentIndex, texture, mipLevel);
        }

        static void AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture,
                                                  u32 mipLevel = 0)
        {
            s_RendererAPI->AttachFramebufferDepthTexture(framebuffer, texture, mipLevel);
        }

        [[nodiscard("Store this!")]] static bool IsFramebufferComplete(RHI::ResourceHandle framebuffer)
        {
            return s_RendererAPI->IsFramebufferComplete(framebuffer);
        }

        static void SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                                  std::span<const u32> attachmentIndices)
        {
            s_RendererAPI->SetFramebufferDrawAttachments(framebuffer, attachmentIndices);
        }

        static void RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer, u32 colorAttachmentCount)
        {
            s_RendererAPI->RestoreAllFramebufferDrawAttachments(framebuffer, colorAttachmentCount);
        }

        static void SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex)
        {
            s_RendererAPI->SetFramebufferReadAttachment(framebuffer, attachmentIndex);
        }

        static void ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                                    const glm::vec4& color)
        {
            s_RendererAPI->ClearFramebufferColorAttachment(framebuffer, attachmentIndex, color);
        }

        static void ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth)
        {
            s_RendererAPI->ClearFramebufferDepth(framebuffer, depth);
        }

        // RHI::NullResource on either side is the DEFAULT framebuffer.
        static void BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer,
                                    i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                                    i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                                    RHI::BlitAspect aspect, RHI::Filter filter = RHI::Filter::Nearest)
        {
            s_RendererAPI->BlitFramebuffer(srcFramebuffer, dstFramebuffer,
                                           srcX0, srcY0, srcX1, srcY1,
                                           dstX0, dstY0, dstX1, dstY1, aspect, filter);
        }

        // Raw buffers
        static void AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes, RHI::MemoryResidency residency)
        {
            s_RendererAPI->AllocateBufferStorage(buffer, sizeBytes, residency);
        }

        static void* AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes)
        {
            return s_RendererAPI->AllocatePersistentUploadStorage(buffer, sizeBytes);
        }

        static void UnmapBuffer(RHI::ResourceHandle buffer)
        {
            s_RendererAPI->UnmapBuffer(buffer);
        }

        static void UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, const void* data)
        {
            s_RendererAPI->UploadBufferSubData(buffer, offsetBytes, sizeBytes, data);
        }

        static void ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest)
        {
            s_RendererAPI->ReadBufferSubData(buffer, offsetBytes, sizeBytes, dest);
        }

        static void CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer,
                                      u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes)
        {
            s_RendererAPI->CopyBufferSubData(srcBuffer, dstBuffer, srcOffsetBytes, dstOffsetBytes, sizeBytes);
        }

        static void ClearBufferUInt(RHI::ResourceHandle buffer, u32 value)
        {
            s_RendererAPI->ClearBufferUInt(buffer, value);
        }

        static void ClearBufferFloat(RHI::ResourceHandle buffer, f32 value)
        {
            s_RendererAPI->ClearBufferFloat(buffer, value);
        }

        // The resource creators (issue #691; the u32 siblings
        // they were added beside are gone as of item 4). Each Delete* both
        // destroys the object and retires its identity.
        [[nodiscard]] static RHI::ResourceHandle CreateTexture2DHandle(u32 width, u32 height, RHI::Format internalFormat)
        {
            return s_RendererAPI->CreateTexture2DHandle(width, height, internalFormat);
        }
        [[nodiscard]] static RHI::ResourceHandle CreateTextureCubemapHandle(u32 width, u32 height, RHI::Format internalFormat)
        {
            return s_RendererAPI->CreateTextureCubemapHandle(width, height, internalFormat);
        }
        [[nodiscard]] static RHI::ResourceHandle CreateFramebufferHandle()
        {
            return s_RendererAPI->CreateFramebufferHandle();
        }
        [[nodiscard]] static RHI::ResourceHandle CreateBufferHandle()
        {
            return s_RendererAPI->CreateBufferHandle();
        }
        [[nodiscard]] static RHI::ResourceHandle CreateVertexArrayHandle()
        {
            return s_RendererAPI->CreateVertexArrayHandle();
        }
        static void DeleteTexture(RHI::ResourceHandle texture)
        {
            s_RendererAPI->DeleteTexture(texture);
        }
        static void DeleteFramebuffer(RHI::ResourceHandle framebuffer)
        {
            s_RendererAPI->DeleteFramebuffer(framebuffer);
        }
        static void DeleteBuffer(RHI::ResourceHandle buffer)
        {
            s_RendererAPI->DeleteBuffer(buffer);
        }
        static void DeleteVertexArray(RHI::ResourceHandle vertexArray)
        {
            s_RendererAPI->DeleteVertexArray(vertexArray);
        }
        static void SetVertexArrayIndexBuffer(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indexBuffer)
        {
            s_RendererAPI->SetVertexArrayIndexBuffer(vertexArray, indexBuffer);
        }

        // Texture clear / upload / readback
        static void ClearTextureFloat(RHI::ResourceHandle texture, u32 mipLevel, const glm::vec4& color)
        {
            s_RendererAPI->ClearTextureFloat(texture, mipLevel, color);
        }

        static void ClearTextureUInt(RHI::ResourceHandle texture, u32 mipLevel, u32 value)
        {
            s_RendererAPI->ClearTextureUInt(texture, mipLevel, value);
        }

        static void UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset,
                                            u32 width, u32 height,
                                            RHI::Format sourceFormat, const void* data)
        {
            s_RendererAPI->UploadTextureSubImage2D(texture, xOffset, yOffset, width, height, sourceFormat, data);
        }

        static void UploadTextureSubImage3D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, i32 zOffset,
                                            u32 width, u32 height, u32 depth,
                                            RHI::Format sourceFormat, const void* data)
        {
            s_RendererAPI->UploadTextureSubImage3D(texture, xOffset, yOffset, zOffset,
                                                   width, height, depth, sourceFormat, data);
        }

        [[nodiscard("Store this!")]] static bool ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                                  RHI::Format destFormat,
                                                                  sizet destSizeBytes, void* dest)
        {
            return s_RendererAPI->ReadTextureImage(texture, mipLevel, destFormat, destSizeBytes, dest);
        }

        [[nodiscard("Store this!")]] static bool ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                                     i32 x, i32 y, i32 z,
                                                                     u32 width, u32 height, u32 depth,
                                                                     RHI::Format destFormat,
                                                                     sizet destSizeBytes, void* dest)
        {
            return s_RendererAPI->ReadTextureSubImage(texture, mipLevel, x, y, z, width, height, depth,
                                                      destFormat, destSizeBytes, dest);
        }

        static void GetTextureDimensions(RHI::ResourceHandle texture, u32 mipLevel, u32& outWidth, u32& outHeight)
        {
            s_RendererAPI->GetTextureDimensions(texture, mipLevel, outWidth, outHeight);
        }

        [[nodiscard("Store this!")]] static bool QueryTextureFormat(RHI::ResourceHandle texture, u32 mipLevel,
                                                                    RHI::TextureFormatInfo& out)
        {
            return s_RendererAPI->QueryTextureFormat(texture, mipLevel, out);
        }

        [[nodiscard("Store this!")]] static RHI::ResourceHandle CreateMatchingTextureHandle(RHI::ResourceHandle source)
        {
            return s_RendererAPI->CreateMatchingTextureHandle(source);
        }

        static void TextureBarrier()
        {
            s_RendererAPI->TextureBarrier();
        }

        // Queries
        static void CreateQueries(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries)
        {
            s_RendererAPI->CreateQueries(type, outQueries);
        }

        static void DeleteQueries(std::span<const RHI::ResourceHandle> queries)
        {
            s_RendererAPI->DeleteQueries(queries);
        }

        static void BeginQuery(RHI::QueryType type, RHI::ResourceHandle query)
        {
            s_RendererAPI->BeginQuery(type, query);
        }

        static void EndQuery(RHI::QueryType type)
        {
            s_RendererAPI->EndQuery(type);
        }

        static void WriteTimestamp(RHI::ResourceHandle query)
        {
            s_RendererAPI->WriteTimestamp(query);
        }

        [[nodiscard("Store this!")]] static bool IsQueryResultAvailable(RHI::ResourceHandle query)
        {
            return s_RendererAPI->IsQueryResultAvailable(query);
        }

        [[nodiscard("Store this!")]] static u32 GetQueryResultU32(RHI::ResourceHandle query)
        {
            return s_RendererAPI->GetQueryResultU32(query);
        }

        [[nodiscard("Store this!")]] static u64 GetQueryResultU64(RHI::ResourceHandle query)
        {
            return s_RendererAPI->GetQueryResultU64(query);
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
        // The pipeline work deletes it by moving u_GridScale into a UBO.
        static void SetProgramUniformFloat(RHI::ResourceHandle program, std::string_view name, f32 value)
        {
            s_RendererAPI->SetProgramUniformFloat(program, name, value);
        }

        static RendererAPI& GetRendererAPI()
        {
            return *s_RendererAPI;
        }

      private:
        static Scope<RendererAPI> s_RendererAPI;
    };
} // namespace OloEngine
