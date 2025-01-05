// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer2D.h"

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MSDFData.h"

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
		glm::vec2 TexCoord;

		// TODO: bg color for outline/bg

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

		Ref<Texture2D> FontAtlasTexture;

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

	void Renderer2D::Init()
	{
		OLO_PROFILE_FUNCTION();

		s_Data.QuadVertexArray = VertexArray::Create();

		s_Data.QuadVertexBuffer = VertexBuffer::Create(OloEngine::Renderer2DData::MaxVertices * sizeof(QuadVertex));
		s_Data.QuadVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position"     },
			{ ShaderDataType::Float4, "a_Color"        },
			{ ShaderDataType::Float2, "a_TexCoord"     },
			{ ShaderDataType::Float,  "a_TexIndex"     },
			{ ShaderDataType::Float,  "a_TilingFactor" },
			{ ShaderDataType::Int,    "a_EntityID"     }
		});
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
		s_Data.PolygonVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float4, "a_Color"    },
			{ ShaderDataType::Int,    "a_EntityID" }
		});
		s_Data.PolygonVertexArray->AddVertexBuffer(s_Data.PolygonVertexBuffer);
		s_Data.PolygonVertexBufferBase = new PolygonVertex[OloEngine::Renderer2DData::MaxVertices];

		// Circles
		s_Data.CircleVertexArray = VertexArray::Create();

		s_Data.CircleVertexBuffer = VertexBuffer::Create(OloEngine::Renderer2DData::MaxVertices * sizeof(CircleVertex));
		s_Data.CircleVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_WorldPosition" },
			{ ShaderDataType::Float3, "a_LocalPosition" },
			{ ShaderDataType::Float4, "a_Color"         },
			{ ShaderDataType::Float,  "a_Thickness"     },
			{ ShaderDataType::Float,  "a_Fade"          },
			{ ShaderDataType::Int,    "a_EntityID"      }
		});
		s_Data.CircleVertexArray->AddVertexBuffer(s_Data.CircleVertexBuffer);
		s_Data.CircleVertexArray->SetIndexBuffer(quadIB); // Use quad IB
		s_Data.CircleVertexBufferBase = new CircleVertex[OloEngine::Renderer2DData::MaxVertices];

		// Lines
		s_Data.LineVertexArray = VertexArray::Create();

		s_Data.LineVertexBuffer = VertexBuffer::Create(OloEngine::Renderer2DData::MaxVertices * sizeof(LineVertex));
		s_Data.LineVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float4, "a_Color"    },
			{ ShaderDataType::Int,    "a_EntityID" }
		});
		s_Data.LineVertexArray->AddVertexBuffer(s_Data.LineVertexBuffer);
		s_Data.LineVertexBufferBase = new LineVertex[OloEngine::Renderer2DData::MaxVertices];

		// Text
		s_Data.TextVertexArray = VertexArray::Create();

		s_Data.TextVertexBuffer = VertexBuffer::Create(s_Data.MaxVertices * sizeof(TextVertex));
		s_Data.TextVertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position"     },
			{ ShaderDataType::Float4, "a_Color"        },
			{ ShaderDataType::Float2, "a_TexCoord"     },
			{ ShaderDataType::Int,    "a_EntityID"     }
		});
		s_Data.TextVertexArray->AddVertexBuffer(s_Data.TextVertexBuffer);
		s_Data.TextVertexArray->SetIndexBuffer(quadIB);
		s_Data.TextVertexBufferBase = new TextVertex[s_Data.MaxVertices];

		s_Data.WhiteTexture = Texture2D::Create(TextureSpecification());
		u32 whiteTextureData = 0xffffffffU;
		s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(u32));

		i32 samplers[OloEngine::Renderer2DData::MaxTextureSlots]{};
		for (u32 i = 0; i < OloEngine::Renderer2DData::MaxTextureSlots; ++i)
		{
			samplers[i] = i;
		}

		s_Data.QuadShader = Shader::Create("assets/shaders/Renderer2D_Quad.glsl");
		s_Data.PolygonShader = Shader::Create("assets/shaders/Renderer2D_Polygon.glsl");
		s_Data.CircleShader = Shader::Create("assets/shaders/Renderer2D_Circle.glsl");
		s_Data.LineShader = Shader::Create("assets/shaders/Renderer2D_Line.glsl");
		s_Data.TextShader = Shader::Create("assets/shaders/Renderer2D_Text.glsl");

		// Set first texture slot to 0
		s_Data.TextureSlots[0] = s_Data.WhiteTexture;

		s_Data.QuadVertexPositions[0] = { -0.5f, -0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[1] = { 0.5f, -0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[2] = { 0.5f,  0.5f, 0.0f, 1.0f };
		s_Data.QuadVertexPositions[3] = { -0.5f,  0.5f, 0.0f, 1.0f };

		s_Data.CameraUniformBuffer = UniformBuffer::Create(sizeof(Renderer2DData::CameraData), 0);
	}

	void Renderer2D::Shutdown()
	{
		OLO_PROFILE_FUNCTION();

		delete[] s_Data.QuadVertexBufferBase;
	}

	void Renderer2D::BeginScene(const OrthographicCamera& camera)
	{
		OLO_PROFILE_FUNCTION();

		s_Data.CameraBuffer.ViewProjection = camera.GetViewProjectionMatrix();
		UniformData data = { &s_Data.CameraBuffer, sizeof(Renderer2DData::CameraData), 0 };
		s_Data.CameraUniformBuffer->SetData(data);

		StartBatch();
	}

	void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform)
	{
		OLO_PROFILE_FUNCTION();

		s_Data.CameraBuffer.ViewProjection = camera.GetProjection() * glm::inverse(transform);
		UniformData data = { &s_Data.CameraBuffer, sizeof(Renderer2DData::CameraData), 0 };
		s_Data.CameraUniformBuffer->SetData(data);

		StartBatch();
	}

	void Renderer2D::BeginScene(const EditorCamera& camera)
	{
		OLO_PROFILE_FUNCTION();

		s_Data.CameraBuffer.ViewProjection = camera.GetViewProjection();
		UniformData data = { &s_Data.CameraBuffer, sizeof(Renderer2DData::CameraData), 0 };
		s_Data.CameraUniformBuffer->SetData(data);

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
		if (s_Data.QuadIndexCount)
		{
			const auto dataSize = static_cast<u32>(reinterpret_cast<u8*>(s_Data.QuadVertexBufferPtr) - reinterpret_cast<u8*>(s_Data.QuadVertexBufferBase));
			VertexData data = { s_Data.QuadVertexBufferBase, dataSize };
			s_Data.QuadVertexBuffer->SetData(data);

			// Bind textures
			for (u32 i = 0; i < s_Data.TextureSlotIndex; i++)
			{
				s_Data.TextureSlots[i]->Bind(i);
			}

			s_Data.QuadShader->Bind();
			RenderCommand::DrawIndexed(s_Data.QuadVertexArray, s_Data.QuadIndexCount);
			++s_Data.Stats.DrawCalls;
		}

		if (s_Data.PolygonVertexCount)
		{
			const auto dataSize = static_cast<u32>(reinterpret_cast<u8*>(s_Data.PolygonVertexBufferPtr) - reinterpret_cast<u8*>(s_Data.PolygonVertexBufferBase));
			VertexData data = { s_Data.PolygonVertexBufferBase, dataSize };
			s_Data.PolygonVertexBuffer->SetData(data);

			s_Data.PolygonShader->Bind();
			RenderCommand::DrawArrays(s_Data.PolygonVertexArray, s_Data.PolygonVertexCount);
			++s_Data.Stats.DrawCalls;
		}

		if (s_Data.CircleIndexCount)
		{
			const auto dataSize = static_cast<u32>(reinterpret_cast<u8*>(s_Data.CircleVertexBufferPtr) - reinterpret_cast<u8*>(s_Data.CircleVertexBufferBase));
			VertexData data = { s_Data.CircleVertexBufferBase, dataSize };
			s_Data.CircleVertexBuffer->SetData(data);

			s_Data.CircleShader->Bind();
			RenderCommand::DrawIndexed(s_Data.CircleVertexArray, s_Data.CircleIndexCount);
			++s_Data.Stats.DrawCalls;
		}

		if (s_Data.LineVertexCount)
		{
			const auto dataSize = static_cast<u32>(reinterpret_cast<u8*>(s_Data.LineVertexBufferPtr) - reinterpret_cast<u8*>(s_Data.LineVertexBufferBase));
			VertexData data = { s_Data.LineVertexBufferBase, dataSize };
			s_Data.LineVertexBuffer->SetData(data);

			s_Data.LineShader->Bind();
			RenderCommand::SetLineWidth(s_Data.LineWidth);
			RenderCommand::DrawLines(s_Data.LineVertexArray, s_Data.LineVertexCount);
			++s_Data.Stats.DrawCalls;
		}

		if (s_Data.TextIndexCount)
		{
			const auto dataSize = static_cast<u32>(reinterpret_cast<u8*>(s_Data.TextVertexBufferPtr) - reinterpret_cast<u8*>(s_Data.TextVertexBufferBase));
			VertexData data = { s_Data.TextVertexBufferBase, dataSize };
			s_Data.TextVertexBuffer->SetData(data);
			s_Data.FontAtlasTexture->Bind(0);
			s_Data.TextShader->Bind();
			RenderCommand::DrawIndexed(s_Data.TextVertexArray, s_Data.TextIndexCount);
			++s_Data.Stats.DrawCalls;
		}
	}

	void Renderer2D::NextBatch()
	{
		Flush();
		StartBatch();
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, color);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
	{
		OLO_PROFILE_FUNCTION();

		glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position)
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuad(transform, color);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
	{
		DrawQuad({ position.x, position.y, 0.0f }, size, texture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
	{
		OLO_PROFILE_FUNCTION();

		glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position)
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

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

		s_Data.PolygonVertexCount += vertices.size();
		++s_Data.Stats.QuadCount; // Update stats (optional)
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, const f32 rotation, const glm::vec4& color)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, color);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, const f32 rotation, const glm::vec4& color)
	{
		OLO_PROFILE_FUNCTION();

		glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuad(transform, color);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, const f32 rotation, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
	{
		DrawRotatedQuad({ position.x, position.y, 0.0f }, size, rotation, texture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, const f32 rotation, const Ref<Texture2D>& texture, const f32 tilingFactor, const glm::vec4& tintColor)
	{
		OLO_PROFILE_FUNCTION();

		glm::mat4 const transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), glm::radians(rotation), { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuad(transform, texture, tilingFactor, tintColor);
	}

	void Renderer2D::DrawCircle(const glm::mat4& transform, const glm::vec4& color, const f32 thickness /*= 1.0f*/, const f32 fade /*= 0.005f*/, const int entityID /*= -1*/)
	{
		OLO_PROFILE_FUNCTION();

		if (s_Data.CircleIndexCount >= Renderer2DData::MaxIndices)
		{
			NextBatch();
		}

		for (auto const & QuadVertexPosition : s_Data.QuadVertexPositions)
		{
			s_Data.CircleVertexBufferPtr->WorldPosition = transform * QuadVertexPosition;
			s_Data.CircleVertexBufferPtr->LocalPosition = QuadVertexPosition * 2.0f;
			s_Data.CircleVertexBufferPtr->Color = color;
			s_Data.CircleVertexBufferPtr->Thickness = thickness;
			s_Data.CircleVertexBufferPtr->Fade = fade;
			s_Data.CircleVertexBufferPtr->EntityID = entityID;
			s_Data.CircleVertexBufferPtr++;
		}

		s_Data.CircleIndexCount += 6;

		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, const int entityID)
	{
		s_Data.LineVertexBufferPtr->Position = p0;
		s_Data.LineVertexBufferPtr->Color = color;
		s_Data.LineVertexBufferPtr->EntityID = entityID;
		s_Data.LineVertexBufferPtr++;

		s_Data.LineVertexBufferPtr->Position = p1;
		s_Data.LineVertexBufferPtr->Color = color;
		s_Data.LineVertexBufferPtr->EntityID = entityID;
		s_Data.LineVertexBufferPtr++;

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
		for (sizet i = 0; i < 4; i++)
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
		const auto& fontGeometry = font->GetMSDFData()->FontGeometry;
		const auto& metrics = fontGeometry.getMetrics();
		Ref<Texture2D> fontAtlas = font->GetAtlasTexture();

		s_Data.FontAtlasTexture = fontAtlas;

		double x = 0.0;
		double fsScale = 1.0 / (metrics.ascenderY - metrics.descenderY);
		double y = 0.0;

		const float spaceGlyphAdvance = fontGeometry.getGlyph(' ')->getAdvance();

		for (sizet i = 0; i < string.size(); i++)
		{
			char character = string[i];
			if (character == '\r')
			{
				continue;
			}

			if (character == '\n')
			{
				x = 0;
				y -= (fsScale * metrics.lineHeight) + textParams.LineSpacing;
				continue;
			}

			if (character == ' ')
			{
				float advance = spaceGlyphAdvance;
				if (i < (string.size() - 1))
				{
					char nextCharacter = string[i + 1];
					double dAdvance{};
					fontGeometry.getAdvance(dAdvance, character, nextCharacter);
					advance = (float)dAdvance;
				}

				x += (fsScale * advance) + textParams.Kerning;
				continue;
			}

			if (character == '\t')
			{
				// NOTE(Yan): is this right?
				x += 4.0f * ((fsScale * spaceGlyphAdvance) + textParams.Kerning);
				continue;
			}

			auto* glyph = fontGeometry.getGlyph(character);
			if (!glyph)
			{
				glyph = fontGeometry.getGlyph('?');
			}
			if (!glyph)
			{
				return;
			}

			double al{};
			double ab{};
			double ar{};
			double at{};
			glyph->getQuadAtlasBounds(al, ab, ar, at);
			glm::vec2 texCoordMin((float)al, (float)ab);
			glm::vec2 texCoordMax((float)ar, (float)at);

			double pl{};
			double pb{};
			double pr{};
			double pt{};
			glyph->getQuadPlaneBounds(pl, pb, pr, pt);
			glm::vec2 quadMin((float)pl, (float)pb);
			glm::vec2 quadMax((float)pr, (float)pt);

			quadMin *= fsScale;
			quadMax *= fsScale;
			quadMin += glm::vec2(x, y);
			quadMax += glm::vec2(x, y);

			float texelWidth = 1.0f / fontAtlas->GetWidth();
			float texelHeight = 1.0f / fontAtlas->GetHeight();
			texCoordMin *= glm::vec2(texelWidth, texelHeight);
			texCoordMax *= glm::vec2(texelWidth, texelHeight);

			// render here
			s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMin, 0.0f, 1.0f);
			s_Data.TextVertexBufferPtr->Color = textParams.Color;
			s_Data.TextVertexBufferPtr->TexCoord = texCoordMin;
			s_Data.TextVertexBufferPtr->EntityID = entityID;
			++s_Data.TextVertexBufferPtr;

			s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMin.x, quadMax.y, 0.0f, 1.0f);
			s_Data.TextVertexBufferPtr->Color = textParams.Color;
			s_Data.TextVertexBufferPtr->TexCoord = { texCoordMin.x, texCoordMax.y };
			s_Data.TextVertexBufferPtr->EntityID = entityID;
			++s_Data.TextVertexBufferPtr;

			s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMax, 0.0f, 1.0f);
			s_Data.TextVertexBufferPtr->Color = textParams.Color;
			s_Data.TextVertexBufferPtr->TexCoord = texCoordMax;
			s_Data.TextVertexBufferPtr->EntityID = entityID;
			++s_Data.TextVertexBufferPtr;

			s_Data.TextVertexBufferPtr->Position = transform * glm::vec4(quadMax.x, quadMin.y, 0.0f, 1.0f);
			s_Data.TextVertexBufferPtr->Color = textParams.Color;
			s_Data.TextVertexBufferPtr->TexCoord = { texCoordMax.x, texCoordMin.y };
			s_Data.TextVertexBufferPtr->EntityID = entityID;
			++s_Data.TextVertexBufferPtr;

			s_Data.TextIndexCount += 6;
			++s_Data.Stats.QuadCount;

			if (i < (string.size() - 1))
			{
				double advance = glyph->getAdvance();
				char nextCharacter = string[i + 1];
				fontGeometry.getAdvance(advance, character, nextCharacter);

				x += (fsScale * advance) + textParams.Kerning;
			}
		}
	}

	void Renderer2D::DrawString(const std::string& string, const glm::mat4& transform, const TextComponent& component, int entityID)
	{
		DrawString(string, component.FontAsset, transform, { component.Color, component.Kerning, component.LineSpacing }, entityID);
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
}
