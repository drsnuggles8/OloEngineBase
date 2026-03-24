#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderWarmup.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Core/Window.h"

namespace OloEngine
{
    // ── Boot shader source (NDC fullscreen triangle + bitmap font) ───────
    // Draws a dark background with a centered progress bar, phase label,
    // and a "current / total" counter rendered with a 4×5 pixel font.

    static constexpr const char* s_BootVertexSrc = R"glsl(
#version 450 core
layout(location = 0) in vec3 a_Position;
layout(location = 0) out vec2 v_UV;

void main()
{
    v_UV = a_Position.xy * 0.5 + 0.5;
    gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}
)glsl";

    static constexpr const char* s_BootFragmentSrc = R"glsl(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Color;

layout(std140, binding = 26) uniform BootData
{
    float u_Progress;       // 0..1
    int   u_CurrentCount;   // current shader number
    int   u_TotalCount;     // total shader count
    int   u_Phase;          // 0=2D, 1=3D, 2=post-process, 3=linking
};

// ── 4x5 bitmap font ────────────────────────────────────────────────
// Each uint packs 5 rows of 4 pixels: row0 in bits [0:3] .. row4 in [16:19].
// Bit 0 of each nibble = leftmost pixel.
// Index: 0-9 = digits, 10-35 = A-Z, 36 = space, 37 = '/'
const uint FONT[38] = uint[38](
    0x69996u, 0x72232u, 0xF2496u, 0x78687u, 0x44F55u,  // 0-4
    0x7871Fu, 0x69716u, 0x2248Fu, 0x69696u, 0x68E96u,  // 5-9
    0x99F96u, 0x79797u, 0x61116u, 0x79997u, 0xF171Fu,  // A-E
    0x1171Fu, 0x69D16u, 0x99F99u, 0x72227u, 0x6544Cu,  // F-J
    0x95359u, 0xF1111u, 0x99FF9u, 0x9DFB9u, 0x69996u,  // K-O
    0x11797u, 0x86996u, 0x95797u, 0x68616u, 0x2222Fu,  // P-T
    0x69999u, 0x66999u, 0x9FF99u, 0x99699u, 0x22699u,  // U-Y
    0xF168Fu,                                            // Z
    0x00000u,                                            // space
    0x01248u                                             // '/'
);

bool fontPixel(int idx, int px, int py)
{
    if (idx < 0 || idx >= 38 || px < 0 || px >= 4 || py < 0 || py >= 5)
        return false;
    return (FONT[idx] & (1u << (uint(py) * 4u + uint(px)))) != 0u;
}

// ── Phase messages packed into one array ───────────────────────────
// 0: "2D SHADERS"  1: "3D SHADERS"  2: "POST PROCESS"  3: "LINKING"
const int MSG[52] = int[52](
    2,13,36,28,17,10,13,14,27,28,                       // offset  0, len 10
    3,13,36,28,17,10,13,14,27,28,                       // offset 10, len 10
    25,24,28,29,36,25,27,24,12,14,28,28,18,23,16,       // offset 20, len 15
    21,18,23,20,18,23,16,36,28,17,10,13,14,27,28,       // offset 35, len 15
    36,36                                                // padding
);

int msgOffset(int p) { return p <= 0 ? 0 : p == 1 ? 10 : p == 2 ? 20 : 35; }
int msgLen(int p)    { return p <= 1 ? 10 : 15; }

// Returns char index for counter digit at position `pos` in "curr / total"
int counterChar(int pos, int curr, int tot)
{
    int p = 0;
    if (curr >= 10) { if (pos == p) return curr / 10; p++; }
    if (pos == p) return curr % 10; p++;
    if (pos == p) return 36; p++;   // space
    if (pos == p) return 37; p++;   // '/'
    if (pos == p) return 36; p++;   // space
    if (tot >= 10) { if (pos == p) return tot / 10; p++; }
    if (pos == p) return tot % 10;
    return -1;
}
int counterLen(int curr, int tot)
{
    return (curr >= 10 ? 2 : 1) + 3 + (tot >= 10 ? 2 : 1);
}

void main()
{
    // Character cell size in UV  (tuned for ~1280x720)
    const float CW = 0.016;   // char pixel width
    const float CH = 0.036;   // char pixel height
    const float GAP = 0.004;  // inter-char gap
    const float CELL = CW + GAP;

    vec3 bg          = vec3(0.08);
    vec3 labelColor  = vec3(0.82);
    vec3 cntColor    = vec3(0.50, 0.65, 0.80);
    vec3 color       = bg;
    bool lit = false;

    // ── Phase label (centred above bar) ────────────────────────────
    int mLen  = msgLen(u_Phase);
    int mOff  = msgOffset(u_Phase);
    float mW  = float(mLen) * CELL;
    float mX  = 0.5 - mW * 0.5;
    float mY  = 0.60;

    int ci = int((v_UV.x - mX) / CELL);
    if (ci >= 0 && ci < mLen)
    {
        float ox = mX + float(ci) * CELL;
        vec2 lc  = (v_UV - vec2(ox, mY)) / vec2(CW, CH);
        if (lc.x >= 0.0 && lc.x < 1.0 && lc.y >= 0.0 && lc.y < 1.0)
        {
            int px = int(lc.x * 4.0);
            int py = 4 - int(lc.y * 5.0);
            if (fontPixel(MSG[mOff + ci], px, py)) { color = labelColor; lit = true; }
        }
    }

    // ── Counter  "curr / total" (centred, just above bar) ──────────
    if (!lit && u_TotalCount > 0)
    {
        int cLen = counterLen(u_CurrentCount, u_TotalCount);
        float cW = float(cLen) * CELL;
        float cX = 0.5 - cW * 0.5;
        float cY = 0.555;

        int ki = int((v_UV.x - cX) / CELL);
        if (ki >= 0 && ki < cLen)
        {
            int ch = counterChar(ki, u_CurrentCount, u_TotalCount);
            if (ch >= 0)
            {
                float ox2 = cX + float(ki) * CELL;
                vec2 lc2  = (v_UV - vec2(ox2, cY)) / vec2(CW, CH);
                if (lc2.x >= 0.0 && lc2.x < 1.0 && lc2.y >= 0.0 && lc2.y < 1.0)
                {
                    int px2 = int(lc2.x * 4.0);
                    int py2 = 4 - int(lc2.y * 5.0);
                    if (fontPixel(ch, px2, py2)) { color = cntColor; lit = true; }
                }
            }
        }
    }

    // ── Progress bar ───────────────────────────────────────────────
    if (!lit)
    {
        vec2 barMin = vec2(0.20, 0.47);
        vec2 barMax = vec2(0.80, 0.53);
        vec2 oMin   = barMin - vec2(0.003);
        vec2 oMax   = barMax + vec2(0.003);

        if (v_UV.x >= oMin.x && v_UV.x <= oMax.x &&
            v_UV.y >= oMin.y && v_UV.y <= oMax.y)
            color = vec3(0.25);

        if (v_UV.x >= barMin.x && v_UV.x <= barMax.x &&
            v_UV.y >= barMin.y && v_UV.y <= barMax.y)
        {
            color = vec3(0.12);
            float fillX = barMin.x + u_Progress * (barMax.x - barMin.x);
            if (v_UV.x <= fillX)
            {
                float t = (v_UV.x - barMin.x) / (barMax.x - barMin.x);
                color = mix(vec3(0.1, 0.6, 0.7), vec3(0.2, 0.4, 0.9), t);
            }
        }
    }

    o_Color = vec4(color, 1.0);
}
)glsl";

    // UBO data that matches the shader's std140 BootData block
    struct BootUBOData
    {
        f32 Progress;
        i32 CurrentCount;
        i32 TotalCount;
        i32 Phase;
    };

    static Ref<Shader> s_BootShader = nullptr;
    static Ref<UniformBuffer> s_BootUBO = nullptr;

    void ShaderWarmup::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_BootShader)
            return;

        s_BootShader = Shader::Create("__WarmupBoot", s_BootVertexSrc, s_BootFragmentSrc);
        s_BootUBO = UniformBuffer::Create(sizeof(BootUBOData), ShaderBindingLayout::UBO_BOOT);
        OLO_CORE_INFO("ShaderWarmup: Boot shader compiled");
    }

    void ShaderWarmup::RenderProgressFrame(const f32 progress, Window& window, const std::string_view label,
                                           const i32 current, const i32 total, const i32 phase)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_TRACE("RenderProgressFrame: progress={:.2f}, label='{}', bootShader={}, bootReady={}, bootStatus={}",
                       progress, label, s_BootShader != nullptr, s_BootShader ? s_BootShader->IsReady() : false,
                       s_BootShader ? static_cast<int>(s_BootShader->GetCompilationStatus()) : -1);

        if (!s_BootShader || !s_BootShader->IsReady())
            return;

        window.PollEvents();
        window.SetTitle("OloEngine — Loading " + std::string(label) + " (" +
                        std::to_string(static_cast<int>(progress * 100.0f)) + "%)");

        auto fullscreenTriangle = MeshPrimitives::GetFullscreenTriangle();

        RenderCommand::BindDefaultFramebuffer();
        RenderCommand::SetViewport(0, 0, window.GetWidth(), window.GetHeight());
        RenderCommand::SetClearColor({ 0.08f, 0.08f, 0.08f, 1.0f });
        RenderCommand::Clear();
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        OLO_CORE_TRACE("RenderProgressFrame: Binding boot shader...");
        s_BootShader->Bind();
        OLO_CORE_TRACE("RenderProgressFrame: Setting UBO data...");
        const BootUBOData uboData{ progress, current, total, phase };
        s_BootUBO->SetData(&uboData, sizeof(BootUBOData));

        OLO_CORE_TRACE("RenderProgressFrame: Drawing...");
        fullscreenTriangle->Bind();
        RenderCommand::DrawIndexed(fullscreenTriangle);

        OLO_CORE_TRACE("RenderProgressFrame: SwapBuffers...");
        window.SwapBuffers();
        OLO_CORE_TRACE("RenderProgressFrame: Done.");
    }

    void ShaderWarmup::RunWarmupScreen(ShaderLibrary& library, Window& window)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_BootShader || !s_BootShader->IsReady())
        {
            OLO_CORE_WARN("ShaderWarmup: Boot shader not available — flushing synchronously");
            library.FlushPendingShaders();
            return;
        }

        if (!library.HasPendingShaders())
        {
            OLO_CORE_INFO("ShaderWarmup: All shaders already ready — skipping warmup screen");
            return;
        }

        const u32 totalShaders = library.GetTotalCount();
        OLO_CORE_INFO("ShaderWarmup: Entering warmup loop ({} shaders pending)", library.GetPendingCount());

        while (library.HasPendingShaders())
        {
            library.PollPendingShaders();

            const u32 pending = library.GetPendingCount();
            const u32 ready = totalShaders - pending;
            const f32 progress = totalShaders > 0 ? static_cast<f32>(ready) / static_cast<f32>(totalShaders) : 1.0f;

            RenderProgressFrame(progress, window, "linking shaders", static_cast<i32>(ready), static_cast<i32>(totalShaders), 3);
        }

        OLO_CORE_INFO("ShaderWarmup: All {} shaders ready", totalShaders);
    }

    void ShaderWarmup::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_BootUBO.Reset();
        s_BootShader.Reset();
    }
} // namespace OloEngine
