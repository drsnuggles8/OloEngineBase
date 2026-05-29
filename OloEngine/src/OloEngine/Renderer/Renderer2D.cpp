#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Core/UTF8.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/SlugData.h"
#include "OloEngine/Renderer/ShaderWarmup.h"
#include "OloEngine/Core/Application.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace OloEngine
{
    struct QuadVertex
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec2 TexCoord;
        f32 TexIndex;
        f32 TilingFactor;

        // Editor-only
        int EntityID;
    };

    struct PolygonVertex
    {
        glm::vec3 Position;
        glm::vec4 Color;

        // Editor-only
        int EntityID;
    };

    struct CircleVertex
    {
        glm::vec3 WorldPosition;
        glm::vec3 LocalPosition;
        glm::vec4 Color;
        f32 Thickness;
        f32 Fade;

        // Editor-only
        int EntityID;
    };

    struct LineVertex
    {
        glm::vec3 Position;
        glm::vec4 Color;

        // Editor-only
        int EntityID;
    };

    struct TextVertex
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec2 TexCoord;      // em-space sample coordinates
        glm::vec4 BandTransform; // (scaleX, scaleY, offsetX, offsetY)
        glm::ivec4 GlyphData;    // (bandTexX, bandTexY, bandMaxX, bandMaxY)

        // Editor-only
        int EntityID;
    };

    struct Renderer2DData
    {
        static constexpr u32 MaxQuads = 20000;
        static constexpr u32 MaxVertices = MaxQuads * 4;
        static constexpr u32 MaxIndices = MaxQuads * 6;
        static constexpr u32 MaxTextureSlots = 32;

        Ref<VertexArray> QuadVertexArray;
        Ref<VertexBuffer> QuadVertexBuffer;
        Ref<Shader> QuadShader;
        Ref<Texture2D> WhiteTexture;

        Ref<VertexArray> PolygonVertexArray;
        Ref<VertexBuffer> PolygonVertexBuffer;
        Ref<Shader> PolygonShader;

        Ref<VertexArray> CircleVertexArray;
        Ref<VertexBuffer> CircleVertexBuffer;
        Ref<Shader> CircleShader;

        Ref<VertexArray> LineVertexArray;
        Ref<VertexBuffer> LineVertexBuffer;
        Ref<Shader> LineShader;

        Ref<VertexArray> TextVertexArray;
        Ref<VertexBuffer> TextVertexBuffer;
        Ref<Shader> TextShader;

        u32 QuadIndexCount = 0;
        QuadVertex* QuadVertexBufferBase = nullptr;
        QuadVertex* QuadVertexBufferPtr = nullptr;

        u32 PolygonVertexCount = 0;
        PolygonVertex* PolygonVertexBufferBase = nullptr;
        PolygonVertex* PolygonVertexBufferPtr = nullptr;

        u32 CircleIndexCount = 0;
        CircleVertex* CircleVertexBufferBase = nullptr;
        CircleVertex* CircleVertexBufferPtr = nullptr;

        u32 LineVertexCount = 0;
        LineVertex* LineVertexBufferBase = nullptr;
        LineVertex* LineVertexBufferPtr = nullptr;

        u32 TextIndexCount = 0;
        TextVertex* TextVertexBufferBase = nullptr;
        TextVertex* TextVertexBufferPtr = nullptr;

        f32 LineWidth = 2.0f;

        std::array<Ref<Texture2D>, MaxTextureSlots> TextureSlots;
        // 0 = white texture
        u32 TextureSlotIndex = 1;

        Ref<Texture2D> SlugCurveTexture;
        Ref<Texture2D> SlugBandTexture;

        glm::vec4 QuadVertexPositions[4]{};

        Renderer2D::Statistics Stats;

        struct CameraData
        {
            glm::mat4 ViewProjection;
        };
        CameraData CameraBuffer{};
        Ref<UniformBuffer> CameraUniformBuffer;
    };

    static Renderer2DData s_Data;

    ShaderLibrary Renderer2D::m_ShaderLibrary;

    void Renderer2D::Init(Window* loadingWindow)
    {
        OLO_PROFILE_FUNCTION();

        Window* window = loadingWindow;

        s_Data.QuadVertexArray = VertexArray::Create();

        s_Data.QuadVertexBuffer = VertexBuffer::Create(OloEngine::Renderer2DData::MaxVertices * sizeof(QuadVertex));
        s_Data.QuadVertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                             { ShaderDataType::Float4, "a_Color" },
                                             { ShaderDataType::Float2, "a_TexCoord" },
                                             { ShaderDataType::Float, "a_TexIndex" },
                                             { ShaderDataType::Float, "a_TilingFactor" },
                                             { ShaderDataType::Int, "a_EntityID" } });
        s_Data.QuadVertexArray->AddVertexBuffer(s_Data.QuadVertexBuffer);

        s_Data.QuadVertexBufferBase = new QuadVertex[OloEngine::Renderer2DData::MaxVertices];

        auto* quadIndices = new u32[OloEngine::Renderer2DData::MaxIndices];

        u32 offset = 0;
        for (u32 i = 0; i < OloEngine::Renderer2DData::MaxIndices; i += 6)
        {
            quadIndices[i + 0] = offset + 0;
            quadIndices[i + 1] = offset + 1;
            quadIndices[i + 2] = offset + 2;

            quadIndices[i + 3] = offset + 2;
            quadIndices[i + 4] = offset + 3;
            quadIndices[i + 5] = offset + 0;

            offset += 4;
        }

        Ref<IndexBuffer> const quadIB = IndexBuffer::Create(quadIndices, OloEngine::Renderer2DData::MaxIndices);
        s_Data.QuadVertexArray->SetIndexBuffer(quadIB);
        delete[] quadIndices;

        // Polygons
        s_Data.PolygonVertexArray = VertexArray::Create();

        s_Data.PolygonVertexBuffer = VertexBuffer::Create(OloEngine::Renderer2DData::MaxVertices * sizeof(PolygonVertex));
        s_Data.PolygonVertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                                { ShaderDataType::Float4, "a_Color" },
                                                { ShaderDataType::Int, "a_EntityID" } });
        s_Data.PolygonVertexArray->AddVertexBuffer(s_Data.PolygonVertexBuffer);
        s_Data.PolygonVertexBufferBase = new PolygonVertex[OloEngine::Renderer2DData::MaxVertices];

        // Circles
        s_Data.CircleVertexArray = VertexArray::Create();

        s_Data.CircleVertexBuffer = VertexBuffer::Create(OloEngine::Renderer2DData::MaxVertices * sizeof(CircleVertex));
        s_Data.CircleVertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_WorldPosition" },
                                               { ShaderDataType::Float3, "a_LocalPosition" },
                                               { ShaderDataType::Float4, "a_Color" },
                                               { ShaderDataType::Float, "a_Thickness" },
                                               { ShaderDataType::Float, "a_Fade" },
                                               { ShaderDataType::Int, "a_EntityID" } });
        s_Data.CircleVertexArray->AddVertexBuffer(s_Data.CircleVertexBuffer);
        s_Data.CircleVertexArray->SetIndexBuffer(quadIB); // Use quad IB
        s_Data.CircleVertexBufferBase = new CircleVertex[Renderer2DData::MaxVertices];

        // Lines
        s_Data.LineVertexArray = VertexArray::Create();

        s_Data.LineVertexBuffer = VertexBuffer::Create(Renderer2DData::MaxVertices * sizeof(LineVertex));
        s_Data.LineVertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                             { ShaderDataType::Float4, "a_Color" },
                                             { ShaderDataType::Int, "a_EntityID" } });
        s_Data.LineVertexArray->AddVertexBuffer(s_Data.LineVertexBuffer);
        s_Data.LineVertexBufferBase = new LineVertex[Renderer2DData::MaxVertices];

        // Text
        s_Data.TextVertexArray = VertexArray::Create();

        s_Data.TextVertexBuffer = VertexBuffer::Create(Renderer2DData::MaxVertices * sizeof(TextVertex));
        s_Data.TextVertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                             { ShaderDataType::Float4, "a_Color" },
                                             { ShaderDataType::Float2, "a_TexCoord" },
                                             { ShaderDataType::Float4, "a_BandTransform" },
                                             { ShaderDataType::Int4, "a_GlyphData" },
                                             { ShaderDataType::Int, "a_EntityID" } });
        s_Data.TextVertexArray->AddVertexBuffer(s_Data.TextVertexBuffer);
        s_Data.TextVertexArray->SetIndexBuffer(quadIB);
        s_Data.TextVertexBufferBase = new TextVertex[Renderer2DData::MaxVertices];

        s_Data.WhiteTexture = Texture2D::Create(TextureSpecification());
        u32 whiteTextureData = 0xffffffffU;
        s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(u32));

        i32 samplers[OloEngine::Renderer2DData::MaxTextureSlots]{};
        for (u32 i = 0; i < OloEngine::Renderer2DData::MaxTextureSlots; ++i)
        {
            samplers[i] = i;
        }

        OLO_CORE_INFO("Renderer2D::Init: Loading Renderer2D_Quad...");
        m_ShaderLibrary.Load("assets/shaders/Renderer2D_Quad.glsl");
        OLO_CORE_INFO("Renderer2D::Init: Renderer2D_Quad loaded, calling RenderProgressFrame(1/5)");
        ShaderWarmup::RenderProgressFrame(1.0f / 5.0f, window, "2D shaders", 1, 5, 0);
        OLO_CORE_INFO("Renderer2D::Init: Loading Renderer2D_Polygon...");
        m_ShaderLibrary.Load("assets/shaders/Renderer2D_Polygon.glsl");
        OLO_CORE_INFO("Renderer2D::Init: Renderer2D_Polygon loaded, calling RenderProgressFrame(2/5)");
        ShaderWarmup::RenderProgressFrame(2.0f / 5.0f, window, "2D shaders", 2, 5, 0);
        OLO_CORE_INFO("Renderer2D::Init: Loading Renderer2D_Circle...");
        m_ShaderLibrary.Load("assets/shaders/Renderer2D_Circle.glsl");
        OLO_CORE_INFO("Renderer2D::Init: Renderer2D_Circle loaded, calling RenderProgressFrame(3/5)");
        ShaderWarmup::RenderProgressFrame(3.0f / 5.0f, window, "2D shaders", 3, 5, 0);
        OLO_CORE_INFO("Renderer2D::Init: Loading Renderer2D_Line...");
        m_ShaderLibrary.Load("assets/shaders/Renderer2D_Line.glsl");
        OLO_CORE_INFO("Renderer2D::Init: Renderer2D_Line loaded, calling RenderProgressFrame(4/5)");
        ShaderWarmup::RenderProgressFrame(4.0f / 5.0f, window, "2D shaders", 4, 5, 0);
        OLO_CORE_INFO("Renderer2D::Init: Loading Renderer2D_Text...");
        m_ShaderLibrary.Load("assets/shaders/Renderer2D_Text.glsl");
        OLO_CORE_INFO("Renderer2D::Init: Renderer2D_Text loaded, calling RenderProgressFrame(5/5)");
        ShaderWarmup::RenderProgressFrame(5.0f / 5.0f, window, "2D shaders", 5, 5, 0);

        // Wait for any remaining async GPU links before resolving shader refs.
        ShaderWarmup::RunWarmupScreen(m_ShaderLibrary, window);

        s_Data.QuadShader = m_ShaderLibrary.Get("Renderer2D_Quad");
        s_Data.PolygonShader = m_ShaderLibrary.Get("Renderer2D_Polygon");
        s_Data.CircleShader = m_ShaderLibrary.Get("Renderer2D_Circle");
        s_Data.LineShader = m_ShaderLibrary.Get("Renderer2D_Line");
        s_Data.TextShader = m_ShaderLibrary.Get("Renderer2D_Text");

        // Set first texture slot to 0
        s_Data.TextureSlots[0] = s_Data.WhiteTexture;

        s_Data.QuadVertexPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
        s_Data.QuadVertexPositions[1] = { 0.5f, -0.5f, 0.0f, 1.0f };
        s_Data.QuadVertexPositions[2] = { 0.5f, 0.5f, 0.0f, 1.0f };
        s_Data.QuadVertexPositions[3] = { -0.5f, 0.5f, 0.0f, 1.0f };

        s_Data.CameraUniformBuffer = UniformBuffer::Create(sizeof(Renderer2DData::CameraData), 0);
    }

    void Renderer2D::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        delete[] s_Data.QuadVertexBufferBase;
        s_Data.QuadVertexBufferBase = nullptr;
        delete[] s_Data.PolygonVertexBufferBase;
        s_Data.PolygonVertexBufferBase = nullptr;
        delete[] s_Data.CircleVertexBufferBase;
        s_Data.CircleVertexBufferBase = nullptr;
        delete[] s_Data.LineVertexBufferBase;
        s_Data.LineVertexBufferBase = nullptr;
        delete[] s_Data.TextVertexBufferBase;
        s_Data.TextVertexBufferBase = nullptr;
    }

    void Renderer2D::BeginScene(const OrthographicCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.CameraBuffer.ViewProjection = camera.GetViewProjectionMatrix();
        UniformData data = { &s_Data.CameraBuffer, sizeof(Renderer2DData::CameraData), 0 };
        s_Data.CameraUniformBuffer->SetData(data);
        // Re-bind to ensure this UBO is active at binding point 0.
        // Renderer3D binds its own camera UBO to the same slot, overwriting the binding.
        s_Data.CameraUniformBuffer->Bind();

        StartBatch();
    }

    void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.CameraBuffer.ViewProjection = camera.GetProjection() * glm::inverse(transform);
        UniformData data = { &s_Data.CameraBuffer, sizeof(Renderer2DData::CameraData), 0 };
        s_Data.CameraUniformBuffer->SetData(data);
        s_Data.CameraUniformBuffer->Bind();

        StartBatch();
    }

    void Renderer2D::BeginScene(const EditorCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.CameraBuffer.ViewProjection = camera.GetViewProjection();
        UniformData data = { &s_Data.CameraBuffer, sizeof(Renderer2DData::CameraData), 0 };
        s_Data.CameraUniformBuffer->SetData(data);
        s_Data.CameraUniformBuffer->Bind();

        StartBatch();
    }

    void Renderer2D::EndScene()
    {
        OLO_PROFILE_FUNCTION();

        Flush();
    }

    void Renderer2D::StartBatch()
    {
        s_Data.QuadIndexCount = 0;
        s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;

        s_Data.PolygonVertexCount = 0;
        s_Data.PolygonVertexBufferPtr = s_Data.PolygonVertexBufferBase;

        s_Data.CircleIndexCount = 0;
        s_Data.CircleVertexBufferPtr = s_Data.CircleVertexBufferBase;

        s_Data.LineVertexCount = 0;
        s_Data.LineVertexBufferPtr = s_Data.LineVertexBufferBase;

        s_Data.TextIndexCount = 0;
        s_Data.TextVertexBufferPtr = s_Data.TextVertexBufferBase;

        s_Data.TextureSlotIndex = 1;
    }

    void Renderer2D::Flush()
    {
        struct DrawCall
        {
            Ref<Shader> Shader;
            std::vector<Ref<Texture2D>> Textures;
            u32 IndexCount = 0;
            void* VertexBufferBase = nullptr;
            u32 VertexBufferSize = 0;
            Ref<VertexArray> VertexArray;
        };

        std::vector<DrawCall> drawCalls;

        // Collect draw calls for quads
        if (s_Data.QuadIndexCount)
        {
            DrawCall drawCall;
            drawCall.Shader = s_Data.QuadShader;
            drawCall.Textures.assign(s_Data.TextureSlots.begin(), s_Data.TextureSlots.begin() + s_Data.TextureSlotIndex);
            drawCall.IndexCount = s_Data.QuadIndexCount;
            drawCall.VertexBufferBase = s_Data.QuadVertexBufferBase;
            drawCall.VertexBufferSize = static_cast<u32>(std::bit_cast<std::byte*>(s_Data.QuadVertexBufferPtr) - std::bit_cast<std::byte*>(s_Data.QuadVertexBufferBase));
            drawCall.VertexArray = s_Data.QuadVertexArray;
            drawCalls.push_back(drawCall);
        }

        // Collect draw calls for polygons
        if (s_Data.PolygonVertexCount)
        {
            DrawCall drawCall;
            drawCall.Shader = s_Data.PolygonShader;
            drawCall.IndexCount = s_Data.PolygonVertexCount;
            drawCall.VertexBufferBase = s_Data.PolygonVertexBufferBase;
            drawCall.VertexBufferSize = static_cast<u32>(std::bit_cast<std::byte*>(s_Data.PolygonVertexBufferPtr) - std::bit_cast<std::byte*>(s_Data.PolygonVertexBufferBase));
            drawCall.VertexArray = s_Data.PolygonVertexArray;
            drawCalls.push_back(drawCall);
        }

        // Collect draw calls for circles
        if (s_Data.CircleIndexCount)
        {
            DrawCall drawCall;
            drawCall.Shader = s_Data.CircleShader;
            drawCall.IndexCount = s_Data.CircleIndexCount;
            drawCall.VertexBufferBase = s_Data.CircleVertexBufferBase;
            drawCall.VertexBufferSize = static_cast<u32>(std::bit_cast<std::byte*>(s_Data.CircleVertexBufferPtr) - std::bit_cast<std::byte*>(s_Data.CircleVertexBufferBase));
            drawCall.VertexArray = s_Data.CircleVertexArray;
            drawCalls.push_back(drawCall);
        }

        // Collect draw calls for lines
        if (s_Data.LineVertexCount)
        {
            DrawCall drawCall;
            drawCall.Shader = s_Data.LineShader;
            drawCall.IndexCount = s_Data.LineVertexCount;
            drawCall.VertexBufferBase = s_Data.LineVertexBufferBase;
            drawCall.VertexBufferSize = static_cast<u32>(std::bit_cast<std::byte*>(s_Data.LineVertexBufferPtr) - std::bit_cast<std::byte*>(s_Data.LineVertexBufferBase));
            drawCall.VertexArray = s_Data.LineVertexArray;
            drawCalls.push_back(drawCall);
        }

        // Collect draw calls for text
        if (s_Data.TextIndexCount)
        {
            DrawCall drawCall;
            drawCall.Shader = s_Data.TextShader;
            if (s_Data.SlugCurveTexture)
            {
                drawCall.Textures.push_back(s_Data.SlugCurveTexture);
            }
            if (s_Data.SlugBandTexture)
            {
                drawCall.Textures.push_back(s_Data.SlugBandTexture);
            }
            drawCall.IndexCount = s_Data.TextIndexCount;
            drawCall.VertexBufferBase = s_Data.TextVertexBufferBase;
            drawCall.VertexBufferSize = static_cast<u32>(std::bit_cast<std::byte*>(s_Data.TextVertexBufferPtr) - std::bit_cast<std::byte*>(s_Data.TextVertexBufferBase));
            drawCall.VertexArray = s_Data.TextVertexArray;
            drawCalls.push_back(drawCall);
        }

        // Sort draw calls by shader and textures
        std::ranges::sort(drawCalls, [](const DrawCall& a, const DrawCall& b)
                          {
			if (a.Shader != b.Shader)
				return a.Shader < b.Shader;
			return a.Textures < b.Textures; });

        // Execute draw calls
        for (const auto& drawCall : drawCalls)
        {
            drawCall.VertexArray->Bind();
            drawCall.Shader->Bind();

            // Bind textures
            for (u32 i = 0; i < drawCall.Textures.size(); ++i)
            {
                drawCall.Textures[i]->Bind(i);
            }

            VertexData data = { drawCall.VertexBufferBase, drawCall.VertexBufferSize };
            for (auto vertexBuffer : drawCall.VertexArray->GetVertexBuffers())
            {
                vertexBuffer->SetData(data);
            }

            if (drawCall.VertexArray == s_Data.QuadVertexArray || drawCall.VertexArray == s_Data.CircleVertexArray || drawCall.VertexArray == s_Data.TextVertexArray)
            {
                RenderCommand::DrawIndexed(drawCall.VertexArray, drawCall.IndexCount);
            }
            else if (drawCall.VertexArray == s_Data.PolygonVertexArray)
            {
                RenderCommand::DrawArrays(drawCall.VertexArray, drawCall.IndexCount);
            }
            else if (drawCall.VertexArray == s_Data.LineVertexArray)
            {
                RenderCommand::SetLineWidth(s_Data.LineWidth);
                RenderCommand::DrawLines(drawCall.VertexArray, drawCall.IndexCount);
            }

            ++s_Data.Stats.DrawCalls;
        }
    }

    void Renderer2D::NextBatch()
    {
        Flush();
        StartBatch();
    }

    ShaderLibrary& Renderer2D::GetShaderLibrary()
    {
        return m_ShaderLibrary;
    }

    void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
    {
        DrawQuad({ position.x, position.y, 0.0f }, size, color);
    }

    void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
    {
        OLO_PROFILE_FUNCTION();

        glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        DrawQuad(transform, color);
    }

    void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
    {
        DrawQuad({ position.x, position.y, 0.0f }, size, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
    {
        OLO_PROFILE_FUNCTION();

        glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        DrawQuad(transform, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, const int entityID)
    {
        OLO_PROFILE_FUNCTION();

        constexpr sizet quadVertexCount = 4;
        const f32 textureIndex = 0.0f; // White Texture
        constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
        const f32 tilingFactor = 1.0f;

        if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();
        }

        for (sizet i = 0; i < quadVertexCount; ++i)
        {
            s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
            s_Data.QuadVertexBufferPtr->Color = color;
            s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
            s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
            s_Data.QuadVertexBufferPtr->EntityID = entityID;
            ++s_Data.QuadVertexBufferPtr;
        }

        s_Data.QuadIndexCount += 6;

        ++s_Data.Stats.QuadCount;
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor, const int entityID)
    {
        OLO_PROFILE_FUNCTION();

        constexpr sizet quadVertexCount = 4;
        constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };

        if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();
        }

        f32 textureIndex = 0.0f;
        for (u32 i = 1; i < s_Data.TextureSlotIndex; ++i)
        {
            if (*s_Data.TextureSlots[i] == *texture)
            {
                textureIndex = static_cast<f32>(i);
                break;
            }
        }

        if (const f64 epsilon = 1e-5; std::abs(textureIndex - 0.0f) < epsilon)
        {
            if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
            {
                NextBatch();
            }

            textureIndex = static_cast<f32>(s_Data.TextureSlotIndex);
            s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
            ++s_Data.TextureSlotIndex;
        }

        for (sizet i = 0; i < quadVertexCount; ++i)
        {
            s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
            s_Data.QuadVertexBufferPtr->Color = tintColor;
            s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
            s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
            s_Data.QuadVertexBufferPtr->EntityID = entityID;
            ++s_Data.QuadVertexBufferPtr;
        }

        s_Data.QuadIndexCount += 6;

        ++s_Data.Stats.QuadCount;
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec4& tintColor, const int entityID)
    {
        OLO_PROFILE_FUNCTION();

        constexpr sizet quadVertexCount = 4;
        const glm::vec2 textureCoords[] = {
            { uvMin.x, uvMin.y },
            { uvMax.x, uvMin.y },
            { uvMax.x, uvMax.y },
            { uvMin.x, uvMax.y }
        };

        if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();
        }

        f32 textureIndex = 0.0f;
        for (u32 i = 1; i < s_Data.TextureSlotIndex; ++i)
        {
            if (*s_Data.TextureSlots[i] == *texture)
            {
                textureIndex = static_cast<f32>(i);
                break;
            }
        }

        if (const f64 epsilon = 1e-5; std::abs(textureIndex - 0.0f) < epsilon)
        {
            if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
            {
                NextBatch();
            }

            textureIndex = static_cast<f32>(s_Data.TextureSlotIndex);
            s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
            ++s_Data.TextureSlotIndex;
        }

        for (sizet i = 0; i < quadVertexCount; ++i)
        {
            s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
            s_Data.QuadVertexBufferPtr->Color = tintColor;
            s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
            s_Data.QuadVertexBufferPtr->TilingFactor = 1.0f;
            s_Data.QuadVertexBufferPtr->EntityID = entityID;
            ++s_Data.QuadVertexBufferPtr;
        }

        s_Data.QuadIndexCount += 6;

        ++s_Data.Stats.QuadCount;
    }

    void Renderer2D::DrawQuadVertices(const glm::vec3 positions[4], const glm::vec4 colors[4], const int entityID)
    {
        OLO_PROFILE_FUNCTION();

        constexpr f32 textureIndex = 0.0f; // White Texture
        constexpr glm::vec2 textureCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
        constexpr f32 tilingFactor = 1.0f;

        if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();
        }

        for (sizet i = 0; i < 4; ++i)
        {
            s_Data.QuadVertexBufferPtr->Position = positions[i];
            s_Data.QuadVertexBufferPtr->Color = colors[i];
            s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
            s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
            s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
            s_Data.QuadVertexBufferPtr->EntityID = entityID;
            ++s_Data.QuadVertexBufferPtr;
        }

        s_Data.QuadIndexCount += 6;
        ++s_Data.Stats.QuadCount;
    }

    void Renderer2D::DrawQuadVertices(const glm::vec3 positions[4], const glm::vec4 colors[4],
                                      const glm::vec2 texCoords[4], const Ref<Texture2D>& texture, const int entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();
        }

        f32 textureIndex = 0.0f;
        for (u32 i = 1; i < s_Data.TextureSlotIndex; ++i)
        {
            if (*s_Data.TextureSlots[i] == *texture)
            {
                textureIndex = static_cast<f32>(i);
                break;
            }
        }

        // textureIndex == 0.0f is the "no match found in the loop above" sentinel.
        // Bit-exact comparison because the loop assigns from integer indices —
        // 0.0f appears only when nothing was assigned (see cpp-coding-quality §2a).
        if (constexpr f32 noTextureSlotSentinel = 0.0f; Math::BitwiseEqual(textureIndex, noTextureSlotSentinel))
        {
            if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
            {
                NextBatch();
            }
            textureIndex = static_cast<f32>(s_Data.TextureSlotIndex);
            s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
            ++s_Data.TextureSlotIndex;
        }

        constexpr f32 tilingFactor = 1.0f;

        for (sizet i = 0; i < 4; ++i)
        {
            s_Data.QuadVertexBufferPtr->Position = positions[i];
            s_Data.QuadVertexBufferPtr->Color = colors[i];
            s_Data.QuadVertexBufferPtr->TexCoord = texCoords[i];
            s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
            s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
            s_Data.QuadVertexBufferPtr->EntityID = entityID;
            ++s_Data.QuadVertexBufferPtr;
        }

        s_Data.QuadIndexCount += 6;
        ++s_Data.Stats.QuadCount;
    }

    void Renderer2D::DrawPolygon(const std::vector<glm::vec3>& vertices, const glm::vec4& color, int entityID)
    {
        OLO_PROFILE_FUNCTION();

        if (vertices.size() < 3)
        {
            // A polygon must have at least 3 vertices
            return;
        }

        if (s_Data.PolygonVertexCount + vertices.size() >= Renderer2DData::MaxVertices)
        {
            NextBatch();
        }

        for (const auto& vertex : vertices)
        {
            s_Data.PolygonVertexBufferPtr->Position = vertex;
            s_Data.PolygonVertexBufferPtr->Color = color;
            s_Data.PolygonVertexBufferPtr->EntityID = entityID;
            ++s_Data.PolygonVertexBufferPtr;
        }

        s_Data.PolygonVertexCount += static_cast<u32>(vertices.size());
        ++s_Data.Stats.QuadCount; // Update stats (optional)
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, const f32 rotation, const glm::vec4& color)
    {
        DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, color);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, const f32 rotation, const glm::vec4& color)
    {
        OLO_PROFILE_FUNCTION();

        glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position) * glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f }) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        DrawQuad(transform, color);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, const f32 rotation, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
    {
        DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, const f32 rotation, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
    {
        OLO_PROFILE_FUNCTION();

        glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position) * glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f }) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

        DrawQuad(transform, texture, tilingFactor, tintColor);
    }

    void Renderer2D::DrawCircle(const glm::mat4& transform, const glm::vec4& color, const f32 thickness /*= 1.0f*/, const f32 fade /*= 0.005f*/, const int entityID /*= -1*/)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.CircleIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();
        }

        for (auto const& QuadVertexPosition : s_Data.QuadVertexPositions)
        {
            s_Data.CircleVertexBufferPtr->WorldPosition = transform * QuadVertexPosition;
            s_Data.CircleVertexBufferPtr->LocalPosition = QuadVertexPosition * 2.0f;
            s_Data.CircleVertexBufferPtr->Color = color;
            s_Data.CircleVertexBufferPtr->Thickness = thickness;
            s_Data.CircleVertexBufferPtr->Fade = fade;
            s_Data.CircleVertexBufferPtr->EntityID = entityID;
            ++s_Data.CircleVertexBufferPtr;
        }

        s_Data.CircleIndexCount += 6;

        ++s_Data.Stats.QuadCount;
    }

    void Renderer2D::DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, const int entityID)
    {
        s_Data.LineVertexBufferPtr->Position = p0;
        s_Data.LineVertexBufferPtr->Color = color;
        s_Data.LineVertexBufferPtr->EntityID = entityID;
        ++s_Data.LineVertexBufferPtr;

        s_Data.LineVertexBufferPtr->Position = p1;
        s_Data.LineVertexBufferPtr->Color = color;
        s_Data.LineVertexBufferPtr->EntityID = entityID;
        ++s_Data.LineVertexBufferPtr;

        s_Data.LineVertexCount += 2;
    }

    void Renderer2D::DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, const int entityID)
    {
        const auto p0 = glm::vec3(position.x - (size.x * 0.5f), position.y - (size.y * 0.5f), position.z);
        const auto p1 = glm::vec3(position.x + (size.x * 0.5f), position.y - (size.y * 0.5f), position.z);
        const auto p2 = glm::vec3(position.x + (size.x * 0.5f), position.y + (size.y * 0.5f), position.z);
        const auto p3 = glm::vec3(position.x - (size.x * 0.5f), position.y + (size.y * 0.5f), position.z);

        DrawLine(p0, p1, color, entityID);
        DrawLine(p1, p2, color, entityID);
        DrawLine(p2, p3, color, entityID);
        DrawLine(p3, p0, color, entityID);
    }

    void Renderer2D::DrawRect(const glm::mat4& transform, const glm::vec4& color, const int entityID)
    {
        glm::vec3 lineVertices[4]{};
        for (sizet i = 0; i < 4; ++i)
        {
            lineVertices[i] = transform * s_Data.QuadVertexPositions[i];
        }

        DrawLine(lineVertices[0], lineVertices[1], color, entityID);
        DrawLine(lineVertices[1], lineVertices[2], color, entityID);
        DrawLine(lineVertices[2], lineVertices[3], color, entityID);
        DrawLine(lineVertices[3], lineVertices[0], color, entityID);
    }

    void Renderer2D::DrawSprite(const glm::mat4& transform, SpriteRendererComponent const& src, const int entityID)
    {
        if (src.Texture)
        {
            DrawQuad(transform, src.Texture, src.TilingFactor, src.Color, entityID);
        }
        else
        {
            DrawQuad(transform, src.Color, entityID);
        }
    }

    void Renderer2D::DrawString(const std::string& string, Ref<Font> font, const glm::mat4& transform, const TextParams& textParams, int entityID)
    {
        OLO_PROFILE_FUNCTION();

        const auto* slugData = font->GetSlugData();
        if (!slugData)
        {
            return;
        }

        const auto& metrics = slugData->Metrics;

        // Store Slug textures for the flush pass.
        // Flush text batch if font changed (different textures) to avoid
        // earlier vertices sampling the wrong font's textures.
        auto curveTexture = font->GetCurveTexture();
        auto bandTexture = font->GetBandTexture();
        if (s_Data.TextIndexCount > 0 && (s_Data.SlugCurveTexture != curveTexture || s_Data.SlugBandTexture != bandTexture))
        {
            NextBatch();
        }
        s_Data.SlugCurveTexture = curveTexture;
        s_Data.SlugBandTexture = bandTexture;

        const auto metricSpan = static_cast<double>(metrics.AscenderY - metrics.DescenderY);
        const double fsScale = std::abs(metricSpan) > 1e-6 ? (1.0 / metricSpan) : 1.0;

        const auto* spaceGlyph = slugData->GetGlyph(' ');
        const f32 spaceGlyphAdvance = spaceGlyph ? spaceGlyph->AdvanceWidth : 0.25f;

        // textParams.RightToLeft is wired through from the active locale
        // via LocalizationSystem, but visual reversal is NOT applied here —
        // doing it correctly requires the Unicode Bidirectional Algorithm
        // for mixed-direction text and glyph joining for Arabic, neither of
        // which we ship. The field is currently a documented no-op that
        // future BiDi work can hang itself on.
        (void)textParams.RightToLeft;

        // Codepoint → (sourceFont, glyph) resolver. Returns the primary font's
        // glyph when present, else walks the fallback chain. When a fallback
        // hits, the emit loop swaps Slug textures to that font's atlas before
        // pushing geometry — a `NextBatch()` flush bounded per font change.
        const Font* primaryFont = font.Raw();
        const auto resolveGlyph = [primaryFont](u32 cp) -> Font::GlyphLookup
        {
            return primaryFont ? primaryFont->FindGlyphWithFallback(cp) : Font::GlyphLookup{};
        };

        // Per-codepoint advance helper. When both `cp` and `next` resolve
        // against the primary font we honour the kerning pair; cross-font
        // pairs fall back to the source font's `AdvanceWidth` (kerning across
        // fonts isn't meaningfully defined).
        const auto resolveAdvance = [&spaceGlyphAdvance, &primaryFont, &resolveGlyph, &slugData](u32 cp, u32 next, const Font::GlyphLookup& lookup) -> f32
        {
            if (!lookup.Glyph)
                return spaceGlyphAdvance;
            if (lookup.SourceFont == primaryFont && next != 0u)
            {
                if (auto nextLookup = resolveGlyph(next); nextLookup.SourceFont == primaryFont)
                    return slugData->GetAdvance(cp, next);
            }
            return lookup.Glyph->AdvanceWidth;
        };

        // Small margin to expand glyph bounding box (prevents edge clipping).
        constexpr f32 kGlyphMargin = 0.02f;

        // First pass: compute word-wrap line breaks when MaxWidth > 0.
        // Iteration is over Unicode codepoints, not raw bytes — the glyph
        // map is keyed by codepoint, and the cursor advance must be charged
        // exactly once per visible character regardless of UTF-8 length.
        // Wrap-break / lastSpace positions are still BYTE indices because
        // codepoint boundaries always coincide with byte boundaries, and
        // the render-pass second loop indexes the same string by byte too.
        const auto peekNextCodepoint = [&string](sizet pos) -> u32
        {
            if (pos >= string.size())
                return 0u;
            u32 cp = 0;
            sizet adv = 0;
            UTF8::DecodeCodepoint(string, pos, cp, adv);
            return cp;
        };

        std::vector<sizet> wrapBreaks;
        std::vector<bool> wrapIsMidWord;
        if (textParams.MaxWidth > 0.0f)
        {
            double wx = 0.0;
            sizet lastSpace = std::string::npos;
            sizet i = 0;
            while (i < string.size())
            {
                u32 character = 0;
                sizet adv = 0;
                UTF8::DecodeCodepoint(string, i, character, adv);
                const sizet next = i + adv;

                if (character == '\n' || character == '\r')
                {
                    wx = 0.0;
                    lastSpace = std::string::npos;
                    i = next;
                    continue;
                }

                if (character == ' ')
                {
                    lastSpace = i;
                    f32 advance = spaceGlyphAdvance;
                    if (next < string.size())
                        advance = slugData->GetAdvance(character, peekNextCodepoint(next));
                    wx += (fsScale * advance) + textParams.Kerning;
                    i = next;
                    continue;
                }

                if (character == '\t')
                {
                    lastSpace = i;
                    wx += 4.0 * ((fsScale * spaceGlyphAdvance) + textParams.Kerning);
                    i = next;
                    continue;
                }

                auto lookup = resolveGlyph(character);
                if (!lookup.Glyph)
                    lookup = resolveGlyph('?');
                if (!lookup.Glyph)
                {
                    i = next;
                    continue;
                }
                const auto* glyph = lookup.Glyph;

                if (f32 quadMaxX = glyph->PlaneBoundsRight * static_cast<f32>(fsScale) + static_cast<f32>(wx); quadMaxX > textParams.MaxWidth)
                {
                    if (lastSpace != std::string::npos)
                    {
                        wrapBreaks.push_back(lastSpace);
                        wrapIsMidWord.push_back(false);
                        i = lastSpace;
                        lastSpace = std::string::npos;
                        wx = 0.0;
                        // Restart loop from the space position; the next
                        // iteration will re-decode and skip past it.
                        continue;
                    }

                    wrapBreaks.push_back(i);
                    wrapIsMidWord.push_back(true);
                    lastSpace = std::string::npos;
                    wx = 0.0;
                }

                const u32 nextCp = (next < string.size()) ? peekNextCodepoint(next) : 0u;
                f32 advance = resolveAdvance(character, nextCp, lookup);
                wx += (fsScale * advance) + textParams.Kerning;
                i = next;
            }
        }

        // Second pass: render with wrap breaks. Same UTF-8 iteration rules
        // as the first pass — `wrapBreaks` holds byte indices that fall on
        // codepoint boundaries by construction.
        sizet wrapIdx = 0;
        double x = 0.0;
        double y = 0.0;

        sizet i = 0;
        while (i < string.size())
        {
            u32 character = 0;
            sizet adv = 0;
            UTF8::DecodeCodepoint(string, i, character, adv);
            const sizet next = i + adv;

            if (wrapIdx < wrapBreaks.size() && i == wrapBreaks[wrapIdx])
            {
                x = 0;
                y -= (fsScale * metrics.LineHeight) + textParams.LineSpacing;
                bool midWord = wrapIsMidWord[wrapIdx];
                ++wrapIdx;
                if (!midWord)
                {
                    i = next;
                    continue;
                }
            }

            if (character == '\r')
            {
                i = next;
                continue;
            }

            if (character == '\n')
            {
                x = 0;
                y -= (fsScale * metrics.LineHeight) + textParams.LineSpacing;
                i = next;
                continue;
            }

            if (character == ' ')
            {
                f32 advance = spaceGlyphAdvance;
                if (next < string.size())
                    advance = slugData->GetAdvance(character, peekNextCodepoint(next));
                x += (fsScale * advance) + textParams.Kerning;
                i = next;
                continue;
            }

            if (character == '\t')
            {
                x += 4.0f * ((fsScale * spaceGlyphAdvance) + textParams.Kerning);
                i = next;
                continue;
            }

            auto lookup = resolveGlyph(character);
            if (!lookup.Glyph)
                lookup = resolveGlyph('?');
            if (!lookup.Glyph)
            {
                i = next;
                continue;
            }
            const auto* glyph = lookup.Glyph;
            const u32 nextCp = (next < string.size()) ? peekNextCodepoint(next) : 0u;

            // Only emit geometry for glyphs with Slug curve data.
            if (!glyph->HasCurves)
            {
                if (next < string.size())
                {
                    f32 advance = resolveAdvance(character, nextCp, lookup);
                    x += (fsScale * advance) + textParams.Kerning;
                }
                i = next;
                continue;
            }

            // Atlas swap for fallback-font glyphs: the per-glyph render data
            // is sampled against a *specific* font's curve/band textures, so
            // we flush the current batch and rebind whenever the source font
            // changes mid-string. The common single-script case never
            // triggers this branch — only mixed scripts (e.g. Latin + CJK).
            if (lookup.SourceFont && lookup.SourceFont != primaryFont)
            {
                auto fallbackCurve = lookup.SourceFont->GetCurveTexture();
                auto fallbackBand = lookup.SourceFont->GetBandTexture();
                if (s_Data.TextIndexCount > 0 && (s_Data.SlugCurveTexture != fallbackCurve || s_Data.SlugBandTexture != fallbackBand))
                    NextBatch();
                s_Data.SlugCurveTexture = fallbackCurve;
                s_Data.SlugBandTexture = fallbackBand;
            }
            else if (lookup.SourceFont == primaryFont)
            {
                // Bouncing back to the primary after a fallback run requires
                // the same flush, otherwise primary glyphs would sample the
                // fallback atlas they're still bound to.
                if (s_Data.TextIndexCount > 0 && (s_Data.SlugCurveTexture != curveTexture || s_Data.SlugBandTexture != bandTexture))
                    NextBatch();
                s_Data.SlugCurveTexture = curveTexture;
                s_Data.SlugBandTexture = bandTexture;
            }

            const auto& rd = glyph->RenderData;

            // Compute glyph quad in text space (with margin for edge coverage).
            glm::vec2 quadMin(glyph->PlaneBoundsLeft - kGlyphMargin, glyph->PlaneBoundsBottom - kGlyphMargin);
            glm::vec2 quadMax(glyph->PlaneBoundsRight + kGlyphMargin, glyph->PlaneBoundsTop + kGlyphMargin);

            // Em-space sample coordinates (what the fragment shader uses for the algorithm).
            auto emMin = quadMin;
            auto emMax = quadMax;

            quadMin *= static_cast<f32>(fsScale);
            quadMax *= static_cast<f32>(fsScale);
            quadMin += glm::vec2(x, y);
            quadMax += glm::vec2(x, y);

            // Flush text batch if full.
            if (s_Data.TextIndexCount + 6 > Renderer2DData::MaxIndices)
                NextBatch();

            // Pack per-glyph data into vertex attributes.
            auto bandTransform = glm::vec4(rd.BandScaleX, rd.BandScaleY, rd.BandOffsetX, rd.BandOffsetY);
            auto glyphData = glm::ivec4(
                static_cast<int>(rd.BandTextureX),
                static_cast<int>(rd.BandTextureY),
                static_cast<int>(rd.VBandCount > 0 ? rd.VBandCount - 1 : 0),
                static_cast<int>(rd.HBandCount > 0 ? rd.HBandCount - 1 : 0));

            // Vertex order must match QuadVertexPositions: BL, BR, TR, TL
            // so the shared index buffer {0,1,2, 2,3,0} produces +Z front faces.

            // v0: Bottom-left
            s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMin, 0.0f, 1.0f);
            s_Data.TextVertexBufferPtr->Color = textParams.Color;
            s_Data.TextVertexBufferPtr->TexCoord = emMin;
            s_Data.TextVertexBufferPtr->BandTransform = bandTransform;
            s_Data.TextVertexBufferPtr->GlyphData = glyphData;
            s_Data.TextVertexBufferPtr->EntityID = entityID;
            ++s_Data.TextVertexBufferPtr;

            // v1: Bottom-right
            s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMax.x, quadMin.y, 0.0f, 1.0f);
            s_Data.TextVertexBufferPtr->Color = textParams.Color;
            s_Data.TextVertexBufferPtr->TexCoord = { emMax.x, emMin.y };
            s_Data.TextVertexBufferPtr->BandTransform = bandTransform;
            s_Data.TextVertexBufferPtr->GlyphData = glyphData;
            s_Data.TextVertexBufferPtr->EntityID = entityID;
            ++s_Data.TextVertexBufferPtr;

            // v2: Top-right
            s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMax, 0.0f, 1.0f);
            s_Data.TextVertexBufferPtr->Color = textParams.Color;
            s_Data.TextVertexBufferPtr->TexCoord = emMax;
            s_Data.TextVertexBufferPtr->BandTransform = bandTransform;
            s_Data.TextVertexBufferPtr->GlyphData = glyphData;
            s_Data.TextVertexBufferPtr->EntityID = entityID;
            ++s_Data.TextVertexBufferPtr;

            // v3: Top-left
            s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMin.x, quadMax.y, 0.0f, 1.0f);
            s_Data.TextVertexBufferPtr->Color = textParams.Color;
            s_Data.TextVertexBufferPtr->TexCoord = { emMin.x, emMax.y };
            s_Data.TextVertexBufferPtr->BandTransform = bandTransform;
            s_Data.TextVertexBufferPtr->GlyphData = glyphData;
            s_Data.TextVertexBufferPtr->EntityID = entityID;
            ++s_Data.TextVertexBufferPtr;

            s_Data.TextIndexCount += 6;
            ++s_Data.Stats.QuadCount;

            f32 advance = resolveAdvance(character, nextCp, lookup);
            x += (fsScale * advance) + textParams.Kerning;
            i = next;
        }
    }

    void Renderer2D::DrawString(const std::string& string, const glm::mat4& transform, const TextComponent& component, int entityID)
    {
        // Currently TextComponent carries no per-entity RTL flag — the
        // localization layer threads direction via the active locale,
        // which a future RTL pass will read off LocalizationManager at
        // emit time. Default to false here so existing scenes render
        // unchanged.
        TextParams params{ component.Color, component.Kerning, component.LineSpacing, component.MaxWidth, /*RightToLeft*/ false };
        DrawString(string, component.FontAsset, transform, params, entityID);
    }

    f32 Renderer2D::GetLineWidth()
    {
        return s_Data.LineWidth;
    }

    void Renderer2D::SetLineWidth(const f32 width)
    {
        s_Data.LineWidth = width;
    }

    void Renderer2D::ResetStats()
    {
        std::memset(&s_Data.Stats, 0, sizeof(Statistics));
    }

    [[nodiscard("Store this!")]] Renderer2D::Statistics Renderer2D::GetStats()
    {
        return s_Data.Stats;
    }
} // namespace OloEngine
