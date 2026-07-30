#pragma once

#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <vector>
#include <string>
#include <unordered_map>

namespace OloEngine::Testing
{
    // A recording mock of RendererAPI for testing command dispatch without a real GPU.
    // Records all API calls as entries that can be inspected after dispatch.
    struct RecordedCall
    {
        std::string Name;
        CommandType SourceCommandType = CommandType::Invalid;

        // Generic parameter storage for validation
        u32 ParamU32_0 = 0;
        u32 ParamU32_1 = 0;
        u32 ParamU32_2 = 0;
        u32 ParamU32_3 = 0;
        f32 ParamF32_0 = 0.0f;
        bool ParamBool_0 = false;
        glm::vec4 ParamVec4_0 = glm::vec4(0);
    };

    class MockRendererAPI : public RendererAPI
    {
      public:
        MockRendererAPI() = default;
        ~MockRendererAPI() override = default;

        void SetMaxUniformBlockSize(u32 size)
        {
            m_MaxUniformBlockSize = size;
        }

        void SetSupportsInt64ShaderAtomics(bool supported)
        {
            m_SupportsInt64Atomics = supported;
        }

        // ----------------------------------------------------------------
        // Recording accessors
        // ----------------------------------------------------------------
        const std::vector<RecordedCall>& GetRecordedCalls() const
        {
            return m_Calls;
        }
        sizet GetCallCount() const
        {
            return m_Calls.size();
        }
        void ClearRecording()
        {
            m_Calls.clear();
            m_BindCount = 0;
            m_DrawCallCount = 0;
        }

        u32 GetBindCount() const
        {
            return m_BindCount;
        }
        u32 GetDrawCallCount() const
        {
            return m_DrawCallCount;
        }

        bool HasCall(const std::string& name) const
        {
            for (const auto& c : m_Calls)
                if (c.Name == name)
                    return true;
            return false;
        }

        sizet CountCalls(const std::string& name) const
        {
            sizet n = 0;
            for (const auto& c : m_Calls)
                if (c.Name == name)
                    ++n;
            return n;
        }

        // ----------------------------------------------------------------
        // RendererAPI overrides — all record and return immediately
        // ----------------------------------------------------------------
        void Init() override
        {
            Record("Init");
        }
        void SetViewport(u32 x, u32 y, u32 w, u32 h) override
        {
            RecordedCall c{ "SetViewport" };
            c.ParamU32_0 = x;
            c.ParamU32_1 = y;
            c.ParamU32_2 = w;
            c.ParamU32_3 = h;
            m_Calls.push_back(c);
            m_Viewport = { x, y, w, h };
        }
        void SetClearColor(const glm::vec4& color) override
        {
            RecordedCall c{ "SetClearColor" };
            c.ParamVec4_0 = color;
            m_Calls.push_back(c);
        }
        void Clear() override
        {
            Record("Clear");
        }
        void ClearDepthOnly() override
        {
            Record("ClearDepthOnly");
        }
        void ClearColorAndDepth() override
        {
            Record("ClearColorAndDepth");
        }
        Viewport GetViewport() const override
        {
            return m_Viewport;
        }

        // Draw calls
        void DrawArrays(const Ref<VertexArray>& /*va*/, u32 vertexCount) override
        {
            RecordedCall c{ "DrawArrays" };
            c.ParamU32_0 = vertexCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexed(const Ref<VertexArray>& /*va*/, u32 indexCount) override
        {
            RecordedCall c{ "DrawIndexed" };
            c.ParamU32_0 = indexCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedInstanced(const Ref<VertexArray>& /*va*/, u32 indexCount, u32 instanceCount) override
        {
            RecordedCall c{ "DrawIndexedInstanced" };
            c.ParamU32_0 = indexCount;
            c.ParamU32_1 = instanceCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawLines(const Ref<VertexArray>& /*va*/, u32 vertexCount) override
        {
            RecordedCall c{ "DrawLines" };
            c.ParamU32_0 = vertexCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedPatches(const Ref<VertexArray>& /*va*/, u32 indexCount, u32 patchVerts) override
        {
            RecordedCall c{ "DrawIndexedPatches" };
            c.ParamU32_0 = indexCount;
            c.ParamU32_1 = patchVerts;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedRaw(u32 vaoID, u32 indexCount) override
        {
            RecordedCall c{ "DrawIndexedRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedRaw(u32 vaoID, u32 indexCount, u32 baseIndex) override
        {
            RecordedCall c{ "DrawIndexedRawBase" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            c.ParamU32_2 = baseIndex;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedInstancedRaw(u32 vaoID, u32 indexCount, u32 baseIndex, u32 instanceCount) override
        {
            RecordedCall c{ "DrawIndexedInstancedRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            c.ParamU32_2 = baseIndex;
            c.ParamU32_3 = instanceCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVerts) override
        {
            RecordedCall c{ "DrawIndexedPatchesRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            c.ParamU32_2 = patchVerts;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }

        // State
        void SetLineWidth(f32 width) override
        {
            RecordedCall c{ "SetLineWidth" };
            c.ParamF32_0 = width;
            m_Calls.push_back(c);
        }
        void EnableCulling() override
        {
            Record("EnableCulling");
        }
        void DisableCulling() override
        {
            Record("DisableCulling");
        }
        void FrontCull() override
        {
            Record("FrontCull");
        }
        void BackCull() override
        {
            Record("BackCull");
        }
        void SetCullFace(RHI::CullMode /*face*/) override
        {
            Record("SetCullFace");
        }
        void SetDepthMask(bool value) override
        {
            RecordedCall c{ "SetDepthMask" };
            c.ParamBool_0 = value;
            m_Calls.push_back(c);
        }
        void SetDepthTest(bool value) override
        {
            RecordedCall c{ "SetDepthTest" };
            c.ParamBool_0 = value;
            m_Calls.push_back(c);
        }
        void SetDepthFunc(RHI::CompareOp /*func*/) override
        {
            Record("SetDepthFunc");
        }
        void SetBlendState(bool value) override
        {
            RecordedCall c{ "SetBlendState" };
            c.ParamBool_0 = value;
            m_Calls.push_back(c);
        }
        void SetBlendFunc(RHI::BlendFactor /*s*/, RHI::BlendFactor /*d*/) override
        {
            Record("SetBlendFunc");
        }
        void SetBlendEquation(RHI::BlendOp /*mode*/) override
        {
            Record("SetBlendEquation");
        }
        void EnableStencilTest() override
        {
            m_StencilEnabled = true;
            Record("EnableStencilTest");
        }
        void DisableStencilTest() override
        {
            m_StencilEnabled = false;
            Record("DisableStencilTest");
        }
        bool IsStencilTestEnabled() const override
        {
            return m_StencilEnabled;
        }
        void SetStencilFunc(RHI::CompareOp /*f*/, i32 /*ref*/, u32 /*mask*/) override
        {
            Record("SetStencilFunc");
        }
        void SetStencilOp(RHI::StencilOp /*sfail*/, RHI::StencilOp /*dpfail*/, RHI::StencilOp /*dppass*/) override
        {
            Record("SetStencilOp");
        }
        void SetStencilMask(u32 /*mask*/) override
        {
            Record("SetStencilMask");
        }
        void ClearStencil() override
        {
            Record("ClearStencil");
        }
        void SetPolygonMode(RHI::PolygonMode /*mode*/) override
        {
            Record("SetPolygonMode");
        }
        void EnableScissorTest() override
        {
            Record("EnableScissorTest");
        }
        void DisableScissorTest() override
        {
            Record("DisableScissorTest");
        }
        void SetScissorBox(i32 /*x*/, i32 /*y*/, u32 /*w*/, u32 /*h*/) override
        {
            Record("SetScissorBox");
        }

        void DrawElementsIndirect(const Ref<VertexArray>& /*va*/, u32 /*bufID*/) override
        {
            Record("DrawElementsIndirect");
            ++m_DrawCallCount;
        }
        void DrawArraysIndirect(const Ref<VertexArray>& /*va*/, u32 /*bufID*/) override
        {
            Record("DrawArraysIndirect");
            ++m_DrawCallCount;
        }
        void DrawElementsIndirectRaw(u32 /*vaoID*/, u32 /*bufID*/) override
        {
            Record("DrawElementsIndirectRaw");
            ++m_DrawCallCount;
        }
        void MultiDrawElementsIndirectCountRaw(u32 /*vaoID*/, u32 /*bufID*/, u32 /*indirectOffset*/, u32 /*paramBufID*/,
                                               u32 /*paramOffset*/, u32 /*maxDrawCount*/, u32 /*stride*/) override
        {
            Record("MultiDrawElementsIndirectCountRaw");
            ++m_DrawCallCount;
        }
        void DispatchCompute(u32 /*x*/, u32 /*y*/, u32 /*z*/) override
        {
            Record("DispatchCompute");
        }
        void MemoryBarrier(MemoryBarrierFlags /*flags*/) override
        {
            Record("MemoryBarrier");
        }

        void BindDefaultFramebuffer() override
        {
            Record("BindDefaultFramebuffer");
            ++m_BindCount;
        }
        void BlitFramebufferToDefault(u32 srcFboID, u32 width, u32 height) override
        {
            RecordedCall c{ "BlitFramebufferToDefault" };
            c.ParamU32_0 = srcFboID;
            c.ParamU32_1 = width;
            c.ParamU32_2 = height;
            m_Calls.push_back(c);
        }
        void BindTexture(u32 slot, u32 texID) override
        {
            RecordedCall c{ "BindTexture" };
            c.ParamU32_0 = slot;
            c.ParamU32_1 = texID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindImageTexture(u32 /*unit*/, u32 /*texID*/, u32 /*mip*/, bool /*layered*/, u32 /*layer*/,
                              RHI::Access /*access*/, RHI::Format /*fmt*/) override
        {
            Record("BindImageTexture");
            ++m_BindCount;
        }

        void SetPolygonOffset(f32 /*factor*/, f32 /*units*/) override
        {
            Record("SetPolygonOffset");
        }
        void EnableMultisampling() override
        {
            Record("EnableMultisampling");
        }
        void DisableMultisampling() override
        {
            Record("DisableMultisampling");
        }
        void SetColorMask(bool /*r*/, bool /*g*/, bool /*b*/, bool /*a*/) override
        {
            Record("SetColorMask");
        }
        void SetColorMaskForAttachment(u32 /*attachment*/, bool /*r*/, bool /*g*/, bool /*b*/, bool /*a*/) override
        {
            Record("SetColorMaskForAttachment");
        }
        void BeginConditionalRender(u32 queryID) override
        {
            RecordedCall c{ "BeginConditionalRender" };
            c.ParamU32_0 = queryID;
            m_Calls.push_back(c);
        }
        void EndConditionalRender() override
        {
            Record("EndConditionalRender");
        }
        // The mock stands in for a working device; the headless no-device path
        // is exercised through the real RendererAPI, which probes the backend.
        [[nodiscard("Store this!")]] bool IsDeviceAvailable() const override
        {
            return true;
        }

        [[nodiscard("Store this!")]] u32 GetMaxUniformBlockSize() const override
        {
            return m_MaxUniformBlockSize;
        }
        [[nodiscard("Store this!")]] bool SupportsInt64ShaderAtomics() const override
        {
            return m_SupportsInt64Atomics;
        }
        void SetBlendStateForAttachment(u32 attachment, bool enabled) override
        {
            RecordedCall c{ "SetBlendStateForAttachment" };
            c.ParamU32_0 = attachment;
            c.ParamBool_0 = enabled;
            m_Calls.push_back(c);
        }

        void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) override
        {
            RecordedCall c{ "SetBlendFuncForAttachment" };
            c.ParamU32_0 = attachment;
            c.ParamU32_1 = static_cast<u32>(src);
            c.ParamU32_2 = static_cast<u32>(dst);
            m_Calls.push_back(c);
        }

        void CopyImageSubData(u32 /*src*/, TextureTargetType /*srcT*/, u32 /*dst*/, TextureTargetType /*dstT*/,
                              u32 /*w*/, u32 /*h*/) override
        {
            Record("CopyImageSubData");
        }
        void CopyImageSubDataFull(u32 /*src*/, TextureTargetType /*srcT*/, i32 /*srcLvl*/, i32 /*srcZ*/,
                                  u32 /*dst*/, TextureTargetType /*dstT*/, i32 /*dstLvl*/, i32 /*dstZ*/,
                                  u32 /*w*/, u32 /*h*/) override
        {
            Record("CopyImageSubDataFull");
        }
        void CopyFramebufferToTexture(u32 /*texID*/, u32 /*w*/, u32 /*h*/) override
        {
            Record("CopyFramebufferToTexture");
        }

        void SetDrawBuffers(std::span<const u32> /*attachments*/) override
        {
            Record("SetDrawBuffers");
        }
        void RestoreAllDrawBuffers(u32 /*count*/) override
        {
            Record("RestoreAllDrawBuffers");
        }

        u32 CreateTexture2D(u32 /*w*/, u32 /*h*/, RHI::Format /*fmt*/) override
        {
            Record("CreateTexture2D");
            return m_NextTextureID++;
        }
        u32 CreateTextureCubemap(u32 /*w*/, u32 /*h*/, RHI::Format /*fmt*/) override
        {
            Record("CreateTextureCubemap");
            return m_NextTextureID++;
        }
        u32 CreateDepthArrayCompareOffView(u32 /*srcTextureID*/, u32 /*numLayers*/) override
        {
            Record("CreateDepthArrayCompareOffView");
            return m_NextTextureID++;
        }
        void SetTextureFilter(u32 /*texID*/, RHI::Filter /*minFilter*/, RHI::Filter /*magFilter*/) override
        {
            Record("SetTextureFilter");
        }
        void SetTextureWrap(u32 /*texID*/, RHI::AddressMode /*wrap*/) override
        {
            Record("SetTextureWrap");
        }
        void UploadTextureSubImage2D(u32 /*texID*/, u32 /*w*/, u32 /*h*/,
                                     RHI::Format /*sourceFormat*/, const void* /*data*/) override
        {
            Record("UploadTextureSubImage2D");
        }
        void DeleteTexture(u32 /*texID*/) override
        {
            Record("DeleteTexture");
        }

        // ----------------------------------------------------------------
        // Phase 2 step 2 additions (issue #691). Same recording convention as
        // above. Note these make the mock STRICTLY safer than before: the call
        // sites they replace issued raw glXxx() through glad, which in a
        // headless test is a null function pointer.
        // ----------------------------------------------------------------
        void BindUniformBuffer(u32 bindingPoint, u32 bufferID) override
        {
            RecordedCall c{ "BindUniformBuffer" };
            c.ParamU32_0 = bindingPoint;
            c.ParamU32_1 = bufferID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindStorageBuffer(u32 bindingPoint, u32 bufferID) override
        {
            RecordedCall c{ "BindStorageBuffer" };
            c.ParamU32_0 = bindingPoint;
            c.ParamU32_1 = bufferID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindShaderProgram(u32 programID) override
        {
            RecordedCall c{ "BindShaderProgram" };
            c.ParamU32_0 = programID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindVertexArrayRaw(u32 vaoID) override
        {
            RecordedCall c{ "BindVertexArrayRaw" };
            c.ParamU32_0 = vaoID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindFramebuffer(u32 framebufferID) override
        {
            RecordedCall c{ "BindFramebuffer" };
            c.ParamU32_0 = framebufferID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }

        void DrawBoundIndexed(RHI::PrimitiveTopology /*topology*/, u32 indexCount,
                              RHI::IndexType /*indexType*/, u32 baseIndex) override
        {
            RecordedCall c{ "DrawBoundIndexed" };
            c.ParamU32_0 = indexCount;
            c.ParamU32_1 = baseIndex;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawBoundIndexedInstanced(RHI::PrimitiveTopology /*topology*/, u32 indexCount,
                                       RHI::IndexType /*indexType*/, u32 baseIndex, u32 instanceCount) override
        {
            RecordedCall c{ "DrawBoundIndexedInstanced" };
            c.ParamU32_0 = indexCount;
            c.ParamU32_1 = baseIndex;
            c.ParamU32_2 = instanceCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawBoundArrays(RHI::PrimitiveTopology /*topology*/, u32 firstVertex, u32 vertexCount) override
        {
            RecordedCall c{ "DrawBoundArrays" };
            c.ParamU32_0 = firstVertex;
            c.ParamU32_1 = vertexCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void SetPatchVertexCount(u32 patchVertices) override
        {
            RecordedCall c{ "SetPatchVertexCount" };
            c.ParamU32_0 = patchVertices;
            m_Calls.push_back(c);
        }

        void SetFrontFace(RHI::FrontFace /*face*/) override
        {
            Record("SetFrontFace");
        }
        void SetBlendFuncSeparate(RHI::BlendFactor /*srcRGB*/, RHI::BlendFactor /*dstRGB*/,
                                  RHI::BlendFactor /*srcAlpha*/, RHI::BlendFactor /*dstAlpha*/) override
        {
            Record("SetBlendFuncSeparate");
        }
        void SetClearDepth(f32 depth) override
        {
            RecordedCall c{ "SetClearDepth" };
            c.ParamF32_0 = depth;
            m_Calls.push_back(c);
        }

        u32 CreateFramebuffer() override
        {
            Record("CreateFramebuffer");
            return m_NextFramebufferID++;
        }
        void DeleteFramebuffer(u32 /*framebufferID*/) override
        {
            Record("DeleteFramebuffer");
        }
        void AttachFramebufferColorTexture(u32 /*fb*/, u32 /*attachmentIndex*/, u32 /*texID*/, u32 /*mip*/) override
        {
            Record("AttachFramebufferColorTexture");
        }
        void AttachFramebufferDepthTexture(u32 /*fb*/, u32 /*texID*/, u32 /*mip*/) override
        {
            Record("AttachFramebufferDepthTexture");
        }
        [[nodiscard("Store this!")]] bool IsFramebufferComplete(u32 /*fb*/) override
        {
            Record("IsFramebufferComplete");
            return true;
        }
        void SetFramebufferDrawAttachments(u32 fb, std::span<const u32> attachmentIndices) override
        {
            RecordedCall c{ "SetFramebufferDrawAttachments" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = static_cast<u32>(attachmentIndices.size());
            m_Calls.push_back(c);
        }
        void SetFramebufferReadAttachment(u32 fb, u32 attachmentIndex) override
        {
            RecordedCall c{ "SetFramebufferReadAttachment" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = attachmentIndex;
            m_Calls.push_back(c);
        }
        void ClearFramebufferColorAttachment(u32 fb, u32 attachmentIndex, const glm::vec4& color) override
        {
            RecordedCall c{ "ClearFramebufferColorAttachment" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = attachmentIndex;
            c.ParamVec4_0 = color;
            m_Calls.push_back(c);
        }
        void ClearFramebufferDepth(u32 fb, f32 depth) override
        {
            RecordedCall c{ "ClearFramebufferDepth" };
            c.ParamU32_0 = fb;
            c.ParamF32_0 = depth;
            m_Calls.push_back(c);
        }
        void BlitFramebuffer(u32 src, u32 dst, i32 /*sx0*/, i32 /*sy0*/, i32 /*sx1*/, i32 /*sy1*/,
                             i32 /*dx0*/, i32 /*dy0*/, i32 /*dx1*/, i32 /*dy1*/,
                             RHI::BlitAspect /*aspect*/, RHI::Filter /*filter*/) override
        {
            RecordedCall c{ "BlitFramebuffer" };
            c.ParamU32_0 = src;
            c.ParamU32_1 = dst;
            m_Calls.push_back(c);
        }

        u32 CreateBuffer() override
        {
            Record("CreateBuffer");
            return m_NextBufferID++;
        }
        void DeleteBuffer(u32 /*bufferID*/) override
        {
            Record("DeleteBuffer");
        }
        void AllocateBufferStorage(u32 /*bufferID*/, u64 /*sizeBytes*/, RHI::MemoryResidency /*residency*/) override
        {
            Record("AllocateBufferStorage");
        }
        void* AllocatePersistentUploadStorage(u32 /*bufferID*/, u64 /*sizeBytes*/) override
        {
            Record("AllocatePersistentUploadStorage");
            // Null is the documented "mapping failed" answer, and every caller
            // already has a fallback path for it (VirtualMeshRegistry falls back
            // to direct uploads). Handing back a fake pointer the caller would
            // memcpy into is the option that would actually crash a test.
            return nullptr;
        }
        void UnmapBuffer(u32 /*bufferID*/) override
        {
            Record("UnmapBuffer");
        }
        void UploadBufferSubData(u32 /*bufferID*/, u64 /*offset*/, u64 /*size*/, const void* /*data*/) override
        {
            Record("UploadBufferSubData");
        }
        void ReadBufferSubData(u32 /*bufferID*/, u64 /*offset*/, u64 /*size*/, void* /*dest*/) override
        {
            Record("ReadBufferSubData");
        }
        void CopyBufferSubData(u32 /*src*/, u32 /*dst*/, u64 /*srcOff*/, u64 /*dstOff*/, u64 /*size*/) override
        {
            Record("CopyBufferSubData");
        }
        void ClearBufferUInt(u32 /*bufferID*/, u32 /*value*/) override
        {
            Record("ClearBufferUInt");
        }
        void ClearBufferFloat(u32 /*bufferID*/, f32 /*value*/) override
        {
            Record("ClearBufferFloat");
        }

        u32 CreateVertexArray() override
        {
            Record("CreateVertexArray");
            return m_NextVertexArrayID++;
        }
        void SetVertexArrayIndexBuffer(u32 /*vaoID*/, u32 /*bufferID*/) override
        {
            Record("SetVertexArrayIndexBuffer");
        }
        void DeleteVertexArray(u32 /*vaoID*/) override
        {
            Record("DeleteVertexArray");
        }

        void ClearTextureFloat(u32 texID, u32 /*mip*/, const glm::vec4& color) override
        {
            RecordedCall c{ "ClearTextureFloat" };
            c.ParamU32_0 = texID;
            c.ParamVec4_0 = color;
            m_Calls.push_back(c);
        }
        void ClearTextureUInt(u32 texID, u32 /*mip*/, u32 value) override
        {
            RecordedCall c{ "ClearTextureUInt" };
            c.ParamU32_0 = texID;
            c.ParamU32_1 = value;
            m_Calls.push_back(c);
        }
        void UploadTextureSubImage2D(u32 /*texID*/, i32 /*x*/, i32 /*y*/, u32 /*w*/, u32 /*h*/,
                                     RHI::Format /*sourceFormat*/, const void* /*data*/) override
        {
            Record("UploadTextureSubImage2DOffset");
        }
        void UploadTextureSubImage3D(u32 /*texID*/, i32 /*x*/, i32 /*y*/, i32 /*z*/,
                                     u32 /*w*/, u32 /*h*/, u32 /*d*/,
                                     RHI::Format /*sourceFormat*/, const void* /*data*/) override
        {
            Record("UploadTextureSubImage3D");
        }
        [[nodiscard("Store this!")]] bool ReadTextureImage(u32 /*texID*/, u32 /*mip*/, RHI::Format /*fmt*/,
                                                           sizet /*destSizeBytes*/, void* /*dest*/) override
        {
            Record("ReadTextureImage");
            // False, not true: there is no device behind the mock, so `dest` is
            // untouched. Claiming success would let a caller consume
            // uninitialised memory and call it a readback.
            return false;
        }
        [[nodiscard("Store this!")]] bool ReadTextureSubImage(u32 /*texID*/, u32 /*mip*/, i32 /*x*/, i32 /*y*/, i32 /*z*/,
                                                              u32 /*w*/, u32 /*h*/, u32 /*d*/, RHI::Format /*fmt*/,
                                                              sizet /*destSizeBytes*/, void* /*dest*/) override
        {
            Record("ReadTextureSubImage");
            return false;
        }
        void GetTextureDimensions(u32 /*texID*/, u32 /*mip*/, u32& outWidth, u32& outHeight) override
        {
            Record("GetTextureDimensions");
            outWidth = m_Viewport.width;
            outHeight = m_Viewport.height;
        }
        void TextureBarrier() override
        {
            Record("TextureBarrier");
        }

        void CreateQueries(RHI::QueryType /*type*/, std::span<u32> outQueryIDs) override
        {
            Record("CreateQueries");
            for (u32& id : outQueryIDs)
            {
                id = m_NextQueryID++;
            }
        }
        void DeleteQueries(std::span<const u32> /*queryIDs*/) override
        {
            Record("DeleteQueries");
        }
        void BeginQuery(RHI::QueryType /*type*/, u32 queryID) override
        {
            RecordedCall c{ "BeginQuery" };
            c.ParamU32_0 = queryID;
            m_Calls.push_back(c);
        }
        void EndQuery(RHI::QueryType /*type*/) override
        {
            Record("EndQuery");
        }
        [[nodiscard("Store this!")]] bool IsQueryResultAvailable(u32 /*queryID*/) override
        {
            Record("IsQueryResultAvailable");
            return false;
        }
        [[nodiscard("Store this!")]] u32 GetQueryResultU32(u32 /*queryID*/) override
        {
            Record("GetQueryResultU32");
            return 0;
        }
        [[nodiscard("Store this!")]] u64 GetQueryResultU64(u32 /*queryID*/) override
        {
            Record("GetQueryResultU64");
            return 0;
        }

        [[nodiscard("Store this!")]] u64 CreateFence() override
        {
            Record("CreateFence");
            return 0; // "no fence" — callers already handle a failed creation.
        }
        [[nodiscard("Store this!")]] RHI::FenceStatus ClientWaitFence(u64 /*fence*/, u64 /*timeoutNs*/) override
        {
            Record("ClientWaitFence");
            return RHI::FenceStatus::AlreadySignaled;
        }
        [[nodiscard("Store this!")]] bool IsFenceSignaled(u64 /*fence*/) override
        {
            Record("IsFenceSignaled");
            return true;
        }
        void DestroyFence(u64 /*fence*/) override
        {
            Record("DestroyFence");
        }

        void PushDebugGroup(u32 /*id*/, std::string_view /*label*/) override
        {
            Record("PushDebugGroup");
        }
        void PopDebugGroup() override
        {
            Record("PopDebugGroup");
        }

        void WaitForDeviceIdle() override
        {
            Record("WaitForDeviceIdle");
        }
        [[nodiscard("Store this!")]] u32 GetMaxFramebufferSamples() const override
        {
            return 8;
        }
        [[nodiscard("Store this!")]] u32 GetMaxColorTextureSamples() const override
        {
            return 8;
        }
        [[nodiscard("Store this!")]] u32 GetMaxDepthTextureSamples() const override
        {
            return 8;
        }
        void SetProgramUniformFloat(u32 programID, std::string_view /*name*/, f32 value) override
        {
            RecordedCall c{ "SetProgramUniformFloat" };
            c.ParamU32_0 = programID;
            c.ParamF32_0 = value;
            m_Calls.push_back(c);
        }

      private:
        void Record(const std::string& name)
        {
            m_Calls.push_back({ name });
        }

        std::vector<RecordedCall> m_Calls;
        u32 m_BindCount = 0;
        u32 m_DrawCallCount = 0;
        u32 m_NextTextureID = 1;
        u32 m_NextFramebufferID = 1;
        u32 m_NextBufferID = 1;
        u32 m_NextVertexArrayID = 1;
        u32 m_NextQueryID = 1;
        u32 m_MaxUniformBlockSize = 65536u;
        bool m_SupportsInt64Atomics = false;
        Viewport m_Viewport{ 0, 0, 1920, 1080 };
        bool m_StencilEnabled = false;
    };

} // namespace OloEngine::Testing
