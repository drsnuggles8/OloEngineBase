#include <filesystem>

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"

namespace OloEngine
{
	Model::Model(const std::string& path)
	{
		LoadModel(path);
	}

	void Model::LoadModel(const std::string& path)
	{
		OLO_PROFILE_FUNCTION();

		// Create an instance of the Importer class
		Assimp::Importer importer;

		// And have it read the given file with some postprocessing
		const aiScene* scene = importer.ReadFile(path,
			aiProcess_Triangulate |           // Make sure we get triangles
			aiProcess_GenNormals |           // Create normals if not present
			aiProcess_CalcTangentSpace |     // Calculate tangents and bitangents
			aiProcess_FlipUVs |             // Flip texture coordinates
			aiProcess_ValidateDataStructure  // Validate the imported data structure
		);

		// Check for errors
		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
		{
			OLO_CORE_ERROR("ASSIMP Error: {0}", importer.GetErrorString());
			return;
		}

		// Store the directory path
		m_Directory = std::filesystem::path(path).parent_path().string();

		// Process all the nodes recursively
		ProcessNode(scene->mRootNode, scene);
		
		// Calculate bounding volumes for the entire model
		CalculateBounds();
	}

	void Model::ProcessNode(const aiNode* node, const aiScene* scene)
	{
		OLO_PROFILE_FUNCTION();

		// Process all the node's meshes (if any)
		for (u32 i = 0; i < node->mNumMeshes; i++)
		{
			const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
			m_Meshes.push_back(ProcessMesh(mesh, scene));
		}

		// Then do the same for each of its children
		for (u32 i = 0; i < node->mNumChildren; i++)
		{
			ProcessNode(node->mChildren[i], scene);
		}
	}

	Ref<Mesh> Model::ProcessMesh(const aiMesh* mesh, const aiScene* scene)
	{
		OLO_PROFILE_FUNCTION();

		std::vector<Vertex> vertices;
		std::vector<u32> indices;
		std::vector<Ref<Texture2D>> textures;

		// Process vertices
		for (u32 i = 0; i < mesh->mNumVertices; i++)
		{
			Vertex vertex;

			// Process vertex positions, normals and texture coordinates
			vertex.Position = glm::vec3(
				mesh->mVertices[i].x,
				mesh->mVertices[i].y,
				mesh->mVertices[i].z
			);

			if (mesh->HasNormals())
			{
				vertex.Normal = glm::vec3(
					mesh->mNormals[i].x,
					mesh->mNormals[i].y,
					mesh->mNormals[i].z
				);
			}

			if (mesh->mTextureCoords[0])
			{
				vertex.TexCoord = glm::vec2(
					mesh->mTextureCoords[0][i].x,
					mesh->mTextureCoords[0][i].y
				);
			}
			else
			{
				vertex.TexCoord = glm::vec2(0.0f, 0.0f);
			}

			vertices.push_back(vertex);
		}

		// Process indices
		for (u32 i = 0; i < mesh->mNumFaces; i++)
		{
			const aiFace face = mesh->mFaces[i];
			for (u32 j = 0; j < face.mNumIndices; j++)
			{
				indices.push_back(face.mIndices[j]);
			}
		}

		// Process material
		if (mesh->mMaterialIndex < scene->mNumMaterials)
		{
			const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

			std::vector<Ref<Texture2D>> diffuseMaps = LoadMaterialTextures(material, aiTextureType_DIFFUSE);
			textures.insert(textures.end(), diffuseMaps.begin(), diffuseMaps.end());

			std::vector<Ref<Texture2D>> specularMaps = LoadMaterialTextures(material, aiTextureType_SPECULAR);
			textures.insert(textures.end(), specularMaps.begin(), specularMaps.end());
		}

		return CreateRef<Mesh>(vertices, indices);
	}

	std::vector<Ref<Texture2D>> Model::LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type)
	{
		OLO_PROFILE_FUNCTION();

		std::vector<Ref<Texture2D>> textures;

		for (u32 i = 0; i < mat->GetTextureCount(type); i++)
		{
			aiString str;
			mat->GetTexture(type, i, &str);

			// Check if texture was loaded before
			std::string texturePath = m_Directory + "/" + str.C_Str();
			if (!m_LoadedTextures.contains(texturePath))
			{
				// Texture not loaded yet - load it
				Ref<Texture2D> texture = Texture2D::Create(texturePath);
				
				if (texture && texture->IsLoaded())
				{
					m_LoadedTextures[texturePath] = texture;
					textures.push_back(texture);
				}
				else
				{
					OLO_CORE_WARN("Failed to load texture at path: {0}", texturePath);
				}
			}
			else
			{
				// Texture already loaded - reuse it
				textures.push_back(m_LoadedTextures[texturePath]);
			}
		}

		return textures;
	}

	void Model::CalculateBounds()
	{
		OLO_PROFILE_FUNCTION();
		
		if (m_Meshes.empty())
		{
			// Default to a unit cube and sphere around origin if no meshes
			m_BoundingBox = BoundingBox(glm::vec3(-0.5f), glm::vec3(0.5f));
			m_BoundingSphere = BoundingSphere(glm::vec3(0.0f), 0.5f);
			return;
		}
		
		// Start with the first mesh's bounding volumes
		m_BoundingBox = m_Meshes[0]->GetBoundingBox();
		m_BoundingSphere = m_Meshes[0]->GetBoundingSphere();
		
		// Expand to include all other meshes
		for (size_t i = 1; i < m_Meshes.size(); i++)
		{
			const BoundingBox& meshBox = m_Meshes[i]->GetBoundingBox();
			
			// Expand the model's bounding box
			m_BoundingBox.Min = glm::min(m_BoundingBox.Min, meshBox.Min);
			m_BoundingBox.Max = glm::max(m_BoundingBox.Max, meshBox.Max);
		}
		
		// Recalculate the bounding sphere based on the final bounding box
		glm::vec3 center = (m_BoundingBox.Min + m_BoundingBox.Max) * 0.5f;
		float radius = glm::length(m_BoundingBox.Max - center);
		
		// Add a small margin (5%) to prevent edge cases
		radius *= 1.05f;
		
		m_BoundingSphere = BoundingSphere(center, radius);
	}

	void Model::Draw(const glm::mat4& transform, const Material& material) const
	{
		OLO_PROFILE_FUNCTION();

		// Draw all meshes and let Renderer3D handle culling
		for (const auto& mesh : m_Meshes)
		{
			// Pass the model's bounding sphere to the Renderer3D for more efficient culling
			Renderer3D::DrawMesh(mesh, transform, material);
		}
	}
}
