#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetManager/AssetManagerBase.h"

#include <string>
#include <filesystem>

namespace OloEngine
{
	struct ProjectConfig
	{
		std::string Name = "Untitled";

		std::filesystem::path StartScene;

		std::filesystem::path AssetDirectory;
		std::filesystem::path ScriptModulePath;
	};

	class Project : public RefCounted
	{
	public:
		static const std::filesystem::path& GetProjectDirectory()
		{
			OLO_CORE_ASSERT(s_ActiveProject);
			return s_ActiveProject->m_ProjectDirectory;
		}

		static std::filesystem::path GetAssetDirectory()
		{
			OLO_CORE_ASSERT(s_ActiveProject);
			return GetProjectDirectory() / s_ActiveProject->m_Config.AssetDirectory;
		}

		static std::filesystem::path GetAssetFileSystemPath(const std::filesystem::path& path)
		{
			OLO_CORE_ASSERT(s_ActiveProject);
			return GetAssetDirectory() / path;
		}

		static std::filesystem::path GetAssetRelativeFileSystemPath(const std::filesystem::path& path)
		{
			OLO_CORE_ASSERT(s_ActiveProject);
			return std::filesystem::relative(path, GetAssetDirectory());
		}

		ProjectConfig& GetConfig() { return m_Config; }

		static Ref<Project> GetActive() { return s_ActiveProject; }

		/**
		 * @brief Get the current project's asset manager
		 * @return Reference to the active asset manager
		 */
		static Ref<AssetManagerBase> GetAssetManager() { return s_AssetManager; }

		/**
		 * @brief Set the active asset manager for the project
		 * @param assetManager Asset manager to set as active
		 */
		static void SetAssetManager(Ref<AssetManagerBase> assetManager) { s_AssetManager = assetManager; }

		static Ref<Project> New();
		static Ref<Project> Load(const std::filesystem::path& path);
		static bool SaveActive(const std::filesystem::path& path);
	private:
		ProjectConfig m_Config;
		std::filesystem::path m_ProjectDirectory;

		inline static Ref<Project> s_ActiveProject;
		inline static Ref<AssetManagerBase> s_AssetManager;
	};
}
