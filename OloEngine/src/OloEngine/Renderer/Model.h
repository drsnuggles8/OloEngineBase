#pragma once

#include <string>
#include <vector>
#include <unordered_map>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Texture.h"

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

	private:
		void ProcessNode(const aiNode* node, const aiScene* scene);
		Ref<Mesh> ProcessMesh(const aiMesh* mesh, const aiScene* scene);
		std::vector<Ref<Texture2D>> LoadMaterialTextures(const aiMaterial* mat, const aiTextureType type);

		std::vector<Ref<Mesh>> m_Meshes;
		std::string m_Directory;
		std::unordered_map<std::string, Ref<Texture2D>> m_LoadedTextures;
	};
}
