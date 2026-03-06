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
                    n++;
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
            m_DrawCallCount++;
        }
        void DrawIndexed(const Ref<VertexArray>& /*va*/, u32 indexCount) override
        {
            RecordedCall c{ "DrawIndexed" };
            c.ParamU32_0 = indexCount;
            m_Calls.push_back(c);
            m_DrawCallCount++;
        }
        void DrawIndexedInstanced(const Ref<VertexArray>& /*va*/, u32 indexCount, u32 instanceCount) override
        {
            RecordedCall c{ "DrawIndexedInstanced" };
            c.ParamU32_0 = indexCount;
            c.ParamU32_1 = instanceCount;
            m_Calls.push_back(c);
            m_DrawCallCount++;
        }
        void DrawLines(const Ref<VertexArray>& /*va*/, u32 vertexCount) override
        {
            RecordedCall c{ "DrawLines" };
            c.ParamU32_0 = vertexCount;
            m_Calls.push_back(c);
            m_DrawCallCount++;
        }
        void DrawIndexedPatches(const Ref<VertexArray>& /*va*/, u32 indexCount, u32 patchVerts) override
        {
            RecordedCall c{ "DrawIndexedPatches" };
            c.ParamU32_0 = indexCount;
            c.ParamU32_1 = patchVerts;
            m_Calls.push_back(c);
            m_DrawCallCount++;
        }
        void DrawIndexedRaw(u32 vaoID, u32 indexCount) override
        {
            RecordedCall c{ "DrawIndexedRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            m_Calls.push_back(c);
            m_DrawCallCount++;
        }
        void DrawIndexedPatchesRaw(u32 vaoID, u32 indexCount, u32 patchVerts) override
        {
            RecordedCall c{ "DrawIndexedPatchesRaw" };
            c.ParamU32_0 = vaoID;
            c.ParamU32_1 = indexCount;
            c.ParamU32_2 = patchVerts;
            m_Calls.push_back(c);
            m_DrawCallCount++;
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
        void SetCullFace(GLenum /*face*/) override
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
        void SetDepthFunc(GLenum /*func*/) override
        {
            Record("SetDepthFunc");
        }
        void SetBlendState(bool value) override
        {
            RecordedCall c{ "SetBlendState" };
            c.ParamBool_0 = value;
            m_Calls.push_back(c);
        }
        void SetBlendFunc(GLenum /*s*/, GLenum /*d*/) override
        {
            Record("SetBlendFunc");
        }
        void SetBlendEquation(GLenum /*mode*/) override
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
        void SetStencilFunc(GLenum /*f*/, GLint /*ref*/, GLuint /*mask*/) override
        {
            Record("SetStencilFunc");
        }
        void SetStencilOp(GLenum /*sfail*/, GLenum /*dpfail*/, GLenum /*dppass*/) override
        {
            Record("SetStencilOp");
        }
        void SetStencilMask(GLuint /*mask*/) override
        {
            Record("SetStencilMask");
        }
        void ClearStencil() override
        {
            Record("ClearStencil");
        }
        void SetPolygonMode(GLenum /*face*/, GLenum /*mode*/) override
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
        void SetScissorBox(GLint /*x*/, GLint /*y*/, GLsizei /*w*/, GLsizei /*h*/) override
        {
            Record("SetScissorBox");
        }

        void DrawElementsIndirect(const Ref<VertexArray>& /*va*/, u32 /*bufID*/) override
        {
            Record("DrawElementsIndirect");
            m_DrawCallCount++;
        }
        void DrawArraysIndirect(const Ref<VertexArray>& /*va*/, u32 /*bufID*/) override
        {
            Record("DrawArraysIndirect");
            m_DrawCallCount++;
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
            m_BindCount++;
        }
        void BindTexture(u32 slot, u32 texID) override
        {
            RecordedCall c{ "BindTexture" };
            c.ParamU32_0 = slot;
            c.ParamU32_1 = texID;
            m_Calls.push_back(c);
            m_BindCount++;
        }
        void BindImageTexture(u32 /*unit*/, u32 /*texID*/, u32 /*mip*/, bool /*layered*/, u32 /*layer*/, GLenum /*access*/, GLenum /*fmt*/) override
        {
            Record("BindImageTexture");
            m_BindCount++;
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
        void SetBlendStateForAttachment(u32 /*attachment*/, bool /*enabled*/) override
        {
            Record("SetBlendStateForAttachment");
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

        u32 CreateTexture2D(u32 /*w*/, u32 /*h*/, GLenum /*fmt*/) override
        {
            Record("CreateTexture2D");
            return m_NextTextureID++;
        }
        u32 CreateTextureCubemap(u32 /*w*/, u32 /*h*/, GLenum /*fmt*/) override
        {
            Record("CreateTextureCubemap");
            return m_NextTextureID++;
        }
        void SetTextureParameter(u32 /*texID*/, GLenum /*pname*/, GLint /*value*/) override
        {
            Record("SetTextureParameter");
        }
        void UploadTextureSubImage2D(u32 /*texID*/, u32 /*w*/, u32 /*h*/,
                                     GLenum /*fmt*/, GLenum /*type*/, const void* /*data*/) override
        {
            Record("UploadTextureSubImage2D");
        }
        void DeleteTexture(u32 /*texID*/) override
        {
            Record("DeleteTexture");
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
        Viewport m_Viewport{ 0, 0, 1920, 1080 };
        bool m_StencilEnabled = false;
    };

} // namespace OloEngine::Testing
