#include "OloEnginePCH.h"
#include "MeshPrimitives.h"
#include "MeshSource.h"

namespace OloEngine {

	Ref<Mesh> MeshPrimitives::CreateCube()
	{
		OLO_PROFILE_FUNCTION();

		std::vector<Vertex> vertices = {
			// Front face
			{ { 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f} }, // 0
			{ { 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f} }, // 1
			{ {-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} }, // 2
			{ {-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f} }, // 3

			// Back face
			{ { 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f} }, // 4
			{ { 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} }, // 5
			{ {-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f} }, // 6
			{ {-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f} }, // 7

			// Right face
			{ { 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f} }, // 8
			{ { 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} }, // 9
			{ { 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f} }, // 10
			{ { 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f} }, // 11

			// Left face
			{ {-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f} }, // 12
			{ {-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f} }, // 13
			{ {-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} }, // 14
			{ {-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f} }, // 15

			// Top face
			{ { 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f} }, // 16
			{ { 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f} }, // 17
			{ {-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f} }, // 18
			{ {-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} }, // 19

			// Bottom face
			{ { 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f} }, // 20
			{ { 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f} }, // 21
			{ {-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} }, // 22
			{ {-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f} }  // 23
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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Cube";
		meshSource->AddSubmesh(submesh);
		
		meshSource->Build();
		return Ref<Mesh>::Create(meshSource, 0);
	}

	Ref<Mesh> MeshPrimitives::CreateSphere(f32 radius, u32 segments)
	{
		OLO_PROFILE_FUNCTION();

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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Sphere";
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
			{ { halfWidth, 0.0f,  halfLength}, { 0.0f, 1.0f, 0.0f}, {1.0f, 1.0f} },
			{ { halfWidth, 0.0f, -halfLength}, { 0.0f, 1.0f, 0.0f}, {1.0f, 0.0f} },
			{ {-halfWidth, 0.0f, -halfLength}, { 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
			{ {-halfWidth, 0.0f,  halfLength}, { 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f} }
		};

		std::vector<u32> indices = {
			0, 1, 3, 1, 2, 3 // Top face
		};

		auto meshSource = Ref<MeshSource>::Create(vertices, indices);
		
		// Create a default submesh for the entire mesh
		Submesh submesh;
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Plane";
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
		vertices.emplace_back(glm::vec3(0.0f, halfHeight, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.5f, 0.5f)); // Top center
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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Cylinder";
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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Cone";
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
			{ { 1.0f,  1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f,  1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			
			// Left face (-X)
			{ {-1.0f,  1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			{ {-1.0f, -1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			{ {-1.0f, -1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			{ {-1.0f,  1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
			
			// Top face (+Y)
			{ {-1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
			{ {-1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
			
			// Bottom face (-Y)
			{ {-1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
			{ {-1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
			
			// Front face (+Z)
			{ {-1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
			{ {-1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
			{ { 1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
			
			// Back face (-Z)
			{ { 1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
			{ {-1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
			{ {-1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} }
		};

		std::vector<u32> indices = {
			// Right face
			 0,  1,  2,  2,  3,  0,
			// Left face
			 4,  5,  6,  6,  7,  4,
			// Top face
			 8,  9, 10, 10, 11,  8,
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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "SkyboxCube";
		meshSource->AddSubmesh(submesh);
		
		meshSource->Build();
		return Ref<Mesh>::Create(meshSource, 0);
	}

	Ref<Mesh> MeshPrimitives::CreateFullscreenQuad()
	{
		OLO_PROFILE_FUNCTION();

		std::vector<Vertex> vertices = {
			{ {-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },
			{ { 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f} },
			{ { 1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} },
			{ {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} }
		};

		std::vector<u32> indices = {
			0, 1, 2, 2, 3, 0
		};

		auto meshSource = Ref<MeshSource>::Create(vertices, indices);
		
		// Create a default submesh for the entire mesh
		Submesh submesh;
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "FullscreenQuad";
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
			{{-1, t, 0}, {}, {}}, {{ 1, t, 0}, {}, {}}, {{-1, -t, 0}, {}, {}}, {{ 1, -t, 0}, {}, {}},
			{{ 0, -1, t}, {}, {}}, {{ 0, 1, t}, {}, {}}, {{ 0, -1, -t}, {}, {}}, {{ 0, 1, -t}, {}, {}},
			{{ t, 0, -1}, {}, {}}, {{ t, 0, 1}, {}, {}}, {{-t, 0, -1}, {}, {}}, {{-t, 0, 1}, {}, {}}
		};

		std::vector<u32> indices = {
			// 20 triangular faces of icosahedron
			0, 11, 5,   0, 5, 1,    0, 1, 7,    0, 7, 10,   0, 10, 11,
			1, 5, 9,    5, 11, 4,   11, 10, 2,  10, 7, 6,   7, 1, 8,
			3, 9, 4,    3, 4, 2,    3, 2, 6,    3, 6, 8,    3, 8, 9,
			4, 9, 5,    2, 4, 11,   6, 2, 10,   8, 6, 7,    9, 8, 1
		};

		// Subdivide faces
		for (u32 sub = 0; sub < subdivisions; sub++)
		{
			std::vector<u32> newIndices;
			newIndices.reserve(indices.size() * 4);

			for (size_t i = 0; i < indices.size(); i += 3)
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

				vertices.push_back({mid12, {}, {}});
				vertices.push_back({mid23, {}, {}});
				vertices.push_back({mid13, {}, {}});

				// Create 4 new triangles
				newIndices.insert(newIndices.end(), {v1, midIdx12, midIdx13});
				newIndices.insert(newIndices.end(), {v2, midIdx23, midIdx12});
				newIndices.insert(newIndices.end(), {v3, midIdx13, midIdx23});
				newIndices.insert(newIndices.end(), {midIdx12, midIdx23, midIdx13});
			}
			indices = std::move(newIndices);
		}

		// Normalize positions to sphere radius and calculate normals/UVs
		for (auto& vertex : vertices)
		{
			vertex.Position = glm::normalize(vertex.Position) * radius;
			vertex.Normal = glm::normalize(vertex.Position);
			
			// Spherical UV mapping
			vertex.TexCoord.x = atan2(vertex.Normal.z, vertex.Normal.x) / (2.0f * glm::pi<f32>()) + 0.5f;
			vertex.TexCoord.y = asin(vertex.Normal.y) / glm::pi<f32>() + 0.5f;
		}

		auto meshSource = Ref<MeshSource>::Create(vertices, indices);
		
		// Create a default submesh for the entire mesh
		Submesh submesh;
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Icosphere";
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
				
				indices.insert(indices.end(), {current, next, currentNext});
				indices.insert(indices.end(), {currentNext, next, nextNext});
			}
		}

		auto meshSource = Ref<MeshSource>::Create(vertices, indices);
		
		// Create a default submesh for the entire mesh
		Submesh submesh;
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Torus";
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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "Grid";
		meshSource->AddSubmesh(submesh);
		
		meshSource->Build();
		return Ref<Mesh>::Create(meshSource, 0);
	}

	Ref<Mesh> MeshPrimitives::CreateWireframeCube()
	{
		OLO_PROFILE_FUNCTION();

		std::vector<Vertex> vertices = {
			// 8 vertices of a cube
			{ {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f} },
			{ { 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f} },
			{ { 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f} },
			{ {-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f} },
			{ {-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f} },
			{ { 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f} },
			{ { 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f} },
			{ {-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f} }
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
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "WireframeCube";
		meshSource->AddSubmesh(submesh);
		
		meshSource->Build();
		return Ref<Mesh>::Create(meshSource, 0);
	}

	Ref<Mesh> MeshPrimitives::CreateCoordinateAxes(f32 length)
	{
		OLO_PROFILE_FUNCTION();

		std::vector<Vertex> vertices = {
			// X-axis (red)
			{ {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f} },
			{ {length, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f} },
			
			// Y-axis (green)
			{ {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
			{ {0.0f, length, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f} },
			
			// Z-axis (blue)
			{ {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },
			{ {0.0f, 0.0f, length}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} }
		};

		std::vector<u32> indices = {
			0, 1,  // X-axis line
			2, 3,  // Y-axis line
			4, 5   // Z-axis line
		};

		auto meshSource = Ref<MeshSource>::Create(vertices, indices);
		
		// Create a default submesh for the entire mesh
		Submesh submesh;
		submesh.BaseVertex = 0;
		submesh.BaseIndex = 0;
		submesh.IndexCount = static_cast<u32>(indices.size());
		submesh.VertexCount = static_cast<u32>(vertices.size());
		submesh.MaterialIndex = 0;
		submesh.IsRigged = false;
		submesh.NodeName = "CoordinateAxes";
		meshSource->AddSubmesh(submesh);
		
		meshSource->Build();
		return Ref<Mesh>::Create(meshSource, 0);
	}

	// TODO: Update these methods to work with new MeshSource bone influence system
	/*
	Ref<SkinnedMesh> MeshPrimitives::CreateSkinnedCube()
	{
		OLO_PROFILE_FUNCTION();

		// Create cube vertices with bone indices and weights (all vertices affected by bone 0)
		std::vector<SkinnedVertex> vertices = {
			// Front face (Z+)
			{{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Back face (Z-)
			{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Left face (X-)
			{{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Right face (X+)
			{{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Bottom face (Y-)
			{{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Top face (Y+)
			{{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}
		};

		std::vector<u32> indices = {
			// Front face
			0, 1, 2, 2, 3, 0,
			// Back face
			4, 5, 6, 6, 7, 4,
			// Left face
			8, 9, 10, 10, 11, 8,
			// Right face
			12, 13, 14, 14, 15, 12,
			// Bottom face
			16, 17, 18, 18, 19, 16,
			// Top face
			20, 21, 22, 22, 23, 20
		};

		return Ref<SkinnedMesh>::Create(std::move(vertices), std::move(indices));
	}
	*/

	// TODO: Update this method to work with new MeshSource bone influence system
	/*
	Ref<SkinnedMesh> MeshPrimitives::CreateMultiBoneSkinnedCube()
	{
		OLO_PROFILE_FUNCTION();

		// Create cube with multiple bone influences for testing
		std::vector<SkinnedVertex> vertices = {
			// Front face (Z+) - influenced by bones 0 and 1
			{{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0, 1, -1, -1}, {0.8f, 0.2f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0, 1, -1, -1}, {0.6f, 0.4f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {0, 1, -1, -1}, {0.4f, 0.6f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0, 1, -1, -1}, {0.2f, 0.8f, 0.0f, 0.0f}},

			// Back face (Z-) - influenced by bones 1 and 2
			{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {1, 2, -1, -1}, {0.7f, 0.3f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {1, 2, -1, -1}, {0.5f, 0.5f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {1, 2, -1, -1}, {0.3f, 0.7f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {1, 2, -1, -1}, {0.1f, 0.9f, 0.0f, 0.0f}},

			// Left face (X-) - influenced by bone 0
			{{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Right face (X+) - influenced by bone 2
			{{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {2, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {2, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {2, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {2, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

			// Bottom face (Y-) - influenced by bones 0 and 2
			{{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {0, 2, -1, -1}, {0.5f, 0.5f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {0, 2, -1, -1}, {0.5f, 0.5f, 0.0f, 0.0f}},
			{{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, 2, -1, -1}, {0.5f, 0.5f, 0.0f, 0.0f}},
			{{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, 2, -1, -1}, {0.5f, 0.5f, 0.0f, 0.0f}},

			// Top face (Y+) - influenced by bone 1
			{{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
			{{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {1, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}
		};

		std::vector<u32> indices = {
			// Front face
			0, 1, 2, 2, 3, 0,
			// Back face
			4, 5, 6, 6, 7, 4,
			// Left face
			8, 9, 10, 10, 11, 8,
			// Right face
			12, 13, 14, 14, 15, 12,
			// Bottom face
			16, 17, 18, 18, 19, 16,
			// Top face
			20, 21, 22, 22, 23, 20
		};

		return Ref<SkinnedMesh>::Create(std::move(vertices), std::move(indices));
	}
	*/

}
