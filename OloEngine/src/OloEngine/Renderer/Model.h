#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <glm/glm.hpp>
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace OloEngine
{
	// Configuration for overriding texture paths when model's embedded paths are incorrect
	struct TextureOverride
	{
		std::string AlbedoPath;
		std::string MetallicPath;
		std::string NormalPath;
		std::string RoughnessPath;
		std::string AOPath;
		std::string EmissivePath;
		
		bool HasAnyTexture() const 
		{
			return !AlbedoPath.empty() || !MetallicPath.empty() || !NormalPath.empty() || 
			       !RoughnessPath.empty() || !AOPath.empty() || !EmissivePath.empty();
		}
	};

	class Model : public RefCounted
	{
	public:
		Model() = default;
		explicit Model(const std::string& path, const TextureOverride& textureOverride = {}, bool flipUV = false);
		~Model() = default;

		void LoadModel(const std::string& path, const TextureOverride& textureOverride = {}, bool flipUV = false);
		void Draw(const glm::mat4& transform, const Material& material) const;

		void GetDrawCommands(const glm::mat4& transform, const Material& material, std::vector<CommandPacket*>& outCommands) const;
		void GetDrawCommands(const glm::mat4& transform, std::vector<CommandPacket*>& outCommands) const;
		
		// Calculate bounding volumes for the entire model
		void CalculateBounds();
		
		// Bounding volume accessors
		[[nodiscard]] const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }
		[[nodiscard]] const BoundingSphere& GetBoundingSphere() const { return m_BoundingSphere; }
		
		// Get transformed bounding volumes
		[[nodiscard]] BoundingBox GetTransformedBoundingBox(const glm::mat4& transform) const { return m_BoundingBox.Transform(transform); }
		[[nodiscard]] BoundingSphere GetTransformedBoundingSphere(const glm::mat4& transform) const { return m_BoundingSphere.Transform(transform); }
		
		// Accessors for materials
		[[nodiscard]] const std::vector<Material>& GetMaterials() const { return m_Materials; }

	private:
		void ProcessNode(const aiNode* node, const aiScene* scene);
		Ref<Mesh> ProcessMesh(const aiMesh* mesh, const aiScene* scene);
		std::vector<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type);
		Material ProcessMaterial(const aiMaterial* mat);

		std::vector<Ref<Mesh>> m_Meshes;
		std::vector<Material> m_Materials;  // Materials corresponding to each mesh
		std::string m_Directory;
		std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;
		std::optional<TextureOverride> m_TextureOverride;
		bool m_FlipUV = false;
		
		BoundingBox m_BoundingBox;
		BoundingSphere m_BoundingSphere;
	};
}
