#include "OloEnginePCH.h"
#include "MeshPrimitives.h"
#include "MeshSource.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"

namespace OloEngine
{
    // Static shared fullscreen triangle VAO (lazy-initialized)
    static Ref<VertexArray> s_FullscreenTriangleVA;

    Ref<VertexArray> MeshPrimitives::GetFullscreenTriangle()
    {
        if (s_FullscreenTriangleVA)
        {
            return s_FullscreenTriangleVA;
        }

        struct FullscreenVertex
        {
            glm::vec3 Position;
            glm::vec2 TexCoord;
        };

        static_assert(sizeof(FullscreenVertex) == sizeof(f32) * 5,
                      "FullscreenVertex must be exactly 5 floats");

        FullscreenVertex vertices[3] = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 3.0f, -1.0f, 0.0f }, { 2.0f, 0.0f } },
            { { -1.0f, 3.0f, 0.0f }, { 0.0f, 2.0f } }
        };

        u32 indices[3] = { 0, 1, 2 };

        s_FullscreenTriangleVA = VertexArray::Create();

        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(
            static_cast<const void*>(vertices),
            static_cast<u32>(sizeof(vertices)));

        vertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                  { ShaderDataType::Float2, "a_TexCoord" } });

        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, 3);

        s_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        s_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);

        return s_FullscreenTriangleVA;
    }

    Ref<Mesh> MeshPrimitives::CreateCube()
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices = {
            // Front face
            { { 0.5f, 0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },   // 0
            { { 0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },  // 1
            { { -0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } }, // 2
            { { -0.5f, 0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },  // 3

            // Back face
            { { 0.5f, 0.5f, -0.5f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } },   // 4
            { { 0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },  // 5
            { { -0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f } }, // 6
            { { -0.5f, 0.5f, -0.5f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },  // 7

            // Right face
            { { 0.5f, 0.5f, 0.5f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },   // 8
            { { 0.5f, -0.5f, 0.5f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },  // 9
            { { 0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } }, // 10
            { { 0.5f, 0.5f, -0.5f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },  // 11

            // Left face
            { { -0.5f, 0.5f, 0.5f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },   // 12
            { { -0.5f, -0.5f, 0.5f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },  // 13
            { { -0.5f, -0.5f, -0.5f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } }, // 14
            { { -0.5f, 0.5f, -0.5f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },  // 15

            // Top face
            { { 0.5f, 0.5f, 0.5f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },   // 16
            { { 0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },  // 17
            { { -0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } }, // 18
            { { -0.5f, 0.5f, 0.5f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },  // 19

            // Bottom face
            { { 0.5f, -0.5f, 0.5f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },   // 20
            { { 0.5f, -0.5f, -0.5f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f } },  // 21
            { { -0.5f, -0.5f, -0.5f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } }, // 22
            { { -0.5f, -0.5f, 0.5f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } }   // 23
        };

        std::vector<u32> indices = {
            // Front face
            0, 1, 3, 1, 2, 3,
            // Back face
            4, 5, 7, 5, 6, 7,
            // Right face
            8, 9, 11, 9, 10, 11,
            // Left face
            12, 13, 15, 13, 14, 15,
            // Top face
            16, 17, 19, 17, 18, 19,
            // Bottom face
            20, 21, 23, 21, 22, 23
        };

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Cube";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateSphere(f32 radius, u32 segments)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(segments >= 2, "CreateSphere requires segments >= 2");

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        const u32 rings = segments;
        const u32 sectors = segments * 2;

        const f32 R = 1.0f / static_cast<f32>(rings - 1);
        const f32 S = 1.0f / static_cast<f32>(sectors - 1);

        vertices.reserve(rings * sectors);

        for (u32 r = 0; r < rings; r++)
        {
            for (u32 s = 0; s < sectors; s++)
            {
                const f32 y = sin(-glm::pi<f32>() / 2 + glm::pi<f32>() * r * R);
                const f32 x = cos(2 * glm::pi<f32>() * s * S) * sin(glm::pi<f32>() * r * R);
                const f32 z = sin(2 * glm::pi<f32>() * s * S) * sin(glm::pi<f32>() * r * R);

                // Position
                glm::vec3 position = glm::vec3(x, y, z) * radius;

                // Normal (normalized position for a sphere)
                glm::vec3 normal = glm::normalize(position);

                // Texture coordinates
                auto texCoord = glm::vec2(s * S, r * R);

                vertices.emplace_back(position, normal, texCoord);
            }
        }

        // Generate indices
        indices.reserve((rings - 1) * (sectors - 1) * 6);
        for (u32 r = 0; r < rings - 1; r++)
        {
            for (u32 s = 0; s < sectors - 1; s++)
            {
                const u32 curRow = r * sectors;
                const u32 nextRow = (r + 1) * sectors;

                indices.push_back(curRow + s);
                indices.push_back(nextRow + s);
                indices.push_back(nextRow + (s + 1));

                indices.push_back(curRow + s);
                indices.push_back(nextRow + (s + 1));
                indices.push_back(curRow + (s + 1));
            }
        }

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Sphere";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreatePlane(f32 width, f32 length)
    {
        OLO_PROFILE_FUNCTION();

        const f32 halfWidth = width * 0.5f;
        const f32 halfLength = length * 0.5f;

        std::vector<Vertex> vertices = {
            // Top face (facing positive Y)
            { { halfWidth, 0.0f, halfLength }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { { halfWidth, 0.0f, -halfLength }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            { { -halfWidth, 0.0f, -halfLength }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -halfWidth, 0.0f, halfLength }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } }
        };

        std::vector<u32> indices = {
            0, 1, 3, 1, 2, 3 // Top face
        };

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Plane";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateCylinder(f32 radius, f32 height, u32 segments)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        const f32 halfHeight = height * 0.5f;
        const f32 angleStep = 2.0f * glm::pi<f32>() / segments;

        // Reserve space for vertices: top circle + bottom circle + side quads
        vertices.reserve(segments * 4 + 2);

        // Center vertices for top and bottom caps
        vertices.emplace_back(glm::vec3(0.0f, halfHeight, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.5f, 0.5f));   // Top center
        vertices.emplace_back(glm::vec3(0.0f, -halfHeight, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.5f, 0.5f)); // Bottom center

        // Generate circle vertices
        for (u32 i = 0; i < segments; i++)
        {
            const f32 angle = i * angleStep;
            const f32 x = cos(angle) * radius;
            const f32 z = sin(angle) * radius;
            const f32 u = static_cast<f32>(i) / segments;

            // Top circle
            vertices.emplace_back(glm::vec3(x, halfHeight, z), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f));

            // Bottom circle
            vertices.emplace_back(glm::vec3(x, -halfHeight, z), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f));

            // Side vertices (two for each position to have different normals)
            vertices.emplace_back(glm::vec3(x, halfHeight, z), glm::vec3(x / radius, 0.0f, z / radius), glm::vec2(u, 1.0f));
            vertices.emplace_back(glm::vec3(x, -halfHeight, z), glm::vec3(x / radius, 0.0f, z / radius), glm::vec2(u, 0.0f));
        }

        // Generate indices
        for (u32 i = 0; i < segments; i++)
        {
            const u32 next = (i + 1) % segments;

            // Top cap (fan triangulation)
            indices.push_back(0);
            indices.push_back(2 + i * 4);
            indices.push_back(2 + next * 4);

            // Bottom cap (fan triangulation)
            indices.push_back(1);
            indices.push_back(2 + next * 4 + 1);
            indices.push_back(2 + i * 4 + 1);

            // Side faces
            const u32 sideTop = 2 + i * 4 + 2;
            const u32 sideBottom = 2 + i * 4 + 3;
            const u32 nextSideTop = 2 + next * 4 + 2;
            const u32 nextSideBottom = 2 + next * 4 + 3;

            indices.push_back(sideTop);
            indices.push_back(sideBottom);
            indices.push_back(nextSideTop);

            indices.push_back(sideBottom);
            indices.push_back(nextSideBottom);
            indices.push_back(nextSideTop);
        }

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Cylinder";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateCone(f32 radius, f32 height, u32 segments)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        const f32 angleStep = 2.0f * glm::pi<f32>() / segments;

        // Tip vertex
        vertices.emplace_back(glm::vec3(0.0f, height, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.5f, 1.0f));

        // Bottom center vertex
        vertices.emplace_back(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.5f, 0.5f));

        // Generate base circle and side vertices
        for (u32 i = 0; i < segments; i++)
        {
            const f32 angle = i * angleStep;
            const f32 x = cos(angle) * radius;
            const f32 z = sin(angle) * radius;
            const f32 u = static_cast<f32>(i) / segments;

            // Base circle vertex
            vertices.emplace_back(glm::vec3(x, 0.0f, z), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f));

            // Side vertex (with normal pointing outward from cone surface)
            const glm::vec3 sideNormal = glm::normalize(glm::vec3(x, height / radius, z));
            vertices.emplace_back(glm::vec3(x, 0.0f, z), sideNormal, glm::vec2(u, 0.0f));
        }

        // Generate indices
        for (u32 i = 0; i < segments; i++)
        {
            const u32 next = (i + 1) % segments;

            // Base triangle (pointing downward)
            indices.push_back(1);
            indices.push_back(2 + next * 2);
            indices.push_back(2 + i * 2);

            // Side triangle
            indices.push_back(0);
            indices.push_back(2 + i * 2 + 1);
            indices.push_back(2 + next * 2 + 1);
        }

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Cone";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateSkyboxCube()
    {
        OLO_PROFILE_FUNCTION();

        // For a skybox, we only need positions as they'll be used as the texture coordinates
        std::vector<Vertex> vertices = {
            // Right face (+X)
            { { 1.0f, 1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },

            // Left face (-X)
            { { -1.0f, 1.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -1.0f, -1.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -1.0f, -1.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -1.0f, 1.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },

            // Top face (+Y)
            { { -1.0f, 1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, 1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },

            // Bottom face (-Y)
            { { -1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -1.0f, -1.0f, 1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, 1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },

            // Front face (+Z)
            { { -1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { -1.0f, -1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },

            // Back face (-Z)
            { { 1.0f, 1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
            { { -1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
            { { -1.0f, 1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } }
        };

        std::vector<u32> indices = {
            // Right face
            0, 1, 2, 2, 3, 0,
            // Left face
            4, 5, 6, 6, 7, 4,
            // Top face
            8, 9, 10, 10, 11, 8,
            // Bottom face
            12, 13, 14, 14, 15, 12,
            // Front face
            16, 17, 18, 18, 19, 16,
            // Back face
            20, 21, 22, 22, 23, 20
        };

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "SkyboxCube";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateFullscreenQuad()
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
            { { 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
            { { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } }
        };

        std::vector<u32> indices = {
            0, 1, 2, 2, 3, 0
        };

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "FullscreenQuad";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateIcosphere(f32 radius, u32 subdivisions)
    {
        OLO_PROFILE_FUNCTION();

        // Start with icosahedron vertices
        const f32 t = (1.0f + sqrt(5.0f)) / 2.0f; // Golden ratio

        std::vector<Vertex> vertices = {
            // 12 vertices of icosahedron
            { { -1, t, 0 }, {}, {} },
            { { 1, t, 0 }, {}, {} },
            { { -1, -t, 0 }, {}, {} },
            { { 1, -t, 0 }, {}, {} },
            { { 0, -1, t }, {}, {} },
            { { 0, 1, t }, {}, {} },
            { { 0, -1, -t }, {}, {} },
            { { 0, 1, -t }, {}, {} },
            { { t, 0, -1 }, {}, {} },
            { { t, 0, 1 }, {}, {} },
            { { -t, 0, -1 }, {}, {} },
            { { -t, 0, 1 }, {}, {} }
        };

        std::vector<u32> indices = {
            // 20 triangular faces of icosahedron
            0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11,
            1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
            3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9,
            4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1
        };

        // Subdivide faces
        for (u32 sub = 0; sub < subdivisions; sub++)
        {
            std::vector<u32> newIndices;
            newIndices.reserve(indices.size() * 4);

            for (sizet i = 0; i < indices.size(); i += 3)
            {
                u32 v1 = indices[i];
                u32 v2 = indices[i + 1];
                u32 v3 = indices[i + 2];

                // Create midpoint vertices
                glm::vec3 mid12 = glm::normalize((vertices[v1].Position + vertices[v2].Position) * 0.5f);
                glm::vec3 mid23 = glm::normalize((vertices[v2].Position + vertices[v3].Position) * 0.5f);
                glm::vec3 mid13 = glm::normalize((vertices[v1].Position + vertices[v3].Position) * 0.5f);

                u32 midIdx12 = static_cast<u32>(vertices.size());
                u32 midIdx23 = midIdx12 + 1;
                u32 midIdx13 = midIdx12 + 2;

                vertices.push_back({ mid12, {}, {} });
                vertices.push_back({ mid23, {}, {} });
                vertices.push_back({ mid13, {}, {} });

                // Create 4 new triangles
                newIndices.insert(newIndices.end(), { v1, midIdx12, midIdx13 });
                newIndices.insert(newIndices.end(), { v2, midIdx23, midIdx12 });
                newIndices.insert(newIndices.end(), { v3, midIdx13, midIdx23 });
                newIndices.insert(newIndices.end(), { midIdx12, midIdx23, midIdx13 });
            }
            indices = std::move(newIndices);
        }

        // Normalize positions to sphere radius and calculate normals/UVs
        for (auto& vertex : vertices)
        {
            vertex.Position = glm::normalize(vertex.Position) * radius;
            vertex.Normal = glm::normalize(vertex.Position);

            // Spherical UV mapping - improved to handle seams
            vertex.TexCoord.x = atan2(vertex.Normal.z, vertex.Normal.x) / (2.0f * glm::pi<f32>()) + 0.5f;
            vertex.TexCoord.y = asin(vertex.Normal.y) / glm::pi<f32>() + 0.5f;
        }

        // Fix UV seam artifacts by detecting triangles that cross the seam and duplicating vertices
        std::vector<Vertex> finalVertices = vertices;
        std::vector<u32> finalIndices;
        finalIndices.reserve(indices.size());

        for (sizet i = 0; i < indices.size(); i += 3)
        {
            u32 v1 = indices[i];
            u32 v2 = indices[i + 1];
            u32 v3 = indices[i + 2];

            f32 u1 = vertices[v1].TexCoord.x;
            f32 u2 = vertices[v2].TexCoord.x;
            f32 u3 = vertices[v3].TexCoord.x;

            // Check if triangle crosses UV seam (large difference in U coordinates)
            const f32 seamThreshold = 0.75f; // If U coordinates differ by more than this, we're crossing the seam
            bool crossesSeam = (std::fabs(u1 - u2) > seamThreshold) || (std::fabs(u2 - u3) > seamThreshold) || (std::fabs(u1 - u3) > seamThreshold);

            if (crossesSeam)
            {
                // Duplicate vertices and adjust UV coordinates to ensure continuity
                std::array<u32, 3> newIndices;
                std::array<f32, 3> originalU = { u1, u2, u3 };
                std::array<u32, 3> originalIndices = { v1, v2, v3 };

                for (int j = 0; j < 3; j++)
                {
                    Vertex newVertex = vertices[originalIndices[j]];

                    // If this vertex has U < 0.25 and triangle contains vertices with U > 0.75, wrap U to > 1.0
                    if (originalU[j] < 0.25f && (originalU[(j + 1) % 3] > 0.75f || originalU[(j + 2) % 3] > 0.75f))
                    {
                        newVertex.TexCoord.x += 1.0f;
                    }

                    finalVertices.push_back(newVertex);
                    newIndices[j] = static_cast<u32>(finalVertices.size() - 1);
                }

                finalIndices.insert(finalIndices.end(), { newIndices[0], newIndices[1], newIndices[2] });
            }
            else
            {
                // No seam crossing, use original indices
                finalIndices.insert(finalIndices.end(), { v1, v2, v3 });
            }
        }

        auto meshSource = Ref<MeshSource>::Create(finalVertices, finalIndices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(finalIndices.size());
        submesh.m_VertexCount = static_cast<u32>(finalVertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Icosphere";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateTorus(f32 majorRadius, f32 minorRadius, u32 majorSegments, u32 minorSegments)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        vertices.reserve(majorSegments * minorSegments);

        for (u32 i = 0; i < majorSegments; i++)
        {
            const f32 u = static_cast<f32>(i) / majorSegments * 2.0f * glm::pi<f32>();

            for (u32 j = 0; j < minorSegments; j++)
            {
                const f32 v = static_cast<f32>(j) / minorSegments * 2.0f * glm::pi<f32>();

                const f32 x = (majorRadius + minorRadius * cos(v)) * cos(u);
                const f32 y = minorRadius * sin(v);
                const f32 z = (majorRadius + minorRadius * cos(v)) * sin(u);

                glm::vec3 position(x, y, z);

                // Calculate normal
                glm::vec3 center(majorRadius * cos(u), 0, majorRadius * sin(u));
                glm::vec3 normal = glm::normalize(position - center);

                glm::vec2 texCoord(static_cast<f32>(i) / majorSegments, static_cast<f32>(j) / minorSegments);

                vertices.emplace_back(position, normal, texCoord);
            }
        }

        // Generate indices
        for (u32 i = 0; i < majorSegments; i++)
        {
            for (u32 j = 0; j < minorSegments; j++)
            {
                const u32 current = i * minorSegments + j;
                const u32 next = ((i + 1) % majorSegments) * minorSegments + j;
                const u32 currentNext = i * minorSegments + ((j + 1) % minorSegments);
                const u32 nextNext = ((i + 1) % majorSegments) * minorSegments + ((j + 1) % minorSegments);

                indices.insert(indices.end(), { current, next, currentNext });
                indices.insert(indices.end(), { currentNext, next, nextNext });
            }
        }

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Torus";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateGrid(f32 size, u32 divisions)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<Vertex> vertices;
        std::vector<u32> indices;

        const f32 halfSize = size * 0.5f;
        const f32 step = size / divisions;

        // Generate grid lines (vertical and horizontal)
        for (u32 i = 0; i <= divisions; i++)
        {
            const f32 pos = -halfSize + i * step;

            // Vertical lines
            vertices.emplace_back(glm::vec3(pos, 0.0f, -halfSize), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f));
            vertices.emplace_back(glm::vec3(pos, 0.0f, halfSize), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f));

            // Horizontal lines
            vertices.emplace_back(glm::vec3(-halfSize, 0.0f, pos), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f));
            vertices.emplace_back(glm::vec3(halfSize, 0.0f, pos), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f));
        }

        // Generate line indices
        for (u32 i = 0; i < (divisions + 1) * 4; i += 2)
        {
            indices.push_back(i);
            indices.push_back(i + 1);
        }

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "Grid";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateWireframeCube()
    {
        OLO_PROFILE_FUNCTION();

        // Note: Normals are set to (0,0,1) as a default since wireframe rendering
        // typically doesn't use normals for lighting calculations
        std::vector<Vertex> vertices = {
            // 8 vertices of a cube
            { { -0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
            { { 0.5f, 0.5f, -0.5f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
            { { -0.5f, 0.5f, -0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
            { { -0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
            { { 0.5f, 0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
            { { -0.5f, 0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } }
        };

        std::vector<u32> indices = {
            // Bottom face edges
            0, 1, 1, 2, 2, 3, 3, 0,
            // Top face edges
            4, 5, 5, 6, 6, 7, 7, 4,
            // Vertical edges
            0, 4, 1, 5, 2, 6, 3, 7
        };

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "WireframeCube";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

    Ref<Mesh> MeshPrimitives::CreateCoordinateAxes(f32 length)
    {
        OLO_PROFILE_FUNCTION();

        // NOTE: In this function, the Normal attribute is repurposed to store axis colors
        // instead of actual surface normals. This is used for debugging and visualization:
        // - X-axis: Red (1, 0, 0)
        // - Y-axis: Green (0, 1, 0)
        // - Z-axis: Blue (0, 0, 1)
        // The shader can interpret these "normals" as color values for axis rendering.
        std::vector<Vertex> vertices = {
            // X-axis (red)
            { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { length, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },

            // Y-axis (green)
            { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 0.0f, length, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },

            // Z-axis (blue)
            { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 0.0f, 0.0f, length }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }
        };

        std::vector<u32> indices = {
            0, 1, // X-axis line
            2, 3, // Y-axis line
            4, 5  // Z-axis line
        };

        auto meshSource = Ref<MeshSource>::Create(vertices, indices);

        // Create a default submesh for the entire mesh
        Submesh submesh;
        submesh.m_BaseVertex = 0;
        submesh.m_BaseIndex = 0;
        submesh.m_IndexCount = static_cast<u32>(indices.size());
        submesh.m_VertexCount = static_cast<u32>(vertices.size());
        submesh.m_MaterialIndex = 0;
        submesh.m_IsRigged = false;
        submesh.m_NodeName = "CoordinateAxes";
        meshSource->AddSubmesh(submesh);

        meshSource->Build();
        return Ref<Mesh>::Create(meshSource, 0);
    }

} // namespace OloEngine
