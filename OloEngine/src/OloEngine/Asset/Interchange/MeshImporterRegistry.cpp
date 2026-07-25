#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/MeshImporterRegistry.h"

#include "OloEngine/Asset/Interchange/AssimpMeshImporter.h"
#include "OloEngine/Core/Log.h"

#if defined(OLO_WITH_USD)
#include "OloEngine/Asset/Interchange/USD/UsdMeshImporter.h"
#endif
#if defined(OLO_WITH_ALEMBIC)
#include "OloEngine/Asset/Interchange/Alembic/AlembicMeshImporter.h"
#endif

#include <algorithm>
#include <cctype>
#include <mutex>

namespace OloEngine
{
    namespace
    {
        // Lower-case, strip a single leading dot. Matches AssetExtensions::NormalizeExtension
        // so a caller passing ".USDA", "usda", or "Usda" all resolve identically.
        std::string NormalizeExtension(std::string_view extension)
        {
            std::string normalized(extension);
            if (!normalized.empty() && normalized.front() == '.')
                normalized.erase(normalized.begin());
            std::ranges::transform(normalized, normalized.begin(),
                                   [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return normalized;
        }
    } // namespace

    MeshImporterRegistry& MeshImporterRegistry::Get()
    {
        static MeshImporterRegistry s_Instance;
        return s_Instance;
    }

    void MeshImporterRegistry::EnsureBuiltinsRegistered() const
    {
        static std::once_flag s_Flag;
        std::call_once(s_Flag,
                       [this]()
                       {
                           auto* self = const_cast<MeshImporterRegistry*>(this);

                           // Assimp is the fallback: every extension that reached the old
                           // Model/Assimp path still resolves to it, so this refactor changes
                           // no behaviour for FBX/glTF/OBJ/DAE/VRM/PLY. Only add a builtin
                           // when the slot is free so an external Register() (a game/plugin
                           // overriding a format) always wins.
                           if (!self->m_Fallback)
                               self->m_Fallback = CreateScope<AssimpMeshImporter>();

                           // Also claim the classic Assimp extensions explicitly so
                           // IsSupported()/GetRegisteredExtensions() report them without
                           // relying on the fallback catch-all.
                           constexpr std::string_view kAssimpExtensions[] = {
                               "fbx", "gltf", "glb", "obj", "dae", "vrm", "ply"
                           };
                           for (std::string_view ext : kAssimpExtensions)
                           {
                               if (!self->m_Importers.contains(std::string(ext)))
                                   self->m_Importers.emplace(std::string(ext), CreateScope<AssimpMeshImporter>());
                           }

#if defined(OLO_WITH_USD)
                           constexpr std::string_view kUsdExtensions[] = { "usd", "usda", "usdc", "usdz" };
                           for (std::string_view ext : kUsdExtensions)
                           {
                               if (!self->m_Importers.contains(std::string(ext)))
                                   self->m_Importers.emplace(std::string(ext), CreateScope<UsdMeshImporter>());
                           }
#endif
#if defined(OLO_WITH_ALEMBIC)
                           if (!self->m_Importers.contains("abc"))
                               self->m_Importers.emplace("abc", CreateScope<AlembicMeshImporter>());
#endif
                       });
    }

    void MeshImporterRegistry::Register(std::string_view extension, Scope<MeshImporter> importer)
    {
        if (!importer)
        {
            OLO_CORE_ERROR("MeshImporterRegistry::Register - null importer for extension '{}'", extension);
            return;
        }
        // insert_or_assign: a later registration replaces an earlier one for the same extension.
        m_Importers.insert_or_assign(NormalizeExtension(extension), std::move(importer));
    }

    void MeshImporterRegistry::SetFallback(Scope<MeshImporter> importer)
    {
        m_Fallback = std::move(importer);
    }

    MeshImporter* MeshImporterRegistry::Find(std::string_view extension) const
    {
        EnsureBuiltinsRegistered();

        if (auto it = m_Importers.find(NormalizeExtension(extension)); it != m_Importers.end())
            return it->second.get();

        return m_Fallback.get();
    }

    MeshImportResult MeshImporterRegistry::Import(const std::filesystem::path& path,
                                                  const MeshImportOptions& options) const
    {
        const std::string extension = path.has_extension() ? path.extension().string() : std::string{};
        MeshImporter* importer = Find(extension);
        if (!importer)
        {
            return MeshImportResult::Failure(
                "No mesh importer registered for extension '" + extension + "' (path: " + path.string() + ")");
        }

        MeshImportResult result = importer->Import(path, options);
        if (!result.Succeeded() && result.Error.empty())
            result.Error = std::string(importer->GetName()) + " import produced no geometry: " + path.string();
        return result;
    }

    bool MeshImporterRegistry::IsSupported(std::string_view extension) const
    {
        EnsureBuiltinsRegistered();
        return m_Importers.contains(NormalizeExtension(extension)) || m_Fallback != nullptr;
    }

    std::vector<std::string> MeshImporterRegistry::GetRegisteredExtensions() const
    {
        EnsureBuiltinsRegistered();

        std::vector<std::string> extensions;
        extensions.reserve(m_Importers.size());
        for (const auto& [ext, importer] : m_Importers)
            extensions.push_back(ext);
        std::ranges::sort(extensions);
        return extensions;
    }
} // namespace OloEngine
