#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
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
        std::vector<u32> ParamU32List;
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

        // Mesh-shader capability gate (issue #813) — default false, matching
        // a backend without VK_EXT_mesh_shader, so tests opt IN to the
        // supported path exactly as they do for int64 atomics.
        void SetSupportsMeshShaders(bool supported)
        {
            m_SupportsMeshShaders = supported;
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
        void DrawIndexedRaw(u32 vaoID, u32 indexCount)
        {
            RecordedCall c{ "DrawIndexedRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedRaw(u32 vaoID, u32 indexCount, u32 baseIndex)
        {
            RecordedCall c{ "DrawIndexedRawBase" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            c.ParamU32_2 = baseIndex;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedInstancedRaw(u32 vaoID, u32 indexCount, u32 baseIndex, u32 instanceCount)
        {
            RecordedCall c{ "DrawIndexedInstancedRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            c.ParamU32_2 = baseIndex;
            c.ParamU32_3 = instanceCount;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVerts)
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

        void DrawElementsIndirect(const Ref<VertexArray>& /*va*/, u32 /*bufID*/)
        {
            Record("DrawElementsIndirect");
            ++m_DrawCallCount;
        }
        void DrawArraysIndirect(const Ref<VertexArray>& /*va*/, u32 /*bufID*/)
        {
            Record("DrawArraysIndirect");
            ++m_DrawCallCount;
        }
        void DrawBoundElementsIndirect(u32 /*bufID*/)
        {
            Record("DrawBoundElementsIndirect");
            ++m_DrawCallCount;
        }
        void MultiDrawElementsIndirectCountRaw(u32 /*vaoID*/, u32 /*bufID*/, u32 /*indirectOffset*/, u32 /*paramBufID*/,
                                               u32 /*paramOffset*/, u32 /*maxDrawCount*/, u32 /*stride*/)
        {
            Record("MultiDrawElementsIndirectCountRaw");
            ++m_DrawCallCount;
        }
        void DispatchCompute(u32 /*x*/, u32 /*y*/, u32 /*z*/) override
        {
            Record("DispatchCompute");
        }
        void DrawMeshTasks(u32 groupsX, u32 groupsY, u32 groupsZ) override
        {
            // Facade contract: zero groups in any dimension is a legal no-op
            // that both real backends return from BEFORE any bookkeeping — the
            // mock must not certify call-count semantics the backends lack.
            if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
                return;
            RecordedCall c{ "DrawMeshTasks" };
            c.ParamU32_0 = groupsX;
            c.ParamU32_1 = groupsY;
            c.ParamU32_2 = groupsZ;
            m_Calls.push_back(c);
            ++m_DrawCallCount;
        }
        void MemoryBarrier(MemoryBarrierFlags /*flags*/) override
        {
            Record("MemoryBarrier");
        }
        void IssueBarrierBatch(MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers) override
        {
            // ParamU32_0 = the GL-lowered flag bits, ParamU32_1 = how many
            // per-resource transitions the batch carried — lets a headless
            // test assert both currencies arrived without a real backend.
            RecordedCall c{ "IssueBarrierBatch" };
            c.ParamU32_0 = static_cast<u32>(std::to_underlying(flags));
            c.ParamU32_1 = static_cast<u32>(barriers.size());
            m_Calls.push_back(std::move(c));
        }

        void BindDefaultFramebuffer() override
        {
            Record("BindDefaultFramebuffer");
            ++m_BindCount;
        }
        void BlitFramebufferToDefault(u32 srcFboID, u32 width, u32 height)
        {
            RecordedCall c{ "BlitFramebufferToDefault" };
            c.ParamU32_0 = srcFboID;
            c.ParamU32_1 = width;
            c.ParamU32_2 = height;
            m_Calls.push_back(c);
        }
        void BindTexture(u32 slot, u32 texID)
        {
            RecordedCall c{ "BindTexture" };
            c.ParamU32_0 = slot;
            c.ParamU32_1 = texID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindImageTexture(u32 /*unit*/, u32 /*texID*/, u32 /*mip*/, bool /*layered*/, u32 /*layer*/,
                              RHI::Access /*access*/, RHI::Format /*fmt*/)
        {
            Record("BindImageTexture");
            ++m_BindCount;
        }

        // ----------------------------------------------------------------
        // Handle-taking siblings (issue #691 step 3, slice 2).
        //
        // The mock plays the part of a backend, so it resolves exactly as a
        // backend does and delegates to the u32 form. That keeps every existing
        // assertion on recorded call names and parameters working unchanged,
        // and means a test driving the handle API observes the same native
        // values a real backend would have used — including 0 for a stale
        // handle, which is the behaviour worth being able to assert on.
        // ----------------------------------------------------------------
        void BindTexture(u32 slot, RHI::ResourceHandle texture) override
        {
            BindTexture(slot, Native(texture, RHI::ResourceKind::Texture));
        }
        void BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) override
        {
            BindUniformBuffer(bindingPoint, Native(buffer, RHI::ResourceKind::Buffer));
        }
        void BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) override
        {
            BindStorageBuffer(bindingPoint, Native(buffer, RHI::ResourceKind::Buffer));
        }
        void BindShaderProgram(RHI::ResourceHandle program) override
        {
            BindShaderProgram(Native(program, RHI::ResourceKind::ShaderProgram));
        }
        void BindVertexArrayRaw(RHI::ResourceHandle vertexArray) override
        {
            BindVertexArrayRaw(Native(vertexArray, RHI::ResourceKind::VertexArray));
        }
        void BindFramebuffer(RHI::ResourceHandle framebuffer) override
        {
            BindFramebuffer(Native(framebuffer, RHI::ResourceKind::Framebuffer));
        }

        // Raw-creator siblings (slice 4). The mock plays a backend: it creates
        // through its own u32 form and registers, so a test driving the handle
        // API sees the same identities a real backend would mint — and the
        // Delete* pair genuinely retires them, which is what makes
        // "handle goes stale after delete" assertable without a GL context.
        [[nodiscard]] RHI::ResourceHandle CreateTexture2DHandle(u32 width, u32 height, RHI::Format fmt) override
        {
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture,
                                                         CreateTexture2D(width, height, fmt),
                                                         RHI::Backend::OpenGL);
        }
        [[nodiscard]] RHI::ResourceHandle CreateTextureCubemapHandle(u32 width, u32 height, RHI::Format fmt) override
        {
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture,
                                                         CreateTextureCubemap(width, height, fmt),
                                                         RHI::Backend::OpenGL);
        }
        [[nodiscard]] RHI::ResourceHandle CreateFramebufferHandle() override
        {
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Framebuffer, CreateFramebuffer(),
                                                         RHI::Backend::OpenGL);
        }
        [[nodiscard]] RHI::ResourceHandle CreateBufferHandle() override
        {
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Buffer, CreateBuffer(),
                                                         RHI::Backend::OpenGL);
        }
        [[nodiscard]] RHI::ResourceHandle CreateVertexArrayHandle() override
        {
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::VertexArray, CreateVertexArray(),
                                                         RHI::Backend::OpenGL);
        }
        void DeleteTexture(RHI::ResourceHandle texture) override
        {
            DeleteTexture(Native(texture, RHI::ResourceKind::Texture));
            RHI::ResourceRegistry::Get().Unregister(texture);
        }
        void DeleteFramebuffer(RHI::ResourceHandle framebuffer) override
        {
            DeleteFramebuffer(Native(framebuffer, RHI::ResourceKind::Framebuffer));
            RHI::ResourceRegistry::Get().Unregister(framebuffer);
        }
        void DeleteBuffer(RHI::ResourceHandle buffer) override
        {
            DeleteBuffer(Native(buffer, RHI::ResourceKind::Buffer));
            RHI::ResourceRegistry::Get().Unregister(buffer);
        }
        void DeleteVertexArray(RHI::ResourceHandle vertexArray) override
        {
            DeleteVertexArray(Native(vertexArray, RHI::ResourceKind::VertexArray));
            RHI::ResourceRegistry::Get().Unregister(vertexArray);
        }

        // Texture-configuration handle forms. Added because migrating a real
        // pass (SSAO's noise texture) needed them — the earlier breadth-first
        // survey of the facade had missed all three, since none of them appear
        // in the bind or create/delete families it was organised around.
        //
        // These deliberately record under the SAME name as their u32 siblings:
        // a test asserting "the pass configured its texture" should keep
        // passing across the migration, and one that wants to distinguish the
        // currencies has the registry to check instead.
        void SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter) override
        {
            SetTextureFilter(Native(texture, RHI::ResourceKind::Texture), minFilter, magFilter);
        }
        void SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap) override
        {
            SetTextureWrap(Native(texture, RHI::ResourceKind::Texture), wrap);
        }
        void UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height,
                                     RHI::Format sourceFormat, const void* data) override
        {
            UploadTextureSubImage2D(Native(texture, RHI::ResourceKind::Texture), width, height, sourceFormat, data);
        }

        // Copy / clear / upload-at-offset / readback handle forms (slice 5).
        // Same "record under the u32 sibling's name" rule as the block above.
        void CopyImageSubData(RHI::ResourceHandle src, TextureTargetType srcTarget,
                              RHI::ResourceHandle dst, TextureTargetType dstTarget,
                              u32 width, u32 height) override
        {
            CopyImageSubData(Native(src, RHI::ResourceKind::Texture), srcTarget,
                             Native(dst, RHI::ResourceKind::Texture), dstTarget, width, height);
        }
        [[nodiscard("Store this!")]] bool ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                              i32 x, i32 y, i32 z,
                                                              u32 width, u32 height, u32 depth,
                                                              RHI::Format destFormat,
                                                              sizet destSizeBytes, void* dest) override
        {
            return ReadTextureSubImage(Native(texture, RHI::ResourceKind::Texture), mipLevel, x, y, z, width, height, depth,
                                       destFormat, destSizeBytes, dest);
        }
        void CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                  RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                  u32 width, u32 height) override
        {
            CopyImageSubDataFull(Native(src, RHI::ResourceKind::Texture), srcTarget, srcLevel, srcZ,
                                 Native(dst, RHI::ResourceKind::Texture), dstTarget, dstLevel, dstZ, width, height);
        }
        void ClearTextureFloat(RHI::ResourceHandle texture, u32 mipLevel, const glm::vec4& color) override
        {
            ClearTextureFloat(Native(texture, RHI::ResourceKind::Texture), mipLevel, color);
        }
        [[nodiscard("Store this!")]] bool ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                           RHI::Format destFormat,
                                                           sizet destSizeBytes, void* dest) override
        {
            return ReadTextureImage(Native(texture, RHI::ResourceKind::Texture), mipLevel, destFormat, destSizeBytes, dest);
        }
        void DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 patchVertices) override
        {
            DrawIndexedPatchesRaw(Native(vertexArray, RHI::ResourceKind::VertexArray), indexCount, patchVertices);
        }
        void DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex,
                                     u32 instanceCount) override
        {
            DrawIndexedInstancedRaw(Native(vertexArray, RHI::ResourceKind::VertexArray), indexCount, baseIndex, instanceCount);
        }
        void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount) override
        {
            DrawIndexedRaw(Native(vertexArray, RHI::ResourceKind::VertexArray), indexCount);
        }
        void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex) override
        {
            DrawIndexedRaw(Native(vertexArray, RHI::ResourceKind::VertexArray), indexCount, baseIndex);
        }
        void SetProgramUniformFloat(RHI::ResourceHandle program, std::string_view name, f32 value) override
        {
            SetProgramUniformFloat(Native(program, RHI::ResourceKind::ShaderProgram), name, value);
        }
        [[nodiscard]] RHI::ResourceHandle CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture,
                                                                               u32 numLayers) override
        {
            const u32 nativeView = CreateDepthArrayCompareOffView(Native(srcTexture, RHI::ResourceKind::Texture), numLayers);
            if (nativeView == 0u)
                return RHI::NullResource;
            return RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Texture, nativeView,
                                                         RHI::Backend::OpenGL);
        }

        // ----------------------------------------------------------------
        // The remaining handle forms (issue #691 step 3, item 4). Same rule as
        // the block above: resolve exactly as a backend does, delegate to the
        // native implementation, and record under the SAME call name — so every
        // existing assertion about which framebuffer/buffer/texture a pass
        // touched keeps working, with the recorded ParamU32 being the native id
        // a real backend would have used (0 for a stale handle).
        // ----------------------------------------------------------------
        void BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height) override
        {
            BlitFramebufferToDefault(Native(srcFramebuffer, RHI::ResourceKind::Framebuffer), width, height);
        }
        void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered,
                              u32 layer, RHI::Access access, RHI::Format format) override
        {
            BindImageTexture(unit, Native(texture, RHI::ResourceKind::Texture), mipLevel, layered, layer, access, format);
        }
        void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) override
        {
            DrawElementsIndirect(vertexArray, Native(indirectBuffer, RHI::ResourceKind::Buffer));
        }
        void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) override
        {
            DrawArraysIndirect(vertexArray, Native(indirectBuffer, RHI::ResourceKind::Buffer));
        }
        void DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer,
                                       RHI::PrimitiveTopology /*topology*/) override
        {
            DrawBoundElementsIndirect(Native(indirectBuffer, RHI::ResourceKind::Buffer));
        }
        void DispatchComputeIndirect(RHI::ResourceHandle /*argsBuffer*/, u32 /*offsetBytes*/) override
        {
            Record("DispatchComputeIndirect");
        }
        void MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indirectBuffer,
                                               u32 indirectOffsetBytes, RHI::ResourceHandle parameterBuffer,
                                               u32 parameterOffsetBytes, u32 maxDrawCount, u32 strideBytes) override
        {
            MultiDrawElementsIndirectCountRaw(Native(vertexArray, RHI::ResourceKind::VertexArray),
                                              Native(indirectBuffer, RHI::ResourceKind::Buffer), indirectOffsetBytes,
                                              Native(parameterBuffer, RHI::ResourceKind::Buffer), parameterOffsetBytes,
                                              maxDrawCount, strideBytes);
        }
        void CopyFramebufferToTexture(RHI::ResourceHandle texture, u32 width, u32 height) override
        {
            CopyFramebufferToTexture(Native(texture, RHI::ResourceKind::Texture), width, height);
        }

        // --- named framebuffers ---
        void AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                           RHI::ResourceHandle texture, u32 mipLevel) override
        {
            AttachFramebufferColorTexture(Native(framebuffer, RHI::ResourceKind::Framebuffer), attachmentIndex,
                                          Native(texture, RHI::ResourceKind::Texture), mipLevel);
        }
        void AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture,
                                           u32 mipLevel) override
        {
            AttachFramebufferDepthTexture(Native(framebuffer, RHI::ResourceKind::Framebuffer),
                                          Native(texture, RHI::ResourceKind::Texture), mipLevel);
        }
        [[nodiscard("Store this!")]] bool IsFramebufferComplete(RHI::ResourceHandle framebuffer) override
        {
            return IsFramebufferComplete(Native(framebuffer, RHI::ResourceKind::Framebuffer));
        }
        void SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                           std::span<const u32> attachmentIndices) override
        {
            SetFramebufferDrawAttachments(Native(framebuffer, RHI::ResourceKind::Framebuffer), attachmentIndices);
        }
        void RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer, u32 colorAttachmentCount) override
        {
            RestoreAllFramebufferDrawAttachments(Native(framebuffer, RHI::ResourceKind::Framebuffer),
                                                 colorAttachmentCount);
        }
        void SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex) override
        {
            SetFramebufferReadAttachment(Native(framebuffer, RHI::ResourceKind::Framebuffer), attachmentIndex);
        }
        void ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                             const glm::vec4& color) override
        {
            ClearFramebufferColorAttachment(Native(framebuffer, RHI::ResourceKind::Framebuffer), attachmentIndex, color);
        }
        void ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth) override
        {
            ClearFramebufferDepth(Native(framebuffer, RHI::ResourceKind::Framebuffer), depth);
        }
        void BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer,
                             i32 sx0, i32 sy0, i32 sx1, i32 sy1, i32 dx0, i32 dy0, i32 dx1, i32 dy1,
                             RHI::BlitAspect aspect, RHI::Filter filter) override
        {
            BlitFramebuffer(Native(srcFramebuffer, RHI::ResourceKind::Framebuffer),
                            Native(dstFramebuffer, RHI::ResourceKind::Framebuffer),
                            sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, aspect, filter);
        }

        // --- raw buffers ---
        void AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes,
                                   RHI::MemoryResidency residency) override
        {
            AllocateBufferStorage(Native(buffer, RHI::ResourceKind::Buffer), sizeBytes, residency);
        }
        void* AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes) override
        {
            return AllocatePersistentUploadStorage(Native(buffer, RHI::ResourceKind::Buffer), sizeBytes);
        }
        void UnmapBuffer(RHI::ResourceHandle buffer) override
        {
            UnmapBuffer(Native(buffer, RHI::ResourceKind::Buffer));
        }
        void UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes,
                                 const void* data) override
        {
            UploadBufferSubData(Native(buffer, RHI::ResourceKind::Buffer), offsetBytes, sizeBytes, data);
        }
        void ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest) override
        {
            ReadBufferSubData(Native(buffer, RHI::ResourceKind::Buffer), offsetBytes, sizeBytes, dest);
        }
        void CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer,
                               u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes) override
        {
            CopyBufferSubData(Native(srcBuffer, RHI::ResourceKind::Buffer),
                              Native(dstBuffer, RHI::ResourceKind::Buffer),
                              srcOffsetBytes, dstOffsetBytes, sizeBytes);
        }
        void ClearBufferUInt(RHI::ResourceHandle buffer, u32 value) override
        {
            ClearBufferUInt(Native(buffer, RHI::ResourceKind::Buffer), value);
        }
        void ClearBufferFloat(RHI::ResourceHandle buffer, f32 value) override
        {
            ClearBufferFloat(Native(buffer, RHI::ResourceKind::Buffer), value);
        }

        // --- vertex arrays / textures ---
        void SetVertexArrayIndexBuffer(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indexBuffer) override
        {
            SetVertexArrayIndexBuffer(Native(vertexArray, RHI::ResourceKind::VertexArray),
                                      Native(indexBuffer, RHI::ResourceKind::Buffer));
        }
        void ClearTextureUInt(RHI::ResourceHandle texture, u32 mipLevel, u32 value) override
        {
            ClearTextureUInt(Native(texture, RHI::ResourceKind::Texture), mipLevel, value);
        }
        void UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset,
                                     u32 width, u32 height, RHI::Format sourceFormat, const void* data) override
        {
            UploadTextureSubImage2D(Native(texture, RHI::ResourceKind::Texture), xOffset, yOffset, width, height,
                                    sourceFormat, data);
        }
        void UploadTextureSubImage3D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, i32 zOffset,
                                     u32 width, u32 height, u32 depth,
                                     RHI::Format sourceFormat, const void* data) override
        {
            UploadTextureSubImage3D(Native(texture, RHI::ResourceKind::Texture), xOffset, yOffset, zOffset,
                                    width, height, depth, sourceFormat, data);
        }
        void GetTextureDimensions(RHI::ResourceHandle texture, u32 mipLevel, u32& outWidth, u32& outHeight) override
        {
            GetTextureDimensions(Native(texture, RHI::ResourceKind::Texture), mipLevel, outWidth, outHeight);
        }

      private:
        // Kind-checked, mirroring Platform/OpenGL's Utils::ResolveNativeAs. An
        // untyped resolve would let a wrong-family handle (a buffer passed where
        // a texture is wanted) succeed here and fail in the real backend, so a
        // green mock test would say nothing about the shipping path.
        [[nodiscard]] static u32 Native(RHI::ResourceHandle handle, RHI::ResourceKind expected) noexcept
        {
            auto& registry = RHI::ResourceRegistry::Get();
            if (handle.IsValid() && registry.KindOf(handle) != expected)
                return 0u;
            return static_cast<u32>(registry.ResolveNativeForBackend(handle));
        }

      public:
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
        void BeginConditionalRender(RHI::ResourceHandle query) override
        {
            RecordedCall c{ "BeginConditionalRender" };
            c.ParamU32_0 = Native(query, RHI::ResourceKind::Query);
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
        [[nodiscard("Store this!")]] bool SupportsMeshShaders() const override
        {
            return m_SupportsMeshShaders;
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
                              u32 /*w*/, u32 /*h*/)
        {
            Record("CopyImageSubData");
        }
        void CopyImageSubDataFull(u32 /*src*/, TextureTargetType /*srcT*/, i32 /*srcLvl*/, i32 /*srcZ*/,
                                  u32 /*dst*/, TextureTargetType /*dstT*/, i32 /*dstLvl*/, i32 /*dstZ*/,
                                  u32 /*w*/, u32 /*h*/)
        {
            Record("CopyImageSubDataFull");
        }
        void CopyFramebufferToTexture(u32 /*texID*/, u32 /*w*/, u32 /*h*/)
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

        u32 CreateTexture2D(u32 /*w*/, u32 /*h*/, RHI::Format /*fmt*/)
        {
            Record("CreateTexture2D");
            return m_NextTextureID++;
        }
        u32 CreateTextureCubemap(u32 /*w*/, u32 /*h*/, RHI::Format /*fmt*/)
        {
            Record("CreateTextureCubemap");
            return m_NextTextureID++;
        }
        u32 CreateDepthArrayCompareOffView(u32 /*srcTextureID*/, u32 /*numLayers*/)
        {
            Record("CreateDepthArrayCompareOffView");
            return m_NextTextureID++;
        }
        void SetTextureFilter(u32 /*texID*/, RHI::Filter /*minFilter*/, RHI::Filter /*magFilter*/)
        {
            Record("SetTextureFilter");
        }
        void SetTextureWrap(u32 /*texID*/, RHI::AddressMode /*wrap*/)
        {
            Record("SetTextureWrap");
        }
        void UploadTextureSubImage2D(u32 /*texID*/, u32 /*w*/, u32 /*h*/,
                                     RHI::Format /*sourceFormat*/, const void* /*data*/)
        {
            Record("UploadTextureSubImage2D");
        }
        void DeleteTexture(u32 /*texID*/)
        {
            Record("DeleteTexture");
        }

        // ----------------------------------------------------------------
        // Phase 2 step 2 additions (issue #691). Same recording convention as
        // above. Note these make the mock STRICTLY safer than before: the call
        // sites they replace issued raw glXxx() through glad, which in a
        // headless test is a null function pointer.
        // ----------------------------------------------------------------
        void BindUniformBuffer(u32 bindingPoint, u32 bufferID)
        {
            RecordedCall c{ "BindUniformBuffer" };
            c.ParamU32_0 = bindingPoint;
            c.ParamU32_1 = bufferID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindStorageBuffer(u32 bindingPoint, u32 bufferID)
        {
            RecordedCall c{ "BindStorageBuffer" };
            c.ParamU32_0 = bindingPoint;
            c.ParamU32_1 = bufferID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindShaderProgram(u32 programID)
        {
            RecordedCall c{ "BindShaderProgram" };
            c.ParamU32_0 = programID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindVertexArrayRaw(u32 vaoID)
        {
            RecordedCall c{ "BindVertexArrayRaw" };
            c.ParamU32_0 = vaoID;
            m_Calls.push_back(c);
            ++m_BindCount;
        }
        void BindFramebuffer(u32 framebufferID)
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

        u32 CreateFramebuffer()
        {
            Record("CreateFramebuffer");
            return m_NextFramebufferID++;
        }
        void DeleteFramebuffer(u32 /*framebufferID*/)
        {
            Record("DeleteFramebuffer");
        }
        void AttachFramebufferColorTexture(u32 /*fb*/, u32 /*attachmentIndex*/, u32 /*texID*/, u32 /*mip*/)
        {
            Record("AttachFramebufferColorTexture");
        }
        void AttachFramebufferDepthTexture(u32 /*fb*/, u32 /*texID*/, u32 /*mip*/)
        {
            Record("AttachFramebufferDepthTexture");
        }
        [[nodiscard("Store this!")]] bool IsFramebufferComplete(u32 /*fb*/)
        {
            Record("IsFramebufferComplete");
            return true;
        }
        void SetFramebufferDrawAttachments(u32 fb, std::span<const u32> attachmentIndices)
        {
            RecordedCall c{ "SetFramebufferDrawAttachments" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = static_cast<u32>(attachmentIndices.size());
            // The indices themselves, not just how many: the interesting
            // assertions are about WHICH attachment a pass steers a draw into
            // (and whether a slot is RHI::NoAttachment), which a count cannot
            // distinguish — DecalRenderPass's four modes all pass 5 entries.
            c.ParamU32List.assign(attachmentIndices.begin(), attachmentIndices.end());
            m_Calls.push_back(c);
        }
        void RestoreAllFramebufferDrawAttachments(u32 fb, u32 colorAttachmentCount)
        {
            RecordedCall c{ "RestoreAllFramebufferDrawAttachments" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = colorAttachmentCount;
            m_Calls.push_back(c);
        }
        void SetFramebufferReadAttachment(u32 fb, u32 attachmentIndex)
        {
            RecordedCall c{ "SetFramebufferReadAttachment" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = attachmentIndex;
            m_Calls.push_back(c);
        }
        void ClearFramebufferColorAttachment(u32 fb, u32 attachmentIndex, const glm::vec4& color)
        {
            RecordedCall c{ "ClearFramebufferColorAttachment" };
            c.ParamU32_0 = fb;
            c.ParamU32_1 = attachmentIndex;
            c.ParamVec4_0 = color;
            m_Calls.push_back(c);
        }
        void ClearFramebufferDepth(u32 fb, f32 depth)
        {
            RecordedCall c{ "ClearFramebufferDepth" };
            c.ParamU32_0 = fb;
            c.ParamF32_0 = depth;
            m_Calls.push_back(c);
        }
        void BlitFramebuffer(u32 src, u32 dst, i32 /*sx0*/, i32 /*sy0*/, i32 /*sx1*/, i32 /*sy1*/,
                             i32 /*dx0*/, i32 /*dy0*/, i32 /*dx1*/, i32 /*dy1*/,
                             RHI::BlitAspect /*aspect*/, RHI::Filter /*filter*/)
        {
            RecordedCall c{ "BlitFramebuffer" };
            c.ParamU32_0 = src;
            c.ParamU32_1 = dst;
            m_Calls.push_back(c);
        }

        u32 CreateBuffer()
        {
            Record("CreateBuffer");
            return m_NextBufferID++;
        }
        void DeleteBuffer(u32 /*bufferID*/)
        {
            Record("DeleteBuffer");
        }
        void AllocateBufferStorage(u32 /*bufferID*/, u64 /*sizeBytes*/, RHI::MemoryResidency /*residency*/)
        {
            Record("AllocateBufferStorage");
        }
        void* AllocatePersistentUploadStorage(u32 /*bufferID*/, u64 /*sizeBytes*/)
        {
            Record("AllocatePersistentUploadStorage");
            // Null is the documented "mapping failed" answer, and every caller
            // already has a fallback path for it (VirtualMeshRegistry falls back
            // to direct uploads). Handing back a fake pointer the caller would
            // memcpy into is the option that would actually crash a test.
            return nullptr;
        }
        void UnmapBuffer(u32 /*bufferID*/)
        {
            Record("UnmapBuffer");
        }
        void UploadBufferSubData(u32 /*bufferID*/, u64 /*offset*/, u64 /*size*/, const void* /*data*/)
        {
            Record("UploadBufferSubData");
        }
        void ReadBufferSubData(u32 /*bufferID*/, u64 /*offset*/, u64 /*size*/, void* /*dest*/)
        {
            Record("ReadBufferSubData");
        }
        void CopyBufferSubData(u32 /*src*/, u32 /*dst*/, u64 /*srcOff*/, u64 /*dstOff*/, u64 /*size*/)
        {
            Record("CopyBufferSubData");
        }
        void ClearBufferUInt(u32 /*bufferID*/, u32 /*value*/)
        {
            Record("ClearBufferUInt");
        }
        void ClearBufferFloat(u32 /*bufferID*/, f32 /*value*/)
        {
            Record("ClearBufferFloat");
        }

        u32 CreateVertexArray()
        {
            Record("CreateVertexArray");
            return m_NextVertexArrayID++;
        }
        void SetVertexArrayIndexBuffer(u32 /*vaoID*/, u32 /*bufferID*/)
        {
            Record("SetVertexArrayIndexBuffer");
        }
        void DeleteVertexArray(u32 /*vaoID*/)
        {
            Record("DeleteVertexArray");
        }

        void ClearTextureFloat(u32 texID, u32 /*mip*/, const glm::vec4& color)
        {
            RecordedCall c{ "ClearTextureFloat" };
            c.ParamU32_0 = texID;
            c.ParamVec4_0 = color;
            m_Calls.push_back(c);
        }
        void ClearTextureUInt(u32 texID, u32 /*mip*/, u32 value)
        {
            RecordedCall c{ "ClearTextureUInt" };
            c.ParamU32_0 = texID;
            c.ParamU32_1 = value;
            m_Calls.push_back(c);
        }
        void UploadTextureSubImage2D(u32 /*texID*/, i32 /*x*/, i32 /*y*/, u32 /*w*/, u32 /*h*/,
                                     RHI::Format /*sourceFormat*/, const void* /*data*/)
        {
            Record("UploadTextureSubImage2DOffset");
        }
        void UploadTextureSubImage3D(u32 /*texID*/, i32 /*x*/, i32 /*y*/, i32 /*z*/,
                                     u32 /*w*/, u32 /*h*/, u32 /*d*/,
                                     RHI::Format /*sourceFormat*/, const void* /*data*/)
        {
            Record("UploadTextureSubImage3D");
        }
        [[nodiscard("Store this!")]] bool ReadTextureImage(u32 /*texID*/, u32 /*mip*/, RHI::Format /*fmt*/,
                                                           sizet /*destSizeBytes*/, void* /*dest*/)
        {
            Record("ReadTextureImage");
            // False, not true: there is no device behind the mock, so `dest` is
            // untouched. Claiming success would let a caller consume
            // uninitialised memory and call it a readback.
            return false;
        }
        [[nodiscard("Store this!")]] bool ReadTextureSubImage(u32 /*texID*/, u32 /*mip*/, i32 /*x*/, i32 /*y*/, i32 /*z*/,
                                                              u32 /*w*/, u32 /*h*/, u32 /*d*/, RHI::Format /*fmt*/,
                                                              sizet /*destSizeBytes*/, void* /*dest*/)
        {
            Record("ReadTextureSubImage");
            return false;
        }
        void GetTextureDimensions(u32 /*texID*/, u32 /*mip*/, u32& outWidth, u32& outHeight)
        {
            Record("GetTextureDimensions");
            outWidth = m_Viewport.width;
            outHeight = m_Viewport.height;
        }
        void TextureBarrier() override
        {
            Record("TextureBarrier");
        }

        // Mints real registry entries, like the backend. A test can therefore
        // assert that a deleted query's handle goes stale — which is the whole
        // reason queries became identities (issue #691 step 3, item 4).
        void CreateQueries(RHI::QueryType /*type*/, std::span<RHI::ResourceHandle> outQueries) override
        {
            Record("CreateQueries");
            for (RHI::ResourceHandle& query : outQueries)
            {
                query = RHI::ResourceRegistry::Get().Register(RHI::ResourceKind::Query, m_NextQueryID++,
                                                              RHI::Backend::OpenGL);
            }
        }
        void DeleteQueries(std::span<const RHI::ResourceHandle> queries) override
        {
            Record("DeleteQueries");
            for (const RHI::ResourceHandle query : queries)
            {
                RHI::ResourceRegistry::Get().Unregister(query);
            }
        }
        void BeginQuery(RHI::QueryType /*type*/, RHI::ResourceHandle query) override
        {
            RecordedCall c{ "BeginQuery" };
            c.ParamU32_0 = Native(query, RHI::ResourceKind::Query);
            m_Calls.push_back(c);
        }
        void EndQuery(RHI::QueryType /*type*/) override
        {
            Record("EndQuery");
        }
        void WriteTimestamp(RHI::ResourceHandle query) override
        {
            RecordedCall c{ "WriteTimestamp" };
            c.ParamU32_0 = Native(query, RHI::ResourceKind::Query);
            m_Calls.push_back(c);
        }
        [[nodiscard("Store this!")]] bool IsQueryResultAvailable(RHI::ResourceHandle /*query*/) override
        {
            Record("IsQueryResultAvailable");
            return false;
        }
        [[nodiscard("Store this!")]] u32 GetQueryResultU32(RHI::ResourceHandle /*query*/) override
        {
            Record("GetQueryResultU32");
            return 0;
        }
        [[nodiscard("Store this!")]] u64 GetQueryResultU64(RHI::ResourceHandle /*query*/) override
        {
            Record("GetQueryResultU64");
            return 0;
        }

        [[nodiscard("Store this!")]] u64 CreateFence() override
        {
            Record("CreateFence");
            // A non-zero opaque handle, so the SUCCESS path is what tests
            // exercise by default. Returning 0 made every caller take its
            // creation-failed branch, which meant the mock could only ever
            // cover the error path (and made FrameResourceManager log an error
            // on a perfectly healthy test).
            return m_NextFenceHandle++;
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
        void SetProgramUniformFloat(u32 programID, std::string_view /*name*/, f32 value)
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
        u64 m_NextFenceHandle = 1;
        u32 m_MaxUniformBlockSize = 65536u;
        bool m_SupportsInt64Atomics = false;
        bool m_SupportsMeshShaders = false;
        Viewport m_Viewport{ 0, 0, 1920, 1080 };
        bool m_StencilEnabled = false;
    };

} // namespace OloEngine::Testing
