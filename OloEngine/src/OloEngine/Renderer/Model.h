#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace OloEngine
{
	class Model
	{
	public:
		Model() = default;
		explicit Model(const std::string& path);
		~Model() = default;

		void LoadModel(const std::string& path);
		void Draw(const glm::mat4& transform, const Material& material) const;
		
		// Calculate bounding volumes for the entire model
		void CalculateBounds();
		
		// Bounding volume accessors
		[[nodiscard]] const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
		[[nodiscard]] const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
		
		// Get transformed bounding volumes
		[[nodiscard]] BoundingBox GetTransformedBoundingBox(const glm::mat4& transform) const { return m_BoundingBox.Transform(transform); }
		[[nodiscard]] BoundingSphere GetTransformedBoundingSphere(const glm::mat4& transform) const { return m_BoundingSphere.Transform(transform); }

	private:
		void ProcessNode(const aiNode* node, const aiScene* scene);
		Ref<Mesh> ProcessMesh(const aiMesh* mesh, const aiScene* scene);
		std::vector<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type);

		std::vector<Ref<Mesh>> m_Meshes;
		std::string m_Directory;
		std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;
		
		BoundingBox m_BoundingBox;
		BoundingSphere m_BoundingSphere;
	};
}
